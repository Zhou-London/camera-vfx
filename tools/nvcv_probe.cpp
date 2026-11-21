// Probe which (pixelFormat, componentType) pairs the VFX 1.3 NVCVImage accepts, and which TrueHDR takes as output.
#include <windows.h>
#include <cstdio>
#include <string>
#include "nvVideoEffects.h"
#include "nvCVImage.h"
char* g_nvVFXSDKPath = nullptr;
int main(int argc, char** argv) {
    std::wstring dir = L"" CAMSS_VFX_DIR;
    SetDllDirectoryW(dir.c_str());
    LoadLibraryExW((dir + L"\NVCVImage.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    LoadLibraryExW((dir + L"\NVVideoEffects.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    static std::string dirA = "C:/Users/Zhou/repositories/camss/third_party/nvvfx_runtime"; g_nvVFXSDKPath = dirA.data();
    NvVFX_Handle hdr = nullptr;
    NvCV_Status s = NvVFX_CreateEffect("TrueHDR", &hdr);
    printf("create TrueHDR: %d\n", (int)s);
    CUstream st = nullptr; NvVFX_CudaStreamCreate(&st);
    NvVFX_SetCudaStream(hdr, NVVFX_CUDA_STREAM, st);
    NvCVImage in; NvCVImage_Alloc(&in, 256, 144, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32);
    for (int pf = 0; pf <= 24; pf++) for (int ct = 0; ct <= 16; ct++) {
        NvCVImage im;
        s = NvCVImage_Alloc(&im, 256, 144, (NvCVImage_PixelFormat)pf, (NvCVImage_ComponentType)ct, NVCV_CHUNKY, NVCV_GPU, 32);
        if (s != NVCV_SUCCESS) continue;
        int okOut = -1;
        if (im.pixelBytes == 4 && im.numComponents <= 4) {
            NvVFX_SetImage(hdr, NVVFX_INPUT_IMAGE, &in);
            okOut = (int)NvVFX_SetImage(hdr, NVVFX_OUTPUT_IMAGE, &im);
            if (okOut == 0) okOut = (int)NvVFX_Load(hdr);
        }
        printf("pf=%2d ct=%2d ok  pixelBytes=%d comps=%d compBytes=%d  truehdr-out=%d\n", pf, ct, im.pixelBytes, im.numComponents, im.componentBytes, okOut);
    }
    return 0;
}
