// D3D11 video processor path with NVIDIA driver extensions:
//   * RTX Video Super Resolution (VSR)  — upscales e.g. 1080p -> 4K
//   * RTX Video HDR ("TrueHDR")         — SDR -> HDR10 (PQ / BT.2020)
// Both are enabled through ID3D11VideoContext::VideoProcessorSetStreamExtension
// with NVIDIA GUIDs, exactly as mpv / VLC / Chromium do. No SDK needed: the
// driver implements them on the tensor cores. Falls back to a plain VP scale.
#pragma once
#include "../gfx/device.h"
#include <unordered_map>

namespace camss {

struct RtxVideoOptions {
    bool vsr = true;
    bool trueHdr = false;
};

class RtxVideo {
public:
    // inFmt: format of the source textures (B8G8R8A8_UNORM). outFmt: BGRA8 (SDR) or R10G10B10A2 (HDR10).
    bool Init(Device& dev, UINT inW, UINT inH, DXGI_FORMAT inFmt, UINT outW, UINT outH, DXGI_FORMAT outFmt,
              const RtxVideoOptions& opt);
    // Process `in` (full frame) into `out` (any texture with RENDER_TARGET bind, outW x outH).
    bool Process(ID3D11Texture2D* in, ID3D11Texture2D* out);
    bool vsrActive() const { return vsrOk_; }
    // Follow a runtime SR toggle (re-sends the stream extension only on change).
    void SetVsr(bool on);
    bool hdrActive() const { return hdrOk_; }
    // Drop cached input/output views (their textures are about to be released, e.g. a VFX chain rebuild).
    void ClearViewCache() { inViews_.clear(); outViews_.clear(); }
private:
    ID3D11VideoProcessorInputView* InView(ID3D11Texture2D* t);
    ID3D11VideoProcessorOutputView* OutView(ID3D11Texture2D* t);
    Device* dev_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext1> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inViews_;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outViews_;
    UINT inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
    bool vsrOk_ = false, hdrOk_ = false, vsrWanted_ = false, vsrOn_ = false;
};

} // namespace camss
