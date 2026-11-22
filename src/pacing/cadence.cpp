#include "cadence.h"
#include <algorithm>
#include <cmath>

namespace cameravfx {

namespace {
constexpr double kMinVisiblePeriod = 0.99;
}

std::vector<double> ContentPeriods(const std::vector<double>& contentRates, double signalHz) {
    std::vector<double> periods;
    for (double rate : contentRates) {
        const double period = signalHz / rate;
        if (period >= kMinVisiblePeriod) periods.push_back(std::max(1.0, period));
    }
    std::sort(periods.begin(), periods.end());
    return periods;
}

CadenceDetector::CadenceDetector(std::vector<double> periods, double contentPeriod) : periods_(std::move(periods)), active_(contentPeriod) {}

double CadenceDetector::UniqueFraction() const {
    return received_ > 0 ? (double)uniques_ / (double)received_ : 1.0;
}

bool CadenceDetector::OnFrame(int64_t n, bool unique) {
    if (n == lastN_) return false;
    const bool freshWindow = lastN_ < 0 || n < lastN_ || n - lastN_ >= kWindowFrames;
    const int64_t advance = lastN_ >= 0 && n > lastN_ ? n - lastN_ : 1;
    if (freshWindow) {
        std::fill(slots_.begin(), slots_.end(), Slot::Missing);
        received_ = uniques_ = 0;
    } else {
        for (int64_t i = lastN_ + 1; i <= n; i++) {
            Slot& leaving = slots_[(size_t)(i % kWindowFrames)];
            if (leaving != Slot::Missing) received_--;
            if (leaving == Slot::Unique) uniques_--;
            leaving = Slot::Missing;
        }
    }
    slots_[(size_t)(n % kWindowFrames)] = unique ? Slot::Unique : Slot::Repeat;
    received_++;
    if (unique) uniques_++;
    lastN_ = n;

    const double estimate = EstimatePeriod();
    if (estimate != heldEstimate_) {
        heldEstimate_ = estimate;
        heldFrames_ = 0;
    }
    heldFrames_ += (int)std::min<int64_t>(advance, kHoldFrames);
    if (estimate > 0 && estimate != active_ && heldFrames_ >= kHoldFrames) {
        active_ = estimate;
        heldFrames_ = 0;
        return true;
    }
    return false;
}

double CadenceDetector::EstimatePeriod() const {
    int64_t previousUnique = -1, intervalSum = 0;
    int intervals = 0;
    bool gapSincePrevious = false;
    for (int64_t i = std::max<int64_t>(0, lastN_ - kWindowFrames + 1); i <= lastN_; i++) {
        const Slot slot = slots_[(size_t)(i % kWindowFrames)];
        if (slot == Slot::Missing) { gapSincePrevious = true; continue; }
        if (slot == Slot::Repeat) continue;
        if (previousUnique >= 0 && !gapSincePrevious && i - previousUnique <= kMaxUniqueInterval) {
            intervalSum += i - previousUnique;
            intervals++;
        }
        previousUnique = i;
        gapSincePrevious = false;
    }
    if (intervals < kMinIntervals) return 0;
    const double meanInterval = (double)intervalSum / (double)intervals;
    double nearest = 0, nearestDistance = kSnapTolerance;
    for (double period : periods_) {
        const double distance = std::abs(meanInterval - period) / period;
        if (distance <= kSnapTolerance && (nearest == 0 || distance < nearestDistance)) {
            nearest = period;
            nearestDistance = distance;
        }
    }
    return nearest;
}

}
