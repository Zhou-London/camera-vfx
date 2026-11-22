#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cameravfx {

namespace stat {
inline constexpr std::string_view kCaptureDelivery = "cap.sample_to_arrival_ms";
inline constexpr std::string_view kCaptureUpload = "cap.upload_ms";
inline constexpr std::string_view kLoopLatency = "loop.scanout_to_arrival_ms";
inline constexpr std::string_view kVfxFg = "vfx.fg";
inline constexpr std::string_view kVfxHdr = "vfx.hdr";
inline constexpr std::string_view kVfxSr = "vfx.sr";
inline constexpr std::string_view kVfxOut = "vfx.out";
inline constexpr std::string_view kVfxGpuTotal = "vfx.gpu_total";
inline constexpr std::string_view kArrivalToShown = "lat.arrival_to_shown_ms";
inline constexpr std::string_view kContentToShown = "lat.content_to_shown_ms";
inline constexpr std::string_view kTimerLate = "timer.late_ms";
inline constexpr std::string_view kWaitReady = "present.wait_ready_ms";
inline constexpr std::string_view kPresentInterval = "present.interval_ms";
inline constexpr std::string_view kRenderCpu = "render.cpu_ms";
}

template <class T>
T Quantile(std::vector<T>& nonEmptySamples, double q) {
    std::vector<T>& v = nonEmptySamples;
    const size_t k = (size_t)std::clamp<double>(q * (double)(v.size() - 1) + 0.5, 0.0, (double)(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

class RollingStat {
public:
    explicit RollingStat(size_t capacity) : capacity_(capacity) { samples_.reserve(capacity); }
    void Add(double v) {
        if (samples_.size() < capacity_) samples_.push_back(v);
        else samples_[next_] = v;
        next_ = (next_ + 1) % capacity_;
    }
    size_t Count() const { return samples_.size(); }
    const std::vector<double>& Samples() const { return samples_; }
    double Quantile(double q) const {
        scratch_.assign(samples_.begin(), samples_.end());
        return cameravfx::Quantile(scratch_, q);
    }
    void Clear() { samples_.clear(); next_ = 0; }
private:
    size_t capacity_;
    size_t next_ = 0;
    std::vector<double> samples_;
    mutable std::vector<double> scratch_;
};

struct Percentiles {
    double p50 = 0, p99 = 0;
};

class StatBook {
public:
    void Add(std::string_view name, double v) {
        auto it = stats_.find(name);
        if (it == stats_.end()) {
            it = stats_.emplace(std::string(name), RollingStat(kWindowSamples)).first;
            order_.push_back(it);
        }
        it->second.Add(v);
    }

    std::string Report() const {
        std::string out;
        char line[256];
        snprintf(line, sizeof(line), "  %-26s %8s %8s %8s %8s\n", "stage(ms)", "p50", "p99", "max", "n");
        out += line;
        std::vector<double> samples;
        for (const auto& it : order_) {
            if (!it->second.Count()) continue;
            samples = it->second.Samples();
            const double max = *std::max_element(samples.begin(), samples.end());
            const double p50 = Quantile(samples, 0.5), p99 = Quantile(samples, 0.99);
            snprintf(line, sizeof(line), "  %-26s %8.3f %8.3f %8.3f %8zu\n", it->first.c_str(), p50, p99, max, samples.size());
            out += line;
        }
        return out;
    }

    std::optional<Percentiles> Get(std::string_view name) const {
        const auto it = stats_.find(name);
        if (it == stats_.end() || !it->second.Count()) return std::nullopt;
        return Percentiles{it->second.Quantile(0.5), it->second.Quantile(0.99)};
    }

    void Clear() { for (auto& [name, stat] : stats_) stat.Clear(); }

private:
    static constexpr size_t kWindowSamples = 2048;
    using Map = std::map<std::string, RollingStat, std::less<>>;
    Map stats_;
    std::vector<Map::const_iterator> order_;
};

}
