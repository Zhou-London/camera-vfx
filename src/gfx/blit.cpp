#include "blit.h"
#include "../capture/timecode.h"
#include <cmath>

namespace cameravfx {

namespace {

const char* kBlitPS = R"(
cbuffer Constants : register(b0) {
    float4 dstRect;
    uint inPq; uint outPq; float whiteNits; uint timecode;
    uint tcValue; uint marker; uint markerOn; uint loading;
    float2 dstSize; float loadDim; float loadProgress;
    float loadTime; uint tcBlocks; float tcBandFrac; float pad;
};
Texture2D<float4> Src : register(t0);
SamplerState LinearClamp : register(s0);

static const float kMarkerSizeFrac = 0.08;
static const float kSdrDisplayGamma = 2.2;

float3 Gamma22ToLinear(float3 c) { return pow(max(c, 0), kSdrDisplayGamma); }
float3 PqEncode(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1 + c3 * y), m2);
}
float3 PqDecode(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1);
}
static const float3x3 Rec709To2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950 };
static const float3x3 Rec2020To709 = {
     1.6604910, -0.5876411, -0.0728499,
    -0.1245505,  1.1328999, -0.0083494,
    -0.0181508, -0.1005789,  1.1187297 };

float4 OutSdr(float3 c) {
    if (outPq) return float4(PqEncode(mul(Rec709To2020, Gamma22ToLinear(c) * whiteNits)), 1);
    return float4(c, 1);
}

float3 PqToSdr(float3 c) {
    float3 relative = mul(Rec2020To709, PqDecode(c)) / whiteNits;
    return pow(saturate(relative), 1.0 / kSdrDisplayGamma);
}

static const uint kGlyphRows = 7;
static const uint kGlyphColumns = 5;
static const uint kLoadingGlyphs = 7;
static const uint kGlyphs[kLoadingGlyphs * kGlyphRows] = {
    16, 16, 16, 16, 16, 16, 31,
    14, 17, 17, 17, 17, 17, 14,
    14, 17, 17, 31, 17, 17, 17,
    30, 17, 17, 17, 17, 17, 30,
    14, 4, 4, 4, 4, 4, 14,
    17, 25, 21, 19, 17, 17, 17,
    14, 17, 16, 23, 17, 17, 15 };

float4 Loading(float2 p) {
    float3 c = 0;
    if (loadDim > 0 && p.x >= dstRect.x && p.y >= dstRect.y && p.x < dstRect.z && p.y < dstRect.w) {
        float2 uv = (p - dstRect.xy) / (dstRect.zw - dstRect.xy);
        c = Src.SampleLevel(LinearClamp, uv, 0).rgb * loadDim;
    }
    const float fontPixel = max(2.0, floor(dstSize.y / 200.0));
    const float textColumns = kLoadingGlyphs * (kGlyphColumns + 1) - 1;
    const float2 origin = floor(dstSize * 0.5 - float2(textColumns * fontPixel * 0.5, 6.0 * fontPixel));
    float2 q = (p - origin) / fontPixel;
    if (q.x >= 0 && q.x < textColumns && q.y >= 0 && q.y < kGlyphRows) {
        int glyph = (int)q.x / (kGlyphColumns + 1), column = (int)q.x % (kGlyphColumns + 1), row = (int)q.y;
        if (column < kGlyphColumns && ((kGlyphs[glyph * kGlyphRows + row] >> (kGlyphColumns - 1 - column)) & 1)) return OutSdr(0.92.xxx);
    }
    if (q.x >= 0 && q.x < textColumns && q.y >= 10 && q.y < 11) {
        float x = q.x / textColumns, a, b;
        if (loadProgress >= 0) { a = 0; b = saturate(loadProgress); }
        else { a = frac(loadTime * 0.7) * 1.35 - 0.35; b = a + 0.35; }
        bool fill = x >= a && x < b;
        return OutSdr(fill ? float3(0.85, 0.47, 0.34) : 0.25.xxx);
    }
    return OutSdr(c);
}

float TimecodeLevel(float x) {
    int block = (int)(x / dstSize.x * tcBlocks);
    if (block <= 0) return 1;
    if (block >= (int)tcBlocks - 1) return 0;
    return (tcValue >> (tcBlocks - 2 - block)) & 1;
}

float4 main(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    if (loading != 0) return Loading(p);
    if (timecode != 0 && p.y < dstSize.y * tcBandFrac) return OutSdr(TimecodeLevel(p.x).xxx);
    if (marker != 0 && p.x < dstSize.y * kMarkerSizeFrac && p.y > dstSize.y * (1 - kMarkerSizeFrac)) return OutSdr(markerOn ? 1.0.xxx : 0.0.xxx);
    if (p.x < dstRect.x || p.y < dstRect.y || p.x >= dstRect.z || p.y >= dstRect.w) return float4(0, 0, 0, 1);
    float2 uv = (p - dstRect.xy) / (dstRect.zw - dstRect.xy);
    float3 c = Src.SampleLevel(LinearClamp, uv, 0).rgb;
    if (inPq == outPq) return float4(c, 1);
    if (outPq) return OutSdr(c);
    return float4(PqToSdr(c), 1);
}
)";

constexpr float kIndeterminateProgress = -1.f;

struct BlitConstants {
    float dstRect[4];
    UINT inPq, outPq;
    float whiteNits;
    UINT timecode;
    UINT timecodeValue, marker, markerOn, loading;
    float dstSize[2], loadingDim, loadingProgress;
    float loadingTime;
    UINT timecodeBlocks;
    float timecodeBandFrac, pad;
};

}

void Blitter::Init(Device& dev) {
    dev_ = &dev;
    ps_ = dev.CompilePS(kBlitPS, "main", "blit_ps");
    constants_ = dev.CreateConstantBuffer(sizeof(BlitConstants));
}

void Blitter::Draw(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                   const BlitParams& p) {
    const FrameSize fit = FitAspect({srcW, srcH}, {dstW, dstH});
    const float w = (float)fit.width, h = (float)fit.height;
    const float x0 = std::floor(((float)dstW - w) * 0.5f), y0 = std::floor(((float)dstH - h) * 0.5f);
    const BlitConstants c{{x0, y0, x0 + w, y0 + h}, p.inEnc == Encoding::Pq, p.outEnc == Encoding::Pq, p.sdrWhiteNits, p.timecode,
                          p.timecodeValue, p.marker, p.markerOn, p.loading, {(float)dstW, (float)dstH}, p.loadingDim, p.loadingProgress.value_or(kIndeterminateProgress),
                          p.loadingTime, (UINT)kTimecodeBlocks, kTimecodeBandFrac, 0};
    dev_->UpdateCB(constants_.Get(), &c, sizeof(c));
    dev_->DrawFullscreen(ps_.Get(), constants_.Get(), {&src, 1}, dst, dstW, dstH);
}

}
