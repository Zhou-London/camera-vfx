#include "mf_capture.h"
#include <mferror.h>
#include <shlwapi.h>
#include <algorithm>
#include "../gfx/blit.h"
#include <initguid.h>
// From mfcaptureengine.h: QPC (100 ns) time at which the device captured the frame.
DEFINE_GUID(CAMSS_MFSampleExtension_DeviceReferenceSystemTime, 0x6523775a, 0xba2d, 0x405f, 0xb2, 0xc5, 0x01, 0xff, 0x88, 0xe2, 0xe8, 0xf6);

namespace camss {

const char* PixFmtName(PixFmt f) {
    switch (f) {
    case PixFmt::YUY2: return "YUY2";
    case PixFmt::NV12: return "NV12";
    case PixFmt::P010: return "P010";
    case PixFmt::RGB24: return "RGB24";
    case PixFmt::RGB32: return "RGB32";
    default: return "?";
    }
}

static PixFmt FromSubtype(const GUID& g) {
    if (g == MFVideoFormat_YUY2) return PixFmt::YUY2;
    if (g == MFVideoFormat_NV12) return PixFmt::NV12;
    if (g == MFVideoFormat_P010) return PixFmt::P010;
    if (g == MFVideoFormat_RGB24) return PixFmt::RGB24;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return PixFmt::RGB32;
    return PixFmt::Unknown;
}

struct DeviceList {
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    DeviceList() {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 1);
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        MFEnumDeviceSources(a.Get(), &devs, &count);
    }
    ~DeviceList() {
        for (UINT32 i = 0; i < count; i++) devs[i]->Release();
        CoTaskMemFree(devs);
    }
    std::wstring Name(UINT32 i) {
        WCHAR* n = nullptr; UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &n, &len);
        std::wstring s = n ? n : L"";
        CoTaskMemFree(n);
        return s;
    }
};

void MfCapture::ListDevices() {
    DeviceList dl;
    for (UINT32 i = 0; i < dl.count; i++) {
        LOGI("capture device [%u] %s", i, WideToUtf8(dl.Name(i).c_str()).c_str());
        ComPtr<IMFMediaSource> src;
        if (FAILED(dl.devs[i]->ActivateObject(IID_PPV_ARGS(&src)))) continue;
        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 1);
        ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        ComPtr<IMFSourceReader> rd;
        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src.Get(), ra.Get(), &rd))) {
            for (DWORD k = 0;; k++) {
                ComPtr<IMFMediaType> mt;
                if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, k, &mt))) break;
                GUID sub{}; mt->GetGUID(MF_MT_SUBTYPE, &sub);
                UINT32 w = 0, h = 0, fn = 0, fd = 1;
                MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &fn, &fd);
                PixFmt pf = FromSubtype(sub);
                if (pf == PixFmt::Unknown) continue;
                LOGI("    %-5s %ux%u @ %.3f", PixFmtName(pf), w, h, fd ? (double)fn / fd : 0.0);
            }
        }
        src->Shutdown();
    }
}

