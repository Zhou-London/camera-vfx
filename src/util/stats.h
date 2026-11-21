// Latency statistics: per-stage rolling samples with p50/p99/max.
#pragma once
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <map>
#include <mutex>
#include <cstdint>
#include <cstdio>

namespace camss {

// Rolling window of samples (ms). Not thread safe; protect externally.
class RollingStat {
public:
    explicit RollingStat(size_t cap = 4096) : cap_(cap) { buf_.reserve(cap); }
    void Add(double v) {
        if (buf_.size() < cap_) buf_.push_back(v); else buf_[pos_] = v;
        pos_ = (pos_ + 1) % cap_;
        total_++;
    }
    size_t Count() const { return buf_.size(); }
    uint64_t Total() const { return total_; }
    // Copies the (unordered) samples into `out`, reusing its capacity.
    void CopySamples(std::vector<double>& out) const { out.assign(buf_.begin(), buf_.end()); }
    // In-place quantile of an arbitrary sample vector (reorders it).
    static double QuantileOf(std::vector<double>& v, double q) {
        if (v.empty()) return 0.0;
        size_t k = (size_t)std::clamp<double>(q * (double)(v.size() - 1) + 0.5, 0.0, (double)(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }
    // No allocation after the first call (scratch buffer keeps its capacity).
    double Quantile(double q) const {
        if (buf_.empty()) return 0.0;
        tmp_.assign(buf_.begin(), buf_.end());
        return QuantileOf(tmp_, q);
    }
    double Mean() const {
        if (buf_.empty()) return 0.0;
        double s = 0; for (double v : buf_) s += v; return s / (double)buf_.size();
    }
    double Max() const { return buf_.empty() ? 0.0 : *std::max_element(buf_.begin(), buf_.end()); }
    void Clear() { buf_.clear(); pos_ = 0; }
private:
    size_t cap_;
    size_t pos_ = 0;
    uint64_t total_ = 0;
    std::vector<double> buf_;
    mutable std::vector<double> tmp_;
};

struct StatSummary {
    std::string name;
    double p50 = 0, p99 = 0, max = 0;
    size_t n = 0;
};

// Named collection of RollingStats, thread safe.
// Add() does not allocate once a name is known (heterogeneous lookup by string_view).
// Snapshot()/Report()/Get() hold the Add() lock only to memcpy one stat's samples at a
// time; quantiles are computed outside the lock.
class StatBook {
public:
    void Add(std::string_view name, double v) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = stats_.find(name);
        if (it == stats_.end()) {
            it = stats_.emplace(std::string(name), RollingStat(2048)).first;
            order_.push_back(&*it);
        }
        it->second.Add(v);
    }

    std::vector<StatSummary> Snapshot() const {
        std::lock_guard<std::mutex> slk(snapM_);
        std::vector<StatSummary> out;
        size_t count;
        { std::lock_guard<std::mutex> lk(m_); count = order_.size(); }
        out.reserve(count);
        for (size_t i = 0;; i++) {
            const Entry* e;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (i >= order_.size()) break;
                e = order_[i];
                e->second.CopySamples(scratch_);   // brief: one memcpy (<= 2048 doubles)
            }
            // Map nodes are never erased and names are immutable: safe to read outside the lock.
            StatSummary s;
            s.name = e->first;
            s.n = scratch_.size();
            if (s.n) {
                s.max = *std::max_element(scratch_.begin(), scratch_.end());
                s.p50 = RollingStat::QuantileOf(scratch_, 0.5);
                s.p99 = RollingStat::QuantileOf(scratch_, 0.99);
            }
            out.push_back(std::move(s));
        }
        return out;
    }

    // Formats a snapshot as the multi-line report "name p50 p99 max n".
    static std::string Report(const std::vector<StatSummary>& snap) {
        std::string out;
        out.reserve(64 + snap.size() * 72);
        char line[256];
        snprintf(line, sizeof(line), "  %-26s %8s %8s %8s %8s\n", "stage(ms)", "p50", "p99", "max", "n");
        out += line;
        for (const auto& s : snap) {
            snprintf(line, sizeof(line), "  %-26s %8.3f %8.3f %8.3f %8zu\n", s.name.c_str(), s.p50, s.p99, s.max, s.n);
            out += line;
        }
        return out;
    }
    std::string Report() const { return Report(Snapshot()); }

    bool Get(std::string_view name, double& p50, double& p99) const {
        std::lock_guard<std::mutex> slk(snapM_);
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = stats_.find(name);
            if (it == stats_.end() || it->second.Count() == 0) return false;
            it->second.CopySamples(scratch_);
        }
        p50 = RollingStat::QuantileOf(scratch_, 0.5);
        p99 = RollingStat::QuantileOf(scratch_, 0.99);
        return true;
    }
    void Clear() { std::lock_guard<std::mutex> lk(m_); for (auto& kv : stats_) kv.second.Clear(); }

private:
    using Map = std::map<std::string, RollingStat, std::less<>>;
    using Entry = Map::value_type;
    mutable std::mutex m_;                 // guards stats_ / order_ (taken by Add)
    mutable std::mutex snapM_;             // serialises readers sharing scratch_ (lock order: snapM_ -> m_)
    Map stats_;
    std::vector<const Entry*> order_;      // insertion order
    mutable std::vector<double> scratch_;
};

} // namespace camss
