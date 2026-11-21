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

// Note: the content period (pc_) is a property of the input, not of the configuration, and is
// kept across Configure(); the caller retimes it with SetContentPeriod.
void Pacer::Configure(const PacerConfig& cfg, double sourceFps) {
    cfg_ = cfg;
    cfg_.vrrMultiplier = std::max(1, cfg_.vrrMultiplier);
    src_.Reset();
    src_.SetNominalPeriod((double)Qpc::Freq() / sourceFps);
    ready_.clear();
    newestN_ = -1;
    newestSignal_ = -1;
    delay_ = (double)Qpc::FromMs(cfg.initialDelayMs);
    slack_.clear();
    lastX_ = -1; vrrX_ = -1;
    regrid_ = false;
    lastVrrV_ = 0;
    lateCount_ = 0;
    lastLateNeed_ = -1;
}

void Pacer::OnSourceFrame(int64_t n, int64_t readyQpc, bool unique) {
    // Every signal frame feeds the clock: the signal rate is what the timeline is built on,
    // whatever the content rate is, so the clock never needs a reset when the content changes.
    src_.Add(n, readyQpc);
    newestSignal_ = std::max(newestSignal_, n);
    if (unique) {
        ready_[n] = Entry{readyQpc, -1};
        newestN_ = std::max(newestN_, n);
        while (!ready_.empty() && ready_.begin()->first < newestN_ - 240) ready_.erase(ready_.begin());
    } else if (!ready_.empty() && n > ready_.rbegin()->first) {
        ready_.rbegin()->second.lastRepeat = n;   // the newest content frame is still on screen at n
    }
}

void Pacer::SetContentPeriod(double pc) {
    pc = std::clamp(std::isfinite(pc) ? pc : 1.0, 1.0, 16.0);
    if (std::abs(pc - pc_) < 1e-9) return;
    Retime(pc, cfg_.vrrMultiplier);
}

void Pacer::SetMultiplier(int k) {
    k = std::max(1, k);
    if (k == cfg_.vrrMultiplier) return;
    if (cfg_.mode != PaceMode::Vrr) { cfg_.vrrMultiplier = k; regrid_ = true; return; }   // no effect on D
    Retime(pc_, k);
}

// Theoretical steady-state extra delay for content period pc: the wait for the next content frame
// from the earliest position that needs it. VRR k-grid: (1 - 1/k) of a content period; fixed
// refresh (all phases occur): about a whole content period.
double Pacer::SteadyExtraTicks(double pc, int k) const {
    const double P = src_.Period();   // nominal period until the fit is valid
    if (cfg_.mode == PaceMode::Vrr) {
        k = std::max(1, k);
        return (double)(k - 1) / (double)k * pc * P;
    }
    return pc * P;
}

void Pacer::SetMode(PaceMode m) {
    if (m == cfg_.mode) return;
    const double before = SteadyExtraTicks(pc_, cfg_.vrrMultiplier);
    cfg_.mode = m;
    delay_ += SteadyExtraTicks(pc_, cfg_.vrrMultiplier) - before;
    delay_ = std::clamp(delay_, (double)Qpc::FromMs(cfg_.minDelayMs), (double)Qpc::FromMs(cfg_.maxDelayMs));
    slack_.clear();
    lastLateNeed_ = -1;
    regrid_ = true;
}

void Pacer::Retime(double pc, int k) {
    const double before = SteadyExtraTicks(pc_, cfg_.vrrMultiplier);
    pc_ = pc;
    cfg_.vrrMultiplier = k;
    delay_ += SteadyExtraTicks(pc_, k) - before;
    delay_ = std::clamp(delay_, (double)Qpc::FromMs(cfg_.minDelayMs), (double)Qpc::FromMs(cfg_.maxDelayMs));
    // The window's slack samples describe the old cadence (which frame was needed when); measured
    // against the new D they would trigger an immediate release that undoes the shift. Start a
    // fresh window: no release until it is 3/4 full again.
    slack_.clear();
    lastLateNeed_ = -1;
    regrid_ = true;
}

