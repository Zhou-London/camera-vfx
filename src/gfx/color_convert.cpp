#include "color_convert.h"
#include <array>

namespace cameravfx {

namespace {

const char* kConvertPS = R"(
cbuffer Constants : register(b0) {
    float4 rowR; float4 rowG; float4 rowB;
    float lumaScale; float lumaOffset; float chromaScale; float chromaOffset;
    uint2 size; uint2 pad;
};
Texture2D<float4> P0 : register(t0);
Texture2D<float4> P1 : register(t1);
SamplerState LinearClamp : register(s0);

float3 YuvToRgb(float y, float2 c) {
    float3 yuv = float3((y - lumaOffset) * lumaScale, (c - chromaOffset) * chromaScale);
    return saturate(float3(dot(rowR.xyz, yuv), dot(rowG.xyz, yuv), dot(rowB.xyz, yuv)));
}

float3 ExpandRgb(float3 c) { return saturate((c - lumaOffset) * lumaScale); }

float4 main(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
#if FMT_YUY2
    float4 y0uy1v = P0.Load(int3(p.x >> 1, p.y, 0));
    float y = (p.x & 1) ? y0uy1v.z : y0uy1v.x;
    float2 c = y0uy1v.yw;
    if (p.x & 1) {
        float4 next = P0.Load(int3(min((p.x >> 1) + 1, (int)size.x / 2 - 1), p.y, 0));
        c = 0.5 * (y0uy1v.yw + next.yw);
    }
    return float4(YuvToRgb(y, c), 1);
#elif FMT_NV12 || FMT_P010
    float y = P0.Load(int3(p, 0)).x;
    float2 leftSitedChroma = float2((p.x * 0.5 + 0.5) / (size.x * 0.5), (p.y + 0.5) / size.y);
    float2 c = P1.SampleLevel(LinearClamp, leftSitedChroma, 0).xy;
    return float4(YuvToRgb(y, c), 1);
#elif FMT_RGB24
    int base = p.x * 3;
    float b = P0.Load(int3(base + 0, p.y, 0)).x;
    float g = P0.Load(int3(base + 1, p.y, 0)).x;
    float r = P0.Load(int3(base + 2, p.y, 0)).x;
    return float4(ExpandRgb(float3(r, g, b)), 1);
#else
    return float4(ExpandRgb(P0.Load(int3(p, 0)).rgb), 1);
#endif
}
)";

struct ConvertConstants {
    float rowR[4], rowG[4], rowB[4];
    float lumaScale, lumaOffset, chromaScale, chromaOffset;
    UINT size[2], pad[2];
};

struct LumaWeights {
    double red, blue;
};

constexpr std::array<LumaWeights, 3> kLumaWeights = {{{0.2126, 0.0722}, {0.299, 0.114}, {0.2627, 0.0593}}};

constexpr float kLimitedLumaScale = 255.f / 219.f;
constexpr float kLimitedLumaOffset = 16.f / 255.f;
constexpr float kLimitedChromaScale = 255.f / 224.f;
constexpr float kChromaZero = 128.f / 255.f;

const char* FormatDefine(PixFmt format) {
    constexpr const char* kDefines[] = {"FMT_YUY2", "FMT_NV12", "FMT_P010", "FMT_RGB24", "FMT_RGB32"};
    return kDefines[(int)format];
}

}

ColorSpec ResolveColor(const ColorOptions& chosen, const ColorOptions& tagged, PixFmt format) {
    const ColorMatrix formatMatrix = format == PixFmt::P010 ? ColorMatrix::Bt2020 : ColorMatrix::Bt709;
    return {chosen.matrix.value_or(tagged.matrix.value_or(formatMatrix)), chosen.fullRange.value_or(tagged.fullRange.value_or(IsRgb(format)))};
}

void ColorConverter::Init(Device& dev, PixFmt format, UINT width, UINT height) {
    dev_ = &dev;
    width_ = width;
    height_ = height;
    const D3D_SHADER_MACRO defines[] = {{FormatDefine(format), "1"}, {nullptr, nullptr}};
    ps_ = dev.CompilePS(kConvertPS, "main", "convert_ps", defines);
    constants_ = dev.CreateConstantBuffer(sizeof(ConvertConstants));
}

void ColorConverter::SetColor(const ColorSpec& color) {
    color_ = color;
    const LumaWeights w = kLumaWeights[(size_t)color.matrix];
    const double green = 1.0 - w.red - w.blue;
    ConvertConstants c{};
    c.rowR[0] = 1; c.rowR[1] = 0; c.rowR[2] = (float)(2 - 2 * w.red);
    c.rowG[0] = 1; c.rowG[1] = (float)(-(2 - 2 * w.blue) * w.blue / green); c.rowG[2] = (float)(-(2 - 2 * w.red) * w.red / green);
    c.rowB[0] = 1; c.rowB[1] = (float)(2 - 2 * w.blue); c.rowB[2] = 0;
    c.lumaScale = color.fullRange ? 1.f : kLimitedLumaScale;
    c.lumaOffset = color.fullRange ? 0.f : kLimitedLumaOffset;
    c.chromaScale = color.fullRange ? 1.f : kLimitedChromaScale;
    c.chromaOffset = kChromaZero;
    c.size[0] = width_; c.size[1] = height_;
    dev_->UpdateCB(constants_.Get(), &c, sizeof(c));
}

void ColorConverter::Convert(ID3D11ShaderResourceView* plane0, ID3D11ShaderResourceView* plane1, const Device::Tex& dst) {
    ID3D11ShaderResourceView* const planes[] = {plane0, plane1};
    dev_->DrawFullscreen(ps_.Get(), constants_.Get(), planes, dst.rtv.Get(), dst.w, dst.h);
}

}
