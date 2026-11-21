#include "presenter.h"

namespace camss {

LRESULT CALLBACK Presenter::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (Presenter*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CLOSE: if (self) self->quit_ = true; return 0;
    case WM_KEYDOWN:
        if (self) {
            if (wp == VK_ESCAPE) self->quit_ = true;
            else self->key_ = (int)wp;
        }
        return 0;
    case WM_SIZE: if (self) self->resized_ = true; return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; } break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Presenter::Init(Device& dev, const PresenterOptions& opt, HINSTANCE inst) {
    dev_ = &dev;
    // Target output (monitor) on our adapter.
    ComPtr<IDXGIOutput> out;
    if (FAILED(dev.adapter()->EnumOutputs(opt.monitorIndex, &out)) && FAILED(dev.adapter()->EnumOutputs(0, &out))) {
        LOGE("no display output on adapter");
        return false;
    }
    HR_CHECK(out.As(&output_));
    DXGI_OUTPUT_DESC1 od{};
    output_->GetDesc1(&od);
    RECT r = od.DesktopCoordinates;
    hdr_ = opt.hdr10;
    if (hdr_ && od.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
        LOGW("HDR output requested but Windows HDR is OFF on this display: using an SDR swap chain and tone-mapping the HDR10 result for preview. Turn on 'Use HDR' in Windows display settings for real HDR.");
        hdr_ = false;
    }
    LOGI("display: %s %ldx%ld, max luminance %.0f nits, HDR %s", WideToUtf8(od.DeviceName).c_str(), r.right - r.left, r.bottom - r.top,
         od.MaxLuminance, od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "on" : "off");

    // Current refresh rate from GDI display settings.
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) refreshHz_ = dm.dmDisplayFrequency;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"camss_window";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    DWORD style = opt.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    if (!opt.fullscreen) {
        RECT wr{0, 0, (LONG)opt.windowW, (LONG)opt.windowH};
        AdjustWindowRect(&wr, style, FALSE);
        w = wr.right - wr.left; h = wr.bottom - wr.top;
        x = r.left + 64; y = r.top + 64;
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"camss", style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) { LOGE("CreateWindow failed"); return false; }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    ShowWindow(hwnd_, SW_SHOW);
    RECT cr; GetClientRect(hwnd_, &cr);
    w_ = cr.right - cr.left; h_ = cr.bottom - cr.top;

    tearing_ = opt.allowTearing && dev.tearingSupported();
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w_; sd.Height = h_;
    sd.Format = hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = std::max(2u, opt.bufferCount);
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    sd.Flags = flags_;
    ComPtr<IDXGISwapChain1> sc1;
    HR_CHECK(dev.factory()->CreateSwapChainForHwnd(dev.dev(), hwnd_, &sd, nullptr, nullptr, &sc1));
    HR_CHECK(sc1.As(&sc_));
    dev.factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    HR_CHECK(sc_->SetMaximumFrameLatency(1));
    waitable_ = sc_->GetFrameLatencyWaitableObject();
    if (hdr_) {
        UINT support = 0;
        sc_->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
        if (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
            HR_CHECK(sc_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
        else LOGW("swap chain does not support HDR10 colour space");
    }
    LOGI("swapchain %ux%u %s, flip-discard, max latency 1, tearing %s, refresh %.2f Hz", w_, h_, hdr_ ? "HDR10" : "SDR",
         tearing_ ? "on" : "off", refreshHz_);
    return true;
}

void Presenter::Shutdown() {
    rtv_.Reset(); bb_.Reset();
    if (waitable_) { CloseHandle(waitable_); waitable_ = nullptr; }
    sc_.Reset();
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool Presenter::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (resized_) { resized_ = false; Resize(); }
    return !quit_;
}

void Presenter::Resize() {
    RECT cr; GetClientRect(hwnd_, &cr);
    UINT w = std::max<LONG>(1, cr.right - cr.left), h = std::max<LONG>(1, cr.bottom - cr.top);
    if (w == w_ && h == h_) return;
    rtv_.Reset(); bb_.Reset();
    dev_->ctx()->ClearState();
    dev_->ctx()->Flush();
    HR_CHECK(sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags_));
    w_ = w; h_ = h;
}

bool Presenter::WaitReady(DWORD timeoutMs) {
    return WaitForSingleObjectEx(waitable_, timeoutMs, TRUE) == WAIT_OBJECT_0;
}

ID3D11Texture2D* Presenter::Backbuffer() {
    if (!bb_) HR_CHECK(sc_->GetBuffer(0, IID_PPV_ARGS(&bb_)));
    return bb_.Get();
}

ID3D11RenderTargetView* Presenter::BackbufferRtv() {
    // FLIP_DISCARD in D3D11: buffer 0 always refers to the current back buffer.
    if (!rtv_) HR_CHECK(dev_->dev()->CreateRenderTargetView(Backbuffer(), nullptr, &rtv_));
    return rtv_.Get();
}

HRESULT Presenter::Present(UINT syncInterval, bool tearing) {
    UINT flags = (tearing && tearing_ && syncInterval == 0) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = sc_->Present(syncInterval, flags);
    if (FAILED(hr)) LOGE("Present failed: %s", HrToString(hr).c_str());
    return hr;
}

bool Presenter::FrameStats(DXGI_FRAME_STATISTICS& st) {
    return SUCCEEDED(sc_->GetFrameStatistics(&st));
}

UINT Presenter::LastPresentCount() {
    UINT c = 0; sc_->GetLastPresentCount(&c); return c;
}

void Presenter::SetTitle(const char* s) { SetWindowTextW(hwnd_, Utf8ToWide(s).c_str()); }

} // namespace camss
