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

PaceDecision Pacer::Decide(int64_t s, int64_t V) {
    PaceDecision d;
    if (!src_.Valid() || newestN_ < 0 || ready_.empty()) return d;
    const double P = src_.Period();
    const double eps = cfg_.snapEps;
    const double margin = (double)Qpc::FromMs(cfg_.marginMs);

    double x = src_.IndexAt((double)(V - (int64_t)delay_));
    if (lastX_ >= 0 && x < lastX_) x = lastX_; // never run time backwards

    // Which real frame do we need for this x?
    double fl = std::floor(x);
    double t = x - fl;
    int64_t need = (int64_t)fl + ((t > eps) ? 1 : 0);

    if (need > newestN_) {
        // Late: the frame we need has not arrived. Clamp to the newest frame and raise D.
        d.late = true;
        lateCount_++;
        lastLateT_ = s;
        double predReady = src_.TimeOf((double)need) + std::max(0.0, src_.ResidualQuantile(0.99));
        double deficit = std::max(0.0, predReady - (double)s);
        // Bounded attack: one late frame raises D by at most half a source period (a one-off
        // stall must not inflate D for seconds; repeated lateness still ramps it up quickly).
        delay_ += std::min(deficit, 0.5 * P) + margin;
        // Show the newest frame we have, but never step content time backwards.
        x = std::max(std::min(x, (double)newestN_), lastX_);
    } else {
        int64_t rt = 0;
        // Store slack minus the delay in force: larger D shifts content earlier, i.e. more slack 1:1.
        if (ReadyTime(need, rt)) slack_.push_back({s, (double)(s - rt) - delay_});
    }

    // Slack window: slack_i re-evaluated at the current D = (slack_i - D_i) + D_now. Keep min over window.
    const int64_t win = Qpc::FromMs(cfg_.windowSec * 1000.0);
    while (!slack_.empty() && slack_.front().first < s - win) slack_.pop_front();
    if (!slack_.empty() && !d.late) {
        double minNorm = slack_.front().second;
        for (auto& p : slack_) minNorm = std::min(minNorm, p.second);
        double minSlack = minNorm + delay_;
        minSlackMs_ = Qpc::ToMs((int64_t)minSlack);
        double dt = lastDecideT_ ? (double)(s - lastDecideT_) : 0.0;
        bool fullWindow = (s - slack_.front().first) > win * 3 / 4;
        double err = minSlack - margin;
        bool holding = lastLateT_ && (s - lastLateT_) < Qpc::FromMs(cfg_.holdAfterLateSec * 1000.0);
        if (err > 0 && fullWindow && !holding) {
            // Release slowly: bounded playback-speed change.
            double maxStep = (double)Qpc::FromMs(cfg_.releasePerSec) * dt / (double)Qpc::Freq();
            delay_ -= std::min(err * 0.05, maxStep);
        } else if (err < 0) {
            // Slack eroding (e.g. phase drift): approach the margin smoothly (4x faster than release).
            double maxStep = 4.0 * (double)Qpc::FromMs(cfg_.releasePerSec) * dt / (double)Qpc::Freq();
            delay_ += std::min(-err * 0.25, maxStep);
        }
    }
    lastDecideT_ = s;
    delay_ = std::clamp(delay_, (double)Qpc::FromMs(cfg_.minDelayMs), (double)Qpc::FromMs(cfg_.maxDelayMs));

    // Resolve the pair among frames that actually exist (dropped frames leave gaps).
    fl = std::floor(x);
    t = x - fl;
    if (t <= eps) t = 0;
    else if (t >= 1.0 - eps && (int64_t)fl + 1 <= newestN_) { fl += 1; t = 0; }
    int64_t nA = (int64_t)fl;
    auto itA = ready_.upper_bound(nA);
    if (itA == ready_.begin()) return d;
    --itA;                                  // largest n <= nA
    nA = itA->first;
    int64_t nB = nA;
    double tt = 0;
    if (t > 0) {
        auto itB = std::next(itA);
        if (itB != ready_.end()) {
            nB = itB->first;
            tt = (x - (double)nA) / (double)(nB - nA);
            tt = std::clamp(tt, 0.0, 1.0);
            if (tt <= eps) { tt = 0; nB = nA; }
        }
    }
    lastX_ = x;
    d.valid = true;
    d.x = x;
    d.nA = nA; d.nB = nB; d.t = (float)tt;
    d.delayMs = Qpc::ToMs((int64_t)delay_);
    d.displayQpc = V;
    (void)P;
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
