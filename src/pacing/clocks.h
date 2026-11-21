// Clock models for the two independent timebases in the pipeline:
//  * the source clock (capture card, nominally 60 Hz, drifts vs. QPC)
//  * the display clock (monitor vblank, e.g. 120/164 Hz, also drifts)
// Both are modelled as t(n) = a + n * P fitted over a sliding window by least
// squares on integer tick indices, which removes USB/driver jitter and tracks drift.
#pragma once
#include "../util/common.h"
#include "../util/stats.h"
#include <deque>
#include <thread>
#include <dxgi1_6.h>

namespace camss {

class LinearClock {
public:
    explicit LinearClock(size_t window = 240) : window_(window) {}
    // Adds an observation of tick `n` at QPC time `t`.
    void Add(int64_t n, int64_t t);
    bool Valid() const { return valid_; }
    double Period() const { return period_; }            // QPC ticks per tick
    double TimeOf(double n) const { return base_t_ + (n - base_n_) * period_ + intercept_; }
    double IndexAt(double t) const { return (t - base_t_ - intercept_) / period_ + base_n_; }
    // Residual (observed - fitted) quantile in ticks, over the window.
    double ResidualQuantile(double q) const;
    double LastResidual() const { return lastResid_; }
    void Reset() { obs_.clear(); valid_ = false; }
    void SetNominalPeriod(double p) { if (!valid_) period_ = p; }
private:
    void Refit();
    struct Obs { int64_t n; int64_t t; };
    size_t window_;
    std::deque<Obs> obs_;
    bool valid_ = false;
    double period_ = 0, intercept_ = 0;   // relative to base
    int64_t base_n_ = 0, base_t_ = 0;
    double lastResid_ = 0;
    std::vector<double> resid_;
};

// Display vblank clock fed by a thread blocked in IDXGIOutput::WaitForVBlank.
class VsyncClock {
public:
    bool Start(IDXGIOutput* output, double nominalHz);
    void Stop();
    ~VsyncClock() { Stop(); }
    // Predicted QPC time of the first vblank strictly after `t`.
    int64_t NextVblankAfter(int64_t t) const;
    double PeriodTicks() const;
    double MeasuredHz() const { double p = PeriodTicks(); return p > 0 ? (double)Qpc::Freq() / p : 0; }
    double JitterMs() const; // p99 |residual|
    bool Valid() const;
private:
    void Run();
    ComPtr<IDXGIOutput> output_;
    std::thread thr_;
    std::atomic<bool> run_{false};
    mutable std::mutex m_;
    LinearClock clock_{600};
    double nominalTicks_ = 0;
    int64_t count_ = 0, lastT_ = 0;
};

} // namespace camss
