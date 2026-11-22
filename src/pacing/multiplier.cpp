#include "multiplier.h"
#include "lead.h"
#include <algorithm>

namespace cameravfx {

namespace {
constexpr double kThresholdTolerance = 0.97;
constexpr double kRefreshToleranceHz = 0.5;
constexpr double kRaiseCostMarginMs = 1.5;
constexpr double kRaiseLeadMarginMs = 1.0;
}

MultiplierChoice ChooseMultiplier(const MultiplierInputs& in) {
    if (!in.fgActive) return {1, MultiplierLimit::Off};
    MultiplierChoice choice{in.requested, MultiplierLimit::None};
    if (choice.multiplier > 1 && in.thresholdFps > 0 && in.contentFps >= kThresholdTolerance * in.thresholdFps)
        return {1, MultiplierLimit::Threshold};
    while (choice.multiplier > 1 && in.contentFps * choice.multiplier > in.maxRefreshHz + kRefreshToleranceHz) {
        choice.multiplier--;
        choice.limit = MultiplierLimit::Refresh;
    }
    if (!in.vrr || !in.generatedFrameMs) return choice;
    const double cost = *in.generatedFrameMs;
    while (choice.multiplier > 1) {
        const double outputPeriodMs = 1000.0 / (in.contentFps * choice.multiplier);
        const bool raising = choice.multiplier > in.current;
        const double needMs = raising ? std::max(cost + kRaiseCostMarginMs, in.leadMs + kRaiseLeadMarginMs) : std::max(cost, in.leadMs) + kOutputPeriodHeadroomMs;
        if (outputPeriodMs >= needMs) break;
        choice.multiplier--;
        choice.limit = MultiplierLimit::Gpu;
    }
    return choice;
}

}
