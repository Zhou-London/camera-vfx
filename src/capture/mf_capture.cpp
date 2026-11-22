#include "mf_capture.h"
#include "timecode.h"
#include <initguid.h>
#include <mfcaptureengine.h>
#include <algorithm>
#include <cmath>
#include <shared_mutex>

namespace cameravfx {

namespace {

constexpr DWORD kVideoStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
constexpr double kHnsPerMs = kHnsPerSecond / 1000.0;
constexpr double kRequestRateToleranceHz = 0.5;
constexpr double kPeriodTrackTolerance = 0.1;
constexpr double kPeriodTrackGain = 0.02;
constexpr double kGapWarnIntervalMs = 500;
constexpr size_t kMaxPendingFrames = 64;

bool Succeeded(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    LOGE("capture: %s failed: %s", what, HrToString(hr).c_str());
    return false;
}

std::optional<PixFmt> FromSubtype(const GUID& g) {
    if (g == MFVideoFormat_YUY2) return PixFmt::YUY2;
    if (g == MFVideoFormat_NV12) return PixFmt::NV12;
    if (g == MFVideoFormat_P010) return PixFmt::P010;
    if (g == MFVideoFormat_RGB24) return PixFmt::RGB24;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return PixFmt::RGB32;
    return std::nullopt;
}

bool MatchesRequest(const CaptureMode& offered, const CaptureMode& wanted) {
    return offered.format == wanted.format && SameSize(offered, wanted) && std::abs(offered.rateHz - wanted.rateHz) < kRequestRateToleranceHz;
}

struct NativeType {
    ComPtr<IMFMediaType> type;
    CaptureMode mode;
    int stride;
};

std::vector<NativeType> NativeTypes(IMFSourceReader* reader) {
    std::vector<NativeType> types;
    for (DWORD k = 0;; k++) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(kVideoStream, k, &type))) break;
        GUID subtype{};
        UINT32 w = 0, h = 0, fpsNum = 0, fpsDen = 0, stride = 0;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        const std::optional<PixFmt> format = FromSubtype(subtype);
        if (!format || !fpsDen || FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) continue;
        const CaptureMode mode{*format, w, h, (double)fpsNum / fpsDen};
        if (std::none_of(types.begin(), types.end(), [&](const NativeType& t) { return SameMode(t.mode, mode); }))
            types.push_back({type, mode, (int)(INT32)stride});
    }
    return types;
}

ColorOptions ReadColorTags(IMFMediaType* type) {
    ColorOptions tags;
    UINT32 v = 0;
    if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &v))) {
        if (v == MFVideoTransferMatrix_BT709) tags.matrix = ColorMatrix::Bt709;
        else if (v == MFVideoTransferMatrix_BT601) tags.matrix = ColorMatrix::Bt601;
        else if (v == MFVideoTransferMatrix_BT2020_10 || v == MFVideoTransferMatrix_BT2020_12) tags.matrix = ColorMatrix::Bt2020;
    }
    if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v))) {
        if (v == MFNominalRange_0_255) tags.fullRange = true;
        else if (v == MFNominalRange_16_235) tags.fullRange = false;
    }
    return tags;
}

class DeviceList {
public:
    DeviceList() {
        ComPtr<IMFAttributes> filter;
        MFCreateAttributes(&filter, 1);
        filter->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        MFEnumDeviceSources(filter.Get(), &devices_, &count_);
    }
    ~DeviceList() {
        for (UINT32 i = 0; i < count_; i++) devices_[i]->Release();
        CoTaskMemFree(devices_);
    }
    DeviceList(const DeviceList&) = delete;
    DeviceList& operator=(const DeviceList&) = delete;

    UINT32 Count() const { return count_; }
    IMFActivate* operator[](UINT32 i) const { return devices_[i]; }
    std::wstring Attribute(UINT32 i, const GUID& key) const {
        WCHAR* value = nullptr;
        UINT32 length = 0;
        devices_[i]->GetAllocatedString(key, &value, &length);
        std::wstring s = value ? value : L"";
        CoTaskMemFree(value);
        return s;
    }
    std::wstring Name(UINT32 i) const { return Attribute(i, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME); }
    std::optional<UINT32> Find(const DeviceSelector& device) const {
        if (const int* index = std::get_if<int>(&device)) return (UINT32)*index < count_ ? std::optional<UINT32>(*index) : std::nullopt;
        if (const std::wstring* namePart = std::get_if<std::wstring>(&device)) {
            for (UINT32 i = 0; i < count_; i++)
                if (ContainsIgnoreCase(Name(i), *namePart)) return i;
            return std::nullopt;
        }
        return count_ ? std::optional<UINT32>(0) : std::nullopt;
    }

private:
    IMFActivate** devices_ = nullptr;
    UINT32 count_ = 0;
};

}

