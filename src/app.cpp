#include "app.h"
#include "vfx/vfx_chain.h"
#include <cmath>
#include <cstdarg>
#include <exception>
#include <cwchar>
#include <shellapi.h>
#include <timeapi.h>

namespace camss {

// ------------------------------------------------------------------ app
App::App(const Options& o) : o_(o) { leadMs_ = o.leadMs; }

App::~App() {
    StopDashboard();
    audio_.Stop();
    vsync_.Stop();
    if (cap_) { cap_->Stop(); delete cap_; cap_ = nullptr; }
    // Teardown order matters with the NVIDIA driver: drain the GPU, drop the video processor
    // (it holds views on CUDA-registered textures), then the CUDA chain, then the rest.
    if (dev_.ctx()) {
        dev_.ctx()->ClearState();
        dev_.ctx()->Flush();
        if (doneQ_) {
            dev_.ctx()->End(doneQ_.Get());
            BOOL done = FALSE;
            for (int i = 0; i < 2000 && (dev_.ctx()->GetData(doneQ_.Get(), &done, sizeof(done), 0) != S_OK || !done); i++) Sleep(1);
        }
    }
    rtx_.reset();
    vpOut_ = {};
    interp_.reset();
    vfx_.reset();
    if (csv_) fclose(csv_);
    pres_.Shutdown();
}

bool App::Init(HINSTANCE inst) {
    if (!dev_.Create(o_.debug)) return false;
    {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        dev_.dev()->CreateQuery(&qd, &doneQ_);
    }
    prof_.Init(dev_.dev());

    // ---- source
    if (o_.synthetic) {
        srcW_ = o_.capW; srcH_ = o_.capH;
        synth_.Init(dev_, srcW_, srcH_);
        LOGI("source: synthetic %ux%u @ %.2f", srcW_, srcH_, o_.capFps);
    } else {
        cap_ = new MfCapture();
        CaptureRequest req;
        req.deviceSubstring = o_.device; req.deviceIndex = o_.deviceIndex;
        req.format = o_.format; req.width = o_.capW; req.height = o_.capH; req.fps = o_.capFps;
        if (!cap_->Open(dev_, req)) return false;
        cap_->decodeTimecode = o_.decodeTimecode;
        srcW_ = cap_->info().width; srcH_ = cap_->info().height;
        conv_.Init(dev_, cap_->info().format, srcW_, srcH_, o_.color);
    }
    for (auto& f : store_) f.rgb = dev_.CreateTex(srcW_, srcH_, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    interpOut_ = dev_.CreateTex(srcW_, srcH_, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);

    // ---- presentation
    hdrOut_ = o_.hdr != "none";
    PresenterOptions po;
    po.fullscreen = !o_.windowed; po.monitorIndex = o_.monitor; po.hdr10 = hdrOut_;
    po.allowTearing = true;
    if (o_.windowed) { po.windowW = 1920; po.windowH = 1080; }
    if (!pres_.Init(dev_, po, inst)) return false;
    hdrOut_ = pres_.hdr();
    blit_.Init(dev_);
    outW_ = o_.outW ? o_.outW : pres_.width();
    outH_ = o_.outH ? o_.outH : pres_.height();
    // Keep the source aspect ratio for processing output.
    if ((double)outW_ / outH_ > (double)srcW_ / srcH_) outW_ = (UINT)std::lround((double)outH_ * srcW_ / srcH_) & ~1u;
    else outH_ = (UINT)std::lround((double)outW_ * srcH_ / srcW_) & ~1u;

    // ---- frame generation
    if (o_.interp == "blend") interp_ = CreateBlendInterpolator();
    else if (o_.interp == "nvof") interp_ = CreateFlowInterpolator(o_.nvofQuality);
    if (interp_ && !interp_->Init(dev_, srcW_, srcH_)) { LOGE("interpolator %s init failed", interp_->Name()); interp_.reset(); }

    // ---- VFX (Maxine) chain: any of frame generation / VSR / TrueHDR
    if (!BuildVfx()) return false;

    // ---- RTX video (driver VSR / TrueHDR via D3D11 video processor)
    if (o_.sr == "rtx" || o_.hdr == "rtx") {
        RtxVideoOptions ro; ro.vsr = o_.sr == "rtx"; ro.trueHdr = o_.hdr == "rtx";
        if (o_.hdr == "vfx" && o_.sr == "rtx") {
            LOGE("--sr rtx after --hdr vfx is not supported (the driver VSR takes SDR input); use --sr vfx or --hdr rtx");
            return false;
        }
        // Input is whatever reaches this stage: VFX VSR output (output size) or a source-size frame.
        rtxInW_ = o_.sr == "vfx" ? outW_ : srcW_;
        rtxInH_ = o_.sr == "vfx" ? outH_ : srcH_;
        UINT vw = ro.vsr ? outW_ : rtxInW_, vh = ro.vsr ? outH_ : rtxInH_;
        DXGI_FORMAT of = ro.trueHdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        DXGI_FORMAT inF = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtx_ = std::make_unique<RtxVideo>();
        if (!rtx_->Init(dev_, rtxInW_, rtxInH_, inF, vw, vh, of, ro)) { LOGE("RTX video init failed"); rtx_.reset(); }
        else vpOut_ = dev_.CreateTex(vw, vh, of, false, true);
    }

    // ---- warm-up: first runs of the VFX effects and the driver VSR/TrueHDR initialise lazily
    // (tens of ms); do it now so the pacer never sees those stalls.
    WarmUp(true);

    // ---- pacing
    const bool haveFg = interp_ || (vfx_ && vfx_->HasFg());
    adaptive_ = o_.fgAdaptive && haveFg && !(o_.mode == PaceMode::Passthrough && !o_.autoMode);
    curK_ = ChooseMultiplier(o_.capFps);
    PacerConfig pc;
    // Auto: assume VRR until the probe decides (the initial delay below is VRR based); SetMode retimes.
    pc.mode = o_.autoMode ? PaceMode::Vrr : o_.mode;
    pc.vrrMultiplier = curK_;
    pc.snapEps = o_.snapEps;
    pc.marginMs = o_.marginMs;
    // Start near the expected steady state (VRR k-grid needs (k-1)/k of a source period,
    // arbitrary-phase fixed refresh up to a full period) so the servo converges quickly.
    pc.initialDelayMs = o_.initialDelayMs > 0 ? o_.initialDelayMs
                                               : 1000.0 / o_.capFps * (o_.mode == PaceMode::Vrr || o_.autoMode ? (double)(curK_ - 1) / curK_ : 1.0) + 7.0;
    pacer_.Configure(pc, o_.capFps);
    vsync_.Start(pres_.output(), pres_.refreshHz());
    if (cap_) {
        RepeatConfig rc;
        rc.levelThreshold = o_.repeatThreshold;
        rc.maxChangedFrac = o_.repeatMaxChanged;
        rc.maxTileChanged = o_.repeatMaxTile;
        cap_->repeatConfig = rc;
        cap_->detectRepeats = adaptive_;
    }
    if (adaptive_) LOGI("adaptive frame generation: repeated frames are detected; output = x%d of the content rate", curK_);


    if (!o_.csvPath.empty()) {
        csv_ = fopen(o_.csvPath.c_str(), "w");
        if (csv_) fprintf(csv_, "idx,present_id,render_start_ms,present_call_ms,predicted_v_ms,actual_v_ms,nA,nB,t,delay_ms,slack_ms,late,"
                                "newest_sample_ms,newest_arrival_ms,newest_uploaded_ms,content_sample_ms,render_cpu_ms,"
                                "arrival_to_scanout_ms,content_to_scanout_ms,hold\n");
    }
    LOGI("pipeline: mode=%s interp=%s%s sr=%s%s hdr=%s src=%ux%u out=%ux%u display=%ux%u@%.0fHz",
         o_.autoMode ? "auto" : PaceModeName(o_.mode), o_.interp.c_str(),
         o_.interp == "vfx" ? (std::string("(") + FgQualityName(o_.vfgMode) + ")").c_str() : "",
         o_.sr.c_str(), o_.sr == "vfx" ? (std::string("(") + SrQualityName(o_.srQuality) + ")").c_str() : "",
         o_.hdr.c_str(), srcW_, srcH_, outW_, outH_, pres_.width(), pres_.height(), pres_.refreshHz());
    return true;
}

bool App::BuildVfx() {
    const bool vfxFg = o_.interp == "vfx", vfxSr = o_.sr == "vfx", vfxHdr = o_.hdr == "vfx";
    if (!(vfxFg || vfxSr || vfxHdr)) return true;
    if (vfxSr && o_.srQuality >= 8 && o_.srQuality <= 15 && (outW_ != srcW_ || outH_ != srcH_)) {
        LOGE("sr-quality %s works only without upscaling (output %ux%u != source %ux%u); use low..ultra, hb-*, streaming-* or --out %ux%u",
             SrQualityName(o_.srQuality), outW_, outH_, srcW_, srcH_, srcW_, srcH_);
        return false;
    }
    VfxChain::Config c;
    c.fg = vfxFg; c.fgMode = o_.vfgMode;
    c.sr = vfxSr; c.srQuality = o_.srQuality;
    c.hdr = vfxHdr; c.hdrBeforeSr = o_.hdrBeforeSr;
    c.srcW = srcW_; c.srcH = srcH_;
    c.outW = vfxSr ? outW_ : srcW_; c.outH = vfxSr ? outH_ : srcH_;
    vfx_ = std::make_unique<VfxChain>();
    if (!vfx_->Init(dev_, c, store_.data(), kStore)) { LOGE("VFX chain init failed"); vfx_.reset(); return false; }
    return true;
}

static void WaitGpuIdle(Device& dev) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> q;
    if (FAILED(dev.dev()->CreateQuery(&qd, &q))) return;
    dev.ctx()->End(q.Get());
    BOOL done = FALSE;
    const int64_t until = Qpc::Now() + Qpc::FromMs(2000);
    while ((dev.ctx()->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) && Qpc::Now() < until) Sleep(0);
}

void App::WarmUp(bool initial) {
    const int64_t t0 = Qpc::Now();
    if (initial) {
        // Positive frame numbers mapping to slots 0/1 (n % kStore must never be negative).
        store_[0].n = (int64_t)kStore * 100000; store_[1].n = store_[0].n + 1;
        for (int i = 0; i < 4; i++) {
            if (vfx_) { vfx_->Ingest(store_[0]); vfx_->Ingest(store_[1]); vfx_->Process(store_[0], &store_[1], 0.5f, true); }
            if (rtx_ && rtxInW_ == srcW_ && rtxInH_ == srcH_) rtx_->Process(store_[0].rgb.tex.Get(), vpOut_.tex.Get());
            if (interp_) { interp_->Prepare(store_[0], store_[1]); interp_->Interpolate(store_[0], store_[1], 0.5f, interpOut_); }
            dev_.ctx()->Flush();
        }
        WaitGpuIdle(dev_);
        store_[0].n = store_[1].n = -1;
    } else if (vfx_) {
        // Live rebuild: the store holds real frames; hand them to the new chain and run it on the
        // newest pair without touching the store.
        SourceFrame* a = nullptr; SourceFrame* b = nullptr;
        for (auto& f : store_) {
            if (f.n < 0) continue;
            vfx_->Ingest(f);
            if (!b || f.n > b->n) { a = b; b = &f; } else if (!a || f.n > a->n) a = &f;
        }
        if (a && b && a->n > b->n) std::swap(a, b);
        for (int i = 0; i < 4 && b; i++) { vfx_->Process(a ? *a : *b, a ? b : nullptr, 0.5f, true); dev_.ctx()->Flush(); }
        WaitGpuIdle(dev_);
    }
    if (vfx_) vfx_->PopTimings();
    LOGI("warm-up of processing stages: %.0f ms", Qpc::ToMs(Qpc::Now() - t0));
}

void App::RebuildVfx(int oldFg, int oldSr) {
    LOGI("reloading Maxine models: frame generation %s, super resolution %s ...",
         o_.interp == "vfx" ? FgQualityName(o_.vfgMode) : "-", o_.sr == "vfx" ? SrQualityName(o_.srQuality) : "-");
    dev_.ctx()->Flush();
    WaitGpuIdle(dev_);
    if (rtx_) rtx_->ClearViewCache();   // views may reference the old chain's output texture
    vfx_.reset();
    if (!BuildVfx()) {
        LOGE("Maxine chain rebuild failed; going back to the previous quality");
        o_.vfgMode = oldFg; o_.srQuality = oldSr;
        if (!BuildVfx()) {
            LOGE("Maxine chain unavailable; continuing without frame generation / super resolution");
            if (!interp_) { adaptive_ = false; if (cap_) cap_->detectRepeats = false; curK_ = 1; pacer_.SetMultiplier(1); pacer_.SetContentPeriod(1.0); }
            stats_.Clear();
            return;
        }
    }
    WarmUp(false);
    stats_.Clear();
}

void App::StartAudio() {
    if (!o_.audio) { LOGI("audio: disabled"); return; }
    AudioOptions ao;
    ao.captureDevice = o_.audioDevice;
    ao.renderDevice = o_.audioOut;
    if (cap_) { ao.videoDeviceName = Utf8ToWide(cap_->info().deviceName.c_str()); ao.videoSymbolicLink = cap_->info().symbolicLink; }
    ao.volume = (float)(o_.audioVolume / 100.0);
    ao.bufferMs = o_.audioBufferMs;
    ao.delayMs = o_.audioDelayMs;
    ao.autoSync = o_.audioSync;
    ao.testTone = o_.audioTone;
    if (!o_.synthetic || !ao.captureDevice.empty()) {
        if (!audio_.Start(ao)) LOGW("audio: not started (see the warning above; --list shows the endpoints, --audio-device / --audio-out select them)");
    }
}

int App::ChooseMultiplier(double contentFps) const {
    int k = o_.fgMultiplier;
    if (o_.fgTargetFps > 0 && contentFps > 0) k = (int)std::lround(o_.fgTargetFps / contentFps);
    k = std::clamp(k, 1, 8);
    // Never ask a VRR display for more than its maximum refresh rate.
    const double maxHz = pres_.refreshHz() > 0 ? pres_.refreshHz() : 1000.0;
    while (k > 1 && contentFps * k > maxHz + 0.5) k--;
    return k;
}

void App::OnContentRateChanged() {
    const double pc = cadence_.ContentPeriod();
    const double sigHz = pacer_.SourceHz() > 0 ? pacer_.SourceHz() : o_.capFps;
    const double contentFps = sigHz / pc;
    const int k = ChooseMultiplier(contentFps);
    pacer_.SetContentPeriod(pc);
    pacer_.SetMultiplier(k);
    curK_ = k;
    BumpGeneration();
    if (o_.mode == PaceMode::Fixed)
        LOGI("content rate %.2f fps (1 new frame per %.2f source frames) -> interpolated to the %.0f Hz refresh", contentFps, pc, pres_.refreshHz());
    else
        LOGI("content rate %.2f fps (1 new frame per %.2f source frames) -> output %.1f fps (x%d)", contentFps, pc, contentFps * k, k);
}

SourceFrame* App::Frame(int64_t n) {
    if (n < 0) return nullptr;
    SourceFrame& f = store_[(size_t)(n % kStore)];
    return f.n == n ? &f : nullptr;
}

void App::IngestCaptured(const RawFrame& rf) {
    const int64_t n = rf.sourceIndex;
    stats_.Add("cap.upload_ms", Qpc::ToMs(rf.uploadedQpc - rf.arrivalQpc));
    double drv = Qpc::ToMs(rf.arrivalQpc - rf.sampleTimeQpc);
    if (drv > -5 && drv < 500) stats_.Add("cap.sample_to_arrival_ms", drv);
    if (lastArrival_) stats_.Add("cap.interval_ms", Qpc::ToMs(rf.arrivalQpc - lastArrival_));
    lastArrival_ = rf.arrivalQpc;
    if (rf.timecodeValid) {
        // Display time encoded in 10 us units (mod 2^32); compare with arrival.
        uint32_t arr = (uint32_t)(int64_t)(Qpc::ToMs(rf.arrivalQpc) * 100.0);
        int32_t diff = (int32_t)(arr - rf.timecode);
        if (diff > 0 && diff < 100000) { stats_.Add("loop.scanout_to_arrival_ms", diff / 100.0); timecodeHits_++; }
    }
    if (adaptive_) stats_.Add("cap.repeat_check_ms", rf.repeatCheckMs);

    // A repeated frame (content slower than the signal) only advances the signal clock: it is not
    // converted, stored or interpolated from; the pair is formed by the real content frames.
    // A frame the app never saw (pending list overflow) may have carried new content: the capture
    // side compares against content the app does not have, so treat frames as new until one is stored.
    if (lastSeq_ && rf.seq != lastSeq_ + 1) contentLost_ = true;
    lastSeq_ = rf.seq;
    const bool unique = !(adaptive_ && rf.repeat) || contentLost_;
    if (!unique) {
        repeats_++;
        pacer_.OnSourceFrame(n, rf.uploadedQpc, false);
        if (cadence_.OnFrame(n, false)) OnContentRateChanged();
        return;
    }
    SourceFrame& f = store_[(size_t)(n % kStore)];
    if (!cap_->LockSlot(rf)) {
        stats_.Add("cap.overwritten", 1);
        if (!rf.repeat || contentLost_) contentLost_ = true;   // its repeats must replace it (see above)
        return;
    }
    contentLost_ = false;
    conv_.Convert(cap_->PlaneSrv(rf.slot, 0), cap_->PlaneCount() > 1 ? cap_->PlaneSrv(rf.slot, 1) : nullptr, f.rgb);
    cap_->UnlockSlot(rf);
    f.n = n; f.seq = rf.seq; f.generation++;
    f.sampleQpc = rf.sampleTimeQpc; f.arrivalQpc = rf.arrivalQpc; f.uploadedQpc = rf.uploadedQpc;
    f.readyQpc = rf.uploadedQpc;
    pacer_.OnSourceFrame(n, rf.uploadedQpc, true);
    if (adaptive_ && cadence_.OnFrame(n, true)) OnContentRateChanged();

    // Precompute for the new pair (previous content frame, this one).
    if (vfx_) vfx_->Ingest(f);
    if (interp_) if (SourceFrame* prev = Frame(lastUniqueN_)) interp_->Prepare(*prev, f);
    lastUniqueN_ = n;
}

void App::Ingest(int64_t now) {
    if (o_.synthetic) {
        const int64_t per = (int64_t)((double)Qpc::Freq() / o_.capFps);
        const double contentFps = o_.syntheticContentFps > 0 ? std::min(o_.syntheticContentFps, o_.capFps) : o_.capFps;
        if (!synthT0_) { synthT0_ = now; synthNext_ = now; }
        while (now >= synthNext_) {
            int64_t n = synthN_++;
            // Content frame shown by signal frame n (e.g. 30 fps content: every frame twice).
            const int64_t c = (int64_t)std::floor((double)n * contentFps / o_.capFps + 1e-6);
            const bool unique = c != synthContent_;
            synthContent_ = c;
            const int64_t tick = synthNext_;
            synthNext_ += per;
            if (!unique && adaptive_) {
                repeats_++;
                pacer_.OnSourceFrame(n, tick, false);
                if (cadence_.OnFrame(n, false)) OnContentRateChanged();
                continue;
            }
            SourceFrame& f = store_[(size_t)(n % kStore)];
            synth_.Render(f.rgb, (double)c / contentFps);
            f.n = n; f.seq = (uint64_t)n + 1; f.generation++;
            // A clock-driven source: frame n exists from its tick time on (like a capture
            // card delivering at exactly 60 Hz); rendering it lazily here does not change that.
            f.sampleQpc = tick; f.arrivalQpc = tick; f.uploadedQpc = tick; f.readyQpc = tick;
            pacer_.OnSourceFrame(n, tick, true);
            if (adaptive_ && cadence_.OnFrame(n, true)) OnContentRateChanged();
            if (vfx_) vfx_->Ingest(f);
            if (interp_) if (SourceFrame* prev = Frame(lastUniqueN_)) interp_->Prepare(*prev, f);
            lastUniqueN_ = n;
        }
        return;
    }
    for (const RawFrame& rf : cap_->Drain()) IngestCaptured(rf);
}

bool App::Render(const PaceDecision& d, int64_t renderStart, int64_t V, UINT sync, bool tearing, int64_t presentAt) {
    auto* ctx = dev_.ctx();
    SourceFrame* A = Frame(d.nA);
    SourceFrame* B = d.nB != d.nA ? Frame(d.nB) : nullptr;
    if (!A && B) { A = B; B = nullptr; }   // A's store slot was reused (long static scene): show B
    if (!A) return false;
    const bool wantInterp = interpOn_ && B && d.t > 0.f;
    bool doInterp = false;   // what actually ran (drives latency accounting)

    prof_.Begin(ctx);
    ID3D11ShaderResourceView* finalSrv = A->rgb.srv.Get();
    ID3D11Texture2D* stageTex = A->rgb.tex.Get();
    UINT finalW = srcW_, finalH = srcH_;
    Encoding enc = Encoding::SdrGamma;

    // D3D interpolators (blend / NVOF) run first; with a VFX chain their output feeds VFX HDR/SR.
    const Device::Tex* external = nullptr;
    if (wantInterp && interp_ && !(vfx_ && vfx_->HasFg())) {
        interp_->Interpolate(*A, *B, d.t, interpOut_);
        finalSrv = interpOut_.srv.Get(); stageTex = interpOut_.tex.Get();
        external = &interpOut_;
        doInterp = true;
        prof_.Mark(ctx, "interp");
    }
    if (vfx_) {
        int64_t t0 = Qpc::Now();
        const bool fg = wantInterp && vfx_->HasFg();
        const Device::Tex* out = vfx_->Process(*A, fg ? B : nullptr, fg ? d.t : 0.f, srOn_, external);
        stats_.Add("vfx.cpu_ms", Qpc::ToMs(Qpc::Now() - t0));
        for (auto& kv : vfx_->PopTimings()) {
            stats_.Add(kv.first, kv.second);
            if (!strcmp(kv.first, "vfx.gpu_total")) lastVfxGpuMs_ = kv.second;
        }
        if (out) {
            finalSrv = out->srv.Get(); stageTex = out->tex.Get(); finalW = out->w; finalH = out->h;
            if (vfx_->LastOutputPq()) enc = Encoding::Pq;
            if (vfx_->LastFgRan()) doInterp = true;
        } else if (!external) {
            doInterp = false;   // chain failed: the plain source frame A is shown
        }
        prof_.Mark(ctx, "vfx");
    }
    if (rtx_ && (srOn_ || o_.hdr == "rtx") && finalW == rtxInW_ && finalH == rtxInH_ && enc == Encoding::SdrGamma) {
        int64_t t0 = Qpc::Now();
        rtx_->SetVsr(srOn_);
        if (rtx_->Process(stageTex, vpOut_.tex.Get())) {
            finalSrv = vpOut_.srv.Get(); finalW = vpOut_.w; finalH = vpOut_.h;
            if (vpOut_.fmt == DXGI_FORMAT_R10G10B10A2_UNORM) enc = Encoding::Pq;
        }
        stats_.Add("rtx.cpu_ms", Qpc::ToMs(Qpc::Now() - t0));
        prof_.Mark(ctx, "rtxvideo");
    }

    BlitParams bp;
    bp.inEnc = enc;
    bp.outEnc = hdrOut_ ? Encoding::Pq : Encoding::SdrGamma;
    bp.sdrWhiteNits = o_.sdrWhiteNits;
    bp.timecode = o_.timecode;
    bp.timecodeValue = (uint32_t)(int64_t)(Qpc::ToMs(V) * 100.0);
    bp.marker = o_.marker;
    bp.markerOn = (d.nA / 30) & 1;
    blit_.Draw(finalSrv, finalW, finalH, pres_.BackbufferRtv(), pres_.width(), pres_.height(), bp);
    prof_.Mark(ctx, "blit");
    prof_.End(ctx);
    if (doneQ_) ctx->End(doneQ_.Get());     // signals when this frame's GPU work (incl. CUDA) is done

    const int64_t submitDone = Qpc::Now();
    if (presentAt) {
        dev_.ctx()->Flush();                       // get the GPU going while we wait
        if (Qpc::Now() > presentAt) stats_.Add("present.late_ms", Qpc::ToMs(Qpc::Now() - presentAt));
        sleeper_.SleepUntil(presentAt, 0.25);
        // Under VRR the flip happens when the GPU finishes: if it is still busy at the scheduled
        // time, the frame will be shown late -> the render lead is too small.
        BOOL done = FALSE;
        if (doneQ_ && (ctx->GetData(doneQ_.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done)) {
            gpuBusyAtPresent_++;
            stats_.Add("present.gpu_busy", 1.0);
            leadMs_ = std::min(leadMs_ + 0.25, 0.75 * 1000.0 / o_.capFps);
            lastLeadAdjust_ = Qpc::Now();
        }
    }
    const int64_t presentCall = Qpc::Now();
    HRESULT hr = pres_.Present(sync, tearing);
    const int64_t after = Qpc::Now();
    haveSlot_ = false;
    if (FAILED(hr)) return false;

    OutRecord r;
    r.idx = outCount_++;
    r.presentId = pres_.LastPresentCount();
    r.renderStart = renderStart; r.presentCall = presentCall; r.predictedV = V;
    r.nA = d.nA; r.nB = doInterp ? d.nB : d.nA; r.t = doInterp ? d.t : 0.f;
    r.delayMs = d.delayMs; r.slackMs = pacer_.MinSlackMs(); r.late = d.late;
    r.hold = d.stall;   // static content / stalled source: the shown frame is old by design
    const SourceFrame* newest = (doInterp && B) ? B : A;
    r.newestArrival = newest->arrivalQpc; r.newestSample = newest->sampleQpc; r.newestUploaded = newest->uploadedQpc;
    r.contentSample = doInterp ? (int64_t)((double)A->sampleQpc + (double)(B->sampleQpc - A->sampleQpc) * d.t) : A->sampleQpc;
    r.renderCpuMs = Qpc::ToMs(submitDone - renderStart);   // excludes the deliberate present hold
    r.gpuMs = vfx_ ? (float)lastVfxGpuMs_ : 0.f;
    r.interp = doInterp;
    records_.push_back(r);

    stats_.Add("render.cpu_ms", r.renderCpuMs);
    // Per-frame cost for the render-lead estimate: CPU submit time or the (async) CUDA chain, whichever is longer.
    if (outCount_ > 60) frameCost_.Add(std::max(r.renderCpuMs, lastVfxGpuMs_ + 0.3));
    stats_.Add("present.call_ms", Qpc::ToMs(after - presentCall));
    if (lastPresentCall_) {
        double iv = Qpc::ToMs(presentCall - lastPresentCall_);
        stats_.Add("present.interval_ms", iv);
        presentIntervalEma_ = presentIntervalEma_ <= 0 ? iv : 0.9 * presentIntervalEma_ + 0.1 * std::min(iv, 100.0);
    }
    lastPresentCall_ = presentCall;
    if (!r.hold) stats_.Add("lat.arrival_to_present_ms", Qpc::ToMs(presentCall - newest->arrivalQpc));
    else stats_.Add("pace.hold", 1.0);
    stats_.Add("pace.delay_ms", d.delayMs);
    if (doInterp) stats_.Add("pace.t", d.t);
    prof_.Collect(ctx, [&](const char* name, double ms) {
        char key[64];
        int n = snprintf(key, sizeof(key), "gpu.%s", name);   // no allocation per frame
        stats_.Add(std::string_view(key, (size_t)std::clamp(n, 0, (int)sizeof(key) - 1)), ms);
    });
    return true;
}

void App::FinalizeRecord(OutRecord& r) {
    r.finalized = true;
    finalizedCount_++;
    {
        // Dashboard / record: one sample per output frame (lock-free, never blocks).
        FrameSample fs;
        fs.qpc = r.presentCall;
        const bool flip = o_.mode == PaceMode::Vrr || vrrActive_ || (o_.mode == PaceMode::Passthrough && o_.passSync == 0);
        const int64_t shown = flip ? r.presentCall : r.actualV;
        if (shown) {
            fs.latencyMs = (float)Qpc::ToMs(shown - r.newestArrival);
            const double cs = Qpc::ToMs(shown - r.contentSample);
            if (cs > 0 && cs < 500) fs.contentMs = (float)cs;
        }
        fs.delayMs = (float)r.delayMs;
        fs.gpuMs = r.gpuMs;
        fs.t = r.t;
        fs.flags = (uint8_t)((r.late ? FrameSample::kLate : 0) | (r.hold ? FrameSample::kHold : 0) | (r.interp ? FrameSample::kInterp : 0));
        tel_.PushFrame(fs);
    }
    // A held picture (static content, stalled source) is old by design: not a pipeline latency.
    if (!r.hold && (o_.mode == PaceMode::Vrr || vrrActive_ || (o_.mode == PaceMode::Passthrough && o_.passSync == 0))) {
        // VRR / immediate: the flip (scan-out start) happens at Present() once the GPU is done;
        // in VRR mode the present is held until the scheduled time, after the GPU work.
        stats_.Add("lat.arrival_to_flip_ms", Qpc::ToMs(r.presentCall - r.newestArrival));
        double cs = Qpc::ToMs(r.presentCall - r.contentSample);
        if (cs > 0 && cs < 500) stats_.Add("lat.content_to_flip_ms", cs);
    }
    if (r.actualV) {
        if (!r.hold) {
            stats_.Add("lat.arrival_to_scanout_ms", Qpc::ToMs(r.actualV - r.newestArrival));
            double cs = Qpc::ToMs(r.actualV - r.contentSample);
            if (cs > 0 && cs < 500) stats_.Add("lat.content_to_scanout_ms", cs);
        }
        stats_.Add("lat.present_to_scanout_ms", Qpc::ToMs(r.actualV - r.presentCall));
        double err = Qpc::ToMs(r.actualV - r.predictedV);
        stats_.Add("present.vblank_error_ms", err);
        if (o_.mode != PaceMode::Vrr && err > 0.5 * 1000.0 / pres_.refreshHz()) {
            missed_++;
            lastMissQpc_ = Qpc::Now();
        }
    }
    if (csv_) {
        auto ms = [&](int64_t q) { return q ? Qpc::ToMs(q - startQpc_) : 0.0; };
        fprintf(csv_, "%llu,%u,%.3f,%.3f,%.3f,%.3f,%lld,%lld,%.4f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n",
                (unsigned long long)r.idx, r.presentId, ms(r.renderStart), ms(r.presentCall), ms(r.predictedV), ms(r.actualV),
                (long long)r.nA, (long long)r.nB, r.t, r.delayMs, r.slackMs, r.late ? 1 : 0, ms(r.newestSample), ms(r.newestArrival),
                ms(r.newestUploaded), ms(r.contentSample), r.renderCpuMs,
                r.actualV ? Qpc::ToMs(r.actualV - r.newestArrival) : 0.0, r.actualV ? Qpc::ToMs(r.actualV - r.contentSample) : 0.0, r.hold ? 1 : 0);
    }
}

void App::PollPresentStats() {
    DXGI_FRAME_STATISTICS st{};
    bool ok = pres_.FrameStats(st);
    if (o_.statsDebug && outCount_ < 40 && !records_.empty()) {
        const auto& last = records_.back();
        LOGI("stats: ok=%d myPresentId=%u presentCall=%.3f | PresentCount=%u PresentRefreshCount=%u SyncRefreshCount=%u SyncQPC=%.3f (rel to my present %+.3f ms)",
             ok, last.presentId, Qpc::ToMs(last.presentCall - startQpc_), st.PresentCount, st.PresentRefreshCount, st.SyncRefreshCount,
             Qpc::ToMs(st.SyncQPCTime.QuadPart - startQpc_), Qpc::ToMs(st.SyncQPCTime.QuadPart - last.presentCall));
    }
    if (ok && st.PresentCount) {
        // Detect VRR: one refresh per present (instead of refresh/fps refreshes per present).
        if (lastStatsPresent_ && st.PresentCount > lastStatsPresent_) {
            double ratio = (double)(st.SyncRefreshCount - lastStatsRefresh_) / (double)(st.PresentCount - lastStatsPresent_);
            vrrRatio_ = 0.9 * vrrRatio_ + 0.1 * ratio;
        }
        lastStatsPresent_ = st.PresentCount; lastStatsRefresh_ = st.SyncRefreshCount;
        // The ratio only discriminates while we present slower than the refresh rate
        // (fixed refresh: ~refresh/fps refreshes per present; VRR: exactly 1).
        const bool slowPresents = presentIntervalEma_ > 1.5 * 1000.0 / pres_.refreshHz();
        if (slowPresents) {
            const bool vrrActive = vrrActive_ ? vrrRatio_ < 1.5 : vrrRatio_ < 1.2;   // hysteresis
            if (vrrActive != vrrActive_) { vrrActive_ = vrrActive; LOGI("display VRR %s (refreshes/present %.2f)", vrrActive ? "ACTIVE" : "inactive", vrrRatio_); }
        }
        // Fixed refresh: SyncQPCTime is the vblank at which present #PresentCount became visible.
        // Under VRR it is the start of the (stretched) vblank instead, so it cannot be used per frame.
        if (!vrrActive_ && o_.mode != PaceMode::Vrr && !(o_.mode == PaceMode::Passthrough && o_.passSync == 0)) {
            for (auto& r : records_)
                if (r.presentId == st.PresentCount && !r.actualV && st.SyncQPCTime.QuadPart >= r.presentCall) r.actualV = st.SyncQPCTime.QuadPart;
        }
    }
    while (!records_.empty()) {
        auto& r = records_.front();
        if (r.actualV || records_.size() > 8) { if (!r.finalized) FinalizeRecord(r); records_.pop_front(); }
        else break;
    }
    // Late-latch lead adaptation (AIMD on missed vblanks).
    const int64_t now = Qpc::Now();
    if (lastMissQpc_ && now - lastMissQpc_ < Qpc::FromMs(5) && now - lastLeadAdjust_ > Qpc::FromMs(100)) {
        leadMs_ = std::min(leadMs_ + 0.5, 1000.0 / pres_.refreshHz());
        lastLeadAdjust_ = now;
        lastMissQpc_ = 0;
    } else if (now - lastLeadAdjust_ > Qpc::FromMs(3000)) {
        double p50, p99;
        double floorMs = 1.0;
        if (frameCost_.Count() > 30) floorMs = frameCost_.Quantile(0.99) + 0.75;
        (void)p50; (void)p99;
        leadMs_ = std::max(floorMs, leadMs_ - 0.1);
        lastLeadAdjust_ = now;
    }
}

// snprintf accumulation that can never run past the buffer.
static int Append(char* buf, size_t size, int n, const char* fmt, ...) {
    if (n < 0) n = 0;
    if ((size_t)n >= size) return (int)size - 1;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf + n, size - (size_t)n, fmt, ap);
    va_end(ap);
    return r < 0 ? n : (int)std::min<size_t>((size_t)n + (size_t)r, size - 1);
}

void App::PrintDetail() {
    const int64_t now = Qpc::Now();
    char head[512];
    snprintf(head, sizeof(head),
             "---- %.1fs  mode=%s interp=%s%s sr=%s%s hdr=%s | src %.3f Hz (jit %.2f ms) content Pc=%.2f x%d disp %.3f Hz (jit %.2f ms) | "
             "D=%.2f ms minslack=%.2f lead=%.2f late=%llu stall=%llu missed=%llu gpubusy=%llu drops=%llu repeats=%llu%s",
             Qpc::ToSec(now - startQpc_), PaceModeName(o_.mode), o_.interp.c_str(), interpOn_ ? "" : "(off)", o_.sr.c_str(),
             srOn_ ? "" : "(off)", o_.hdr.c_str(), pacer_.SourceHz(), pacer_.SourceJitterMs(), pacer_.ContentPeriod(), curK_,
             vsync_.MeasuredHz(), vsync_.JitterMs(), pacer_.DelayMs(), pacer_.MinSlackMs(), leadMs_, (unsigned long long)pacer_.LateCount(),
             (unsigned long long)pacer_.StallCount(), (unsigned long long)missed_, (unsigned long long)gpuBusyAtPresent_,
             (unsigned long long)(cap_ ? cap_->DroppedFrames() : 0), (unsigned long long)repeats_,
             o_.decodeTimecode ? (timecodeHits_ ? " tc=ok" : " tc=none") : "");
    LOGI("%s\n%s", head, stats_.Report().c_str());
}

void App::Report(bool final) {
    const int64_t now = Qpc::Now();
    if (!warmupDone_ && Qpc::ToSec(now - startQpc_) > o_.warmupSec) {
        warmupDone_ = true;
        stats_.Clear();
        LOGI("warm-up (%.0f s) done: statistics reset", o_.warmupSec);
    }
    if (now - lastStatusPub_ >= Qpc::FromMs(100)) PublishStatus(now);
    // The status text is refreshed every second (pinned status line / dashboard); it is logged as a
    // line every `report` seconds only on a non-interactive console (redirected output, log file).
    const double interval = statusLine_ || tui_ ? 1.0 : (o_.reportSec > 0 ? o_.reportSec : 2.0);
    if (!final && now - lastReport_ < Qpc::FromMs(interval * 1000.0)) return;
    const bool logLine = final || (!statusLine_ && !tui_ && o_.reportSec > 0);
    const double dt = Qpc::ToSec(now - lastReport_);
    double outFps = dt > 0.25 ? (double)(outCount_ - lastReportOut_) / dt : lastOutFps_;   // final report may follow a periodic one
    lastOutFps_ = outFps;
    lastReport_ = now;
    lastReportOut_ = outCount_;

    // Latency: scan-out based where the display reports it (fixed refresh), flip based under VRR.
    double p50 = 0, p99 = 0, c50 = 0, c99 = 0;
    const char* metric = "arrival->scanout";
    bool haveLat = stats_.Get("lat.arrival_to_scanout_ms", p50, p99);
    bool haveContent = stats_.Get("lat.content_to_scanout_ms", c50, c99);
    if (!haveLat) { haveLat = stats_.Get("lat.arrival_to_flip_ms", p50, p99); metric = "arrival->flip"; }
    if (!haveContent) haveContent = stats_.Get("lat.content_to_flip_ms", c50, c99);
    // Audio follows the video path (capture timestamp -> display).
    if (audio_.Running() && haveContent) audio_.SetVideoLatencyMs(c50);
    // Stage breakdown for the dashboard (p50 over the recent window).
    {
        double a, b;
        stCapture_ = stats_.Get("cap.sample_to_arrival_ms", a, b) ? a : -1;
        stUpload_ = stats_.Get("cap.upload_ms", a, b) ? a : -1;
        stFg_ = stats_.Get("vfx.fg", a, b) ? a : -1;
        stSr_ = stats_.Get("vfx.sr", a, b) ? a : -1;
        stHdr_ = stats_.Get("vfx.hdr", a, b) ? a : -1;
        stGpu_ = stats_.Get("vfx.gpu_total", a, b) ? a : 0;
        if (audio_.Running()) audioStats_ = audio_.Stats();
    }

    const double sigHz = pacer_.SourceHz();
    const double contentFps = sigHz > 0 ? sigHz / pacer_.ContentPeriod() : 0;
    const char* modeName = o_.mode == PaceMode::Vrr ? "vrr" : o_.mode == PaceMode::Fixed ? "fixed" : "passthrough";
    char line[1024];
    int n = snprintf(line, sizeof(line), "in %.2f Hz", sigHz);
    if (adaptive_) {
        if (cadence_.UniqueFraction() < 0.05) n = Append(line, sizeof(line), n, " | content static");
        else n = Append(line, sizeof(line), n, " | content %.1f fps", contentFps);
    }
    n = Append(line, sizeof(line), n, " -> out %.1f fps (%s%s", outFps, modeName, o_.mode == PaceMode::Passthrough ? ")" : "");
    if (o_.mode == PaceMode::Vrr) n = Append(line, sizeof(line), n, ", x%d)", curK_);
    else if (o_.mode == PaceMode::Fixed) n = Append(line, sizeof(line), n, ")");
    if (haveLat) n = Append(line, sizeof(line), n, " | %s %.1f/%.1f ms", metric, p50, p99);
    if (o_.mode != PaceMode::Passthrough) n = Append(line, sizeof(line), n, " | D %.1f lead %.1f", pacer_.DelayMs(), leadMs_);
    double g50, g99;
    if (stats_.Get("vfx.gpu_total", g50, g99)) {
        n = Append(line, sizeof(line), n, " | gpu %.2f/%.2f ms", g50, g99);
        // The processing must fit into one output interval, or frames are skipped.
        const double budget = o_.mode == PaceMode::Vrr && contentFps > 0 ? 1000.0 / (contentFps * curK_)
                            : o_.mode == PaceMode::Fixed ? 1000.0 / std::max(1.0, pres_.refreshHz()) : 0;
        if (budget > 0 && g50 > 0.9 * budget) {
            LOG_EVERY_MS(LogLevel::Warn, 10000, "GPU time %.1f ms per output frame exceeds the %.1f ms frame budget: frames are skipped; "
                         "lower fg-quality / sr-quality (keys F / Q) or the multiplier", g50, budget);
            char nb[160];
            snprintf(nb, sizeof(nb), "GPU %.1f ms > budget %.1f ms: frames skipped (F / Q lower the quality)", g50, budget);
            notice_ = nb;
            noticeUntil_ = now + Qpc::FromMs(3000);
        }
    }
    n = Append(line, sizeof(line), n, " | late %llu hold %llu drop %llu", (unsigned long long)pacer_.LateCount(),
                  (unsigned long long)pacer_.StallCount(), (unsigned long long)(cap_ ? cap_->DroppedFrames() : 0));
    if (o_.mode == PaceMode::Fixed) n = Append(line, sizeof(line), n, " miss %llu", (unsigned long long)missed_);
    if (audio_.Running()) {
        const AudioStats& as = audioStats_;
        if (!as.running) n = Append(line, sizeof(line), n, " | audio: reopening device");
        else if (!as.playing) n = Append(line, sizeof(line), n, " | audio: buffering");
        else n = Append(line, sizeof(line), n, " | audio %.0f ms%s buf %.0f ms", as.latencyMs, audio_.Muted() ? " (muted)" : "", as.bufferMs);
        if (as.running && as.syncDelayMs > 0.5) n = Append(line, sizeof(line), n, " sync +%.0f", as.syncDelayMs);
        if (as.underruns) n = Append(line, sizeof(line), n, " underrun %llu", (unsigned long long)as.underruns);
    }
    if (o_.decodeTimecode) {
        double l50, l99;
        if (stats_.Get("loop.scanout_to_arrival_ms", l50, l99)) n = Append(line, sizeof(line), n, " | loop %.1f/%.1f ms", l50, l99);
        else n = Append(line, sizeof(line), n, " | loop: no timecode");
    }
    if (final || (o_.reportDetail && !tui_)) PrintDetail();
    if (statusLine_ && !final) SetStatusLine(line);
    if (logLine && !tui_) LOGI("%s", line);

    char title[256];
    char kk[16] = "";
    if (o_.mode == PaceMode::Vrr) snprintf(kk, sizeof(kk), " x%d", curK_);
    snprintf(title, sizeof(title), "camss | %s%s | %s p50 %.1f p99 %.1f ms | D %.1f ms", modeName, kk, metric, p50, p99,
             pacer_.DelayMs());
    pres_.SetTitle(title);
}

void App::HandleKeys() {
    int k = pres_.PopKey();
    if (!k) k = tel_.PopKey();   // keys typed into the dashboard
    switch (k) {
    case VK_ESCAPE: g_quitRequested = true; break;
    case 'I': interpOn_ = !interpOn_; BumpGeneration(); LOGI("frame generation %s", interpOn_ ? "on" : "off"); break;
    case 'S': srOn_ = !srOn_; BumpGeneration(); LOGI("super resolution %s", srOn_ ? "on" : "off"); break;
    case 'F':
        if (o_.interp != "vfx") { LOGI("F: frame generation quality applies to --interp vfx"); break; }
        { const int of = o_.vfgMode; o_.vfgMode = (o_.vfgMode + 1) % 3; RebuildVfx(of, o_.srQuality); BumpGeneration(); }
        break;
    case 'Q':
        if (o_.sr != "vfx") { LOGI("Q: super resolution quality applies to --sr vfx"); break; }
        {
            const int nq = NextSrQuality(o_.srQuality);
            if (nq == o_.srQuality) { LOGI("Q: no other level in the %s family", SrQualityName(nq)); break; }
            const int os = o_.srQuality; o_.srQuality = nq; RebuildVfx(o_.vfgMode, os); BumpGeneration();
        }
        break;
    case 'M':
        if (audio_.Running()) { audio_.SetMuted(!audio_.Muted()); LOGI("audio %s", audio_.Muted() ? "muted" : "unmuted"); }
        break;
    case VK_OEM_PLUS: case VK_ADD: case VK_OEM_MINUS: case VK_SUBTRACT:
        if (audio_.Running()) {
            const bool up = k == VK_OEM_PLUS || k == VK_ADD;
            const float v = std::clamp(audio_.Volume() + (up ? 0.1f : -0.1f), 0.0f, 2.0f);
            audio_.SetVolume(v);
            LOGI("audio volume %.0f%%", v * 100.0f);
        }
        break;
    case 'L': PrintDetail(); break;
    case 'R': stats_.Clear(); LOGI("stats reset"); break;
    case 'T': o_.timecode = !o_.timecode; LOGI("timecode %s", o_.timecode ? "on" : "off"); break;
    default: break;
    }
}

std::atomic<bool> g_quitRequested{false};

void App::RunFixed() {
    while (pres_.PumpMessages() && !g_quitRequested) {
        if (o_.durationSec > 0 && Qpc::ToSec(Qpc::Now() - startQpc_) > o_.durationSec) break;
        HandleKeys();
        if (!haveSlot_) haveSlot_ = pres_.WaitReady(100);   // keep the slot if we did not present last time
        int64_t wake = Qpc::Now();
        const int64_t lead = Qpc::FromMs(leadMs_);
        int64_t V = vsync_.NextVblankAfter(wake + lead);
        if (o_.lateLatch) sleeper_.SleepUntil(V - lead);
        int64_t s = Qpc::Now();
        Ingest(s);
        PaceDecision d = pacer_.Decide(s, V);
        if (!d.valid) {
            // No timing yet: show the newest frame if we have one.
            int64_t n = pacer_.NewestIndex();
            if (n < 0) { pres_.Present(1, false); haveSlot_ = false; continue; }
            d.valid = true; d.nA = d.nB = n; d.t = 0;
        }
        Render(d, s, V, 1, false);
        PollPresentStats();
        Report(false);
    }
}

void App::RunVrr() {
    while (pres_.PumpMessages() && !g_quitRequested) {
        if (o_.durationSec > 0 && Qpc::ToSec(Qpc::Now() - startQpc_) > o_.durationSec) break;
        HandleKeys();
        if (!haveSlot_) haveSlot_ = pres_.WaitReady(100);   // keep the slot if we did not present last time
        int64_t now = Qpc::Now();
        Ingest(now);
        int64_t V = pacer_.NextVrrDisplayTime(now, leadMs_);
        if (!V) {
            WaitForSingleObject(cap_ ? cap_->FrameEvent() : GetCurrentThread(), 5);
            continue;
        }
        const int64_t lead = Qpc::FromMs(leadMs_);
        sleeper_.SleepUntil(V - lead);
        int64_t s = Qpc::Now();
        Ingest(s);
        PaceDecision d = pacer_.Decide(s, V);
        if (!d.valid) continue;
        Render(d, s, V, 0, true, V - Qpc::FromMs(0.3));
        PollPresentStats();
        Report(false);
    }
}

void App::RunPassthrough(double untilSec, int sync) {
    int64_t shown = -1;
    if (sync < 0) sync = o_.passSync;
    while (pres_.PumpMessages() && !g_quitRequested) {
        if (o_.durationSec > 0 && Qpc::ToSec(Qpc::Now() - startQpc_) > o_.durationSec) break;
        if (untilSec > 0 && Qpc::ToSec(Qpc::Now() - startQpc_) > untilSec) break;
        HandleKeys();
        if (!haveSlot_) haveSlot_ = pres_.WaitReady(100);   // keep the slot if we did not present last time
        if (o_.synthetic) {
            int64_t next = synthNext_ ? synthNext_ : Qpc::Now();
            sleeper_.SleepUntil(next);
        } else {
            WaitForSingleObject(cap_->FrameEvent(), 50);
        }
        int64_t s = Qpc::Now();
        Ingest(s);
        // One present per received signal frame (a repeat re-shows the newest content frame), so the
        // present rate stays at the signal rate (inside the VRR range) whatever the content rate.
        const int64_t sig = pacer_.NewestSignalIndex();
        int64_t n = pacer_.NewestIndex();
        if (n < 0 || sig == shown) continue;
        PaceDecision d;
        d.valid = true; d.nA = d.nB = n; d.t = 0; d.delayMs = 0;
        int64_t V = sync ? vsync_.NextVblankAfter(s + Qpc::FromMs(1.0)) : s + Qpc::FromMs(1.0);
        Render(d, s, V, (UINT)sync, sync == 0);
        shown = sig;
        PollPresentStats();
        Report(false);
    }
}

int App::Run() {
    startQpc_ = lastReport_ = Qpc::Now();
    StartDashboard();
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (cap_) cap_->Start();
    if (o_.autoMode) {
        // Probe: present each source frame with sync interval 1 for a moment. A VRR display
        // refreshes once per present; a fixed-refresh display refreshes refresh/fps times.
        LOGI("auto mode: probing the display for VRR...");
        o_.mode = PaceMode::Passthrough;
        RunPassthrough(2.5, 1);
        o_.mode = (vrrActive_ && pres_.tearingEnabled()) ? PaceMode::Vrr : PaceMode::Fixed;
        pacer_.SetMode(o_.mode);
        BumpGeneration();
        LOGI("auto mode: refreshes/present %.2f -> %s pacing", vrrRatio_, PaceModeName(o_.mode));
        stats_.Clear();
        records_.clear();
    }
    // Audio starts once video capture is streaming (and after the probe in auto mode): the USB card
    // delivers audio packets irregularly while its video stream starts up. Never fatal.
    StartAudio();
    switch (o_.mode) {
    case PaceMode::Fixed: RunFixed(); break;
    case PaceMode::Vrr: RunVrr(); break;
    case PaceMode::Passthrough: RunPassthrough(); break;
    }
    while (!records_.empty()) { if (!records_.front().finalized) FinalizeRecord(records_.front()); records_.pop_front(); }
    PublishStatus(Qpc::Now());
    StopDashboard();   // gives the console back before the final report
    Report(true);
    const std::string rec = dash_.Summary();
    if (!rec.empty()) LOGI("settings record (this session%s%s):\n%s", o_.historyPath.empty() ? "" : ", appended to ",
                           o_.historyPath.c_str(), rec.c_str());
    timeEndPeriod(1);
    return 0;
}

void App::StartDashboard() {
    const bool tty = LogConsoleIsTty();
    const bool wantTui = o_.ui == "tui" || (o_.ui == "auto" && tty);
    DashboardOptions d;
    d.tui = wantTui;
    d.historyPath = o_.historyPath;
    if (wantTui) {
        // Route the log to the dashboard before it takes the screen: nothing may write to the console after this.
        LogFlush();
        SetLogSink([](void* ctx, LogLevel lvl, double t, const char* msg, size_t len) {
            static_cast<Dashboard*>(ctx)->OnLog(lvl, t, msg, len);
        }, &dash_);
        SetLogConsole(false);
        g_tuiActive = true;
    }
    const bool started = dash_.Start(&tel_, d);
    tui_ = started && dash_.TuiActive();
    if (wantTui && !tui_) {
        SetLogConsole(true);
        SetLogSink(nullptr, nullptr);
        g_tuiActive = false;
        LOGW("terminal dashboard unavailable (ui = %s); plain console output", o_.ui.c_str());
    }
    statusLine_ = !tui_ && tty && o_.reportSec > 0;
}

void App::StopDashboard() {
    if (statusLine_) { SetStatusLine(""); statusLine_ = false; }
    dash_.Stop();   // TUI: gives the screen back; record-only: flushes the history file (idempotent)
    if (tui_) {
        SetLogConsole(true);
        SetLogSink(nullptr, nullptr);
        g_tuiActive = false;
        tui_ = false;
    }
}

void App::PublishStatus(int64_t now) {
    lastStatusPub_ = now;
    AppStatus& s = statusPub_;
    s.generation = generation_;
    s.uptimeSec = Qpc::ToSec(now - startQpc_);
    s.warmupDone = warmupDone_;
    if (s.configPath != o_.configPath) s.configPath = o_.configPath;
    if (s.captureName.empty()) {
        if (cap_) {
            s.captureName = cap_->info().deviceName;
            char b[96];
            snprintf(b, sizeof(b), "%s %ux%u@%.2f", PixFmtName(cap_->info().format), cap_->info().width, cap_->info().height, cap_->info().fps);
            s.captureFormat = b;
        } else {
            s.captureName = "synthetic";
            char b[96];
            snprintf(b, sizeof(b), "%ux%u@%.2f", srcW_, srcH_, o_.capFps);
            s.captureFormat = b;
        }
    }
    s.srcW = srcW_; s.srcH = srcH_; s.outW = outW_; s.outH = outH_;
    s.dispW = pres_.width(); s.dispH = pres_.height(); s.dispHz = pres_.refreshHz();
    s.vrrActive = vrrActive_;
    const bool probing = o_.autoMode && o_.mode == PaceMode::Passthrough;
    s.mode = probing ? "auto (probing)" : o_.mode == PaceMode::Vrr ? "vrr" : o_.mode == PaceMode::Fixed ? "fixed" : "passthrough";
    s.interp = o_.interp;
    s.fgQuality = o_.interp == "vfx" ? FgQualityName(o_.vfgMode) : o_.interp == "nvof" ? (o_.nvofQuality == 0 ? "q0" : o_.nvofQuality == 1 ? "q1" : "q2") : "-";
    s.sr = o_.sr;
    s.srQuality = o_.sr == "vfx" ? SrQualityName(o_.srQuality) : "-";
    s.hdr = o_.hdr;
    s.interpOn = interpOn_; s.srOn = srOn_;
    s.adaptive = adaptive_;
    s.multiplier = curK_;
    s.targetFps = o_.fgTargetFps;
    s.signalHz = pacer_.SourceHz();
    s.contentFps = s.signalHz > 0 ? s.signalHz / pacer_.ContentPeriod() : 0;
    s.outFps = lastOutFps_;
    s.contentStatic = adaptive_ && cadence_.UniqueFraction() < 0.05;
    s.delayMs = pacer_.DelayMs(); s.leadMs = leadMs_; s.minSlackMs = pacer_.MinSlackMs();
    s.gpuBudgetMs = o_.mode == PaceMode::Vrr && s.contentFps > 0 ? 1000.0 / (s.contentFps * curK_)
                  : o_.mode == PaceMode::Fixed ? 1000.0 / std::max(1.0, pres_.refreshHz()) : 1000.0 / std::max(1.0, s.signalHz);
    s.latencyMetric = (o_.mode == PaceMode::Vrr || vrrActive_ || (o_.mode == PaceMode::Passthrough && o_.passSync == 0)) ? "arrival->flip" : "arrival->scanout";
    s.late = pacer_.LateCount(); s.hold = pacer_.StallCount(); s.drops = cap_ ? cap_->DroppedFrames() : 0;
    s.missed = missed_; s.repeats = repeats_; s.gpuBusy = gpuBusyAtPresent_;
    s.audioEnabled = o_.audio;
    s.audioRunning = audio_.Running() && audioStats_.running;
    s.audioPlaying = audioStats_.playing;
    s.audioMuted = audio_.Running() && audio_.Muted();
    s.audioVolume = audio_.Running() ? audio_.Volume() : (float)(o_.audioVolume / 100.0);
    s.audioLatencyMs = audioStats_.latencyMs; s.audioBufferMs = audioStats_.bufferMs; s.audioSyncMs = audioStats_.syncDelayMs;
    s.audioUnderruns = audioStats_.underruns;
    if (s.audioIn != audioStats_.captureName) s.audioIn = audioStats_.captureName;
    if (s.audioOut != audioStats_.renderName) s.audioOut = audioStats_.renderName;
    s.captureMs = stCapture_; s.uploadMs = stUpload_; s.fgMs = stFg_; s.srMs = stSr_; s.hdrMs = stHdr_;
    if (now < noticeUntil_) { if (s.notice != notice_) s.notice = notice_; } else if (!s.notice.empty()) s.notice.clear();
    tel_.PublishStatus(s);
}

} // namespace camss

