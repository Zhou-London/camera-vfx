#pragma once
#include "../gfx/device.h"

namespace cameravfx {

class SyntheticSource {
public:
    void Init(Device& dev);
    void Render(const Device::Tex& dst, double timeSec);

private:
    Device* dev_ = nullptr;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
};

}
