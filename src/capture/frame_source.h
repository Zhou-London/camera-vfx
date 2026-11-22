#pragma once
#include "mf_capture.h"
#include "synthetic_source.h"
#include "../gfx/color_convert.h"
#include "../gfx/source_frame.h"
#include "../util/stats.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cameravfx {

struct SourceEvent {
    int64_t n = 0;
    int64_t readyQpc = 0;
    SourceFrame* stored = nullptr;
};

std::unique_ptr<MfCapture> OpenCapture(Device& dev, const CaptureRequest& request);

class FrameSource {
public:
    static constexpr int kStoreFrames = 16;

    FrameSource(Device& dev, StatBook& stats) : dev_(dev), stats_(stats) {}
    void UseSynthetic(const CaptureMode& mode, std::optional<double> contentFps);
    void UseCapture(std::unique_ptr<MfCapture> capture, const ColorOptions& color);
    void CloseCapture() { capture_.reset(); }
    void Start(const CaptureAnalysis& analysis);
    void SetColor(const ColorOptions& chosen);

    std::span<const SourceEvent> Ingest(int64_t now);
    void WaitFrame(DWORD maxMs);
    SourceFrame* Frame(int64_t n);
    std::span<SourceFrame> Store() { return store_; }

    const CaptureMode& Mode() const { return mode_; }
    const CaptureInfo* Capture() const { return capture_ ? &capture_->Info() : nullptr; }
    const ColorSpec& Color() const { return converter_.Color(); }
    uint64_t Repeats() const { return repeats_; }
    uint64_t Drops() const { return capture_ ? capture_->DroppedFrames() : 0; }

private:
    void Reset(const CaptureMode& mode);
    void IngestCaptured(const RawFrame& f);
    void IngestSynthetic(int64_t now);
    SourceFrame& NextSlot() { return store_[nextSlot_]; }
    void Commit(SourceFrame& f, int64_t n, int64_t sampleQpc, int64_t arrivalQpc, int64_t uploadedQpc);
    void Repeat(int64_t n, int64_t readyQpc);

    Device& dev_;
    StatBook& stats_;
    CaptureMode mode_;
    std::unique_ptr<MfCapture> capture_;
    ColorConverter converter_;
    std::unique_ptr<SyntheticSource> synthetic_;
    PreciseSleeper sleeper_;
    std::array<SourceFrame, kStoreFrames> store_;
    size_t nextSlot_ = 0;
    std::vector<SourceEvent> events_;
    bool detectRepeats_ = false;
    uint64_t repeats_ = 0;
    uint64_t lastSeq_ = 0;
    bool contentLost_ = false;
    int64_t lastArrival_ = 0;
    double syntheticContentFps_ = 0;
    int64_t syntheticNext_ = 0;
    int64_t syntheticN_ = 0;
    int64_t syntheticContent_ = -1;
};

}
