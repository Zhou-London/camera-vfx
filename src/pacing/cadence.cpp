#include "cadence.h"
#include <algorithm>
#include <cmath>

namespace camss {

CadenceDetector::CadenceDetector(const CadenceConfig& c) : cfg_(c) {
    cfg_.windowFrames = std::max(cfg_.windowFrames, 8);
    cfg_.maxInterval = std::max(cfg_.maxInterval, 1);
    cfg_.minIntervals = std::max(cfg_.minIntervals, 1);
    cfg_.holdFrames = std::max(cfg_.holdFrames, 1);
    Reset();
}

void CadenceDetector::Reset() {
    active_ = 1.0;
    estimate_ = 0;
    pendingFrames_ = 0;
    slot_.assign((size_t)cfg_.windowFrames, 0);
    lastN_ = -1;
    received_ = uniques_ = 0;
}

double CadenceDetector::UniqueFraction() const {
    return received_ > 0 ? (double)uniques_ / (double)received_ : 1.0;
}

bool CadenceDetector::OnFrame(int64_t n, bool unique) {
    if (n < 0) return false;
    const int64_t W = cfg_.windowFrames;
    int64_t adv = 1;
    if (lastN_ >= 0 && n == lastN_) return false;            // duplicate delivery
    if (lastN_ < 0 || n < lastN_ || n - lastN_ >= W) {
        // Start, index reset, or a signal gap longer than the window: start a fresh window.
        // The active period is kept (a gap says nothing about the content rate).
        std::fill(slot_.begin(), slot_.end(), (uint8_t)0);
        received_ = uniques_ = 0;
        if (lastN_ >= 0 && n > lastN_) adv = n - lastN_;
    } else {
        // Indices lastN_+1 .. n enter the window; their slots held indices that now leave it.
        // Skipped indices (dropped frames) stay 0 = not received.
        adv = n - lastN_;
        for (int64_t i = lastN_ + 1; i <= n; i++) {
            uint8_t& s = slot_[(size_t)(i % W)];
            if (s) { received_--; if (s == 2) uniques_--; }
            s = 0;
        }
    }
    slot_[(size_t)(n % W)] = unique ? 2 : 1;
    received_++;
    if (unique) uniques_++;
    lastN_ = n;
    Estimate();

    // Hysteresis: switch only after the estimate has differed from the active period by more than
    // switchFrac for holdFrames consecutive signal frames. No estimate (static scene, too few
    // intervals) never switches.
    if (estimate_ > 0 && std::abs(estimate_ - active_) > cfg_.switchFrac * active_) {
        pendingFrames_ += (int)std::min<int64_t>(adv, cfg_.holdFrames);
        if (pendingFrames_ >= cfg_.holdFrames) {
            active_ = estimate_;
            pendingFrames_ = 0;
            return true;
        }
    } else {
        pendingFrames_ = 0;
    }
    return false;
}

void CadenceDetector::Estimate() {
    const int64_t W = cfg_.windowFrames;
    int64_t prev = -1, sum = 0;
    int cnt = 0;
    bool gap = false;
    for (int64_t i = std::max<int64_t>(0, lastN_ - W + 1); i <= lastN_; i++) {
        const uint8_t s = slot_[(size_t)(i % W)];
        if (s == 0) { gap = true; continue; }                 // not received: the interval across it is unknown
        if (s != 2) continue;
        if (prev >= 0 && !gap && i - prev <= cfg_.maxInterval) { sum += i - prev; cnt++; }
        prev = i;
        gap = false;
    }
    if (cnt < cfg_.minIntervals) { estimate_ = 0; return; }
    double e = (double)sum / (double)cnt;
    static const double kRatios[] = {1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0};
    for (double r : kRatios)
        if (std::abs(e - r) <= cfg_.snapTol * r) { e = r; break; }
    estimate_ = std::clamp(e, 1.0, (double)cfg_.maxInterval);
}

} // namespace camss
