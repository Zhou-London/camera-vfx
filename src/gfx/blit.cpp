#include "blit.h"
#include "color_convert.h"
#include <cmath>

namespace camss {

static const char* kBlitPS = R"(
cbuffer CB : register(b0) {
    float4 dstRect;        // x0, y0, x1, y1 in pixels (letterboxed image area)
    uint inPq; uint outPq; float whiteNits; uint timecode;
    uint tcValue; uint marker; uint markerOn; uint pad;
    float2 dstSize; float2 pad2;
};
Texture2D<float4> Src : register(t0);
SamplerState Lin : register(s0);

float3 SrgbToLinear(float3 c) { return pow(max(c, 0), 2.2); }          // gamma 2.2 display EOTF
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

float4 main(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    // Loopback timecode band across the top of the full output.
    if (timecode != 0) {
        float bandH = dstSize.y * BAND_FRAC;
        if (p.y < bandH) {
            int blk = (int)(p.x / dstSize.x * BLOCKS);
            float v;
            if (blk <= 0) v = 1;
            else if (blk >= BLOCKS - 1) v = 0;
            else v = ((tcValue >> (32 - blk)) & 1) ? 1 : 0;
            float3 c = v.xxx;
            if (outPq) c = PqEncode(c * whiteNits);
            return float4(c, 1);
        }
    }
    if (marker != 0 && p.x < dstSize.y * 0.08 && p.y > dstSize.y * 0.92) {
        float3 c = markerOn ? 1.0.xxx : 0.0.xxx;
        if (outPq) c = PqEncode(c * whiteNits);
        return float4(c, 1);
    }
    if (p.x < dstRect.x || p.y < dstRect.y || p.x >= dstRect.z || p.y >= dstRect.w) return float4(0, 0, 0, 1);
    float2 uv = (p - dstRect.xy) / (dstRect.zw - dstRect.xy);
    float3 c = Src.SampleLevel(Lin, uv, 0).rgb;
    if (inPq == outPq) return float4(c, 1);
    if (outPq) {
        // SDR (gamma 2.2, BT.709) -> HDR10 (PQ, BT.2020) at `whiteNits` paper white.
        float3 lin = SrgbToLinear(c) * whiteNits;
        return float4(PqEncode(mul(Rec709To2020, lin)), 1);
    }
    // HDR10 -> SDR: simple clip at paper white.
    float3 lin = mul(Rec2020To709, PqDecode(c)) / whiteNits;
    return float4(pow(saturate(lin), 1.0 / 2.2), 1);
}
)";

bool Blitter::Init(Device& dev) {
    dev_ = &dev;
    vs_ = dev.CompileVS(kFullscreenVS, "main", "fullscreen_vs");
    char blocks[16], frac[32];
    snprintf(blocks, sizeof(blocks), "%d", kTimecodeBlocks);
    snprintf(frac, sizeof(frac), "%.8f", kTimecodeBandFrac);
    D3D_SHADER_MACRO defs[] = {{"BLOCKS", blocks}, {"BAND_FRAC", frac}, {nullptr, nullptr}};
    ps_ = dev.CompilePS(kBlitPS, "main", "blit_ps", defs);
    cb_ = dev.CreateConstantBuffer(64);
    return true;
}

void Blitter::Draw(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
                   const BlitParams& p) {
    // Aspect-fit the source into the destination.
    float sa = (float)srcW / (float)srcH, da = (float)dstW / (float)dstH;
    float w = (float)dstW, h = (float)dstH;
    if (sa > da) h = w / sa; else w = h * sa;
    float x0 = std::floor(((float)dstW - w) * 0.5f), y0 = std::floor(((float)dstH - h) * 0.5f);
    struct {
        float rect[4];
        UINT inPq, outPq; float white; UINT tc;
        UINT tcValue, marker, markerOn, pad;
        float size[2], pad2[2];
    } cb{{x0, y0, x0 + w, y0 + h}, p.inEnc == Encoding::Pq, p.outEnc == Encoding::Pq, p.sdrWhiteNits, p.timecode,
         p.timecodeValue, p.marker, p.markerOn, 0, {(float)dstW, (float)dstH}, {0, 0}};
    dev_->UpdateCB(cb_.Get(), &cb, sizeof(cb));
    auto* ctx = dev_->ctx();
    D3D11_VIEWPORT vp{0, 0, (float)dstW, (float)dstH, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ID3D11SamplerState* s = dev_->linearClamp();
    ctx->PSSetSamplers(0, 1, &s);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// ------------------------------------------------------------------ synthetic source
static const char* kSynthPS = R"(
cbuffer CB : register(b0) { float time; float2 size; float pad; };
float4 main(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    float3 c = 0.15.xxx;
    // Scrolling checkerboard background (horizontal pan 600 px/s).
    float2 q = p + float2(time * 600.0, 0);
    float chk = ((int)floor(q.x / 64) + (int)floor(q.y / 64)) & 1;
    c = lerp(float3(0.2, 0.22, 0.25), float3(0.35, 0.37, 0.4), chk);
    // Vertical bar sweeping across at 1 screen/2s.
    float bx = frac(time * 0.5) * size.x;
    if (abs(p.x - bx) < 24) c = float3(1, 0.9, 0.2);
    // Orbiting circle.
    float2 ctr = size * 0.5 + float2(cos(time * 2.5), sin(time * 2.5)) * size.y * 0.3;
    if (length(p - ctr) < 60) c = float3(0.2, 0.6, 1.0);
    // Fine text-like stripes moving down (tests detail).
    if (p.x > size.x * 0.8 && frac((p.y - time * 240.0) / 12.0) < 0.5) c *= 0.4;
    return float4(c, 1);
}
)";

bool SyntheticSource::Init(Device& dev, UINT, UINT) {
    dev_ = &dev;
    vs_ = dev.CompileVS(kFullscreenVS, "main", "fullscreen_vs");
    ps_ = dev.CompilePS(kSynthPS, "main", "synth_ps");
    cb_ = dev.CreateConstantBuffer(16);
    return true;
}

void SyntheticSource::Render(const Device::Tex& dst, double t) {
    struct { float time; float size[2]; float pad; } cb{(float)std::fmod(t, 3600.0), {(float)dst.w, (float)dst.h}, 0};
    dev_->UpdateCB(cb_.Get(), &cb, sizeof(cb));
    auto* ctx = dev_->ctx();
    D3D11_VIEWPORT vp{0, 0, (float)dst.w, (float)dst.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, dst.rtv.GetAddressOf(), nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

} // namespace camss
