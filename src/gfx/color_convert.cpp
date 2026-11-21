#include "color_convert.h"

namespace camss {

const char* kFullscreenVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char* kConvertPS = R"(
cbuffer CB : register(b0) {
    float4 rowR; float4 rowG; float4 rowB;   // yuv->rgb matrix rows (xyz) + offset (w)
    float4 yScaleOffset;                     // x = y scale, y = y offset, z = c scale, w = c offset
    uint2 size; uint2 pad;
};
Texture2D<float4> P0 : register(t0);
Texture2D<float4> P1 : register(t1);

float3 YuvToRgb(float y, float2 c) {
    y = (y - yScaleOffset.y) * yScaleOffset.x;
    c = (c - yScaleOffset.w) * yScaleOffset.z;
    float3 yuv = float3(y, c);
    return saturate(float3(dot(rowR.xyz, yuv), dot(rowG.xyz, yuv), dot(rowB.xyz, yuv)));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int2 p = int2(pos.xy);
#if FMT_YUY2
    float4 t = P0.Load(int3(p.x >> 1, p.y, 0));             // Y0 U Y1 V
    float y = (p.x & 1) ? t.z : t.x;
    // Horizontal chroma: average with the neighbour pair for odd pixels (co-sited left).
    float2 c = t.yw;
    if (p.x & 1) {
        float4 tn = P0.Load(int3(min((p.x >> 1) + 1, (int)size.x / 2 - 1), p.y, 0));
        c = 0.5 * (t.yw + tn.yw);
    }
    return float4(YuvToRgb(y, c), 1);
#elif FMT_NV12 || FMT_P010
    float y = P0.Load(int3(p, 0)).x;
    // Chroma is 2x subsampled, left co-sited horizontally, centred vertically.
    float2 cuv = float2((p.x * 0.5 + 0.25 + 0.25) / (size.x * 0.5), (p.y + 0.5) / size.y);
    float2 c = P1.SampleLevel(LinSampler, cuv, 0).xy;
    return float4(YuvToRgb(y, c), 1);
#elif FMT_RGB24
    int base = p.x * 3;
    float b = P0.Load(int3(base + 0, p.y, 0)).x;
    float g = P0.Load(int3(base + 1, p.y, 0)).x;
    float r = P0.Load(int3(base + 2, p.y, 0)).x;
    return float4(r, g, b, 1);
#else
    return float4(P0.Load(int3(p, 0)).rgb, 1);
#endif
}
)";

bool ColorConverter::Init(Device& dev, PixFmt fmt, UINT w, UINT h, const ColorOptions& opt) {
    dev_ = &dev; fmt_ = fmt; w_ = w; h_ = h;
    vs_ = dev.CompileVS(kFullscreenVS, "main", "fullscreen_vs");
    std::string src = std::string("SamplerState LinSampler : register(s0);\n") + kConvertPS;
    D3D_SHADER_MACRO defs[2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    switch (fmt) {
    case PixFmt::YUY2: defs[0] = {"FMT_YUY2", "1"}; break;
    case PixFmt::NV12: defs[0] = {"FMT_NV12", "1"}; break;
    case PixFmt::P010: defs[0] = {"FMT_P010", "1"}; break;
    case PixFmt::RGB24: defs[0] = {"FMT_RGB24", "1"}; break;
    default: break;
    }
    ps_ = dev.CompilePS(src.c_str(), "main", "convert_ps", defs);
    cb_ = dev.CreateConstantBuffer(96);

    // YCbCr -> R'G'B' matrix.
    double kr = opt.bt601 ? 0.299 : 0.2126, kb = opt.bt601 ? 0.114 : 0.0722, kg = 1.0 - kr - kb;
    struct { float rowR[4], rowG[4], rowB[4], yso[4]; UINT size[2], pad[2]; } cb{};
    cb.rowR[0] = 1; cb.rowR[1] = 0; cb.rowR[2] = (float)(2 - 2 * kr);
    cb.rowG[0] = 1; cb.rowG[1] = (float)(-(2 - 2 * kb) * kb / kg); cb.rowG[2] = (float)(-(2 - 2 * kr) * kr / kg);
    cb.rowB[0] = 1; cb.rowB[1] = (float)(2 - 2 * kb); cb.rowB[2] = 0;
    // P010 stores 10-bit in the high bits of 16; UNORM read gives the same normalised scale as 8-bit.
    if (opt.fullRange) { cb.yso[0] = 1.f; cb.yso[1] = 0.f; cb.yso[2] = 1.f; cb.yso[3] = 128.f / 255.f; }
    else { cb.yso[0] = 255.f / 219.f; cb.yso[1] = 16.f / 255.f; cb.yso[2] = 255.f / 224.f; cb.yso[3] = 128.f / 255.f; }
    cb.size[0] = w; cb.size[1] = h;
    dev.UpdateCB(cb_.Get(), &cb, sizeof(cb));
    return true;
}

void ColorConverter::Convert(ID3D11ShaderResourceView* p0, ID3D11ShaderResourceView* p1, const Device::Tex& dst) {
    auto* ctx = dev_->ctx();
    D3D11_VIEWPORT vp{0, 0, (float)dst.w, (float)dst.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {p0, p1};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ID3D11SamplerState* s = dev_->linearClamp();
    ctx->PSSetSamplers(0, 1, &s);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    ctx->PSSetShaderResources(0, 2, nulls);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace camss
