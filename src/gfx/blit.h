#pragma once
#include "device.h"
#include "encoding.h"
#include <optional>

namespace cameravfx {

struct BlitParams {
    Encoding inEnc = Encoding::SdrGamma;
    Encoding outEnc = Encoding::SdrGamma;
    float sdrWhiteNits = 0;
    bool timecode = false;
    uint32_t timecodeValue = 0;
    bool marker = false;
    bool markerOn = false;
    bool loading = false;
    float loadingDim = 0;
    std::optional<float> loadingProgress;
    float loadingTime = 0.f;
};

class Blitter {
public:
    void Init(Device& dev);
    void Draw(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
              const BlitParams& p);

private:
    Device* dev_ = nullptr;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
};

}
