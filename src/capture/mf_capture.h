// Media Foundation UVC capture: async source reader, low-latency mode, native
// uncompressed formats only (converters disabled). Each sample is copied once
// (CPU memcpy) into a D3D11 dynamic texture slot; the render thread converts it.
#pragma once
#include "../util/common.h"
#include "../gfx/device.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <array>
#include <functional>

namespace camss {

enum class PixFmt { Unknown, YUY2, NV12, P010, RGB24, RGB32 };
const char* PixFmtName(PixFmt f);

struct CaptureRequest {
    std::wstring deviceSubstring;  // match friendly name (empty = first device)
    int deviceIndex = -1;          // or explicit index
    PixFmt format = PixFmt::YUY2;  // preferred; falls back through the list
    UINT width = 1920, height = 1080;
    double fps = 60.0;
};

struct CaptureInfo {
    std::string deviceName;
    PixFmt format = PixFmt::Unknown;
    UINT width = 0, height = 0;
    double fps = 0;
    int stride = 0;   // bytes; negative = bottom-up
};

// One uploaded frame, ready for colour conversion.
struct RawFrame {
    uint64_t seq = 0;            // capture sequence number (1-based)
    int slot = -1;
    int64_t sampleTimeQpc = 0;   // MF sample timestamp converted to QPC ticks (device/driver capture time)
    int64_t arrivalQpc = 0;      // OnReadSample entry
    int64_t uploadedQpc = 0;     // after memcpy into GPU texture
    int64_t sampleDurQpc = 0;
    int64_t sourceIndex = 0;     // position on the source timeline (gaps = dropped frames)
    bool timecodeValid = false;  // loopback timecode decoded from the frame's top band
    uint32_t timecode = 0;
};

class MfCapture : public IMFSourceReaderCallback {
public:
    static void ListDevices();

    MfCapture();
    bool Open(Device& dev, const CaptureRequest& req);
    void Start();
    void Stop();
    const CaptureInfo& info() const { return info_; }

    // Latest uploaded frame (seq==0 if none yet).
    RawFrame Latest() const;
    // All frames uploaded since the previous call (oldest first).
    std::vector<RawFrame> Drain();
    // Auto-reset event signalled after each upload.
    HANDLE FrameEvent() const { return frameEvent_; }
    // Lock a slot while the render thread converts it (prevents the capture
    // thread from overwriting it mid-dispatch). Returns false if overwritten.
    bool LockSlot(const RawFrame& f);
    void UnlockSlot(const RawFrame& f);
    // Plane SRVs for a slot.
    ID3D11ShaderResourceView* PlaneSrv(int slot, int plane) const { return slots_[slot].planes[plane].srv.Get(); }
    int PlaneCount() const { return planeCount_; }
    uint64_t DroppedFrames() const { return dropped_.load(); }
    uint64_t Errors() const { return errors_.load(); }
    // Decode the loopback timecode band (HDMI out -> capture in) from each frame.
    bool decodeTimecode = false;
    // Optional hook called on the capture thread after each upload.
    std::function<void(const RawFrame&)> onFrame;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = --ref_; return r; } // owned by app, not deleted via COM
    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD streamIndex, DWORD flags, LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    ~MfCapture();

private:
    static constexpr int kSlots = 6;
    struct Plane { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; UINT rowBytes = 0, rows = 0; };
    struct Slot {
        std::array<Plane, 2> planes;
        std::mutex lock;
        uint64_t seq = 0;
    };
    bool CreateSlots();
    void Upload(IMFSample* sample, Slot& s, RawFrame& f);
    void DecodeTimecode(const BYTE* top, LONG pitch, RawFrame& f) const;
    void RequestNext();

    Device* dev_ = nullptr;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> source_;
    CaptureInfo info_;
    int planeCount_ = 1;
    std::array<Slot, kSlots> slots_;
    mutable std::mutex latestMutex_;
    RawFrame latest_;
    std::vector<RawFrame> pending_;
    HANDLE frameEvent_ = nullptr;
    int64_t firstSampleTime_ = -1;
    int64_t lastIndex_ = -1;
    uint64_t seq_ = 0;
    int64_t lastSampleTime_ = -1;
    int64_t lastArrival_ = 0;
    std::atomic<ULONG> ref_{1};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0}, errors_{0};
    std::vector<uint8_t> flipBuf_;
};

} // namespace camss