class MfCapture::ReaderCallback final : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(MfCapture* owner) : owner_(owner) {}

    void Detach() {
        std::unique_lock lock(ownerMutex_);
        owner_ = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSourceReaderCallback)) {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) override {
        std::shared_lock lock(ownerMutex_);
        if (owner_) owner_->OnSample(status, flags, timestamp, sample);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    std::shared_mutex ownerMutex_;
    MfCapture* owner_;
    std::atomic<ULONG> refs_{1};
};

std::vector<VideoDevice> VideoDevices() {
    DeviceList devices;
    std::vector<VideoDevice> out;
    for (UINT32 i = 0; i < devices.Count(); i++)
        out.push_back({devices.Name(i), devices.Attribute(i, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)});
    return out;
}

void MfCapture::ListDevices() {
    DeviceList devices;
    for (UINT32 i = 0; i < devices.Count(); i++) {
        LOGI("capture device [%u] %s", i, WideToUtf8(devices.Name(i).c_str()).c_str());
        ComPtr<IMFMediaSource> source;
        if (FAILED(devices[i]->ActivateObject(IID_PPV_ARGS(&source)))) continue;
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 1);
        attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), attributes.Get(), &reader)))
            for (const NativeType& t : NativeTypes(reader.Get()))
                LOGI("    %-5s %ux%u @ %.3f", PixFmtName(t.mode.format), t.mode.width, t.mode.height, t.mode.rateHz);
        source->Shutdown();
    }
}

MfCapture::MfCapture() : frameEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

MfCapture::~MfCapture() {
    if (callback_) callback_->Detach();
    reader_.Reset();
    if (source_) source_->Shutdown();
    CloseHandle(frameEvent_);
}

bool MfCapture::Open(Device& dev, const CaptureRequest& req) {
    dev_ = &dev;
    DeviceList devices;
    const std::optional<UINT32> index = devices.Find(req.device);
    if (!index) { LOGE("capture device not found (%u device(s))", devices.Count()); return false; }
    info_.deviceName = WideToUtf8(devices.Name(*index).c_str());
    info_.symbolicLink = devices.Attribute(*index, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    if (!Succeeded(devices[*index]->ActivateObject(IID_PPV_ARGS(&source_)), "activate device")) return false;

    callback_.Attach(new ReaderCallback(this));
    ComPtr<IMFAttributes> attributes;
    if (!Succeeded(MFCreateAttributes(&attributes, 4), "reader attributes") ||
        !Succeeded(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get()), "reader callback") ||
        !Succeeded(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "low latency") ||
        !Succeeded(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE), "disable converters") ||
        !Succeeded(attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE), "disconnect on shutdown") ||
        !Succeeded(MFCreateSourceReaderFromMediaSource(source_.Get(), attributes.Get(), &reader_), "create source reader"))
        return false;

    const std::vector<NativeType> types = NativeTypes(reader_.Get());
    for (const NativeType& t : types) info_.modes.push_back(t.mode);
    const PixFmt formatOrder[] = {req.mode.format, PixFmt::YUY2, PixFmt::NV12, PixFmt::RGB32, PixFmt::P010, PixFmt::RGB24};
    const NativeType* chosen = nullptr;
    for (PixFmt format : formatOrder) {
        const CaptureMode wanted{format, req.mode.width, req.mode.height, req.mode.rateHz};
        const auto it = std::find_if(types.begin(), types.end(), [&](const NativeType& t) { return MatchesRequest(t.mode, wanted); });
        if (it != types.end()) { chosen = &*it; break; }
    }
    if (!chosen) { LOGE("no native media type %ux%u@%.2f found", req.mode.width, req.mode.height, req.mode.rateHz); return false; }
    info_.mode = chosen->mode;
    info_.stride = chosen->stride;
    info_.tagged = ReadColorTags(chosen->type.Get());
    if (!Succeeded(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE), "deselect streams") ||
        !Succeeded(reader_->SetStreamSelection(kVideoStream, TRUE), "select video stream") ||
        !Succeeded(reader_->SetCurrentMediaType(kVideoStream, nullptr, chosen->type.Get()), "set media type"))
        return false;
    LOGI("capture: '%s' %s %ux%u @ %.3f fps (default stride %d)", info_.deviceName.c_str(), PixFmtName(info_.mode.format),
         info_.mode.width, info_.mode.height, info_.mode.rateHz, info_.stride);
    periodHns_ = kHnsPerSecond / info_.mode.rateHz;
    return CreateSlots();
}

