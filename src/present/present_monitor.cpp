#include "present_monitor.h"

namespace cameravfx {

namespace {
constexpr size_t kMaxPendingRecords = 8;
constexpr double kMaxPlausibleLatencyMs = 500;
constexpr double kMissedVblankRefreshes = 0.5;
constexpr double kIntervalSmoothing = 0.1;
constexpr double kMaxSmoothedIntervalMs = 100;
constexpr double kRatioSmoothing = 0.1;
constexpr double kSlowPresentRefreshes = 1.5;
constexpr double kVrrRangeMinFps = 48;
constexpr double kVrrOnBelowRatio = 1.2;
constexpr double kVrrOffAboveRatio = 1.5;
constexpr double kModePollMs = 500;

bool Plausible(double latencyMs) { return latencyMs > 0 && latencyMs < kMaxPlausibleLatencyMs; }

const char* PresentationModeName(int mode) {
    switch (mode) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed by DWM";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "overlay";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "direct (independent flip)";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    default: return "not reported";
    }
}
}

PresentMonitor::~PresentMonitor() {
    if (csv_) fclose(csv_);
}

void PresentMonitor::OpenCsv(const std::string& path, int64_t startQpc) {
    csv_ = OpenUtf8(path, L"w");
    if (!csv_) { LOGW("cannot write %s", path.c_str()); return; }
    csvStartQpc_ = startQpc;
    fprintf(csv_, "idx,present_id,render_start_ms,present_call_ms,predicted_v_ms,actual_v_ms,frame_a,frame_b,t,delay_ms,slack_ms,late,"
                  "newest_sample_ms,newest_arrival_ms,newest_uploaded_ms,content_sample_ms,render_cpu_ms,"
                  "arrival_to_scanout_ms,content_to_scanout_ms,hold\n");
}

void PresentMonitor::OnPresented(const PresentedFrame& frame) {
    records_.push_back({presented_++, frame, 0});
    if (lastPresentCall_) {
        const double intervalMs = Qpc::ToMs(frame.presentCall - lastPresentCall_);
        stats_.Add(stat::kPresentInterval, intervalMs);
        presentIntervalEmaMs_ = presentIntervalEmaMs_ <= 0 ? intervalMs
                              : (1 - kIntervalSmoothing) * presentIntervalEmaMs_ + kIntervalSmoothing * std::min(intervalMs, kMaxSmoothedIntervalMs);
    }
    lastPresentCall_ = frame.presentCall;
    stats_.Add(stat::kRenderCpu, frame.renderCpuMs);
    if (frame.hold) stats_.Add("pace.hold", 1.0);
    else stats_.Add("lat.arrival_to_present_ms", Qpc::ToMs(frame.presentCall - frame.newestArrival));
    stats_.Add("pace.delay_ms", frame.delayMs);
    if (frame.frameB != frame.frameA) stats_.Add("pace.t", frame.t);
}

PresentEvents PresentMonitor::Poll(int64_t now, bool vrrPacing) {
    PresentEvents events;
    DXGI_FRAME_STATISTICS st{};
    if (presenter_.FrameStats(st) && st.PresentCount) {
        TrackVrr(st, vrrPacing);
        for (Record& r : records_)
            if (!FlipTimed(r) && !r.scanout && r.frame.presentId == st.PresentCount && st.SyncQPCTime.QuadPart >= r.frame.presentCall)
                r.scanout = st.SyncQPCTime.QuadPart;
    }
    while (!records_.empty() && (records_.front().scanout || records_.size() > kMaxPendingRecords)) {
        events.missedVblank |= Finalize(records_.front());
        records_.pop_front();
    }
    PollPresentationMode(now, events);
    return events;
}

void PresentMonitor::FinalizeAll() {
    for (const Record& r : records_) Finalize(r);
    records_.clear();
}

