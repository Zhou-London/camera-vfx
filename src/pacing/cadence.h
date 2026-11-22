#pragma once
#include <cstdint>
#include <vector>

namespace cameravfx {

inline const std::vector<double> kDefaultContentPeriods = {1.0, 1.25, 4.0 / 3.0, 1.5, 2.0, 2.5, 3.0, 4.0};

std::vector<double> ContentPeriods(const std::vector<double>& contentRates, double signalHz);

class CadenceDetector {
public:
    static constexpr int kWindowFrames = 40;
    static constexpr int kMaxUniqueInterval = 6;
    static constexpr int kMinIntervals = 6;
    static constexpr int kHoldFrames = 30;
    static constexpr double kSnapTolerance = 0.10;

    explicit CadenceDetector(std::vector<double> periods = kDefaultContentPeriods, double contentPeriod = 1.0);
    bool OnFrame(int64_t n, bool unique);
    double ContentPeriod() const { return active_; }
    double UniqueFraction() const;

private:
    enum class Slot : uint8_t { Missing, Repeat, Unique };
    double EstimatePeriod() const;
    std::vector<double> periods_;
    std::vector<Slot> slots_ = std::vector<Slot>(kWindowFrames, Slot::Missing);
    double active_;
    double heldEstimate_ = 0;
    int heldFrames_ = 0;
    int64_t lastN_ = -1;
    int received_ = 0, uniques_ = 0;
};

}