MfCapture::MfCapture() { frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
MfCapture::~MfCapture() { Stop(); if (frameEvent_) CloseHandle(frameEvent_); }

STDMETHODIMP MfCapture::QueryInterface(REFIID iid, void** ppv) {
    static const QITAB qit[] = {QITABENT(MfCapture, IMFSourceReaderCallback), {0}};
    return QISearch(this, qit, iid, ppv);
}

bool MfCapture::Open(Device& dev, const CaptureRequest& req) {
    dev_ = &dev;
    DeviceList dl;
    if (dl.count == 0) { LOGE("no video capture devices"); return false; }
    int idx = req.deviceIndex;
    if (idx < 0) {
        for (UINT32 i = 0; i < dl.count; i++) {
            std::wstring n = dl.Name(i);
            if (req.deviceSubstring.empty() || n.find(req.deviceSubstring) != std::wstring::npos) { idx = (int)i; break; }
        }
    }
    if (idx < 0 || idx >= (int)dl.count) { LOGE("capture device not found"); return false; }
    info_.deviceName = WideToUtf8(dl.Name(idx).c_str());
    HR_CHECK(dl.devs[idx]->ActivateObject(IID_PPV_ARGS(&source_)));

    ComPtr<IMFAttributes> ra;
    HR_CHECK(MFCreateAttributes(&ra, 4));
    HR_CHECK(ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this)));
    HR_CHECK(ra->SetUINT32(MF_LOW_LATENCY, TRUE));
    HR_CHECK(ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE)); // native uncompressed only
    HR_CHECK(ra->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE));
    HR_CHECK(MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(), &reader_));

    // Pick the native type: preferred format first, then any uncompressed format with matching size/rate.
    const PixFmt order[] = {req.format, PixFmt::YUY2, PixFmt::NV12, PixFmt::RGB32, PixFmt::P010, PixFmt::RGB24};
    ComPtr<IMFMediaType> chosen;
    for (PixFmt want : order) {
        for (DWORD k = 0; !chosen; k++) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(reader_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, k, &mt))) break;
            GUID sub{}; mt->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 w = 0, h = 0, fn = 0, fd = 1;
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &fn, &fd);
            double fps = fd ? (double)fn / fd : 0;
            if (FromSubtype(sub) == want && w == req.width && h == req.height && std::abs(fps - req.fps) < 0.5) {
                chosen = mt;
                info_.format = want; info_.width = w; info_.height = h; info_.fps = fps;
                UINT32 st = 0;
                if (SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) info_.stride = (int)st;
            }
        }
        if (chosen) break;
    }
    if (!chosen) { LOGE("no native media type %ux%u@%.2f found", req.width, req.height, req.fps); return false; }
    HR_CHECK(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE));
    HR_CHECK(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));
    HR_CHECK(reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen.Get()));
    LOGI("capture: '%s' %s %ux%u @ %.3f fps (default stride %d)", info_.deviceName.c_str(), PixFmtName(info_.format),
         info_.width, info_.height, info_.fps, info_.stride);
    return CreateSlots();
}

bool MfCapture::CreateSlots() {
    const UINT w = info_.width, h = info_.height;
    struct PlaneDesc { DXGI_FORMAT fmt; UINT w, h, bpp; };
    std::vector<PlaneDesc> pd;
    switch (info_.format) {
    case PixFmt::YUY2: pd = {{DXGI_FORMAT_R8G8B8A8_UNORM, w / 2, h, 4}}; break;  // texel = Y0 U Y1 V
    case PixFmt::NV12: pd = {{DXGI_FORMAT_R8_UNORM, w, h, 1}, {DXGI_FORMAT_R8G8_UNORM, w / 2, h / 2, 2}}; break;
    case PixFmt::P010: pd = {{DXGI_FORMAT_R16_UNORM, w, h, 2}, {DXGI_FORMAT_R16G16_UNORM, w / 2, h / 2, 4}}; break;
    case PixFmt::RGB24: pd = {{DXGI_FORMAT_R8_UNORM, w * 3, h, 1}}; break;           // raw bytes, decoded in shader
    case PixFmt::RGB32: pd = {{DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 4}}; break;
    default: return false;
    }
    planeCount_ = (int)pd.size();
    for (auto& s : slots_) {
        for (size_t p = 0; p < pd.size(); p++) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = pd[p].w; td.Height = pd[p].h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = pd[p].fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HR_CHECK(dev_->dev()->CreateTexture2D(&td, nullptr, &s.planes[p].tex));
            HR_CHECK(dev_->dev()->CreateShaderResourceView(s.planes[p].tex.Get(), nullptr, &s.planes[p].srv));
            s.planes[p].rowBytes = pd[p].w * pd[p].bpp;
            s.planes[p].rows = pd[p].h;
        }
    }
    return true;
}

void MfCapture::Start() {
    running_ = true;
    RequestNext();
}

