#include "clocks.h"
#include <algorithm>
#include <cmath>

namespace camss {

void LinearClock::Add(int64_t n, int64_t t) {
    if (!obs_.empty() && n <= obs_.back().n) { obs_.clear(); valid_ = false; } // index reset
    obs_.push_back({n, t});
    while (obs_.size() > window_) obs_.pop_front();
    Refit();
    lastResid_ = valid_ ? (double)t - TimeOf((double)n) : 0.0;
}

void LinearClock::Refit() {
    const size_t N = obs_.size();
    if (N < 8) { valid_ = false; return; }
    base_n_ = obs_.front().n;
    base_t_ = obs_.front().t;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& o : obs_) {
        double x = (double)(o.n - base_n_), y = (double)(o.t - base_t_);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double den = (double)N * sxx - sx * sx;
    if (den <= 0) { valid_ = false; return; }
    double slope = ((double)N * sxy - sx * sy) / den;
    double icpt = (sy - slope * sx) / (double)N;
    // One robust pass: drop observations whose residual exceeds 4x the median
    // absolute residual (e.g. a frame delivered late by a USB hiccup), refit.
    resid_.resize(N);
    size_t i = 0;
    for (auto& o : obs_) resid_[i++] = std::abs((double)(o.t - base_t_) - (icpt + slope * (double)(o.n - base_n_)));
    std::vector<double> tmp = resid_;
    std::nth_element(tmp.begin(), tmp.begin() + N / 2, tmp.end());
    double mad = std::max(tmp[N / 2], 1.0);
    sx = sy = sxx = sxy = 0; size_t M = 0; i = 0;
    for (auto& o : obs_) {
        if (resid_[i++] > 4.0 * mad + slope * 0.02) continue;
        double x = (double)(o.n - base_n_), y = (double)(o.t - base_t_);
        sx += x; sy += y; sxx += x * x; sxy += x * y; M++;
    }
    den = (double)M * sxx - sx * sx;
    if (M >= 8 && den > 0) {
        slope = ((double)M * sxy - sx * sy) / den;
        icpt = (sy - slope * sx) / (double)M;
    }
    period_ = slope;
    intercept_ = icpt;
    valid_ = period_ > 0;
}

double LinearClock::ResidualQuantile(double q) const {
    if (!valid_ || obs_.empty()) return 0;
    std::vector<double> r;
    r.reserve(obs_.size());
    for (auto& o : obs_) r.push_back((double)o.t - TimeOf((double)o.n));
    size_t k = (size_t)std::clamp(q * (double)(r.size() - 1), 0.0, (double)(r.size() - 1));
    std::nth_element(r.begin(), r.begin() + k, r.end());
    return r[k];
}

// ------------------------------------------------------------------ VsyncClock
bool VsyncClock::Start(IDXGIOutput* output, double nominalHz) {
    output_ = output;
    nominalTicks_ = (double)Qpc::Freq() / nominalHz;
    clock_.SetNominalPeriod(nominalTicks_);
    run_ = true;
    thr_ = std::thread([this] { Run(); });
    return true;
}

void VsyncClock::Stop() {
    run_ = false;
    if (thr_.joinable()) thr_.join();
}

void VsyncClock::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int64_t n = 0;
    while (run_) {
        if (FAILED(output_->WaitForVBlank())) { Sleep(1); continue; }
        int64_t t = Qpc::Now();
        std::lock_guard<std::mutex> lk(m_);
        // Assign an integer vblank index. Once the fit is valid, index from the fitted line
        // (not from the jittery previous wake) so one late wake cannot shift all later indices;
        // a wake that rounds onto an already-used index is a duplicate and is dropped.
        if (lastT_ != 0) {
            int64_t idx;
            if (clock_.Valid()) idx = (int64_t)std::llround(clock_.IndexAt((double)t));
            else idx = n + std::max<int64_t>(1, (int64_t)std::llround((double)(t - lastT_) / nominalTicks_));
            if (idx <= n) { lastT_ = t; continue; }
            n = idx;
        }
        lastT_ = t;
        clock_.Add(n, t);
        count_++;
    }
}

bool VsyncClock::Valid() const { std::lock_guard<std::mutex> lk(m_); return clock_.Valid(); }

double VsyncClock::PeriodTicks() const {
    std::lock_guard<std::mutex> lk(m_);
    return clock_.Valid() ? clock_.Period() : nominalTicks_;
}

double VsyncClock::JitterMs() const {
    std::lock_guard<std::mutex> lk(m_);
    double hi = clock_.ResidualQuantile(0.99), lo = clock_.ResidualQuantile(0.01);
    return Qpc::ToMs((int64_t)std::max(std::abs(hi), std::abs(lo)));
}

int64_t VsyncClock::NextVblankAfter(int64_t t) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!clock_.Valid()) {
        if (lastT_ == 0) return t + (int64_t)nominalTicks_;
        double k = std::floor((double)(t - lastT_) / nominalTicks_) + 1;
        return lastT_ + (int64_t)(k * nominalTicks_);
    }
    double idx = std::floor(clock_.IndexAt((double)t)) + 1;
    return (int64_t)clock_.TimeOf(idx);
}

} // namespace camss
