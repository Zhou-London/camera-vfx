#include "pacer.h"
#include <cmath>
#include <algorithm>

namespace camss {

const char* PaceModeName(PaceMode m) {
    switch (m) {
    case PaceMode::Passthrough: return "passthrough";
    case PaceMode::Fixed: return "fixed-refresh";
    case PaceMode::Vrr: return "vrr";
    }
    return "?";
}

void Pacer::Configure(const PacerConfig& cfg, double sourceFps) {
    cfg_ = cfg;
    src_.Reset();
    src_.SetNominalPeriod((double)Qpc::Freq() / sourceFps);
    ready_.clear();
    newestN_ = -1;
    delay_ = (double)Qpc::FromMs(cfg.initialDelayMs);
    slack_.clear();
    lastX_ = -1; vrrX_ = -1;
    lateCount_ = 0;
}

void Pacer::OnSourceFrame(int64_t n, int64_t readyQpc) {
    src_.Add(n, readyQpc);
    ready_[n] = readyQpc;
    newestN_ = std::max(newestN_, n);
    while (!ready_.empty() && ready_.begin()->first < newestN_ - 240) ready_.erase(ready_.begin());
}

double Pacer::SourceJitterMs() const {
    if (!src_.Valid()) return 0;
    return Qpc::ToMs((int64_t)std::max(std::abs(src_.ResidualQuantile(0.99)), std::abs(src_.ResidualQuantile(0.01))));
}

bool Pacer::ReadyTime(int64_t n, int64_t& t) const {
    auto it = ready_.find(n);
    if (it == ready_.end()) return false;
    t = it->second;
    return true;
}

// Frame needed to show content position x: frame floor(x), plus its successor unless t
// snaps to 0. (Gaps from dropped frames are handled when the pair is resolved.)
static int64_t NeedFor(double x, double eps) {
    double fl = std::floor(x);
    return (int64_t)fl + ((x - fl > eps) ? 1 : 0);
}

double Pacer::TrialMinSlack(double trialDelay) const {
    // Re-evaluate the window as if D had been `trialDelay`: in fixed-refresh mode V is set by
    // vblanks, so slack only changes when the needed frame changes (piecewise, not 1:1 with D).
    double m = 1e300;
    for (const auto& w : slack_) {
        int64_t need = NeedFor(src_.IndexAt((double)w.V - trialDelay), cfg_.snapEps);
        auto it = ready_.find(need);
        if (it == ready_.end() || it->second > w.s) return -1e300;   // would have been late
        m = std::min(m, (double)(w.s - it->second));
    }
    return m;
}

PaceDecision Pacer::Decide(int64_t s, int64_t V) {
    PaceDecision d;
    if (!src_.Valid() || newestN_ < 0 || ready_.empty()) return d;
    const double P = src_.Period();
    const double eps = cfg_.snapEps;
    const double margin = (double)Qpc::FromMs(cfg_.marginMs);
    const bool gridMode = cfg_.mode == PaceMode::Vrr;   // V follows D (k-grid): slack moves 1:1 with D

    double x = src_.IndexAt((double)(V - (int64_t)delay_));
    if (lastX_ >= 0 && x < lastX_) x = lastX_; // never run time backwards
    const int64_t need = NeedFor(x, eps);

    if (need > newestN_) {
        // The frame we need has not arrived: show the newest one (never stepping back in time).
        d.late = true;
        x = std::max(std::min(x, (double)newestN_), lastX_);
        const double resid = std::max(0.0, src_.ResidualQuantile(0.99));
        const double overdue = (double)s - (src_.TimeOf((double)(newestN_ + 1)) + resid);
        if (overdue > 0.5 * P) {
            // The source itself is stalled / dropping (USB hiccup, re-sync): raising D cannot help.
            stallCount_++;
        } else if (need != lastLateNeed_) {
            // Genuinely too little delay. Penalise each needed frame once, bounded attack.
            lateCount_++;
            lastLateT_ = s;
            lastLateNeed_ = need;
            const double deficit = std::max(0.0, src_.TimeOf((double)need) + resid - (double)s);
            delay_ += std::min(deficit, 0.5 * P) + margin;
        }
    } else {
        int64_t rt = 0;
        if (ReadyTime(need, rt)) slack_.push_back({s, V, (double)(s - rt) - delay_, (double)(s - rt)});
    }

    const int64_t win = Qpc::FromMs(cfg_.windowSec * 1000.0);
    while (!slack_.empty() && slack_.front().s < s - win) slack_.pop_front();
    if (!slack_.empty() && !d.late) {
        double minNorm = 1e300, minRaw = 1e300;
        for (auto& w : slack_) { minNorm = std::min(minNorm, w.slackMinusD); minRaw = std::min(minRaw, w.raw); }
        // VRR grid: stored slack shifts 1:1 with D. Fixed refresh: re-evaluate every window
        // sample at the current D (which frame each would need now, and when it was ready).
        (void)minRaw;
        const double minSlack = gridMode ? minNorm + delay_ : TrialMinSlack(delay_);
        minSlackMs_ = Qpc::ToMs((int64_t)minSlack);
        const double dt = lastDecideT_ ? (double)(s - lastDecideT_) : 0.0;
        const double maxRelease = (double)Qpc::FromMs(cfg_.releasePerSec) * dt / (double)Qpc::Freq();
        const bool fullWindow = (s - slack_.front().s) > win * 3 / 4;
        const bool holding = lastLateT_ && (s - lastLateT_) < Qpc::FromMs(cfg_.holdAfterLateSec * 1000.0);
        const double err = minSlack - margin;
        if (err > 0 && fullWindow && !holding) {
            // Release slowly (bounded playback-speed change).
            const double step = std::min(gridMode ? err * 0.05 : maxRelease, maxRelease);
            if (gridMode || TrialMinSlack(delay_ - step) >= margin) delay_ -= step;
        } else if (err < 0) {
            // Slack eroding (e.g. phase drift): approach the margin smoothly (4x faster than release).
            delay_ += std::min(-err * 0.25, 4.0 * maxRelease);
        }
    }
    lastDecideT_ = s;
    delay_ = std::clamp(delay_, (double)Qpc::FromMs(cfg_.minDelayMs), (double)Qpc::FromMs(cfg_.maxDelayMs));

    // Resolve the real pair among frames that exist (dropped frames leave gaps), then snap.
    auto itA = ready_.upper_bound((int64_t)std::floor(x));
    if (itA == ready_.begin()) return d;
    --itA;                                   // largest existing n <= x
    int64_t nA = itA->first, nB = nA;
    double tt = 0;
    auto itB = std::next(itA);
    if (itB != ready_.end()) {
        tt = std::clamp((x - (double)nA) / (double)(itB->first - nA), 0.0, 1.0);
        if (tt <= eps) tt = 0;                                     // show A alone
        else if (tt >= 1.0 - eps) { nA = itB->first; tt = 0; }    // show B alone
        else nB = itB->first;
    }
    lastX_ = x;
    d.valid = true;
    d.x = x;
    d.nA = nA; d.nB = nB; d.t = (float)tt;
    d.delayMs = Qpc::ToMs((int64_t)delay_);
    d.displayQpc = V;
    return d;
}

int64_t Pacer::NextVrrDisplayTime(int64_t now, double renderLeadMs) {
    if (!src_.Valid()) return 0;
    const double k = std::max(1, cfg_.vrrMultiplier);
    const int64_t lead = Qpc::FromMs(renderLeadMs);
    double earliestX = src_.IndexAt((double)(now + lead - (int64_t)delay_));
    double x = (vrrX_ < 0) ? std::ceil(earliestX * k) / k : vrrX_ + 1.0 / k;
    if (x < earliestX) x = std::ceil(earliestX * k) / k;  // fell behind: skip to the next grid point
    vrrX_ = x;
    return (int64_t)(src_.TimeOf(x) + delay_);
}

} // namespace camss