double Pacer::SourceJitterMs() const {
    if (!src_.Valid()) return 0;
    return Qpc::ToMs((int64_t)std::max(std::abs(src_.ResidualQuantile(0.99)), std::abs(src_.ResidualQuantile(0.01))));
}

bool Pacer::ReadyTime(int64_t n, int64_t& t) const {
    auto it = ready_.find(n);
    if (it == ready_.end()) return false;
    t = it->second.ready;
    return true;
}

// Content-aware pair for position x: nA = newest unique <= x, nB = the next unique (if it has
// arrived), then snap. If nB has not arrived, it is expected at nA + Pc: x needs it as soon as the
// step (x - nA)/Pc exceeds eps. Dropped frames (signal gaps, no repeats received) are interpolated
// across; a gap filled with repeats longer than 1.5 Pc (static scene that started moving again,
// game hitch) means nA's image is the content at nB - Pc, so interpolation starts there.
// Interpolation start for the pair (nA, nB) (nB may be a predicted, fractional index): nA, or
// later when the gap was filled with repeats (static scene / hitch: nA's image is the content at nB - Pc).
double Pacer::PairStart(int64_t nA, double nB, int64_t rep) const {
    if (nB - (double)nA > 1.5 * pc_ && rep > nA && (double)rep < nB)
        return std::max((double)nA, std::min((double)rep + 1.0, nB) - pc_);
    return (double)nA;
}

bool Pacer::Resolve(double x, Pick& p) const {
    auto itA = ready_.upper_bound((int64_t)std::floor(x));
    if (itA == ready_.begin()) return false;
    --itA;                                   // largest existing unique n <= x
    const double eps = cfg_.snapEps;
    p = Pick{};
    p.nA = p.nB = p.need = itA->first;
    p.needReady = itA->second.ready;
    p.start = (double)p.nA;
    const int64_t rep = itA->second.lastRepeat;
    auto itB = std::next(itA);
    if (itB != ready_.end()) {
        const int64_t nB = itB->first;
        p.start = PairStart(p.nA, (double)nB, rep);
        double tt = std::clamp((x - p.start) / ((double)nB - p.start), 0.0, 1.0);
        if (tt <= eps) tt = 0;                                     // show A alone
        else if (tt >= 1.0 - eps) { p.nA = p.nB = nB; tt = 0; }    // show B alone
        else p.nB = nB;
        p.tt = tt;
        if (p.nB == nB) { p.need = nB; p.needReady = itB->second.ready; }
    } else {
        // nB not here yet: it is expected at nA + Pc, and in any case after the newest signal frame
        // (those were repeats). Use the same snap rule it will get once it arrives (with intervals
        // that differ from Pc, e.g. 3:2 pulldown, (x - nA)/Pc would flag frames that will snap to A).
        // Static content / hitch: the signal frame where the next content frame was due has arrived
        // as a repeat; anything past nA then waits for new content (a stall, see Decide).
        p.pred = std::max((double)p.nA + pc_, (double)(newestSignal_ + 1));
        p.overdueContent = newestSignal_ >= (int64_t)std::ceil((double)p.nA + pc_ - 1e-9);
        if (p.overdueContent) {
            p.missing = (x - (double)p.nA) / pc_ > eps;
        } else {
            const double st = PairStart(p.nA, p.pred, rep);
            p.missing = (x - st) / (p.pred - st) > eps;
        }
    }
    return true;
}

