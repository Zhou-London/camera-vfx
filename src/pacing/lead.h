#pragma once
#include "../util/stats.h"
#include <cstdint>
#include <optional>

namespace cameravfx {

inline constexpr double kOutputPeriodHeadroomMs = 0.5;

class LeadController {
public:
    explicit LeadController(double initialLeadMs) : leadMs_(initialLeadMs) {}
    double LeadMs() const { return leadMs_; }
    void OnFrameCost(double ms, bool generated);
    std::optional<double> GeneratedFrameMs() const;
    void OnGpuBusyAtPresent(int64_t now, double outputPeriodMs);
    void OnMissedVblank(int64_t now, double outputPeriodMs);
    void Relax(int64_t now, double outputPeriodMs);

private:
    static constexpr size_t kCostWindow = 300;
    void Raise(int64_t now, double stepMs, double outputPeriodMs);
    double leadMs_;
    RollingStat frameCost_{kCostWindow};
    RollingStat generatedCost_{kCostWindow};
    uint64_t costSamples_ = 0;
    int64_t lastAdjust_ = 0;
};

}