// ------------------------------------------------------------------ entry
namespace camss { std::atomic<bool> g_tuiActive{false}; }

static BOOL WINAPI CtrlHandler(DWORD ev) {
    camss::g_quitRequested = true;
    // Closing the console / logoff / shutdown ends the process when this returns: write out the log.
    if (ev == CTRL_CLOSE_EVENT || ev == CTRL_LOGOFF_EVENT || ev == CTRL_SHUTDOWN_EVENT) camss::LogFlush(1000);
    return TRUE;
}

// Log the faulting module + offset on a crash (no dump files, no system settings touched).
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    if (camss::g_tuiActive) { fputs("\x1b[?1049l\x1b[?25h\x1b[0m", stdout); fflush(stdout); }   // leave the dashboard screen
    else if (camss::LogConsoleIsTty()) { fputs("\r\x1b[2K", stdout); fflush(stdout); }               // erase the status line
    camss::LogFlush(200);
    fprintf(stderr, "FATAL exception 0x%08lX at %p (%s+0x%llX)\n", ep->ExceptionRecord->ExceptionCode, addr, name,
            (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int wmain(int argc, wchar_t** argv) {
    using namespace camss;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] { camss::LogFlush(200); std::abort(); });
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HR_CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HR_CHECK(MFStartup(MF_VERSION));
    int rc = 0;
    try {
        Options o;
        ParseResult pr = LoadOptions(argc, argv, o);
        if (pr != ParseResult::Run) rc = pr == ParseResult::Error ? 1 : 0;
        else {
            SetLogLevel(o.debug ? LogLevel::Debug : ParseLogLevel(o.logLevel.c_str(), LogLevel::Info));
            if (!o.logPath.empty()) SetLogFile(o.logPath.c_str());
            if (o.list) { MfCapture::ListDevices(); AudioMonitor::ListDevices(); }
            else if (o.selftest) rc = RunSelfTest(o);
            else {
                App app(o);
                if (!app.Init(GetModuleHandleW(nullptr))) { LOGE("init failed"); rc = 1; }
                else rc = app.Run();
            }
        }
    } catch (const std::exception& e) {
        LOGE("fatal: %s", e.what());
        LogFlush();
        rc = 2;
    }
    LogShutdown();
    MFShutdown();
    CoUninitialize();
    return rc;
}
