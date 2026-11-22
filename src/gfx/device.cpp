#include "device.h"
#include <d3dcompiler.h>
#include <stdexcept>

namespace cameravfx {

namespace {

constexpr double kIdleTimeoutMs = 2000.0;
constexpr UINT kFullscreenTriangleVertices = 3;
constexpr UINT kClearedComputeSlots = 8;

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

ComPtr<ID3DBlob> Compile(const char* src, const char* entry, const char* target, const char* name, const D3D_SHADER_MACRO* defines) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(src, strlen(src), name, defines, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        LOGE("shader %s:%s compile failed:\n%s", name, entry, errors ? (const char*)errors->GetBufferPointer() : "?");
        ThrowHr(hr, "D3DCompile", __FILE__, __LINE__);
    }
    return code;
}

ComPtr<IDXGIAdapter4> HighPerformanceAdapter(IDXGIFactory6* factory) {
    ComPtr<IDXGIAdapter4> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC3 desc{};
        adapter->GetDesc3(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
        LOGI("GPU: %s (VRAM %llu MB)", WideToUtf8(desc.Description).c_str(), (unsigned long long)(desc.DedicatedVideoMemory >> 20));
        return adapter;
    }
    throw std::runtime_error("no hardware GPU adapter");
}

}

void Device::Create(bool debug) {
    HR_CHECK(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)));
    adapter_ = HighPerformanceAdapter(factory_.Get());

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1};
    auto create = [&](ComPtr<ID3D11Device>& d, ComPtr<ID3D11DeviceContext>& c) {
        return D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, _countof(levels), D3D11_SDK_VERSION, &d, nullptr, &c);
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = create(device, context);
    if (FAILED(hr) && debug) {
        LOGW("debug layer unavailable, creating without it");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = create(device, context);
    }
    HR_CHECK(hr);
    HR_CHECK(device.As(&dev_));
    const D3D11_QUERY_DESC idleQueryDesc{D3D11_QUERY_EVENT, 0};
    HR_CHECK(dev_->CreateQuery(&idleQueryDesc, &idleQuery_));
    HR_CHECK(context.As(&ctx_));
    ComPtr<ID3D11Multithread> multithread;
    HR_CHECK(ctx_.As(&multithread));
    multithread->SetMultithreadProtected(TRUE);

    BOOL allowTearing = FALSE;
    tearing_ = SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))) &&
               allowTearing;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    HR_CHECK(dev_->CreateSamplerState(&sd, &linearClamp_));

    const ComPtr<ID3DBlob> vs = Compile(kFullscreenVS, "main", "vs_5_0", "fullscreen_vs", nullptr);
    HR_CHECK(dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &fullscreenVs_));
}

ComPtr<ID3D11ComputeShader> Device::CompileCS(const char* src, const char* entry, const char* name) {
    const ComPtr<ID3DBlob> code = Compile(src, entry, "cs_5_0", name, nullptr);
    ComPtr<ID3D11ComputeShader> cs;
    HR_CHECK(dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
    return cs;
}

ComPtr<ID3D11PixelShader> Device::CompilePS(const char* src, const char* entry, const char* name, const D3D_SHADER_MACRO* defines) {
    const ComPtr<ID3DBlob> code = Compile(src, entry, "ps_5_0", name, defines);
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

Device::Tex Device::CreateTex(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool rtv) {
    Tex t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    HR_CHECK(dev_->CreateTexture2D(&td, nullptr, &t.tex));
    HR_CHECK(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv));
    if (uav) HR_CHECK(dev_->CreateUnorderedAccessView(t.tex.Get(), nullptr, &t.uav));
    if (rtv) HR_CHECK(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv));
    t.w = w; t.h = h;
    return t;
}

void Device::DrawFullscreen(ID3D11PixelShader* ps, ID3D11Buffer* constants, std::span<ID3D11ShaderResourceView* const> inputs,
                            ID3D11RenderTargetView* target, UINT width, UINT height) {
    const D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
    ctx_->RSSetViewports(1, &viewport);
    ctx_->OMSetRenderTargets(1, &target, nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(fullscreenVs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps, nullptr, 0);
    ctx_->PSSetShaderResources(0, (UINT)inputs.size(), inputs.data());
    ctx_->PSSetConstantBuffers(0, 1, &constants);
    ctx_->PSSetSamplers(0, 1, linearClamp_.GetAddressOf());
    ctx_->Draw(kFullscreenTriangleVertices, 0);
    ID3D11ShaderResourceView* const unbound[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ctx_->PSSetShaderResources(0, (UINT)inputs.size(), unbound);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

void Device::ClearCS() {
    ID3D11ShaderResourceView* const unboundSrvs[kClearedComputeSlots] = {};
    ID3D11UnorderedAccessView* const unboundUavs[kClearedComputeSlots] = {};
    ctx_->CSSetShaderResources(0, kClearedComputeSlots, unboundSrvs);
    ctx_->CSSetUnorderedAccessViews(0, kClearedComputeSlots, unboundUavs, nullptr);
}

void Device::WaitIdle() {
    ctx_->End(idleQuery_.Get());
    BOOL done = FALSE;
    const int64_t deadline = Qpc::Now() + Qpc::FromMs(kIdleTimeoutMs);
    while ((ctx_->GetData(idleQuery_.Get(), &done, sizeof(done), 0) != S_OK || !done) && Qpc::Now() < deadline) Sleep(0);
}

}
