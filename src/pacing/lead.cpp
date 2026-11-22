#include "lead.h"
#include "../util/common.h"
#include <algorithm>

namespace cameravfx {

namespace {
constexpr uint64_t kStartUpFramesIgnored = 60;
constexpr size_t kMinCostSamples = 30;
constexpr double kCostQuantile = 0.99;
constexpr double kCostMarginMs = 0.75;
constexpr double kBusyStepMs = 0.25;
constexpr double kMissStepMs = 0.5;
constexpr double kMissMinIntervalMs = 100;
constexpr double kRelaxIntervalMs = 3000;
constexpr double kRelaxStepMs = 0.1;
constexpr double kMinLeadMs = 1.0;

std::optional<double> CostWithMargin(const RollingStat& cost) {
    if (cost.Count() < kMinCostSamples) return std::nullopt;
    return cost.Quantile(kCostQuantile) + kCostMarginMs;
}

double LeadCeilingMs(double outputPeriodMs) { return outputPeriodMs - kOutputPeriodHeadroomMs; }
}

void LeadController::OnFrameCost(double ms, bool generated) {
    if (++costSamples_ <= kStartUpFramesIgnored) return;
    frameCost_.Add(ms);
    if (generated) generatedCost_.Add(ms);
}

std::optional<double> LeadController::GeneratedFrameMs() const { return CostWithMargin(generatedCost_); }

void LeadController::Raise(int64_t now, double stepMs, double outputPeriodMs) {
    leadMs_ = std::min(leadMs_ + stepMs, LeadCeilingMs(outputPeriodMs));
    lastAdjust_ = now;
}

void LeadController::OnGpuBusyAtPresent(int64_t now, double outputPeriodMs) { Raise(now, kBusyStepMs, outputPeriodMs); }

void LeadController::OnMissedVblank(int64_t now, double outputPeriodMs) {
    if (now - lastAdjust_ > Qpc::FromMs(kMissMinIntervalMs)) Raise(now, kMissStepMs, outputPeriodMs);
}

void LeadController::Relax(int64_t now, double outputPeriodMs) {
    if (now - lastAdjust_ <= Qpc::FromMs(kRelaxIntervalMs)) return;
    const double floorMs = CostWithMargin(frameCost_).value_or(kMinLeadMs);
    leadMs_ = std::min(std::max(floorMs, leadMs_ - kRelaxStepMs), LeadCeilingMs(outputPeriodMs));
    lastAdjust_ = now;
}

}
