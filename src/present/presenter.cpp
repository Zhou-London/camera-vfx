#include "presenter.h"

namespace cameravfx {

namespace {
constexpr DXGI_COLOR_SPACE_TYPE kHdr10ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
constexpr const wchar_t* kWindowName = L"CameraVFX";
constexpr LONG kWindowedWidth = 1920, kWindowedHeight = 1080, kWindowedOffset = 64;
constexpr UINT kBufferCount = 2;
constexpr UINT kMaxFrameLatency = 1;
constexpr DWORD kWaitReadyTimeoutMs = 100;
}

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
    RECT desktop{};
    if (!PickOutput(opt.monitorIndex, opt.hdr, desktop) || !OpenWindow(inst, opt.windowed, desktop)) return false;
    CreateSwapChain();
    return true;
}

bool Presenter::PickOutput(int monitorIndex, bool wantHdr, RECT& desktop) {
    ComPtr<IDXGIOutput> out;
    if (FAILED(dev_->Adapter()->EnumOutputs(monitorIndex, &out))) {
        LOGW("display output %d not found on the adapter; using output 0", monitorIndex);
        if (FAILED(dev_->Adapter()->EnumOutputs(0, &out))) {
            LOGE("no display output on the adapter");
            return false;
        }
    }
    HR_CHECK(out.As(&output_));
    DXGI_OUTPUT_DESC1 od{};
    output_->GetDesc1(&od);
    desktop = od.DesktopCoordinates;
    const bool displayHdr = od.ColorSpace == kHdr10ColorSpace;
    hdr_ = wantHdr && displayHdr;
    if (wantHdr && !displayHdr)
        LOGW("HDR output requested but Windows HDR is OFF on this display: using an SDR swap chain and tone-mapping the HDR10 result for preview. Turn on 'Use HDR' in Windows display settings for real HDR.");
    LOGI("display: %s %ldx%ld, max luminance %.0f nits, HDR %s", WideToUtf8(od.DeviceName).c_str(), desktop.right - desktop.left,
         desktop.bottom - desktop.top, od.MaxLuminance, displayHdr ? "on" : "off");
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) refreshHz_ = dm.dmDisplayFrequency;
    return true;
}

bool Presenter::OpenWindow(HINSTANCE inst, bool windowed, const RECT& desktop) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowName;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    const DWORD style = windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT wr = desktop;
    if (windowed) {
        wr = {0, 0, kWindowedWidth, kWindowedHeight};
        AdjustWindowRect(&wr, style, FALSE);
        OffsetRect(&wr, desktop.left + kWindowedOffset - wr.left, desktop.top + kWindowedOffset - wr.top);
    }
    hwnd_ = CreateWindowExW(0, kWindowName, kWindowName, style, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, nullptr,
                            nullptr, inst, nullptr);
    if (!hwnd_) {
        LOGE("CreateWindow failed");
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    ShowWindow(hwnd_, SW_SHOW);
    RECT cr;
    GetClientRect(hwnd_, &cr);
    w_ = cr.right - cr.left;
    h_ = cr.bottom - cr.top;
    return true;
}

void Presenter::CreateSwapChain() {
    Device& dev = *dev_;
    tearing_ = dev.TearingSupported();
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w_;
    sd.Height = h_;
    sd.Format = hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = kBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    flags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    sd.Flags = flags_;
    ComPtr<IDXGISwapChain1> sc1;
    HR_CHECK(dev.Factory()->CreateSwapChainForHwnd(dev.D3dDevice(), hwnd_, &sd, nullptr, nullptr, &sc1));
    HR_CHECK(sc1.As(&sc_));
    dev.Factory()->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    HR_CHECK(sc_->SetMaximumFrameLatency(kMaxFrameLatency));
    waitable_ = sc_->GetFrameLatencyWaitableObject();
    if (hdr_) HR_CHECK(sc_->SetColorSpace1(kHdr10ColorSpace));
    LOGI("swapchain %ux%u %s, flip-discard, max latency %u, tearing %s, refresh %.2f Hz", w_, h_, hdr_ ? "HDR10" : "SDR", kMaxFrameLatency,
         tearing_ ? "on" : "off", refreshHz_);
}

void Presenter::Shutdown() {
    rtv_.Reset();
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
    if (std::exchange(resized_, false)) Resize();
    return !quit_;
}

void Presenter::Resize() {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    const UINT w = std::max<LONG>(1, cr.right - cr.left), h = std::max<LONG>(1, cr.bottom - cr.top);
    if (w == w_ && h == h_) return;
    rtv_.Reset();
    dev_->Context()->ClearState();
    dev_->Context()->Flush();
    HR_CHECK(sc_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags_));
    w_ = w;
    h_ = h;
}

bool Presenter::WaitReady() {
    return WaitForSingleObjectEx(waitable_, kWaitReadyTimeoutMs, TRUE) == WAIT_OBJECT_0;
}

ID3D11RenderTargetView* Presenter::BackbufferRtv() {
    if (!rtv_) {
        ComPtr<ID3D11Texture2D> currentBackbuffer;
        HR_CHECK(sc_->GetBuffer(0, IID_PPV_ARGS(&currentBackbuffer)));
        HR_CHECK(dev_->D3dDevice()->CreateRenderTargetView(currentBackbuffer.Get(), nullptr, &rtv_));
    }
    return rtv_.Get();
}

HRESULT Presenter::Present(UINT syncInterval) {
    const UINT flags = syncInterval == 0 && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT hr = sc_->Present(syncInterval, flags);
    if (FAILED(hr)) LOGE("Present failed: %s", HrToString(hr).c_str());
    return hr;
}

bool Presenter::FrameStats(DXGI_FRAME_STATISTICS& st) {
    return SUCCEEDED(sc_->GetFrameStatistics(&st));
}

int Presenter::PresentationMode() {
    ComPtr<IDXGISwapChainMedia> media;
    if (FAILED(sc_.As(&media))) return -1;
    DXGI_FRAME_STATISTICS_MEDIA st{};
    if (FAILED(media->GetFrameStatisticsMedia(&st))) return -1;
    return (int)st.CompositionMode;
}

UINT Presenter::LastPresentCount() const {
    UINT count = 0;
    sc_->GetLastPresentCount(&count);
    return count;
}

void Presenter::SetTitle(const char* title) { SetWindowTextW(hwnd_, Utf8ToWide(title).c_str()); }

}
