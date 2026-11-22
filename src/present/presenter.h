#pragma once
#include "../gfx/device.h"
#include "../gfx/encoding.h"
#include <utility>

namespace cameravfx {

struct PresenterOptions {
    bool windowed = false;
    int monitorIndex = 0;
    bool hdr = false;
};

class Presenter {
public:
    bool Init(Device& dev, const PresenterOptions& opt, HINSTANCE inst);
    void Shutdown();
    bool PumpMessages();
    bool WaitReady();
    HRESULT Present(UINT syncInterval);
    ID3D11RenderTargetView* BackbufferRtv();
    UINT Width() const { return w_; }
    UINT Height() const { return h_; }
    Encoding OutputEncoding() const { return hdr_ ? Encoding::Pq : Encoding::SdrGamma; }
    bool TearingEnabled() const { return tearing_; }
    IDXGIOutput6* Output() const { return output_.Get(); }
    double RefreshHz() const { return refreshHz_; }
    bool FrameStats(DXGI_FRAME_STATISTICS& st);
    int PresentationMode();
    UINT LastPresentCount() const;
    void SetTitle(const char* title);
    int PopKey() { return std::exchange(key_, 0); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool PickOutput(int monitorIndex, bool wantHdr, RECT& desktop);
    bool OpenWindow(HINSTANCE inst, bool windowed, const RECT& desktop);
    void CreateSwapChain();
    void Resize();
    Device* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain4> sc_;
    ComPtr<IDXGIOutput6> output_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    UINT w_ = 0, h_ = 0;
    UINT flags_ = 0;
    bool hdr_ = false, tearing_ = false, quit_ = false, resized_ = false;
    double refreshHz_ = 60.0;
    int key_ = 0;
};

}
