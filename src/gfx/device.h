#pragma once
#include "../util/common.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <span>

namespace cameravfx {

class Device {
public:
    void Create(bool debug);
    ID3D11Device5* D3dDevice() const { return dev_.Get(); }
    ID3D11DeviceContext4* Context() const { return ctx_.Get(); }
    IDXGIFactory6* Factory() const { return factory_.Get(); }
    IDXGIAdapter4* Adapter() const { return adapter_.Get(); }
    bool TearingSupported() const { return tearing_; }

    ComPtr<ID3D11ComputeShader> CompileCS(const char* src, const char* entry, const char* name);
    ComPtr<ID3D11PixelShader> CompilePS(const char* src, const char* entry, const char* name,
                                        const D3D_SHADER_MACRO* defines = nullptr);
    ComPtr<ID3D11Buffer> CreateConstantBuffer(size_t bytes);
    void UpdateCB(ID3D11Buffer* cb, const void* data, size_t bytes);

    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        UINT w = 0, h = 0;
    };
    Tex CreateTex(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, bool rtv);

    void DrawFullscreen(ID3D11PixelShader* ps, ID3D11Buffer* constants, std::span<ID3D11ShaderResourceView* const> inputs,
                        ID3D11RenderTargetView* target, UINT width, UINT height);
    void ClearCS();
    void WaitIdle();

private:
    ComPtr<ID3D11Device5> dev_;
    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D11SamplerState> linearClamp_;
    ComPtr<ID3D11VertexShader> fullscreenVs_;
    ComPtr<ID3D11Query> idleQuery_;
    bool tearing_ = false;
};

}
