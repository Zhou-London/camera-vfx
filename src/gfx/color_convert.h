#pragma once
#include "device.h"
#include "../capture/video_format.h"

namespace cameravfx {

struct ColorSpec {
    ColorMatrix matrix = ColorMatrix::Bt709;
    bool fullRange = false;
};

ColorSpec ResolveColor(const ColorOptions& chosen, const ColorOptions& tagged, PixFmt format);

class ColorConverter {
public:
    void Init(Device& dev, PixFmt format, UINT width, UINT height);
    void SetColor(const ColorSpec& color);
    const ColorSpec& Color() const { return color_; }
    void Convert(ID3D11ShaderResourceView* plane0, ID3D11ShaderResourceView* plane1, const Device::Tex& dst);

private:
    Device* dev_ = nullptr;
    UINT width_ = 0, height_ = 0;
    ColorSpec color_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
};

}
