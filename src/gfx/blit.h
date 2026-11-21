// Final composition into the swap chain back buffer (aspect-fit scale, SDR->HDR10
// encode when needed, loopback timecode bar), plus the synthetic test source.
#pragma once
#include "device.h"

namespace camss {

enum class Encoding { SdrGamma, Pq };   // what the texture contains

struct BlitParams {
    Encoding inEnc = Encoding::SdrGamma;
    Encoding outEnc = Encoding::SdrGamma;
    float sdrWhiteNits = 203.f;         // paper white when mapping SDR into PQ
    bool timecode = false;
    uint32_t timecodeValue = 0;
    bool marker = false;                // flashing corner square (photodiode test)
    bool markerOn = false;
};

class Blitter {
public:
    bool Init(Device& dev);
    void Draw(ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, ID3D11RenderTargetView* dst, UINT dstW, UINT dstH,
              const BlitParams& p);
private:
    Device* dev_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
};

// Synthetic moving test pattern (for running without a live source).
class SyntheticSource {
public:
    bool Init(Device& dev, UINT w, UINT h);
    void Render(const Device::Tex& dst, double timeSec);
private:
    Device* dev_ = nullptr;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
};

// Timecode bar layout shared by the renderer and the capture-side decoder.
constexpr int kTimecodeBits = 32;
constexpr int kTimecodeBlocks = kTimecodeBits + 2;   // start marker (white) + 32 bits + end marker (black)
constexpr float kTimecodeBandFrac = 1.0f / 24.0f;    // band height as fraction of output height

} // namespace camss
