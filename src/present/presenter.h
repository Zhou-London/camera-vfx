// Flip-model swap chain presenter: waitable object, max frame latency 1,
// tearing (for VRR) when supported, SDR (BGRA8) or HDR10 (R10G10B10A2 / PQ / BT.2020).
#pragma once
#include "../gfx/device.h"

namespace camss {

struct PresenterOptions {
    bool fullscreen = true;     // borderless window covering the target monitor
    int monitorIndex = 0;       // DXGI output index on our adapter
    UINT windowW = 1920, windowH = 1080;
    bool hdr10 = false;
    bool allowTearing = true;   // needed for VRR / immediate present
    UINT bufferCount = 2;
};

class Presenter {
public:
    bool Init(Device& dev, const PresenterOptions& opt, HINSTANCE inst);
    void Shutdown();
    // Pumps window messages; returns false when the window was closed / ESC.
    bool PumpMessages();
    // Blocks until the swap chain can accept a new frame (or timeout ms).
    bool WaitReady(DWORD timeoutMs = 100);
    // syncInterval 0 + tearing = immediate (VRR); 1 = next vblank.
    HRESULT Present(UINT syncInterval, bool tearing);
    ID3D11RenderTargetView* BackbufferRtv();
    ID3D11Texture2D* Backbuffer();
    UINT width() const { return w_; }
    UINT height() const { return h_; }
    bool hdr() const { return hdr_; }
    bool tearingEnabled() const { return tearing_; }
    IDXGISwapChain4* swapchain() const { return sc_.Get(); }
    IDXGIOutput6* output() const { return output_.Get(); }
    // Refresh rate reported by the current display mode (Hz).
    double refreshHz() const { return refreshHz_; }
    // Last-present statistics (DXGI); returns false if unavailable.
    bool FrameStats(DXGI_FRAME_STATISTICS& st);
    UINT LastPresentCount();
    HWND hwnd() const { return hwnd_; }
    void SetTitle(const char* s);
    // Pending keyboard input (latest key pressed), 0 if none.
    int PopKey() { int k = key_; key_ = 0; return k; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Resize();
    Device* dev_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain4> sc_;
    ComPtr<IDXGIOutput6> output_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> bb_;
    HANDLE waitable_ = nullptr;
    UINT w_ = 0, h_ = 0;
    UINT flags_ = 0;
    bool hdr_ = false, tearing_ = false, quit_ = false, resized_ = false;
    double refreshHz_ = 60.0;
    int key_ = 0;
};

} // namespace camss