bool MfCapture::CreateSlots() {
    const UINT w = info_.mode.width, h = info_.mode.height;
    switch (info_.mode.format) {
    case PixFmt::YUY2: planes_ = {{DXGI_FORMAT_R8G8B8A8_UNORM, w / 2, h, 4}}; break;
    case PixFmt::NV12: planes_ = {{DXGI_FORMAT_R8_UNORM, w, h, 1}, {DXGI_FORMAT_R8G8_UNORM, w / 2, h / 2, 2}}; break;
    case PixFmt::P010: planes_ = {{DXGI_FORMAT_R16_UNORM, w, h, 2}, {DXGI_FORMAT_R16G16_UNORM, w / 2, h / 2, 4}}; break;
    case PixFmt::RGB24: planes_ = {{DXGI_FORMAT_R8_UNORM, w * 3, h, 1}}; break;
    case PixFmt::RGB32: planes_ = {{DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 4}}; break;
    }
    for (Slot& s : slots_) {
        for (size_t p = 0; p < planes_.size(); p++) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = planes_[p].texelsPerRow; td.Height = planes_[p].rows; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = planes_[p].format; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (!Succeeded(dev_->D3dDevice()->CreateTexture2D(&td, nullptr, &s.planes[p].tex), "upload texture") ||
                !Succeeded(dev_->D3dDevice()->CreateShaderResourceView(s.planes[p].tex.Get(), nullptr, &s.planes[p].srv), "upload view"))
                return false;
        }
    }
    return true;
}

void MfCapture::Start(const CaptureAnalysis& analysis) {
    analysis_ = analysis;
    if (analysis_.repeat) {
        const DiffLayout layout = info_.mode.format == PixFmt::P010 ? DiffLayout::HighBytes16 : DiffLayout::Bytes8;
        repeat_.Configure(layout, (int)planes_[0].RowBytes(), (int)planes_[0].rows, *analysis_.repeat);
    }
    RequestNext();
}

void MfCapture::RequestNext() {
    const HRESULT hr = reader_->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) LOGE("capture stopped: ReadSample failed: %s", HrToString(hr).c_str());
}