double Pacer::TrialMinSlack(double trialDelay) const {
    // Re-evaluate the window as if D had been `trialDelay`: in fixed-refresh mode V is set by
    // vblanks, so slack only changes when the needed frame changes (piecewise, not 1:1 with D).
    double m = 1e300;
    Pick p;
    for (const auto& w : slack_) {
        if (!Resolve(src_.IndexAt((double)w.V - trialDelay), p) || p.missing || p.needReady > w.s) return -1e300;   // would have been late
        m = std::min(m, (double)(w.s - p.needReady));
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
    Pick pk;
    const bool have = Resolve(x, pk);

    if (have && pk.missing) {
        // The next content frame is needed but has not arrived: show the newest one (never stepping
        // back in time). Predicted index of the needed frame: pk.pred for the next content frame,
        // + Pc for each further one. It is penalised once per content frame (m-th after nA), even
        // when its predicted index moves later because a repeat arrived where it was expected.
        const double resid = std::max(0.0, src_.ResidualQuantile(0.99));
        const double over = (x - pk.pred) / pc_;   // content frames needed beyond the next one
        double m = 1.0;
        if (over > eps) { const double fl = std::floor(over); m += fl + ((over - fl > eps) ? 1.0 : 0.0); }
        const double needIdx = pk.pred + pc_ * (m - 1.0);
        const double needKey = (double)pk.nA + pc_ * m;
        // Static content: the signal frames where the next content frame was due have arrived as
        // repeats. Source stall: the next signal frame itself is overdue (USB hiccup, re-sync).
        // Raising D cannot help either way.
        const bool contentStall = pk.overdueContent;
        const double overdue = (double)s - (src_.TimeOf((double)(newestSignal_ + 1)) + resid);
        if (contentStall || overdue > 0.5 * P) {
            stallCount_++;
            d.stall = true;
            d.late = !contentStall;
        } else {
            d.late = true;
            if (needKey != lastLateNeed_) {
                // Genuinely too little delay. Penalise each needed frame once, bounded attack.
                lateCount_++;
                lastLateT_ = s;
                lastLateNeed_ = needKey;
                const double deficit = std::max(0.0, src_.TimeOf(needIdx) + resid - (double)s);
                delay_ += std::min(deficit, 0.5 * P * pc_) + margin;
            }
        }
        x = std::max(std::min(x, (double)pk.nA), lastX_);
        pk.nB = pk.nA; pk.tt = 0; pk.start = (double)pk.nA;
    } else if (have) {
        slack_.push_back({s, V, (double)(s - pk.needReady) - delay_, (double)(s - pk.needReady)});
    }

    const int64_t win = Qpc::FromMs(cfg_.windowSec * 1000.0);
    while (!slack_.empty() && slack_.front().s < s - win) slack_.pop_front();
    if (!slack_.empty() && !d.late && !d.stall) {
        double minNorm = 1e300;
        for (auto& w : slack_) minNorm = std::min(minNorm, w.slackMinusD);
        // VRR grid: stored slack shifts 1:1 with D. Fixed refresh: re-evaluate every window
        // sample at the current D (which frame each would need now, and when it was ready).
        const double guard = (!gridMode && pc_ > 1.0) ? (double)Qpc::FromMs(cfg_.phaseGuardMs) : 0.0;
        const double minSlack = gridMode ? minNorm + delay_ : TrialMinSlack(delay_ - guard);
        minSlackMs_ = minSlack > -1e299 ? Qpc::ToMs((int64_t)minSlack) : -1.0;
        const double dt = lastDecideT_ ? (double)(s - lastDecideT_) : 0.0;
        const double maxRelease = (double)Qpc::FromMs(cfg_.releasePerSec) * dt / (double)Qpc::Freq();
        const bool fullWindow = (s - slack_.front().s) > win * 3 / 4;
        const bool holding = lastLateT_ && (s - lastLateT_) < Qpc::FromMs(cfg_.holdAfterLateSec * 1000.0);
        const double err = minSlack - margin;
        if (err > 0 && fullWindow && !holding) {
            // Release slowly (bounded playback-speed change). A large proven excess (e.g. after a cadence
            // retime on top of a transient) is given back faster: <= fastReleasePerSec ms/s (0.8 % speed).
            const double rel = err > (double)Qpc::FromMs(cfg_.fastReleaseAboveMs)
                                   ? (double)Qpc::FromMs(cfg_.fastReleasePerSec) * dt / (double)Qpc::Freq() : maxRelease;
            const double step = std::min(gridMode ? err * 0.05 : rel, rel);
            if (gridMode || TrialMinSlack(delay_ - step - guard) >= margin) delay_ -= step;
        } else if (err < 0) {
            // Slack eroding (e.g. phase drift): approach the margin smoothly (4x faster than release).
            delay_ += std::min(-err * 0.25, 4.0 * maxRelease);
        }
    }
    lastDecideT_ = s;
    delay_ = std::clamp(delay_, (double)Qpc::FromMs(cfg_.minDelayMs), (double)Qpc::FromMs(cfg_.maxDelayMs));
    if (!have) return d;

    lastX_ = x;
    d.valid = true;
    d.x = x;
    d.nA = pk.nA; d.nB = pk.nB; d.t = (float)pk.tt;
    d.cx = pk.tt > 0 ? pk.start + pk.tt * ((double)pk.nB - pk.start) : (double)pk.nA;
    d.delayMs = Qpc::ToMs((int64_t)delay_);
    d.displayQpc = V;
    return d;
}

int64_t Pacer::NextVrrDisplayTime(int64_t now, double renderLeadMs) {
    if (!src_.Valid()) return 0;
    const double k = std::max(1, cfg_.vrrMultiplier);
    const double step = pc_ / k;            // k output frames per content frame
    const int64_t lead = Qpc::FromMs(renderLeadMs);
    double earliestX = src_.IndexAt((double)(now + lead - (int64_t)delay_));
    // Next grid point at or after e, with the grid phase anchored to the unique frames: the newest
    // unique <= e. With a non-integer content period (3:2 pulldown, 2:1 ...) the intervals alternate;
    // anchoring at a recent unique that starts a LONG interval puts grid points on the frames that
    // arrive late relative to the Pc grid, which minimises the worst wait (24p: 1.75 P instead of 2.5 P).
    const bool irregular = std::abs(pc_ - std::round(pc_)) > 1e-6;
    auto gridAfter = [&](double e) {
        auto it = ready_.upper_bound((int64_t)std::floor(e));
        if (it == ready_.begin()) return std::ceil(e / step) * step;
        --it;
        double a = (double)it->first;
        if (irregular) {
            for (int i = 0; i < 8; i++) {
                auto nx = std::next(it);
                // A cadence interval longer than Pc (not a discontinuity such as a static scene).
                const double iv = nx != ready_.end() ? (double)(nx->first - it->first) : 0.0;
                if (iv > pc_ && iv <= std::ceil(pc_)) { a = (double)it->first; break; }
                if (it == ready_.begin()) break;
                --it;
            }
        }
        return a + std::ceil((e - a) / step) * step;
    };
    double x;
    if (vrrX_ < 0) x = gridAfter(earliestX);              // start
    else if (regrid_) {
        // Cadence / multiplier changed (and D was retimed): new grid, but keep at least half a new
        // step after the previous output so the retime never produces two presents back to back.
        const double minX = src_.IndexAt((double)lastVrrV_ + 0.5 * step * src_.Period() - delay_);
        x = gridAfter(std::max(earliestX, minX));
    } else {
        x = vrrX_ + step;
        if (x < earliestX) x = gridAfter(earliestX);       // fell behind: skip to the next grid point
        else if (pc_ > 1.0 && !irregular) {
            // Integer cadence whose phase moved (a game hitch: 0,2,4,7,9 ...; a static scene ending on
            // another phase): when k does not divide evenly the grid would stay off the unique frames,
            // so no output shows a real frame and D needs up to half a step more. Once the cadence is
            // re-established (last interval == Pc), re-anchor at that unique, at least half a step on.
            auto it = ready_.upper_bound((int64_t)std::floor(x));
            if (it != ready_.begin() && --it != ready_.begin()) {
                const double a = (double)it->first;
                const double r = (x - a) / step;
                if ((double)(it->first - std::prev(it)->first) == pc_ && std::abs(r - std::round(r)) > 1e-3) {
                    const double e = std::max(earliestX, vrrX_ + 0.5 * step);
                    x = a + std::ceil((e - a) / step - 1e-9) * step;
                }
            }
        }
    }
    regrid_ = false;
    vrrX_ = x;
    lastVrrV_ = (int64_t)(src_.TimeOf(x) + delay_);
    return lastVrrV_;
}

} // namespace camss
