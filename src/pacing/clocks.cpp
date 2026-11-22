#include "clocks.h"
#include <algorithm>
#include <cmath>

namespace cameravfx {

namespace {
constexpr size_t kMinObservations = 8;
constexpr double kOutlierMedianFactor = 4.0;
constexpr double kOutlierPeriodFraction = 0.02;
constexpr double kMinMedianAbsResidualTicks = 1.0;
}

void LinearClock::Add(int64_t n, int64_t t) {
    const bool indexRestarted = !observations_.empty() && n <= observations_.back().n;
    if (indexRestarted) observations_.clear();
    observations_.push_back({n, t});
    while (observations_.size() > window_) observations_.pop_front();
    Refit();
}

void LinearClock::Refit() {
    valid_ = false;
    const size_t count = observations_.size();
    if (count < kMinObservations) return;
    baseIndex_ = observations_.front().n;
    baseTime_ = observations_.front().t;
    auto fitLine = [&](auto keep, double& slope, double& intercept) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        size_t kept = 0, i = 0;
        for (const Observation& o : observations_) {
            if (!keep(i++)) continue;
            const double x = (double)(o.n - baseIndex_), y = (double)(o.t - baseTime_);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
            kept++;
        }
        const double den = (double)kept * sxx - sx * sx;
        if (kept < kMinObservations || den <= 0) return false;
        slope = ((double)kept * sxy - sx * sy) / den;
        intercept = (sy - slope * sx) / (double)kept;
        return true;
    };
    double slope = 0, intercept = 0;
    if (!fitLine([](size_t) { return true; }, slope, intercept)) return;
    absResiduals_.resize(count);
    size_t i = 0;
    for (const Observation& o : observations_)
        absResiduals_[i++] = std::abs((double)(o.t - baseTime_) - (intercept + slope * (double)(o.n - baseIndex_)));
    scratch_.assign(absResiduals_.begin(), absResiduals_.end());
    std::nth_element(scratch_.begin(), scratch_.begin() + count / 2, scratch_.end());
    const double medianAbsResidual = std::max(scratch_[count / 2], kMinMedianAbsResidualTicks);
    const double outlierAbove = kOutlierMedianFactor * medianAbsResidual + kOutlierPeriodFraction * slope;
    fitLine([&](size_t k) { return absResiduals_[k] <= outlierAbove; }, slope, intercept);
    period_ = slope;
    intercept_ = intercept;
    valid_ = period_ > 0;
}

double LinearClock::AbsResidualQuantile(double q) const {
    if (!valid_) return 0;
    scratch_.clear();
    for (const Observation& o : observations_) scratch_.push_back(std::abs((double)o.t - TimeOf((double)o.n)));
    const auto rank = scratch_.begin() + (ptrdiff_t)(q * (double)(scratch_.size() - 1));
    std::nth_element(scratch_.begin(), rank, scratch_.end());
    return *rank;
}

int64_t VblankIndex(const LinearClock& clock, int64_t lastIndex, int64_t lastTime, int64_t time, double nominalPeriod) {
    if (lastIndex < 0) return 0;
    if (clock.Valid()) return std::llround(clock.IndexAt((double)time));
    return lastIndex + std::max<int64_t>(1, std::llround((double)(time - lastTime) / nominalPeriod));
}

void VsyncClock::Start(IDXGIOutput* output, double nominalHz) {
    output_ = output;
    nominalPeriod_ = (double)Qpc::Freq() / nominalHz;
    clock_.SetNominalPeriod(nominalPeriod_);
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void VsyncClock::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void VsyncClock::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t index = -1;
    while (running_) {
        if (FAILED(output_->WaitForVBlank())) { Sleep(1); continue; }
        const int64_t time = Qpc::Now();
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t next = VblankIndex(clock_, index, lastVblankTime_, time, nominalPeriod_);
        lastVblankTime_ = time;
        if (next <= index) continue;
        index = next;
        clock_.Add(index, time);
    }
}

double VsyncClock::MeasuredHz() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (double)Qpc::Freq() / (clock_.Valid() ? clock_.Period() : nominalPeriod_);
}

double VsyncClock::JitterMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clock_.JitterMs();
}

int64_t VsyncClock::NextVblankAfter(int64_t t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clock_.Valid()) return (int64_t)clock_.TimeOf(std::floor(clock_.IndexAt((double)t)) + 1);
    if (lastVblankTime_ == 0) return t + (int64_t)nominalPeriod_;
    const double periodsSinceLast = std::floor((double)(t - lastVblankTime_) / nominalPeriod_) + 1;
    return lastVblankTime_ + (int64_t)(periodsSinceLast * nominalPeriod_);
}

}
