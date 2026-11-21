#include "device.h"
#include <d3dcompiler.h>

namespace camss {

bool Device::Create(bool debug) {
    HR_CHECK(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)));
    // Pick the high-performance (discrete NVIDIA) adapter.
    ComPtr<IDXGIAdapter4> ad;
    for (UINT i = 0; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC3 d{};
        ad->GetDesc3(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) { ad.Reset(); continue; }
        LOGI("GPU: %s (VRAM %llu MB)", WideToUtf8(d.Description).c_str(), (unsigned long long)(d.DedicatedVideoMemory >> 20));
        luid_ = d.AdapterLuid;
        break;
    }
    if (!ad) { LOGE("no hardware adapter"); return false; }
    adapter_ = ad;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
    ComPtr<ID3D11Device> d0;
    ComPtr<ID3D11DeviceContext> c0;
    HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, fls, _countof(fls), D3D11_SDK_VERSION, &d0, nullptr, &c0);
    if (FAILED(hr) && debug) {
        LOGW("debug layer unavailable, creating without it");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, fls, _countof(fls), D3D11_SDK_VERSION, &d0, nullptr, &c0);
    }
    HR_CHECK(hr);
    HR_CHECK(d0.As(&dev_));
    HR_CHECK(c0.As(&ctx_));
    // The capture thread maps upload textures while the render thread draws.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        tearing_ = allowTearing != FALSE;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    HR_CHECK(dev_->CreateSamplerState(&sd, &linearClamp_));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    HR_CHECK(dev_->CreateSamplerState(&sd, &pointClamp_));
    return true;
}

static ComPtr<ID3DBlob> Compile(const char* src, const char* entry, const char* target, const char* name, const D3D_SHADER_MACRO* defines) {
    ComPtr<ID3DBlob> code, err;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(src, strlen(src), name, defines, nullptr, entry, target, flags, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("shader %s:%s compile failed:\n%s", name, entry, err ? (const char*)err->GetBufferPointer() : "?");
        ThrowHr(hr, "D3DCompile", __FILE__, __LINE__);
    }
    return code;
}

ComPtr<ID3D11ComputeShader> Device::CompileCS(const char* src, const char* entry, const char* name, const D3D_SHADER_MACRO* defines) {
    auto code = Compile(src, entry, "cs_5_0", name, defines);
    ComPtr<ID3D11ComputeShader> cs;
    HR_CHECK(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
    return cs;
}
ComPtr<ID3D11VertexShader> Device::CompileVS(const char* src, const char* entry, const char* name) {
    auto code = Compile(src, entry, "vs_5_0", name, nullptr);
    ComPtr<ID3D11VertexShader> vs;
    HR_CHECK(dev_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    return vs;
}
ComPtr<ID3D11PixelShader> Device::CompilePS(const char* src, const char* entry, const char* name, const D3D_SHADER_MACRO* defines) {
    auto code = Compile(src, entry, "ps_5_0", name, defines);
    ComPtr<ID3D11PixelShader> ps;
    HR_CHECK(dev_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
    return ps;
}

ComPtr<ID3D11Buffer> Device::CreateConstantBuffer(size_t bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)((bytes + 15) & ~size_t(15));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> b;
    HR_CHECK(dev_->CreateBuffer(&bd, nullptr, &b));
    return b;
}

void Device::UpdateCB(ID3D11Buffer* cb, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    HR_CHECK(ctx_->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    memcpy(m.pData, data, bytes);
    ctx_->Unmap(cb, 0);
}

Device::Tex Device::CreateTex(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool rtv, UINT miscFlags) {
    Tex t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = miscFlags;
    HR_CHECK(dev_->CreateTexture2D(&td, nullptr, &t.tex));
    HR_CHECK(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv));
    if (uav) HR_CHECK(dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav));
    if (rtv) HR_CHECK(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv));
    t.w = w; t.h = h; t.fmt = fmt;
    return t;
}

void Device::ClearCS() {
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ID3D11UnorderedAccessView* nullUav[8] = {};
    ctx_->CSSetShaderResources(0, 8, nullSrv);
    ctx_->CSSetUnorderedAccessViews(0, 8, nullUav, nullptr);
}

// ------------------------------------------------------------------ GpuProfiler
void GpuProfiler::Init(ID3D11Device* dev) {
    for (auto& f : frames_) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HR_CHECK(dev->CreateQuery(&qd, &f.disjoint));
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& q : f.ts) HR_CHECK(dev->CreateQuery(&qd, &q));
    }
}
void GpuProfiler::Begin(ID3D11DeviceContext* ctx) {
    cur_ = (cur_ + 1) % kFrames;
    auto& f = frames_[cur_];
    if (f.pending) { active_ = false; return; } // still in flight; skip this frame
    active_ = true;
    f.count = 0;
    ctx->Begin(f.disjoint.Get());
    Mark(ctx, "begin");
}
void GpuProfiler::Mark(ID3D11DeviceContext* ctx, const char* name) {
    if (!active_) return;
    auto& f = frames_[cur_];
    if (f.count >= kMarks) return;
    f.names[f.count] = name;
    ctx->End(f.ts[f.count].Get());
    f.count++;
}
void GpuProfiler::End(ID3D11DeviceContext* ctx) {
    if (!active_) return;
    auto& f = frames_[cur_];
    ctx->End(f.disjoint.Get());
    f.pending = true;
    active_ = false;
}

} // namespace camss
