// mfenum: list Media Foundation video capture devices and their native media types.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstdio>
#include <string>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static std::string SubtypeName(const GUID& g) {
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_ARGB32) return "ARGB32";
    if (g == MFVideoFormat_RGB24) return "RGB24";
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_UYVY) return "UYVY";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MFVideoFormat_H264) return "H264";
    char buf[64];
    char fcc[5] = {(char)(g.Data1 & 0xff), (char)((g.Data1 >> 8) & 0xff), (char)((g.Data1 >> 16) & 0xff), (char)((g.Data1 >> 24) & 0xff), 0};
    snprintf(buf, sizeof(buf), "%08lX(%s)", g.Data1, fcc);
    return buf;
}

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    MFEnumDeviceSources(attr, &devs, &n);
    for (UINT32 i = 0; i < n; i++) {
        WCHAR* name = nullptr; UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        WCHAR* link = nullptr;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
        wprintf(L"[%u] %s\n    %s\n", i, name, link);
        IMFMediaSource* src = nullptr;
        if (SUCCEEDED(devs[i]->ActivateObject(IID_PPV_ARGS(&src)))) {
            IMFSourceReader* rd = nullptr;
            if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src, nullptr, &rd))) {
                for (DWORD k = 0;; k++) {
                    IMFMediaType* mt = nullptr;
                    if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, k, &mt))) break;
                    GUID sub{}; mt->GetGUID(MF_MT_SUBTYPE, &sub);
                    UINT32 w = 0, h = 0, fn = 0, fd = 0;
                    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
                    MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &fn, &fd);
                    INT32 stride = 0; mt->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride);
                    printf("    #%lu %-8s %ux%u @ %.3f fps  stride=%d\n", k, SubtypeName(sub).c_str(), w, h, fd ? (double)fn / fd : 0.0, stride);
                    mt->Release();
                }
                rd->Release();
            }
            src->Shutdown();
            src->Release();
        }
        CoTaskMemFree(name); CoTaskMemFree(link);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    MFShutdown();
    return 0;
}
