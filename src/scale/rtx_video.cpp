#include "rtx_video.h"

namespace camss {

// NVIDIA "PPE" (post-processing engine) stream extension: RTX Video Super Resolution.
static const GUID kNvidiaPPEInterfaceGUID = {0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
// NVIDIA TrueHDR stream extension: RTX Video HDR.
static const GUID kNvidiaTrueHDRInterfaceGUID = {0xfdd62bb4, 0x620b, 0x4fd7, {0x9a, 0xb3, 0x1e, 0x59, 0xd0, 0xd5, 0x44, 0xb3}};

bool RtxVideo::Init(Device& dev, UINT inW, UINT inH, DXGI_FORMAT inFmt, UINT outW, UINT outH, DXGI_FORMAT outFmt,
                    const RtxVideoOptions& opt) {
    dev_ = &dev;
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
    if (FAILED(dev.dev()->QueryInterface(IID_PPV_ARGS(&vdev_))) || FAILED(dev.ctx()->QueryInterface(IID_PPV_ARGS(&vctx_)))) {
        LOGE("video device interfaces unavailable");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW; cd.InputHeight = inH;
    cd.OutputWidth = outW; cd.OutputHeight = outH;
    cd.InputFrameRate = {60, 1}; cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HR_CHECK(vdev_->CreateVideoProcessorEnumerator(&cd, &enum_));
    UINT flags = 0;
    if (FAILED(enum_->CheckVideoProcessorFormat(inFmt, &flags)) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        LOGE("video processor: input format %d unsupported", (int)inFmt); return false;
    }
    if (FAILED(enum_->CheckVideoProcessorFormat(outFmt, &flags)) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LOGE("video processor: output format %d unsupported", (int)outFmt); return false;
    }
    HR_CHECK(vdev_->CreateVideoProcessor(enum_.Get(), 0, &vp_));

    const bool hdrOut = outFmt == DXGI_FORMAT_R10G10B10A2_UNORM || outFmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    vctx_->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vctx_->VideoProcessorSetOutputColorSpace1(vp_.Get(), hdrOut ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    RECT src{0, 0, (LONG)inW, (LONG)inH}, dst{0, 0, (LONG)outW, (LONG)outH};
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);

    if (opt.vsr) {
        struct { UINT version; UINT method; UINT enable; } ext = {0x1, 0x2, 1};
        HRESULT hr = vctx_->VideoProcessorSetStreamExtension(vp_.Get(), 0, &kNvidiaPPEInterfaceGUID, sizeof(ext), &ext);
        vsrOk_ = SUCCEEDED(hr);
        if (vsrOk_) LOGI("RTX Video Super Resolution: enabled (%ux%u -> %ux%u)", inW, inH, outW, outH);
        else LOGW("RTX VSR extension rejected: %s", HrToString(hr).c_str());
    }
    if (opt.trueHdr) {
        if (!hdrOut) LOGW("TrueHDR requested but output is SDR; ignoring");
        else {
            struct { UINT version; UINT method; UINT enable : 1; UINT reserved : 31; } ext = {0x4, 0x3, 1, 0};
            HRESULT hr = vctx_->VideoProcessorSetStreamExtension(vp_.Get(), 0, &kNvidiaTrueHDRInterfaceGUID, sizeof(ext), &ext);
            hdrOk_ = SUCCEEDED(hr);
            if (hdrOk_) LOGI("RTX Video HDR (TrueHDR): enabled");
            else LOGW("TrueHDR extension rejected: %s", HrToString(hr).c_str());
        }
    }
    return true;
}

ID3D11VideoProcessorInputView* RtxVideo::InView(ID3D11Texture2D* t) {
    auto it = inViews_.find(t);
    if (it != inViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> v;
    HR_CHECK(vdev_->CreateVideoProcessorInputView(t, enum_.Get(), &d, &v));
    inViews_[t] = v;
    return v.Get();
}

ID3D11VideoProcessorOutputView* RtxVideo::OutView(ID3D11Texture2D* t) {
    auto it = outViews_.find(t);
    if (it != outViews_.end()) return it->second.Get();
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> v;
    HR_CHECK(vdev_->CreateVideoProcessorOutputView(t, enum_.Get(), &d, &v));
    outViews_[t] = v;
    return v.Get();
}

bool RtxVideo::Process(ID3D11Texture2D* in, ID3D11Texture2D* out) {
    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE;
    s.pInputSurface = InView(in);
    HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), OutView(out), 0, 1, &s);
    if (FAILED(hr)) { LOGE("VideoProcessorBlt: %s", HrToString(hr).c_str()); return false; }
    return true;
}

} // namespace camss
