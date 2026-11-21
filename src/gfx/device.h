// D3D11 device wrapper, shader compilation, GPU timestamp profiling.
#pragma once
#include "../util/common.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <vector>
#include <array>
#include <string>

namespace camss {

class Device {
public:
    bool Create(bool debug);
    ID3D11Device5* dev() const { return dev_.Get(); }
    ID3D11DeviceContext4* ctx() const { return ctx_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }
    IDXGIAdapter4* adapter() const { return adapter_.Get(); }
    LUID adapterLuid() const { return luid_; }
    bool tearingSupported() const { return tearing_; }

    ComPtr<ID3D11ComputeShader> CompileCS(const char* src, const char* entry, const char* name,
                                          const D3D_SHADER_MACRO* defines = nullptr);
    ComPtr<ID3D11VertexShader> CompileVS(const char* src, const char* entry, const char* name);
    ComPtr<ID3D11PixelShader> CompilePS(const char* src, const char* entry, const char* name,
                                        const D3D_SHADER_MACRO* defines = nullptr);
    ComPtr<ID3D11Buffer> CreateConstantBuffer(size_t bytes);
    void UpdateCB(ID3D11Buffer* cb, const void* data, size_t bytes);

    // Unordered-access + SRV-capable 2D texture.
    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        UINT w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        explicit operator bool() const { return tex != nullptr; }
    };
    Tex CreateTex(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool rtv, UINT miscFlags = 0);

    ID3D11SamplerState* linearClamp() const { return linearClamp_.Get(); }
    ID3D11SamplerState* pointClamp() const { return pointClamp_.Get(); }

    // Unbinds CS resources (to avoid SRV/UAV hazards between passes).
    void ClearCS();

private:
    ComPtr<ID3D11Device5> dev_;
    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D11SamplerState> linearClamp_, pointClamp_;
    LUID luid_{};
    bool tearing_ = false;
};

// GPU timing via D3D11 timestamp queries. Each frame: Begin(), Mark("x")..., End().
// Results become available a few frames later; Collect() pushes them into callback.
class GpuProfiler {
public:
    static constexpr int kFrames = 6;
    static constexpr int kMarks = 16;
    void Init(ID3D11Device* dev);
    void Begin(ID3D11DeviceContext* ctx);
    void Mark(ID3D11DeviceContext* ctx, const char* name); // time from previous mark
    void End(ID3D11DeviceContext* ctx);
    // Calls fn(name, ms) for each resolved mark and fn("gpu_total", ms).
    template <typename F> void Collect(ID3D11DeviceContext* ctx, F&& fn) {
        for (int i = 0; i < kFrames; i++) {
            auto& f = frames_[(cur_ + 1 + i) % kFrames];
            if (!f.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            f.pending = false;
            if (dj.Disjoint || f.count < 1) continue;
            UINT64 prev = 0, first = 0; bool ok = true;
            std::array<UINT64, kMarks> ts{};
            for (int m = 0; m < f.count; m++) {
                if (ctx->GetData(f.ts[m].Get(), &ts[m], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ok = false; break; }
            }
            if (!ok) continue;
            first = prev = ts[0];
            for (int m = 1; m < f.count; m++) {
                fn(f.names[m], (double)(ts[m] - prev) * 1000.0 / (double)dj.Frequency);
                prev = ts[m];
            }
            fn("gpu_total", (double)(prev - first) * 1000.0 / (double)dj.Frequency);
        }
    }
private:
    struct Frame {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>, kMarks> ts;
        std::array<const char*, kMarks> names{};
        int count = 0;
        bool pending = false;
    };
    std::array<Frame, kFrames> frames_;
    int cur_ = 0;
    bool active_ = false;
};

} // namespace camss
