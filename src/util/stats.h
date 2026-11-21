// Latency statistics: per-stage rolling samples with p50/p99/max.
#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <mutex>

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
    double Quantile(double q) const {
        if (buf_.empty()) return 0.0;
        tmp_ = buf_;
        size_t k = (size_t)std::clamp<double>(q * (double)(tmp_.size() - 1) + 0.5, 0.0, (double)(tmp_.size() - 1));
        std::nth_element(tmp_.begin(), tmp_.begin() + k, tmp_.end());
        return tmp_[k];
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

// Named collection of RollingStats, thread safe.
class StatBook {
public:
    void Add(const std::string& name, double v) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = stats_.find(name);
        if (it == stats_.end()) { order_.push_back(name); it = stats_.emplace(name, RollingStat(2048)).first; }
        it->second.Add(v);
    }
    // Returns a multi-line report "name p50 p99 max n".
    std::string Report() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string out;
        char line[256];
        snprintf(line, sizeof(line), "  %-26s %8s %8s %8s %8s\n", "stage(ms)", "p50", "p99", "max", "n");
        out += line;
        for (auto& n : order_) {
            auto& s = stats_.at(n);
            snprintf(line, sizeof(line), "  %-26s %8.3f %8.3f %8.3f %8zu\n", n.c_str(), s.Quantile(0.5), s.Quantile(0.99), s.Max(), s.Count());
            out += line;
        }
        return out;
    }
    bool Get(const std::string& name, double& p50, double& p99) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = stats_.find(name);
        if (it == stats_.end() || it->second.Count() == 0) return false;
        p50 = it->second.Quantile(0.5); p99 = it->second.Quantile(0.99);
        return true;
    }
    void Clear() { std::lock_guard<std::mutex> lk(m_); for (auto& kv : stats_) kv.second.Clear(); }
private:
    mutable std::mutex m_;
    std::map<std::string, RollingStat> stats_;
    std::vector<std::string> order_;
};

} // namespace camss