void PresentMonitor::TrackVrr(const DXGI_FRAME_STATISTICS& st, bool vrrPacing) {
    if (lastStatsPresent_ && st.PresentCount > lastStatsPresent_) {
        const double ratio = (double)(st.SyncRefreshCount - lastStatsRefresh_) / (double)(st.PresentCount - lastStatsPresent_);
        refreshesPerPresent_ = (1 - kRatioSmoothing) * refreshesPerPresent_ + kRatioSmoothing * ratio;
    }
    lastStatsPresent_ = st.PresentCount;
    lastStatsRefresh_ = st.SyncRefreshCount;
    const double refreshMs = 1000.0 / presenter_.RefreshHz();
    const bool distinguishable = presentIntervalEmaMs_ > kSlowPresentRefreshes * refreshMs && presentIntervalEmaMs_ < 1000.0 / kVrrRangeMinFps;
    if (!distinguishable) return;
    const bool active = refreshesPerPresent_ < (vrrActive_ ? kVrrOffAboveRatio : kVrrOnBelowRatio);
    if (active == vrrActive_) return;
    vrrActive_ = active;
    Log(vrrPacing ? LogLevel::Debug : LogLevel::Info, "display VRR %s (refreshes/present %.2f)", active ? "ACTIVE" : "inactive", refreshesPerPresent_);
}

void PresentMonitor::PollPresentationMode(int64_t now, PresentEvents& events) {
    if (now - lastModePoll_ <= Qpc::FromMs(kModePollMs)) return;
    lastModePoll_ = now;
    const int mode = presenter_.PresentationMode();
    if (presentationMode_ == mode) return;
    const bool first = !presentationMode_;
    if (!first || mode != DXGI_FRAME_PRESENTATION_MODE_NONE) LOGI("presentation: %s", PresentationModeName(mode));
    events.becameComposed = !first && mode == DXGI_FRAME_PRESENTATION_MODE_COMPOSED;
    presentationMode_ = mode;
}

bool PresentMonitor::Finalize(const Record& r) {
    const PresentedFrame& f = r.frame;
    const bool flip = FlipTimed(r);
    FrameSample sample;
    sample.qpc = f.presentCall;
    if (const int64_t shown = flip ? f.presentCall : r.scanout) {
        const double arrivalMs = Qpc::ToMs(shown - f.newestArrival), contentMs = Qpc::ToMs(shown - f.contentSample);
        sample.latencyMs = (float)arrivalMs;
        if (Plausible(contentMs)) sample.contentMs = (float)contentMs;
        if (!f.hold) {
            shownAtScanout_ = !flip;
            stats_.Add(stat::kArrivalToShown, arrivalMs);
            if (Plausible(contentMs)) stats_.Add(stat::kContentToShown, contentMs);
        }
    }
    sample.delayMs = (float)f.delayMs;
    sample.gpuMs = f.gpuMs;
    sample.flags = (uint8_t)((f.late ? FrameSample::kLate : 0) | (f.hold ? FrameSample::kHold : 0));
    telemetry_.PushFrame(sample);

    bool missed = false;
    if (r.scanout) {
        stats_.Add("lat.present_to_scanout_ms", Qpc::ToMs(r.scanout - f.presentCall));
        const double errorMs = Qpc::ToMs(r.scanout - f.predictedVblank);
        stats_.Add("present.vblank_error_ms", errorMs);
        missed = errorMs > kMissedVblankRefreshes * 1000.0 / presenter_.RefreshHz();
        if (missed) missedVblanks_++;
    }
    if (csv_) WriteCsv(r);
    return missed;
}

void PresentMonitor::WriteCsv(const Record& r) {
    const PresentedFrame& f = r.frame;
    auto ms = [&](int64_t q) { return q ? Qpc::ToMs(q - csvStartQpc_) : 0.0; };
    auto since = [&](int64_t from) { return r.scanout ? Qpc::ToMs(r.scanout - from) : 0.0; };
    fprintf(csv_, "%llu,%u,%.3f,%.3f,%.3f,%.3f,%lld,%lld,%.4f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", (unsigned long long)r.index,
            f.presentId, ms(f.renderStart), ms(f.presentCall), ms(f.predictedVblank), ms(r.scanout), (long long)f.frameA, (long long)f.frameB, f.t,
            f.delayMs, f.minSlackMs, f.late ? 1 : 0, ms(f.newestSample), ms(f.newestArrival), ms(f.newestUploaded), ms(f.contentSample),
            f.renderCpuMs, since(f.newestArrival), since(f.contentSample), f.hold ? 1 : 0);
}

}
