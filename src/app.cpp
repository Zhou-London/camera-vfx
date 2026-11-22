#include "app.h"
#include "capture/timecode.h"
#include "vfx/vfx_chain.h"
#include "vfx/vfx_quality.h"
#include <cmath>
#include <format>
#include <thread>

namespace cameravfx {

std::atomic<bool> g_quitRequested{false};

namespace {
constexpr double kProbeSec = 2.5;
constexpr int kWarmUpRuns = 4;
constexpr float kWarmUpTimestep = 0.5f;
constexpr float kModelLoadShareOfProgress = 0.9f;
constexpr float kPauseDim = 0.3f;
constexpr double kLoadingFrameMs = 33;
constexpr DWORD kIdleWaitMs = 5;
constexpr DWORD kPassthroughWaitMs = 50;
constexpr double kPassthroughPresentAheadMs = 1.0;
constexpr double kVrrPresentBeforeVblankMs = 0.3;
constexpr double kPresentHoldSpinMs = 0.25;
constexpr double kFullLeadToleranceMs = 0.5;
constexpr double kGpuToPresentMarginMs = 0.3;
constexpr int kReopenAttempts = 10;
constexpr DWORD kReopenRetryMs = 300;
constexpr double kSettleAfterModelSwitchSec = 2.0;
constexpr double kSettleAfterCaptureSwitchSec = 3.0;
constexpr double kComposedNoticeSec = 10;
constexpr double kMultiplierCheckMs = 2000;
constexpr double kMultiplierNoticeSec = 15;
constexpr int kYuvRepeatThreshold = 6;
constexpr int kRgbRepeatThreshold = 8;
constexpr int64_t kMarkerToggleFrames = 30;
}

App::App(const Options& options) : options_(options), lead_(options.leadMs) {}

App::~App() {
    StopDashboard();
    audio_.Stop();
    vsync_.Stop();
    source_.CloseCapture();
    if (dev_.Context()) {
        dev_.Context()->ClearState();
        dev_.Context()->Flush();
        dev_.WaitIdle();
    }
    vfx_.reset();
    presenter_.Shutdown();
}

bool App::Init(HINSTANCE inst) {
    dev_.Create(options_.d3dDebug);
    const D3D11_QUERY_DESC frameDoneQuery{D3D11_QUERY_EVENT, 0};
    HR_CHECK(dev_.D3dDevice()->CreateQuery(&frameDoneQuery, &frameDone_));
    profiler_.Init(dev_.D3dDevice());

    if (options_.synthetic) {
        source_.UseSynthetic(options_.capture, options_.syntheticContentFps);
        LOGI("source: synthetic %ux%u @ %.2f Hz", options_.capture.width, options_.capture.height, options_.capture.rateHz);
    } else {
        std::unique_ptr<MfCapture> capture = OpenCapture(dev_, CaptureRequest{options_.device, options_.capture});
        if (!capture) return false;
        source_.UseCapture(std::move(capture), options_.color);
    }

    PresenterOptions po;
    po.windowed = options_.windowed;
    po.monitorIndex = options_.monitor;
    po.hdr = options_.hdr;
    if (!presenter_.Init(dev_, po, inst)) return false;
    blit_.Init(dev_);
    loading_ = Loading{"模型", options_.fg, options_.sr, Qpc::Now()};
    if (!SetupProcessing()) return false;
    WarmUpChain();
    ShowLoading(1.f);
    loading_.reset();

    probing_ = !options_.mode;
    mode_ = options_.mode.value_or(PaceMode::Passthrough);
    ResetPacing(1.0);
    vsync_.Start(presenter_.Output(), presenter_.RefreshHz());
    if (detectRepeats_) LOGI("repeat detection on: output = x%d of the content rate", pacer_.Multiplier());
    const CaptureMode& src = source_.Mode();
    LOGI("pipeline: mode=%s fg=%s sr=%s hdr=%s src=%ux%u out=%ux%u display=%ux%u@%.0fHz", ModeLabel().c_str(),
         options_.fg ? FgQualityName(options_.fgQuality) : "off", options_.sr ? SrQualityName(options_.srQuality) : "off", options_.hdr ? "on" : "off",
         src.width, src.height, outputSize_.width, outputSize_.height, presenter_.Width(), presenter_.Height(), presenter_.RefreshHz());
    return true;
}

bool App::SetupProcessing() {
    const CaptureMode& src = source_.Mode();
    pauseTex_ = dev_.CreateTex(src.width, src.height, DXGI_FORMAT_R8G8B8A8_UNORM, false, false);
    pauseValid_ = false;
    outputSize_ = {src.width, src.height};
    if (options_.sr) {
        const FrameSize fit = FitAspect(outputSize_, options_.out.value_or(FrameSize{presenter_.Width(), presenter_.Height()}));
        outputSize_ = {fit.width & ~1u, fit.height & ~1u};
    }
    ShowLoading(0.f);
    return BuildVfx();
}

bool App::BuildVfx() {
    if (!options_.fg && !options_.sr && !options_.hdr) return true;
    const CaptureMode& src = source_.Mode();
    if (options_.sr && SrQualityNeedsSameSize(options_.srQuality) && (outputSize_.width != src.width || outputSize_.height != src.height)) {
        LOGE("sr-quality %s works only without upscaling (output %ux%u != source %ux%u); use low..ultra, hb-*, streaming-* or --out %ux%u",
             SrQualityName(options_.srQuality), outputSize_.width, outputSize_.height, src.width, src.height, src.width, src.height);
        return false;
    }
    VfxChain::Config c;
    c.fg = options_.fg;
    c.fgQuality = options_.fgQuality;
    c.sr = options_.sr;
    c.srQuality = options_.srQuality;
    c.hdr = options_.hdr;
    c.hdrAfterSr = options_.hdrAfterSr;
    c.srcW = src.width;
    c.srcH = src.height;
    c.outW = outputSize_.width;
    c.outH = outputSize_.height;
    c.onLoadProgress = [this](float loaded) { ShowLoading(kModelLoadShareOfProgress * loaded); };
    vfx_ = VfxChain::Create(dev_, c, source_.Store());
    if (!vfx_) LOGE("VFX chain init failed");
    return vfx_ != nullptr;
}

void App::WarmUpChain() {
    if (!vfx_) return;
    const int64_t t0 = Qpc::Now();
    std::span<SourceFrame> store = source_.Store();
    const bool empty = std::none_of(store.begin(), store.end(), [](const SourceFrame& f) { return f.n >= 0; });
    if (empty) {
        store[0].n = 0;
        store[1].n = 1;
    }
    SourceFrame* newest = nullptr;
    SourceFrame* previous = nullptr;
    for (SourceFrame& f : store) {
        if (f.n < 0) continue;
        vfx_->Ingest(f);
        if (!newest || f.n > newest->n) {
            previous = newest;
            newest = &f;
        } else if (!previous || f.n > previous->n) {
            previous = &f;
        }
    }
    for (int i = 0; i < kWarmUpRuns; i++) {
        vfx_->Process(previous ? *previous : *newest, previous ? newest : nullptr, kWarmUpTimestep, true);
        dev_.Context()->Flush();
    }
    dev_.WaitIdle();
    if (empty) store[0].n = store[1].n = -1;
    vfx_->PopTimings();
    LOGI("warm-up of processing stages: %.0f ms", Qpc::ToMs(Qpc::Now() - t0));
}

void App::ResetPacing(double contentPeriod) {
    const double signalHz = source_.Mode().rateHz;
    const std::vector<double> fitting = ContentPeriods(options_.contentRates, signalHz);
    if (!options_.contentRates.empty() && fitting.empty())
        LOGW("content rates: none of them fits the %.4g Hz signal; every common rate is detected", signalHz);
    if (!fitting.empty()) {
        std::string list;
        for (double period : fitting) list += std::format("{}{:.4g}", list.empty() ? "" : ", ", signalHz / period);
        LOGI("content rates: only %s fps are detected", list.c_str());
    }
    const std::vector<double>& periods = fitting.empty() ? kDefaultContentPeriods : fitting;
    detectRepeats_ = options_.detectRepeats && options_.fg && options_.mode != PaceMode::Passthrough;
    const bool keepPeriod = detectRepeats_ && std::find(periods.begin(), periods.end(), contentPeriod) != periods.end();
    PacerConfig config;
    config.mode = mode_;
    config.multiplier = 1;
    config.fg = FgActive();
    config.contentPeriod = keepPeriod ? contentPeriod : 1.0;
    config.snapEps = options_.snap;
    config.marginMs = options_.marginMs;
    config.baseDelayMs = options_.baseDelayMs;
    cadence_ = CadenceDetector(periods, config.contentPeriod);
    pacer_.Configure(config, signalHz);
    UpdateMultiplier();
    settingsDirty_ = true;
}

CaptureAnalysis App::Analysis() const {
    CaptureAnalysis analysis;
    analysis.decodeTimecode = options_.decodeTimecode;
    if (detectRepeats_) {
        RepeatConfig repeat;
        repeat.levelThreshold = options_.repeatThreshold.value_or(IsRgb(source_.Mode().format) ? kRgbRepeatThreshold : kYuvRepeatThreshold);
        repeat.maxChangedFrac = options_.repeatMaxChanged;
        repeat.maxTileChanged = options_.repeatMaxTile;
        analysis.repeat = repeat;
    }
    return analysis;
}

double App::SignalHz() const { return pacer_.SignalHz() > 0 ? pacer_.SignalHz() : source_.Mode().rateHz; }

double App::ContentFps() const { return SignalHz() / pacer_.ContentPeriod(); }

double App::OutputPeriodMs() const {
    if (mode_ == PaceMode::Vrr) return 1000.0 / (ContentFps() * pacer_.Multiplier());
    if (mode_ == PaceMode::Fixed) return 1000.0 / presenter_.RefreshHz();
    return 1000.0 / SignalHz();
}

std::string App::ModeLabel() const { return probing_ ? "auto (probing)" : PaceModeName(mode_); }

MultiplierChoice App::MultiplierFor(int requested, double thresholdFps) const {
    MultiplierInputs in;
    in.fgActive = FgActive();
    in.vrr = mode_ == PaceMode::Vrr;
    in.contentFps = ContentFps();
    in.requested = requested;
    in.thresholdFps = thresholdFps;
    in.maxRefreshHz = presenter_.RefreshHz();
    in.generatedFrameMs = lead_.GeneratedFrameMs();
    in.leadMs = lead_.LeadMs();
    in.current = pacer_.Multiplier();
    return ChooseMultiplier(in);
}

bool App::UpdateMultiplier() {
    const MultiplierChoice choice = MultiplierFor(options_.fgMultiplier, options_.fgThresholdFps);
    const bool changed = choice.multiplier != pacer_.Multiplier();
    if (changed || choice.limit != multiplierLimit_) settingsDirty_ = true;
    multiplierLimit_ = choice.limit;
    if (!changed) return false;
    pacer_.SetMultiplier(choice.multiplier);
    statsEpoch_++;
    return true;
}

void App::OnContentRateChanged() {
    pacer_.SetContentPeriod(cadence_.ContentPeriod());
    UpdateMultiplier();
    settingsDirty_ = true;
    statsEpoch_++;
    const double content = ContentFps(), period = pacer_.ContentPeriod();
    const int multiplier = pacer_.Multiplier();
    if (mode_ == PaceMode::Fixed) {
        LOGI("content rate %.2f fps (1 new frame per %.2f source frames) -> %s the %.0f Hz refresh", content, period,
             FgActive() ? "frame generation to" : "repeated at", presenter_.RefreshHz());
        return;
    }
    const char* limit = multiplierLimit_ == MultiplierLimit::Threshold ? ": at the frame generation threshold"
                      : multiplierLimit_ == MultiplierLimit::Refresh   ? ": display refresh limit"
                      : multiplierLimit_ == MultiplierLimit::Gpu       ? ": GPU time limit"
                                                                       : "";
    LOGI("content rate %.2f fps (1 new frame per %.2f source frames) -> output %.1f fps (x%d%s)", content, period, content * multiplier, multiplier,
         limit);
}

void App::RecheckMultiplier(int64_t now) {
    if (mode_ != PaceMode::Vrr || !warmupDone_ || now - lastMultiplierCheck_ <= Qpc::FromMs(kMultiplierCheckMs)) return;
    lastMultiplierCheck_ = now;
    const int previous = pacer_.Multiplier();
    if (!UpdateMultiplier()) return;
    const int multiplier = pacer_.Multiplier();
    const double content = ContentFps();
    const double generatedMs = lead_.GeneratedFrameMs().value_or(0);
    LOGI("frame generation x%d -> x%d: output %.1f fps, a generated frame needs %.1f ms (render lead %.1f ms)", previous, multiplier,
         content * multiplier, generatedMs, lead_.LeadMs());
    if (multiplier < previous)
        ShowNotice(std::format("补帧 ×{} 放不下（生成一帧约 {:.1f} ms，{:.0f} fps 时每帧只有 {:.1f} ms）：降低补帧或超分档位", previous, generatedMs,
                               content * previous, 1000.0 / (content * previous)),
                   kMultiplierNoticeSec);
}

int App::Run() {
    startQpc_ = lastReport_ = Qpc::Now();
    if (!options_.csv.empty()) monitor_.OpenCsv(options_.csv, startQpc_);
    StartDashboard();
    source_.Start(Analysis());
    if (probing_) {
        LOGI("auto mode: probing the display for VRR...");
        RunLoop(kProbeSec);
        probing_ = false;
        mode_ = monitor_.VrrActive() && presenter_.TearingEnabled() ? PaceMode::Vrr : PaceMode::Fixed;
        pacer_.SetMode(mode_);
        UpdateMultiplier();
        settingsDirty_ = true;
        LOGI("auto mode: refreshes/present %.2f -> %s pacing", monitor_.RefreshesPerPresent(), PaceModeName(mode_));
        ResetStats();
        monitor_.Discard();
    }
    StartAudio();
    RunLoop(0);
    monitor_.FinalizeAll();
    PublishStatus(Qpc::Now());
    StopDashboard();
    Report(Qpc::Now(), true);
    return 0;
}

bool App::KeepRunning(double untilSec) const {
    const double elapsed = Qpc::ToSec(Qpc::Now() - startQpc_);
    return !g_quitRequested && (options_.durationSec <= 0 || elapsed <= options_.durationSec) && (untilSec <= 0 || elapsed <= untilSec);
}

void App::RunLoop(double untilSec) {
    while (presenter_.PumpMessages() && KeepRunning(untilSec)) {
        HandleKeys();
        if (g_quitRequested) break;
        if (!haveSlot_) {
            const int64_t waitStart = Qpc::Now();
            haveSlot_ = presenter_.WaitReady();
            stats_.Add(stat::kWaitReady, Qpc::ToMs(Qpc::Now() - waitStart));
        }
        if (const std::optional<OutputSlot> slot = NextSlot()) Render(*slot);
        AfterPresents(Qpc::Now());
    }
}

std::optional<App::OutputSlot> App::NextSlot() {
    switch (mode_) {
    case PaceMode::Fixed: return NextFixedSlot();
    case PaceMode::Vrr: return NextVrrSlot();
    case PaceMode::Passthrough: return NextPassthroughSlot();
    }
    return std::nullopt;
}

PaceDecision App::ShowNewest() const {
    PaceDecision d;
    d.frameA = d.frameB = pacer_.NewestUniqueIndex();
    d.valid = d.frameA >= 0;
    return d;
}

std::optional<App::OutputSlot> App::NextFixedSlot() {
    const int64_t lead = Qpc::FromMs(lead_.LeadMs());
    const int64_t vblank = vsync_.NextVblankAfter(Qpc::Now() + lead);
    if (options_.fixedLateLatch) {
        sleeper_.SleepUntil(vblank - lead);
        stats_.Add(stat::kTimerLate, sleeper_.LastWakeLateMs());
    }
    const int64_t start = Qpc::Now();
    Ingest(start);
    PaceDecision d = pacer_.Decide(start, vblank);
    if (!d.valid) d = ShowNewest();
    if (!d.valid) {
        source_.WaitFrame(kIdleWaitMs);
        return std::nullopt;
    }
    return OutputSlot{d, start, vblank, 1, 0};
}

std::optional<App::OutputSlot> App::NextVrrSlot() {
    Ingest(Qpc::Now());
    const int64_t vblank = pacer_.NextVrrDisplayTime(Qpc::Now(), lead_.LeadMs());
    if (!vblank) {
        source_.WaitFrame(kIdleWaitMs);
        return std::nullopt;
    }
    sleeper_.SleepUntil(vblank - Qpc::FromMs(lead_.LeadMs()));
    stats_.Add(stat::kTimerLate, sleeper_.LastWakeLateMs());
    const int64_t start = Qpc::Now();
    Ingest(start);
    const PaceDecision d = pacer_.Decide(start, vblank);
    if (!d.valid) return std::nullopt;
    return OutputSlot{d, start, vblank, 0, vblank - Qpc::FromMs(kVrrPresentBeforeVblankMs)};
}

std::optional<App::OutputSlot> App::NextPassthroughSlot() {
    source_.WaitFrame(kPassthroughWaitMs);
    const int64_t start = Qpc::Now();
    Ingest(start);
    const int64_t signal = pacer_.NewestSignalIndex();
    const PaceDecision d = ShowNewest();
    if (!d.valid || signal == shownSignal_) return std::nullopt;
    shownSignal_ = signal;
    const UINT sync = probing_ ? 1 : (UINT)options_.passthroughSync;
    const int64_t presentTime = start + Qpc::FromMs(kPassthroughPresentAheadMs);
    return OutputSlot{d, start, sync ? vsync_.NextVblankAfter(presentTime) : presentTime, sync, 0};
}

void App::Ingest(int64_t now) {
    for (const SourceEvent& e : source_.Ingest(now)) {
        const bool unique = e.stored != nullptr;
        pacer_.OnSourceFrame(e.n, e.readyQpc, unique);
        if (detectRepeats_ && cadence_.OnFrame(e.n, unique)) OnContentRateChanged();
        if (unique && vfx_ && !vfxReloading_) vfx_->Ingest(*e.stored);
    }
}

void App::Render(const OutputSlot& slot) {
    const PaceDecision& d = slot.decision;
    const SourceFrame* a = source_.Frame(d.frameA);
    const SourceFrame* b = d.frameB != d.frameA ? source_.Frame(d.frameB) : nullptr;
    if (!a) return;
    ID3D11DeviceContext4* ctx = dev_.Context();
    profiler_.Begin(ctx);
    const Device::Tex* image = &a->rgb;
    Encoding encoding = Encoding::SdrGamma;
    bool generated = false;
    if (vfx_) {
        const int64_t t0 = Qpc::Now();
        const VfxOutput out = vfx_->Process(*a, b, d.t, srOn_);
        stats_.Add("vfx.cpu_ms", Qpc::ToMs(Qpc::Now() - t0));
        for (const VfxTimings& timings : vfx_->PopTimings()) {
            RecordVfxTimings(stats_, timings);
            lastVfxGpuMs_ = timings.totalMs;
        }
        if (out.tex) {
            image = out.tex;
            encoding = out.encoding;
            generated = out.generated;
        }
        profiler_.Mark(ctx, "gpu.vfx");
    }
    BlitParams bp;
    bp.inEnc = encoding;
    bp.outEnc = presenter_.OutputEncoding();
    bp.sdrWhiteNits = (float)options_.sdrWhiteNits;
    bp.timecode = options_.timecode;
    bp.timecodeValue = TimecodeAt(slot.vblank);
    bp.marker = options_.marker;
    bp.markerOn = (d.frameA / kMarkerToggleFrames) % 2 != 0;
    blit_.Draw(image->srv.Get(), image->w, image->h, presenter_.BackbufferRtv(), presenter_.Width(), presenter_.Height(), bp);
    profiler_.Mark(ctx, "gpu.blit");
    profiler_.End(ctx);
    ctx->End(frameDone_.Get());

    const int64_t submitted = Qpc::Now();
    if (slot.presentAt) HoldPresent(slot);
    const int64_t presentCall = Qpc::Now();
    const HRESULT hr = presenter_.Present(slot.syncInterval);
    haveSlot_ = false;
    if (FAILED(hr)) return;
    stats_.Add("present.call_ms", Qpc::ToMs(Qpc::Now() - presentCall));
    const PresentedFrame f = DescribePresented(slot, *a, b, generated, submitted, presentCall);
    monitor_.OnPresented(f);
    lead_.OnFrameCost(std::max(f.renderCpuMs, lastVfxGpuMs_ + kGpuToPresentMarginMs), generated);
    profiler_.Collect(ctx, [&](const char* key, double ms) { stats_.Add(key, ms); });
}

PresentedFrame App::DescribePresented(const OutputSlot& slot, const SourceFrame& a, const SourceFrame* b, bool generated, int64_t submitted,
                                      int64_t presentCall) const {
    const PaceDecision& d = slot.decision;
    const SourceFrame& newest = generated ? *b : a;
    PresentedFrame f;
    f.presentId = presenter_.LastPresentCount();
    f.syncInterval = slot.syncInterval;
    f.renderStart = slot.renderStart;
    f.presentCall = presentCall;
    f.predictedVblank = slot.vblank;
    f.frameA = d.frameA;
    f.frameB = generated ? d.frameB : d.frameA;
    f.t = generated ? d.t : 0.f;
    f.delayMs = d.delayMs;
    f.minSlackMs = pacer_.MinSlackMs();
    f.late = d.late;
    f.hold = d.hold;
    f.newestSample = newest.sampleQpc;
    f.newestArrival = newest.arrivalQpc;
    f.newestUploaded = newest.uploadedQpc;
    f.contentSample = generated ? (int64_t)((double)a.sampleQpc + (double)(b->sampleQpc - a.sampleQpc) * d.t) : a.sampleQpc;
    f.renderCpuMs = Qpc::ToMs(submitted - slot.renderStart);
    f.gpuMs = (float)lastVfxGpuMs_;
    return f;
}

void App::HoldPresent(const OutputSlot& slot) {
    ID3D11DeviceContext4* ctx = dev_.Context();
    ctx->Flush();
    if (Qpc::Now() > slot.presentAt) stats_.Add("present.late_ms", Qpc::ToMs(Qpc::Now() - slot.presentAt));
    sleeper_.SleepUntil(slot.presentAt, kPresentHoldSpinMs);
    BOOL done = FALSE;
    const bool gpuBusy = ctx->GetData(frameDone_.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done;
    if (!gpuBusy) return;
    gpuBusyAtPresent_++;
    stats_.Add("present.gpu_busy", 1.0);
    const bool hadFullLead = slot.renderStart <= slot.vblank - Qpc::FromMs(lead_.LeadMs()) + Qpc::FromMs(kFullLeadToleranceMs);
    if (hadFullLead) lead_.OnGpuBusyAtPresent(Qpc::Now(), OutputPeriodMs());
}

void App::AfterPresents(int64_t now) {
    const PresentEvents events = monitor_.Poll(now, mode_ == PaceMode::Vrr);
    if (events.missedVblank && mode_ == PaceMode::Fixed) lead_.OnMissedVblank(now, OutputPeriodMs());
    if (events.becameComposed) ShowNotice("画面被 Windows 合成（VRR 失效）：有窗口、通知或叠加层盖在全屏画面上", kComposedNoticeSec);
    lead_.Relax(now, OutputPeriodMs());
    Report(now, false);
}

void App::ShowLoading(std::optional<float> progress) {
    if (!presenter_.PumpMessages()) g_quitRequested = true;
    if (!haveSlot_) presenter_.WaitReady();
    BlitParams bp;
    bp.outEnc = presenter_.OutputEncoding();
    bp.sdrWhiteNits = (float)options_.sdrWhiteNits;
    bp.loading = true;
    bp.loadingDim = pauseValid_ ? kPauseDim : 0.f;
    bp.loadingProgress = progress;
    bp.loadingTime = loading_ ? (float)Qpc::ToSec(Qpc::Now() - loading_->since) : 0.f;
    blit_.Draw(pauseTex_.srv.Get(), pauseTex_.w, pauseTex_.h, presenter_.BackbufferRtv(), presenter_.Width(), presenter_.Height(), bp);
    presenter_.Present(1);
    haveSlot_ = false;
}

void App::BeginPause(Loading loading) {
    pauseValid_ = false;
    if (const SourceFrame* f = source_.Frame(pacer_.NewestUniqueIndex())) {
        dev_.Context()->CopyResource(pauseTex_.tex.Get(), f->rgb.tex.Get());
        pauseValid_ = true;
    }
    loading.since = Qpc::Now();
    loading_ = std::move(loading);
    PublishStatus(loading_->since);
    audio_.SetPaused(true);
    dev_.Context()->Flush();
    dev_.WaitIdle();
}

void App::EndPause(int64_t t0, double settleSec) {
    audio_.SetPaused(false);
    loading_.reset();
    pauseValid_ = false;
    ResetStats();
    statsResetAt_ = Qpc::Now() + Qpc::FromMs(settleSec * 1000.0);
    settingsDirty_ = true;
    LOGI("ready in %.0f ms: playback resumed", Qpc::ToMs(Qpc::Now() - t0));
}

bool App::WhileLoading(const std::function<bool()>& work, LoadingIngest ingest) {
    std::atomic<bool> done{false};
    bool ok = false;
    std::thread worker([&] {
        SetThreadDescription(GetCurrentThread(), L"CameraVFX loading");
        ok = work();
        done.store(true);
    });
    int64_t lastPublish = 0;
    while (!done.load()) {
        const int64_t now = Qpc::Now();
        if (ingest == LoadingIngest::KeepSourceClock) Ingest(now);
        if (presenter_.PopKey() == VK_ESCAPE || telemetry_.PopKey() == VK_ESCAPE) g_quitRequested = true;
        ShowLoading(std::nullopt);
        if (now - lastPublish >= Qpc::FromMs(kStatusPublishMs)) {
            PublishStatus(now);
            lastPublish = now;
        }
        sleeper_.SleepUntil(now + Qpc::FromMs(kLoadingFrameMs));
    }
    worker.join();
    return ok;
}

void App::SwitchModels(int fgQuality, int srQuality) {
    const bool fgChange = fgQuality != options_.fgQuality, srChange = srQuality != options_.srQuality;
    if (!fgChange && !srChange) return;
    const int64_t t0 = Qpc::Now();
    Loading loading{"", fgChange, srChange};
    std::string models;
    if (fgChange) {
        loading.label = std::string("补帧 ") + FgQualityName(fgQuality);
        models = std::string("frame generation ") + FgQualityName(fgQuality);
    }
    if (srChange) {
        loading.label += std::string(loading.label.empty() ? "" : " · ") + "超分 " + SrQualityName(srQuality);
        models += std::string(models.empty() ? "" : ", ") + "super resolution " + SrQualityName(srQuality);
    }
    LOGI("loading Maxine models (%s): playback paused", models.c_str());
    BeginPause(std::move(loading));
    vfxReloading_ = true;
    const bool ok = WhileLoading([&] { return vfx_->Reload(fgQuality, srQuality); }, LoadingIngest::KeepSourceClock);
    vfxReloading_ = false;
    options_.fgQuality = vfx_->FgQuality();
    options_.srQuality = vfx_->SrQuality();
    if (!ok) LOGE("Maxine model load failed; keeping the previous quality");
    if (!vfx_->Loaded()) {
        LOGE("Maxine models cannot be loaded again; quitting");
        g_quitRequested = true;
        return;
    }
    ShowLoading(std::nullopt);
    WarmUpChain();
    EndPause(t0, kSettleAfterModelSwitchSec);
}

std::unique_ptr<MfCapture> App::ReopenCapture(const CaptureMode& mode) {
    const CaptureRequest request{options_.device, mode};
    std::unique_ptr<MfCapture> opened;
    WhileLoading(
        [&] {
            for (int attempt = 0; attempt < kReopenAttempts && !opened; attempt++) {
                if (attempt) Sleep(kReopenRetryMs);
                opened = OpenCapture(dev_, request);
            }
            return opened != nullptr;
        },
        LoadingIngest::None);
    return opened;
}

void App::RestartCapture(const CaptureMode& mode) {
    const int64_t t0 = Qpc::Now();
    LOGI("capture: switching to %s %ux%u @ %.2f Hz: playback paused", PixFmtName(mode.format), mode.width, mode.height, mode.rateHz);
    BeginPause(Loading{std::format("采集 {} {}x{}@{:.0f}", PixFmtName(mode.format), mode.width, mode.height, mode.rateHz)});
    const CaptureMode previous = source_.Mode();
    source_.CloseCapture();
    std::unique_ptr<MfCapture> opened = ReopenCapture(mode);
    if (opened) {
        options_.capture = mode;
    } else {
        LOGE("capture: %s %ux%u @ %.2f Hz failed; back to the previous mode", PixFmtName(mode.format), mode.width, mode.height, mode.rateHz);
        opened = ReopenCapture(previous);
    }
    if (!opened) {
        LOGE("capture: the device cannot be reopened; quitting");
        g_quitRequested = true;
        return;
    }
    const bool resized = !SameSize(opened->Info().mode, previous);
    if (resized) {
        LOGI("capture: frame size %ux%u -> %ux%u: rebuilding the processing chain", previous.width, previous.height, opened->Info().mode.width,
             opened->Info().mode.height);
        dev_.WaitIdle();
        vfx_.reset();
    }
    source_.UseCapture(std::move(opened), options_.color);
    if (resized && !SetupProcessing()) {
        LOGE("capture: processing chain rebuild failed; quitting");
        g_quitRequested = true;
        return;
    }
    ResetPacing(SameRate(previous, source_.Mode()) ? cadence_.ContentPeriod() : 1.0);
    source_.Start(Analysis());
    if (resized) WarmUpChain();
    EndPause(t0, kSettleAfterCaptureSwitchSec);
}

void App::StartAudio() {
    if (!options_.audio) {
        LOGI("audio: disabled");
        return;
    }
    const CaptureInfo* capture = source_.Capture();
    if (!capture && options_.audioIn.empty()) {
        LOGI("audio: no capture device to follow (--audio-in selects an endpoint)");
        return;
    }
    AudioOptions ao;
    ao.captureDevice = options_.audioIn;
    ao.renderDevice = options_.audioOut;
    if (capture) {
        ao.videoDeviceName = Utf8ToWide(capture->deviceName.c_str());
        ao.videoSymbolicLink = capture->symbolicLink;
    }
    ao.volume = (float)(options_.audioVolumePercent / 100.0);
    ao.bufferMs = options_.audioBufferMs;
    ao.offsetMs = options_.audioOffsetMs;
    ao.autoSync = options_.audioSync;
    ao.testTone = options_.audioTone;
    if (!audio_.Start(ao)) LOGW("audio: not started (see the warning above; --list shows the endpoints, --audio-in / --audio-out select them)");
}

}
