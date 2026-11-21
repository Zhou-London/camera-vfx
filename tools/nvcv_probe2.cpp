#include <windows.h>
#include <cstdio>
#include <string>
#include "nvVideoEffects.h"
#include "nvCVImage.h"
char* g_nvVFXSDKPath = nullptr;
int main() {
    std::wstring dir = L"" CAMSS_VFX_DIR;
    static std::string dirA = "C:/Users/Zhou/repositories/camss/third_party/nvvfx_runtime"; g_nvVFXSDKPath = dirA.data();
    SetDllDirectoryW(dir.c_str());
    LoadLibraryExW((dir + L"\NVCVImage.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    LoadLibraryExW((dir + L"\NVVideoEffects.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    unsigned v = 0; NvVFX_GetVersion(&v); printf("ver %x\n", v);
    int dims[][2] = {{256,144},{1920,1080},{3840,2160}};
    for (auto& d : dims) for (unsigned layout : {0u, 1u}) for (unsigned mem : {1u}) for (unsigned al : {0u, 1u, 32u}) {
        NvCVImage im;
        NvCV_Status s = NvCVImage_Alloc(&im, d[0], d[1], (NvCVImage_PixelFormat)13, (NvCVImage_ComponentType)11, layout, mem, al);
        printf("%dx%d layout=%u mem=%u align=%u -> %d\n", d[0], d[1], layout, mem, al, (int)s);
    }
    NvCVImage a; NvCV_Status s1 = NvCVImage_Alloc(&a, 1920, 1080, NVCV_RGBA, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32);
    NvCVImage b; NvCV_Status s2 = NvCVImage_Alloc(&b, 1920, 1080, (NvCVImage_PixelFormat)13, (NvCVImage_ComponentType)11, NVCV_CHUNKY, NVCV_GPU, 32);
    printf("after rgba alloc: %d %d\n", (int)s1, (int)s2);
    NvCVImage c; NvCVImage_Init(&c, 0, 0, 0, nullptr, (NvCVImage_PixelFormat)0, (NvCVImage_ComponentType)0, 0, 0);
    NvCV_Status s3 = NvCVImage_Alloc(&c, 1920, 1080, (NvCVImage_PixelFormat)13, (NvCVImage_ComponentType)11, NVCV_CHUNKY, NVCV_GPU, 32);
    printf("explicit-init then alloc: %d\n", (int)s3);
    return 0;
}
