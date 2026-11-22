#pragma once
#include "../util/common.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <dxgi1_6.h>

namespace cameravfx {

class LinearClock {
public:
    explicit LinearClock(size_t window) : window_(window) {}
    void SetNominalPeriod(double period) { period_ = period; }
    void Add(int64_t n, int64_t t);
    bool Valid() const { return valid_; }
    double Period() const { return period_; }
    double TimeOf(double n) const { return (double)baseTime_ + (n - (double)baseIndex_) * period_ + intercept_; }
    double IndexAt(double t) const { return (t - (double)baseTime_ - intercept_) / period_ + (double)baseIndex_; }
    double AbsResidualQuantile(double q) const;
    double JitterMs() const { return Qpc::ToMs((int64_t)AbsResidualQuantile(0.99)); }

private:
    struct Observation { int64_t n; int64_t t; };
    void Refit();
    size_t window_;
    std::deque<Observation> observations_;
    bool valid_ = false;
    double period_ = 0, intercept_ = 0;
    int64_t baseIndex_ = 0, baseTime_ = 0;
    std::vector<double> absResiduals_;
    mutable std::vector<double> scratch_;
};

int64_t VblankIndex(const LinearClock& clock, int64_t lastIndex, int64_t lastTime, int64_t time, double nominalPeriod);

class VsyncClock {
public:
    ~VsyncClock() { Stop(); }
    void Start(IDXGIOutput* output, double nominalHz);
    void Stop();
    int64_t NextVblankAfter(int64_t t) const;
    double MeasuredHz() const;
    double JitterMs() const;

private:
    static constexpr size_t kWindow = 600;
    void Run();
    ComPtr<IDXGIOutput> output_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    LinearClock clock_{kWindow};
    double nominalPeriod_ = 0;
    int64_t lastVblankTime_ = 0;
};

}
