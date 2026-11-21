// Baseline: linear cross-fade between the two frames (no motion compensation).
// Used to validate pacing and as a last-resort fallback.
#include "interpolator.h"

namespace camss {

static const char* kBlendCS = R"(
cbuffer CB : register(b0) { float t; uint w; uint h; uint pad; };
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
RWTexture2D<unorm float4> O : register(u0);
[numthreads(16, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    O[id.xy] = lerp(A[id.xy], B[id.xy], t);
}
)";

class BlendInterpolator : public Interpolator {
public:
    const char* Name() const override { return "blend"; }
    bool Init(Device& dev, UINT w, UINT h) override {
        dev_ = &dev; w_ = w; h_ = h;
        cs_ = dev.CompileCS(kBlendCS, "main", "blend_cs");
        cb_ = dev.CreateConstantBuffer(16);
        return true;
    }
    void Interpolate(const SourceFrame& a, const SourceFrame& b, float t, const Device::Tex& out) override {
        struct { float t; UINT w, h, pad; } cb{t, w_, h_, 0};
        dev_->UpdateCB(cb_.Get(), &cb, sizeof(cb));
        auto* ctx = dev_->ctx();
        ID3D11ShaderResourceView* srvs[2] = {a.rgb.srv.Get(), b.rgb.srv.Get()};
        ctx->CSSetShader(cs_.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, out.uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->Dispatch((w_ + 15) / 16, (h_ + 7) / 8, 1);
        dev_->ClearCS();
    }
private:
    Device* dev_ = nullptr;
    UINT w_ = 0, h_ = 0;
    ComPtr<ID3D11ComputeShader> cs_;
    ComPtr<ID3D11Buffer> cb_;
};

std::unique_ptr<Interpolator> CreateBlendInterpolator() { return std::make_unique<BlendInterpolator>(); }

} // namespace camss
