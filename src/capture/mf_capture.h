#pragma once
#include "../util/common.h"
#include "../gfx/device.h"
#include "frame_diff.h"
#include "video_format.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cameravfx {

struct CaptureRequest {
    DeviceSelector device;
    CaptureMode mode;
};

struct VideoDevice {
    std::wstring name, symbolicLink;
};

std::vector<VideoDevice> VideoDevices();

struct CaptureInfo {
    std::string deviceName;
    std::wstring symbolicLink;
    CaptureMode mode;
    int stride = 0;
    std::vector<CaptureMode> modes;
    ColorOptions tagged;
};

struct CaptureAnalysis {
    bool decodeTimecode = false;
    std::optional<RepeatConfig> repeat;
};

struct RawFrame {
    uint64_t seq = 0;
    int slot = 0;
    int64_t sampleQpc = 0, arrivalQpc = 0, uploadedQpc = 0;
    int64_t sourceIndex = 0;
    std::optional<uint32_t> timecode;
    bool repeat = false;
    float repeatCheckMs = 0;
};

class MfCapture {
public:
    static void ListDevices();

    MfCapture();
    ~MfCapture();
    MfCapture(const MfCapture&) = delete;
    MfCapture& operator=(const MfCapture&) = delete;

    bool Open(Device& dev, const CaptureRequest& req);
    void Start(const CaptureAnalysis& analysis);
    const CaptureInfo& Info() const { return info_; }

    std::vector<RawFrame> Drain();
    HANDLE FrameEvent() const { return frameEvent_; }
    bool LockSlot(const RawFrame& f);
    void UnlockSlot(const RawFrame& f);
    ID3D11ShaderResourceView* PlaneSrv(int slot, int plane) const { return slots_[slot].planes[plane].srv.Get(); }
    int PlaneCount() const { return (int)planes_.size(); }
    uint64_t DroppedFrames() const { return dropped_.load(); }

private:
    class ReaderCallback;
    static constexpr int kSlots = 6;
    struct PlaneLayout {
        DXGI_FORMAT format;
        UINT texelsPerRow, rows, bytesPerTexel;
        UINT RowBytes() const { return texelsPerRow * bytesPerTexel; }
    };
    struct PlaneTexture {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    struct Slot {
        std::array<PlaneTexture, 2> planes;
        std::mutex lock;
        uint64_t seq = 0;
    };

    bool CreateSlots();
    void RequestNext();
    void OnSample(HRESULT status, DWORD flags, LONGLONG timestamp, IMFSample* sample);
    void Publish(IMFSample* sample, LONGLONG timestamp, int64_t arrival);
    int64_t NextSourceIndex(LONGLONG timestamp, int64_t arrival);
    bool Upload(IMFSample* sample, Slot& slot, RawFrame& f);
    bool CopyPlanes(const BYTE* top, LONG pitch, Slot& slot);
    void Analyse(const BYTE* top, LONG pitch, RawFrame& f);

    Device* dev_ = nullptr;
    ComPtr<ReaderCallback> callback_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    CaptureInfo info_;
    CaptureAnalysis analysis_;
    std::vector<PlaneLayout> planes_;
    std::array<Slot, kSlots> slots_;
    std::mutex pendingMutex_;
    std::vector<RawFrame> pending_;
    HANDLE frameEvent_ = nullptr;
    RepeatDetector repeat_;
    uint64_t seq_ = 0;
    int64_t lastIndex_ = -1;
    int64_t lastSampleTime_ = -1;
    int64_t lastArrival_ = 0;
    int64_t lastGapWarnQpc_ = 0;
    double periodHns_ = 0;
    std::atomic<uint64_t> dropped_{0};
};

}
