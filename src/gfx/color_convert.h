// YUV/RGB capture planes -> RGBA (B8G8R8A8_UNORM, BT.709 gamma-encoded) at source resolution.
#pragma once
#include "device.h"
#include "../capture/mf_capture.h"

namespace camss {

struct ColorOptions {
    bool fullRange = false;   // video (16-235) vs full (0-255) range
    bool bt601 = false;       // matrix: BT.709 default
};

class ColorConverter {
public:
    bool Init(Device& dev, PixFmt fmt, UINT w, UINT h, const ColorOptions& opt);
    // Converts planes -> dst (must be w*h, with RTV).
    void Convert(ID3D11ShaderResourceView* plane0, ID3D11ShaderResourceView* plane1, const Device::Tex& dst);
private:
    Device* dev_ = nullptr;
    PixFmt fmt_ = PixFmt::Unknown;
    UINT w_ = 0, h_ = 0;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
};

// Shared fullscreen-triangle vertex shader source (SV_VertexID based).
extern const char* kFullscreenVS;

} // namespace camss
