#include "synthetic_source.h"
#include <cmath>

namespace cameravfx {

namespace {

constexpr double kTimeWrapSec = 3600.0;

const char* kSyntheticPS = R"(
cbuffer Constants : register(b0) { float time; float2 size; float pad; };

static const float kPanPixelsPerSec = 600.0;
static const float kCheckerCell = 64.0;
static const float kSweepScreensPerSec = 0.5;
static const float kSweepHalfWidth = 24.0;
static const float kOrbitRadiansPerSec = 2.5;
static const float kOrbitRadiusFrac = 0.3;
static const float kDiscRadius = 60.0;
static const float kStripeScrollPixelsPerSec = 240.0;
static const float kStripePeriod = 12.0;
static const float kStripeRegionFrac = 0.8;

float4 main(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    float2 panned = p + float2(time * kPanPixelsPerSec, 0);
    float checker = ((int)floor(panned.x / kCheckerCell) + (int)floor(panned.y / kCheckerCell)) & 1;
    float3 c = lerp(float3(0.2, 0.22, 0.25), float3(0.35, 0.37, 0.4), checker);
    float sweepX = frac(time * kSweepScreensPerSec) * size.x;
    if (abs(p.x - sweepX) < kSweepHalfWidth) c = float3(1, 0.9, 0.2);
    float2 orbit = size * 0.5 + float2(cos(time * kOrbitRadiansPerSec), sin(time * kOrbitRadiansPerSec)) * size.y * kOrbitRadiusFrac;
    if (length(p - orbit) < kDiscRadius) c = float3(0.2, 0.6, 1.0);
    if (p.x > size.x * kStripeRegionFrac && frac((p.y - time * kStripeScrollPixelsPerSec) / kStripePeriod) < 0.5) c *= 0.4;
    return float4(c, 1);
}
)";

struct SyntheticConstants {
    float time;
    float size[2];
    float pad;
};

}

void SyntheticSource::Init(Device& dev) {
    dev_ = &dev;
    ps_ = dev.CompilePS(kSyntheticPS, "main", "synthetic_ps");
    constants_ = dev.CreateConstantBuffer(sizeof(SyntheticConstants));
}

void SyntheticSource::Render(const Device::Tex& dst, double timeSec) {
    const SyntheticConstants c{(float)std::fmod(timeSec, kTimeWrapSec), {(float)dst.w, (float)dst.h}, 0};
    dev_->UpdateCB(constants_.Get(), &c, sizeof(c));
    dev_->DrawFullscreen(ps_.Get(), constants_.Get(), {}, dst.rtv.Get(), dst.w, dst.h);
}

}
