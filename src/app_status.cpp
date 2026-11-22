#include "app.h"
#include "vfx/vfx_chain.h"
#include "vfx/vfx_quality.h"
#include <cmath>
#include <format>

namespace cameravfx {

namespace {
constexpr double kDashboardReportSec = 1.0;
constexpr double kQuietReportSec = 2.0;
constexpr double kMinRateWindowSec = 0.25;
constexpr double kStaticUniqueFraction = 0.05;
constexpr double kGpuBudgetWarnFraction = 0.9;
constexpr double kGpuWarnIntervalMs = 10000;
constexpr double kGpuNoticeSec = 3;
constexpr double kSlowOutputFraction = 0.92;
constexpr int kSlowOutputReports = 3;
constexpr double kSlowOutputWarnIntervalMs = 30000;
constexpr double kAudioSyncShownMs = 0.5;
constexpr double kCaptureNoteChangeMs = 0.05;

Percentiles PercentilesOrZero(const StatBook& stats, std::string_view key) { return stats.Get(key).value_or(Percentiles{}); }
}

void App::ResetStats() {
    stats_.Clear();
    statsEpoch_++;
}

void App::ShowNotice(std::string text, double seconds) {
    notice_ = std::move(text);
    noticeUntil_ = Qpc::Now() + Qpc::FromMs(seconds * 1000.0);
}

void App::Report(int64_t now, bool final) {
    if (!warmupDone_ && Qpc::ToSec(now - startQpc_) > options_.warmupSec) {
        warmupDone_ = true;
        ResetStats();
        LOGI("warm-up (%.0f s) done: statistics reset", options_.warmupSec);
    }
    if (statsResetAt_ && now >= statsResetAt_) {
        ResetStats();
        statsResetAt_ = 0;
    }
    RecheckMultiplier(now);
    if (now - lastStatusPublish_ >= Qpc::FromMs(kStatusPublishMs)) PublishStatus(now);

    const double intervalSec = tui_ ? kDashboardReportSec : options_.reportSec > 0 ? options_.reportSec : kQuietReportSec;
    if (!final && now - lastReport_ < Qpc::FromMs(intervalSec * 1000.0)) return;
    const double windowSec = Qpc::ToSec(now - lastReport_);
    if (windowSec > kMinRateWindowSec) outFps_ = (double)(monitor_.Presented() - lastReportPresented_) / windowSec;
    lastReport_ = now;
    lastReportPresented_ = monitor_.Presented();

    UpdateStageTimes();
    const std::optional<MeasuredLatency> latency = Latency();
    if (audio_.Running()) {
        if (latency && latency->content) audio_.SetVideoLatencyMs(latency->content->p50);
        audioStats_ = audio_.Stats();
    }
    WarnGpuOverBudget();
    WarnSlowOutput();
    if (final || (options_.reportDetail && !tui_)) PrintDetail();
    if (!tui_ && (final || options_.reportSec > 0)) LOGI("%s", StatusLine(latency).c_str());

    std::string title = "CameraVFX | " + ModeLabel();
    if (mode_ == PaceMode::Vrr) title += std::format(" x{}", pacer_.Multiplier());
    if (latency) title += std::format(" | {} p50 {:.1f} p99 {:.1f} ms", latency->metric, latency->arrival.p50, latency->arrival.p99);
    title += std::format(" | D {:.1f} ms", pacer_.DelayMs());
    presenter_.SetTitle(title.c_str());
}

void App::UpdateStageTimes() {
    auto p50 = [&](std::string_view key) {
        const std::optional<Percentiles> p = stats_.Get(key);
        return p ? p->p50 : kUnknownMs;
    };
    stages_ = {p50(stat::kCaptureDelivery), p50(stat::kCaptureUpload), p50(stat::kVfxFg), p50(stat::kVfxSr), p50(stat::kVfxHdr)};
    if (source_.Capture() && std::abs(stages_.captureMs - publishedCaptureMs_) > kCaptureNoteChangeMs) settingsDirty_ = true;
}

std::optional<App::MeasuredLatency> App::Latency() const {
    const std::optional<Percentiles> arrival = stats_.Get(stat::kArrivalToShown);
    if (!arrival) return std::nullopt;
    return MeasuredLatency{std::string("arrival->") + monitor_.ShownAt(), *arrival, stats_.Get(stat::kContentToShown)};
}

std::string App::StatusLine(const std::optional<MeasuredLatency>& latency) const {
    std::string line = std::format("in {:.2f} Hz", SignalHz());
    if (detectRepeats_)
        line += cadence_.UniqueFraction() < kStaticUniqueFraction ? std::string(" | content static") : std::format(" | content {:.1f} fps", ContentFps());
    line += std::format(" -> out {:.1f} fps ({}", outFps_, ModeLabel());
    if (mode_ == PaceMode::Vrr) line += std::format(", x{}", pacer_.Multiplier());
    line += ")";
    if (latency) line += std::format(" | {} {:.1f}/{:.1f} ms", latency->metric, latency->arrival.p50, latency->arrival.p99);
    if (mode_ != PaceMode::Passthrough) line += std::format(" | D {:.1f} lead {:.1f}", pacer_.DelayMs(), lead_.LeadMs());
    if (const std::optional<Percentiles> gpu = stats_.Get(stat::kVfxGpuTotal)) line += std::format(" | gpu {:.2f}/{:.2f} ms", gpu->p50, gpu->p99);
    line += std::format(" | late {} hold {} drop {}", pacer_.LateCount(), pacer_.HoldCount(), source_.Drops());
    if (mode_ == PaceMode::Fixed) line += std::format(" miss {}", monitor_.MissedVblanks());
    if (audio_.Running()) {
        const AudioStats& as = audioStats_;
        if (!as.streaming) line += " | audio: reopening device";
        else if (!as.playing) line += " | audio: buffering";
        else line += std::format(" | audio {:.0f} ms{} buf {:.0f} ms", as.latencyMs, audio_.Muted() ? " (muted)" : "", as.bufferMs);
        if (as.streaming && as.syncDelayMs > kAudioSyncShownMs) line += std::format(" sync +{:.0f}", as.syncDelayMs);
        if (as.underruns) line += std::format(" underrun {}", as.underruns);
    }
    if (options_.decodeTimecode) {
        const std::optional<Percentiles> loop = stats_.Get(stat::kLoopLatency);
        line += loop ? std::format(" | loop {:.1f}/{:.1f} ms", loop->p50, loop->p99) : std::string(" | loop: no timecode");
    }
    return line;
}

void App::WarnGpuOverBudget() {
    const std::optional<Percentiles> gpu = stats_.Get(stat::kVfxGpuTotal);
    const double budgetMs = OutputPeriodMs();
    if (!gpu || gpu->p50 <= kGpuBudgetWarnFraction * budgetMs) return;
    LOG_EVERY_MS(LogLevel::Warn, kGpuWarnIntervalMs,
                 "GPU time %.1f ms per output frame exceeds the %.1f ms frame budget: frames are skipped; lower fg-quality / sr-quality or the multiplier (settings: s)",
                 gpu->p50, budgetMs);
    ShowNotice(std::format("GPU {:.1f} ms > 预算 {:.1f} ms：跳帧（设置 s 里降低档位）", gpu->p50, budgetMs), kGpuNoticeSec);
}

void App::WarnSlowOutput() {
    const double expectedFps = 1000.0 / OutputPeriodMs();
    const bool contentStatic = detectRepeats_ && cadence_.UniqueFraction() < kStaticUniqueFraction;
    const bool slow = warmupDone_ && !contentStatic && outFps_ < kSlowOutputFraction * expectedFps;
    if (statsEpoch_ != slowReportsEpoch_) slowReports_ = 0;
    slowReportsEpoch_ = statsEpoch_;
    slowReports_ = slow ? slowReports_ + 1 : 0;
    if (slowReports_ < kSlowOutputReports) return;
    const std::optional<Percentiles> gpu = stats_.Get(stat::kVfxGpuTotal);
    const Percentiles timerLate = PercentilesOrZero(stats_, stat::kTimerLate), waitReady = PercentilesOrZero(stats_, stat::kWaitReady);
    const Percentiles interval = PercentilesOrZero(stats_, stat::kPresentInterval), renderCpu = PercentilesOrZero(stats_, stat::kRenderCpu);
    LOG_EVERY_MS(LogLevel::Warn, kSlowOutputWarnIntervalMs,
                 "output %.1f fps, expected %.1f: GPU %s the %.1f ms budget, %llu grid points skipped, timer late %.2f/%.2f ms, "
                 "wait for display %.2f/%.2f ms, present interval %.2f/%.2f ms, render lead %.1f ms, render CPU p99 %.2f ms, presentation %s",
                 outFps_, expectedFps, gpu && gpu->p50 > kGpuBudgetWarnFraction * OutputPeriodMs() ? "over" : "within", OutputPeriodMs(),
                 (unsigned long long)pacer_.SkipCount(), timerLate.p50, timerLate.p99, waitReady.p50, waitReady.p99, interval.p50, interval.p99,
                 lead_.LeadMs(), renderCpu.p99, monitor_.Composed() ? "composed by DWM" : "direct");
}

void App::PrintDetail() {
    const std::string head = std::format(
        "---- {:.1f}s  mode={} fg={}{} sr={}{} hdr={} | src {:.3f} Hz (jit {:.2f} ms) content Pc={:.2f} x{} disp {:.3f} Hz (jit {:.2f} ms) | "
        "D={:.2f} ms minslack={:.2f} lead={:.2f} late={} hold={} missed={} gpubusy={} drops={} repeats={}",
        Qpc::ToSec(Qpc::Now() - startQpc_), ModeLabel(), options_.fg ? "on" : "off", fgOn_ ? "" : " (off)", options_.sr ? "on" : "off",
        srOn_ ? "" : " (off)", options_.hdr ? "on" : "off", pacer_.SignalHz(), pacer_.SignalJitterMs(), pacer_.ContentPeriod(), pacer_.Multiplier(),
        vsync_.MeasuredHz(), vsync_.JitterMs(), pacer_.DelayMs(), pacer_.MinSlackMs(), lead_.LeadMs(), pacer_.LateCount(), pacer_.HoldCount(),
        monitor_.MissedVblanks(), gpuBusyAtPresent_, source_.Drops(), source_.Repeats());
    LOGI("%s\n%s", head.c_str(), stats_.Report().c_str());
}

void App::PublishStatus(int64_t now) {
    lastStatusPublish_ = now;
    if (settingsDirty_ && !loading_) PublishSettings();
    AppStatus s;
    s.statsEpoch = statsEpoch_;
    s.uptimeSec = Qpc::ToSec(now - startQpc_);
    s.warmupDone = warmupDone_;
    s.configPath = options_.config;
    const CaptureMode& m = source_.Mode();
    if (const CaptureInfo* capture = source_.Capture()) {
        const ColorSpec& color = source_.Color();
        const std::string range = color.fullRange ? "全范围" : "有限范围";
        s.captureName = capture->deviceName;
        s.captureFormat = std::format("{} {}x{}@{:.2f} · {}", PixFmtName(m.format), m.width, m.height, m.rateHz,
                                      IsRgb(m.format) ? range : std::string(MatrixName(color.matrix)) + " " + range);
    } else if (options_.synthetic) {
        s.captureName = "synthetic";
        s.captureFormat = std::format("{}x{}@{:.2f}", m.width, m.height, m.rateHz);
    }
    s.sourceSize = {m.width, m.height};
    s.outputSize = outputSize_;
    s.displaySize = {presenter_.Width(), presenter_.Height()};
    s.displayHz = presenter_.RefreshHz();
    s.vrrActive = monitor_.VrrActive();
    s.probing = probing_;
    s.mode = ModeLabel() + (monitor_.Composed() ? " · DWM 合成" : "");
    s.fg = options_.fg;
    s.fgQuality = FgQualityName(options_.fgQuality);
    s.sr = options_.sr;
    s.srQuality = SrQualityName(options_.srQuality);
    s.hdr = options_.hdr;
    s.fgOn = fgOn_;
    s.srOn = srOn_;
    s.fgActive = FgActive();
    s.timecode = options_.timecode;
    s.detectRepeats = detectRepeats_;
    s.fgMultiplier = pacer_.Multiplier();
    s.fgMultiplierRequested = options_.fgMultiplier;
    s.fgThresholdFps = options_.fgThresholdFps;
    s.fgMultiplierLimit = multiplierLimit_;
    s.signalHz = SignalHz();
    s.contentFps = ContentFps();
    s.outFps = outFps_;
    s.contentStatic = detectRepeats_ && cadence_.UniqueFraction() < kStaticUniqueFraction;
    s.leadMs = lead_.LeadMs();
    s.gpuBudgetMs = OutputPeriodMs();
    s.late = pacer_.LateCount();
    s.drops = source_.Drops();
    s.missed = monitor_.MissedVblanks();
    s.audioEnabled = options_.audio;
    s.audioRunning = audio_.Running() && audioStats_.streaming;
    s.audioMuted = audio_.Running() && audio_.Muted();
    s.audioVolume = (float)(options_.audioVolumePercent / 100.0);
    s.audioLatencyMs = audioStats_.latencyMs;
    s.audioSyncMs = audioStats_.syncDelayMs;
    s.audioUnderruns = audioStats_.underruns;
    s.audioIn = audioStats_.captureName;
    s.audioOut = audioStats_.renderName;
    s.stages = stages_;
    if (now < noticeUntil_) s.notice = notice_;
    if (loading_) {
        s.loading = loading_->label;
        s.fgLoading = loading_->fgModel;
        s.srLoading = loading_->srModel;
    }
    telemetry_.PublishStatus(std::move(s));
}

void App::StartDashboard() {
    const bool interactive = ConsoleIsInteractive();
    tui_ = options_.ui != UiMode::Plain && interactive;
    if (options_.ui == UiMode::Tui && !interactive) LOGW("terminal dashboard unavailable: not an interactive console; plain output");
    if (tui_) dash_.Start(&telemetry_);
}

void App::StopDashboard() {
    if (!tui_) return;
    dash_.Stop();
    tui_ = false;
}

}