void MfCapture::OnSample(HRESULT status, DWORD flags, LONGLONG timestamp, IMFSample* sample) {
    const int64_t arrival = Qpc::Now();
    if (FAILED(status)) { LOGE("capture stopped: %s", HrToString(status).c_str()); return; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { LOGW("capture stopped: end of stream"); return; }
    if (flags & MF_SOURCE_READERF_STREAMTICK) repeat_.Reset();
    if (sample) Publish(sample, timestamp, arrival);
    RequestNext();
}

int64_t MfCapture::NextSourceIndex(LONGLONG timestamp, int64_t arrival) {
    int64_t steps = 1;
    if (lastSampleTime_ >= 0) {
        const double gap = (double)(timestamp - lastSampleTime_);
        steps = std::max<int64_t>(1, std::llround(gap / periodHns_));
        if (steps == 1 && std::abs(gap - periodHns_) < kPeriodTrackTolerance * periodHns_) periodHns_ += kPeriodTrackGain * (gap - periodHns_);
        if (steps > 1) {
            repeat_.Reset();
            const uint64_t lost = (uint64_t)(steps - 1);
            dropped_ += lost;
            if (arrival - lastGapWarnQpc_ > Qpc::FromMs(kGapWarnIntervalMs)) {
                LOGW("capture gap: %.1f ms (%llu frame(s) lost), callback-to-callback %.1f ms", gap / kHnsPerMs, (unsigned long long)lost,
                     Qpc::ToMs(arrival - lastArrival_));
                lastGapWarnQpc_ = arrival;
            }
        }
    }
    lastSampleTime_ = timestamp;
    lastArrival_ = arrival;
    return lastIndex_ += steps;
}

void MfCapture::Publish(IMFSample* sample, LONGLONG timestamp, int64_t arrival) {
    RawFrame f;
    f.seq = ++seq_;
    f.slot = (int)(f.seq % kSlots);
    f.arrivalQpc = arrival;
    f.sourceIndex = NextSourceIndex(timestamp, arrival);
    UINT64 deviceTime = 0;
    const bool haveDeviceTime = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceReferenceSystemTime, &deviceTime)) && deviceTime;
    f.sampleQpc = Qpc::FromHns(haveDeviceTime ? (int64_t)deviceTime : timestamp);

    Slot& slot = slots_[f.slot];
    {
        std::lock_guard lock(slot.lock);
        if (!Upload(sample, slot, f)) {
            slot.seq = 0;
            repeat_.Reset();
            LOG_EVERY_MS(LogLevel::Warn, 1000, "capture: frame upload failed");
            return;
        }
        slot.seq = f.seq;
    }
    f.uploadedQpc = Qpc::Now();
    {
        std::lock_guard lock(pendingMutex_);
        if (pending_.size() == kMaxPendingFrames) pending_.erase(pending_.begin());
        pending_.push_back(f);
    }
    SetEvent(frameEvent_);
}

bool MfCapture::Upload(IMFSample* sample, Slot& slot, RawFrame& f) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
    ComPtr<IMF2DBuffer> buffer2d;
    const bool is2d = SUCCEEDED(buffer.As(&buffer2d));
    BYTE* top = nullptr;
    LONG pitch = info_.stride;
    if (FAILED(is2d ? buffer2d->Lock2D(&top, &pitch) : buffer->Lock(&top, nullptr, nullptr))) return false;
    if (!is2d && pitch < 0) top += (size_t)(-pitch) * (info_.mode.height - 1);
    const bool copied = CopyPlanes(top, pitch, slot);
    if (copied) Analyse(top, pitch, f);
    if (is2d) buffer2d->Unlock2D(); else buffer->Unlock();
    return copied;
}

bool MfCapture::CopyPlanes(const BYTE* top, LONG pitch, Slot& slot) {
    const BYTE* src = top;
    for (size_t p = 0; p < planes_.size(); p++) {
        const PlaneLayout& layout = planes_[p];
        ID3D11Texture2D* tex = slot.planes[p].tex.Get();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(dev_->Context()->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        BYTE* dst = (BYTE*)mapped.pData;
        const UINT rowBytes = layout.RowBytes();
        if ((LONG)mapped.RowPitch == pitch && pitch == (LONG)rowBytes)
            memcpy(dst, src, (size_t)rowBytes * layout.rows);
        else
            for (UINT y = 0; y < layout.rows; y++) memcpy(dst + (size_t)y * mapped.RowPitch, src + (ptrdiff_t)y * pitch, rowBytes);
        dev_->Context()->Unmap(tex, 0);
        src += (ptrdiff_t)pitch * layout.rows;
    }
    return true;
}

void MfCapture::Analyse(const BYTE* top, LONG pitch, RawFrame& f) {
    if (analysis_.decodeTimecode)
        f.timecode = DecodeTimecode(top, pitch, info_.mode.format, (int)info_.mode.width, (int)info_.mode.height);
    if (analysis_.repeat) {
        const RepeatCheck check = repeat_.Check(top, pitch);
        f.repeat = check.repeat;
        f.repeatCheckMs = (float)check.costMs;
    }
}

std::vector<RawFrame> MfCapture::Drain() {
    std::lock_guard lock(pendingMutex_);
    std::vector<RawFrame> frames;
    frames.swap(pending_);
    return frames;
}

bool MfCapture::LockSlot(const RawFrame& f) {
    Slot& slot = slots_[f.slot];
    slot.lock.lock();
    if (slot.seq == f.seq) return true;
    slot.lock.unlock();
    return false;
}

void MfCapture::UnlockSlot(const RawFrame& f) { slots_[f.slot].lock.unlock(); }

}