void MfCapture::Stop() {
    if (!running_.exchange(false)) return;
    if (reader_) reader_->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    Sleep(50);
    reader_.Reset();
    if (source_) { source_->Shutdown(); source_.Reset(); }
}

void MfCapture::RequestNext() {
    if (!running_ || !reader_) return;
    HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) { errors_++; LOGE("ReadSample failed: %s", HrToString(hr).c_str()); }
}

STDMETHODIMP MfCapture::OnReadSample(HRESULT hrStatus, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) {
    const int64_t arrival = Qpc::Now();
    if (!running_) return S_OK;
    // Issue the next read immediately so the source never starves.
    RequestNext();
    if (FAILED(hrStatus)) { errors_++; LOGE("capture error: %s", HrToString(hrStatus).c_str()); return S_OK; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { LOGW("capture end of stream"); return S_OK; }
    if (!sample) return S_OK;

    LONGLONG dur = 0;
    sample->GetSampleDuration(&dur);
    if (dur <= 0) dur = (LONGLONG)(1e7 / std::max(1.0, info_.fps));
    if (lastSampleTime_ >= 0) {
        LONGLONG gap = timestamp - lastSampleTime_;
        if (gap > dur * 3 / 2) {
            uint64_t lost = (uint64_t)((gap + dur / 2) / dur - 1);
            dropped_ += lost;
            static int64_t lastWarn = 0;
            if (arrival - lastWarn > Qpc::FromMs(500)) {
                LOGW("capture gap: %.1f ms (%llu frame(s) lost), callback-to-callback %.1f ms", gap / 1e4, (unsigned long long)lost,
                     lastArrival_ ? Qpc::ToMs(arrival - lastArrival_) : 0.0);
                lastWarn = arrival;
            }
        }
    }
    lastSampleTime_ = timestamp;
    lastArrival_ = arrival;
    if (firstSampleTime_ < 0) firstSampleTime_ = timestamp;

    RawFrame f;
    f.seq = ++seq_;
    f.slot = (int)(f.seq % kSlots);
    f.arrivalQpc = arrival;
    f.sampleDurQpc = Qpc::FromHns(dur);
    {
        // Source timeline index from the nominal frame period; robust to dropped frames.
        const double nominal = 1e7 / std::max(1.0, info_.fps);
        f.sourceIndex = (int64_t)std::llround((double)(timestamp - firstSampleTime_) / nominal);
        if (f.sourceIndex <= lastIndex_) f.sourceIndex = lastIndex_ + 1;
        lastIndex_ = f.sourceIndex;
    }
    // Absolute device capture time (QPC in 100ns) when the driver provides it.
    UINT64 devRef = 0;
    if (SUCCEEDED(sample->GetUINT64(CAMSS_MFSampleExtension_DeviceReferenceSystemTime, &devRef)) && devRef)
        f.sampleTimeQpc = Qpc::FromHns((int64_t)devRef);
    else
        f.sampleTimeQpc = Qpc::FromHns(timestamp); // MF sample time is QPC-based for capture sources

    Slot& s = slots_[f.slot];
    {
        std::lock_guard<std::mutex> lk(s.lock);
        Upload(sample, s, f);
        s.seq = f.seq;
    }
    f.uploadedQpc = Qpc::Now();
    {
        std::lock_guard<std::mutex> lk(latestMutex_);
        latest_ = f;
        pending_.push_back(f);
        if (pending_.size() > 64) pending_.erase(pending_.begin());
    }
    SetEvent(frameEvent_);
    if (onFrame) onFrame(f);
    return S_OK;
}

void MfCapture::Upload(IMFSample* sample, Slot& s, RawFrame& f) {
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) { errors_++; return; }
    BYTE* base = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> b2d;
    DWORD curLen = 0;
    bool locked2d = false;
    if (SUCCEEDED(buf.As(&b2d)) && SUCCEEDED(b2d->Lock2D(&base, &pitch))) {
        locked2d = true;
    } else {
        DWORD maxLen = 0;
        if (FAILED(buf->Lock(&base, &maxLen, &curLen))) { errors_++; return; }
        pitch = info_.stride ? info_.stride : (LONG)s.planes[0].rowBytes;
        if (pitch < 0) base += (size_t)(-pitch) * (info_.height - 1); // bottom-up contiguous buffer
    }
    // `base` points at scanline 0 (top row); `pitch` may be negative for bottom-up RGB.
    const BYTE* src = base;
    if (decodeTimecode) DecodeTimecode(base, pitch, f);
    for (int p = 0; p < planeCount_; p++) {
        Plane& pl = s.planes[p];
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(dev_->ctx()->Map(pl.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { errors_++; break; }
        BYTE* dst = (BYTE*)m.pData;
        if ((LONG)m.RowPitch == pitch && pitch == (LONG)pl.rowBytes) {
            memcpy(dst, src, (size_t)pl.rowBytes * pl.rows);
        } else {
            for (UINT y = 0; y < pl.rows; y++) memcpy(dst + (size_t)y * m.RowPitch, src + (ptrdiff_t)y * pitch, pl.rowBytes);
        }
        dev_->ctx()->Unmap(pl.tex.Get(), 0);
        // NV12/P010 chroma plane follows the luma plane with the same pitch.
        src = src + (ptrdiff_t)pitch * pl.rows;
    }
    if (locked2d) b2d->Unlock2D(); else buf->Unlock();
}

void MfCapture::DecodeTimecode(const BYTE* top, LONG pitch, RawFrame& f) const {
    const int W = (int)info_.width, H = (int)info_.height;
    const int y = std::max(1, (int)(H * kTimecodeBandFrac * 0.5f));
    const BYTE* row = top + (ptrdiff_t)y * pitch;
    auto luma = [&](int x) -> int {
        switch (info_.format) {
        case PixFmt::YUY2: return row[x * 2];
        case PixFmt::NV12: return row[x];
        case PixFmt::P010: return row[x * 2 + 1];
        case PixFmt::RGB24: return row[x * 3 + 1];
        case PixFmt::RGB32: return row[x * 4 + 1];
        default: return 0;
        }
    };
    const float bw = (float)W / kTimecodeBlocks;
    int vals[kTimecodeBlocks];
    for (int b = 0; b < kTimecodeBlocks; b++) {
        int cx = (int)((b + 0.5f) * bw), r = std::max(1, (int)(bw * 0.25f)), sum = 0, n = 0;
        for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); x++) { sum += luma(x); n++; }
        vals[b] = n ? sum / n : 0;
    }
    const int thr = (vals[0] + vals[kTimecodeBlocks - 1]) / 2;
    if (vals[0] - vals[kTimecodeBlocks - 1] < 80) return;   // markers not present
    uint32_t v = 0;
    for (int b = 1; b <= kTimecodeBits; b++) {
        if (std::abs(vals[b] - thr) < 30) return;               // ambiguous (mid-transition / scaled)
        v = (v << 1) | (vals[b] > thr ? 1u : 0u);
    }
    f.timecode = v;
    f.timecodeValid = true;
}

RawFrame MfCapture::Latest() const {
    std::lock_guard<std::mutex> lk(latestMutex_);
    return latest_;
}

std::vector<RawFrame> MfCapture::Drain() {
    std::lock_guard<std::mutex> lk(latestMutex_);
    std::vector<RawFrame> out;
    out.swap(pending_);
    return out;
}

bool MfCapture::LockSlot(const RawFrame& f) {
    if (f.slot < 0) return false;
    slots_[f.slot].lock.lock();
    if (slots_[f.slot].seq != f.seq) { slots_[f.slot].lock.unlock(); return false; }
    return true;
}
void MfCapture::UnlockSlot(const RawFrame& f) { slots_[f.slot].lock.unlock(); }

} // namespace camss
