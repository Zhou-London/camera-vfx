#include "frame_source.h"
#include "timecode.h"
#include <cmath>

namespace cameravfx {

namespace {
constexpr double kMinPlausibleDeliveryMs = -5;
constexpr double kMaxPlausibleDeliveryMs = 500;
constexpr int32_t kMaxPlausibleLoopUnits = 100000;
constexpr double kContentIndexEpsilon = 1e-6;
}

std::unique_ptr<MfCapture> OpenCapture(Device& dev, const CaptureRequest& request) {
    auto capture = std::make_unique<MfCapture>();
    if (!capture->Open(dev, request)) return nullptr;
    return capture;
}

void FrameSource::Reset(const CaptureMode& mode) {
    if (!SameSize(mode, mode_))
        for (SourceFrame& f : store_) f.rgb = dev_.CreateTex(mode.width, mode.height, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    mode_ = mode;
    for (SourceFrame& f : store_) f.n = -1;
    nextSlot_ = 0;
    lastSeq_ = 0;
    contentLost_ = false;
    lastArrival_ = 0;
}

void FrameSource::UseSynthetic(const CaptureMode& mode, std::optional<double> contentFps) {
    synthetic_ = std::make_unique<SyntheticSource>();
    synthetic_->Init(dev_);
    Reset(mode);
    syntheticContentFps_ = contentFps.value_or(mode.rateHz);
}

void FrameSource::UseCapture(std::unique_ptr<MfCapture> capture, const ColorOptions& color) {
    capture_ = std::move(capture);
    Reset(capture_->Info().mode);
    converter_.Init(dev_, mode_.format, mode_.width, mode_.height);
    SetColor(color);
}

void FrameSource::SetColor(const ColorOptions& chosen) {
    const CaptureInfo& info = capture_->Info();
    converter_.SetColor(ResolveColor(chosen, info.tagged, info.mode.format));
}

void FrameSource::Start(const CaptureAnalysis& analysis) {
    detectRepeats_ = analysis.repeat.has_value();
    if (capture_) capture_->Start(analysis);
}

std::span<const SourceEvent> FrameSource::Ingest(int64_t now) {
    events_.clear();
    if (synthetic_) IngestSynthetic(now);
    else for (const RawFrame& f : capture_->Drain()) IngestCaptured(f);
    return events_;
}

void FrameSource::WaitFrame(DWORD maxMs) {
    if (!synthetic_) {
        WaitForSingleObject(capture_->FrameEvent(), maxMs);
        return;
    }
    const int64_t now = Qpc::Now();
    sleeper_.SleepUntil(syntheticNext_ ? std::min(syntheticNext_, now + Qpc::FromMs(maxMs)) : now);
}

SourceFrame* FrameSource::Frame(int64_t n) {
    if (n < 0) return nullptr;
    const auto it = std::find_if(store_.begin(), store_.end(), [&](const SourceFrame& f) { return f.n == n; });
    return it == store_.end() ? nullptr : &*it;
}

void FrameSource::Commit(SourceFrame& f, int64_t n, int64_t sampleQpc, int64_t arrivalQpc, int64_t uploadedQpc) {
    f.n = n;
    f.sampleQpc = sampleQpc;
    f.arrivalQpc = arrivalQpc;
    f.uploadedQpc = uploadedQpc;
    nextSlot_ = (nextSlot_ + 1) % store_.size();
    events_.push_back({n, uploadedQpc, &f});
}

void FrameSource::Repeat(int64_t n, int64_t readyQpc) {
    repeats_++;
    events_.push_back({n, readyQpc, nullptr});
}

void FrameSource::IngestCaptured(const RawFrame& f) {
    stats_.Add(stat::kCaptureUpload, Qpc::ToMs(f.uploadedQpc - f.arrivalQpc));
    const double deliveryMs = Qpc::ToMs(f.arrivalQpc - f.sampleQpc);
    if (deliveryMs > kMinPlausibleDeliveryMs && deliveryMs < kMaxPlausibleDeliveryMs) stats_.Add(stat::kCaptureDelivery, deliveryMs);
    if (lastArrival_) stats_.Add("cap.interval_ms", Qpc::ToMs(f.arrivalQpc - lastArrival_));
    lastArrival_ = f.arrivalQpc;
    if (f.timecode) {
        const int32_t loopUnits = (int32_t)(TimecodeAt(f.arrivalQpc) - *f.timecode);
        if (loopUnits > 0 && loopUnits < kMaxPlausibleLoopUnits) stats_.Add(stat::kLoopLatency, loopUnits / kTimecodeUnitsPerMs);
    }
    if (detectRepeats_) stats_.Add("cap.repeat_check_ms", f.repeatCheckMs);

    if (lastSeq_ && f.seq != lastSeq_ + 1) contentLost_ = true;
    lastSeq_ = f.seq;
    if (f.repeat && !contentLost_) {
        Repeat(f.sourceIndex, f.uploadedQpc);
        return;
    }
    SourceFrame& slot = NextSlot();
    if (!capture_->LockSlot(f)) {
        stats_.Add("cap.overwritten", 1);
        contentLost_ = true;
        return;
    }
    contentLost_ = false;
    converter_.Convert(capture_->PlaneSrv(f.slot, 0), capture_->PlaneCount() > 1 ? capture_->PlaneSrv(f.slot, 1) : nullptr, slot.rgb);
    capture_->UnlockSlot(f);
    Commit(slot, f.sourceIndex, f.sampleQpc, f.arrivalQpc, f.uploadedQpc);
}

void FrameSource::IngestSynthetic(int64_t now) {
    const int64_t period = (int64_t)((double)Qpc::Freq() / mode_.rateHz);
    if (!syntheticNext_) syntheticNext_ = now;
    for (; now >= syntheticNext_; syntheticNext_ += period) {
        const int64_t n = syntheticN_++;
        const int64_t content = (int64_t)std::floor((double)n * syntheticContentFps_ / mode_.rateHz + kContentIndexEpsilon);
        const bool repeat = content == syntheticContent_;
        syntheticContent_ = content;
        if (repeat && detectRepeats_) {
            Repeat(n, syntheticNext_);
            continue;
        }
        SourceFrame& slot = NextSlot();
        synthetic_->Render(slot.rgb, (double)content / syntheticContentFps_);
        Commit(slot, n, syntheticNext_, syntheticNext_, syntheticNext_);
    }
}

}
