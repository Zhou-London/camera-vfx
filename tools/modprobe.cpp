#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
static void dump(const char* when) {
    HMODULE mods[2048]; DWORD need = 0;
    EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need);
    int n = 0;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        wchar_t m[MAX_PATH]; GetModuleFileNameW(mods[i], m, MAX_PATH);
        std::wstring w = m;
        if (w.find(L"NVCVImage") != std::wstring::npos || w.find(L"Video Effects") != std::wstring::npos) { wprintf(L"  [%hs] %s\n", when, m); n++; }
    }
    printf("%s: %d matching modules (of %lu)\n", when, n, need / (DWORD)sizeof(HMODULE));
}
int main() {
    dump("start");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    dump("after MFStartup");
    ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c);
    dump("after D3D11 device (no video)");
    ID3D11Device* d2 = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c);
    dump("after D3D11 device (video)");
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"mp";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"mp", L"mp", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    dump("after window");
    IDXGIFactory2* f = nullptr; CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 640; sd.Height = 360; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = f->CreateSwapChainForHwnd(d2, h, &sd, nullptr, nullptr, &sc);
    dump("after swapchain");
    for (int i = 0; i < 30; i++) { MSG m; while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m); sc->Present(1, 0); }
    dump("after presents");
    Sleep(1000);
    dump("after 1s");
    return 0;
}
