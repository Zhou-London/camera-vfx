#include "app.h"
#include "vfx/vfx_chain.h"
#include <cmath>
#include <cwchar>
#include <shellapi.h>
#include <timeapi.h>

namespace camss {

// ------------------------------------------------------------------ options
void PrintUsage() {
    printf(
        "camss - capture -> frame generation -> super resolution -> HDR -> low-latency present\n"
        "\n"
        "  --list                     list capture devices and native formats\n"
        "  --device <name|index>      capture device (substring of friendly name, or index)\n"
        "  --format yuy2|nv12|p010|rgb24   preferred native capture format (default yuy2)\n"
        "  --capture WxH@fps          capture mode (default 1920x1080@60)\n"
        "  --synthetic                built-in moving test pattern instead of a capture device\n"
        "  --range limited|full       YCbCr range (default limited), --bt601 for SD matrix\n"
        "\n"
        "  --mode auto|passthrough|fixed|vrr  pacing (default auto: probe display for VRR)\n"
        "  --k N                      VRR output multiplier (default 2 -> 120 fps from 60)\n"
        "  --no-latch                 fixed mode: render immediately after vblank instead of late\n"
        "  --lead MS                  initial render lead (default 3.0)\n"
        "  --delay MS                 initial pipeline delay (default: estimated, then adapts)\n"
        "  --margin MS                pacing slack margin (default 1.0)\n"
        "  --snap EPS                 snap |t| < eps to a real frame (default 0.06)\n"
        "  --sync 0|1                 passthrough: present interval (0 = immediate, tearing)\n"
        "\n"
        "  --interp none|blend|nvof|vfx    frame generation (vfx = Maxine Video Frame Generation)\n"
        "  --vfg-mode 0|1|2           VFX frame generation quality (default 0 = low/fastest)\n"
        "  --nvof-quality 0|1|2       NVOF interpolator quality (default 1)\n"
        "  --sr none|rtx|vfx          super resolution (rtx = driver RTX VSR, vfx = Maxine VSR)\n"
        "  --sr-quality N             Maxine VSR QualityLevel (default 21 streaming-medium; 1-4, 16-19, 23)\n"
        "  --hdr none|rtx|vfx         SDR->HDR (TrueHDR); needs Windows HDR on for correct display\n"
        "  --hdr-after-sr             VFX: run TrueHDR at output resolution (default: before VSR)\n"
        "  --out WxH                  processing output size (default = display size)\n"
        "  --white NITS               SDR paper white when composing into HDR10 (default 203)\n"
        "\n"
        "  --windowed                 windowed instead of borderless fullscreen\n"
        "  --monitor N                display output index (default 0)\n"
        "  --duration SEC             run for SEC seconds then exit\n"
        "  --csv FILE                 per-output-frame timing log\n"
        "  --log FILE                 log file\n"
        "  --timecode                 draw loopback timecode band (for HDMI-out -> capture-in tests)\n"
        "  --decode-timecode          decode the timecode band from captured frames\n"
        "  --marker                   flashing corner square for photodiode / slow-motion camera tests\n"
        "  --warmup SEC               reset statistics once after SEC seconds (default 6)\n"
        "  --report SEC               stats report interval (default 2)\n"
        "  --debug                    D3D11 debug layer, verbose log\n"
        "\n"
        "keys: ESC quit, I toggle interpolation, S toggle SR, R reset stats, T toggle timecode\n");
}

static bool ParseWxH(const wchar_t* s, UINT& w, UINT& h) { return swscanf_s(s, L"%ux%u", &w, &h) == 2; }

bool ParseOptions(int argc, wchar_t** argv, Options& o) {
    auto narrow = [](const wchar_t* w) { return WideToUtf8(w); };
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* {
            if (i + 1 >= argc) { LOGE("missing value for %s", narrow(a.c_str()).c_str()); throw std::runtime_error("args"); }
            return argv[++i];
        };
        if (a == L"--help" || a == L"-h") { PrintUsage(); return false; }
        else if (a == L"--list") { return false; }
        else if (a == L"--device") {
            std::wstring v = next();
            if (!v.empty() && iswdigit(v[0]) && v.size() < 3) o.deviceIndex = _wtoi(v.c_str()); else o.device = v;
        }
        else if (a == L"--format") {
            std::wstring v = next();
            if (v == L"yuy2") o.format = PixFmt::YUY2; else if (v == L"nv12") o.format = PixFmt::NV12;
            else if (v == L"p010") o.format = PixFmt::P010; else if (v == L"rgb24") o.format = PixFmt::RGB24;
            else if (v == L"rgb32") o.format = PixFmt::RGB32;
        }
        else if (a == L"--capture") { double f = 60; UINT w = 0, h = 0; if (swscanf_s(next(), L"%ux%u@%lf", &w, &h, &f) >= 2) { o.capW = w; o.capH = h; o.capFps = f; } }
        else if (a == L"--synthetic") o.synthetic = true;
        else if (a == L"--range") o.color.fullRange = std::wstring(next()) == L"full";
        else if (a == L"--bt601") o.color.bt601 = true;
        else if (a == L"--mode") {
            std::wstring v = next();
            o.autoMode = v == L"auto";
            o.mode = v == L"passthrough" ? PaceMode::Passthrough : v == L"vrr" ? PaceMode::Vrr : PaceMode::Fixed;
        }
        else if (a == L"--k") o.vrrK = std::max(1, _wtoi(next()));
        else if (a == L"--no-latch") o.lateLatch = false;
        else if (a == L"--lead") o.leadMs = _wtof(next());
        else if (a == L"--delay") o.initialDelayMs = _wtof(next());
        else if (a == L"--margin") o.marginMs = _wtof(next());
        else if (a == L"--snap") o.snapEps = _wtof(next());
        else if (a == L"--sync") o.passSync = _wtoi(next());
        else if (a == L"--interp") o.interp = narrow(next());
        else if (a == L"--vfg-mode") o.vfgMode = _wtoi(next());
        else if (a == L"--nvof-quality") o.nvofQuality = _wtoi(next());
        else if (a == L"--sr") o.sr = narrow(next());
        else if (a == L"--sr-quality") o.srQuality = _wtoi(next());
        else if (a == L"--hdr") o.hdr = narrow(next());
        else if (a == L"--hdr-after-sr") o.hdrBeforeSr = false;
        else if (a == L"--out") ParseWxH(next(), o.outW, o.outH);
        else if (a == L"--white") o.sdrWhiteNits = (float)_wtof(next());
        else if (a == L"--windowed") o.windowed = true;
        else if (a == L"--monitor") o.monitor = _wtoi(next());
        else if (a == L"--duration") o.durationSec = _wtof(next());
        else if (a == L"--csv") o.csvPath = narrow(next());
        else if (a == L"--log") o.logPath = narrow(next());
        else if (a == L"--timecode") o.timecode = true;
        else if (a == L"--decode-timecode") o.decodeTimecode = true;
        else if (a == L"--marker") o.marker = true;
        else if (a == L"--report") o.reportSec = _wtof(next());
        else if (a == L"--warmup") o.warmupSec = _wtof(next());
        else if (a == L"--debug") o.debug = true;
        else if (a == L"--stats-debug") o.statsDebug = true;
        else if (a == L"--selftest") o.selftest = true;
        else { LOGE("unknown option %s", narrow(a.c_str()).c_str()); PrintUsage(); return false; }
    }
    return true;
}

// ------------------------------------------------------------------ app
App::App(const Options& o) : o_(o) { leadMs_ = o.leadMs; }

App::~App() {
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
    const bool vfxFg = o_.interp == "vfx", vfxSr = o_.sr == "vfx", vfxHdr = o_.hdr == "vfx";
    if (vfxFg || vfxSr || vfxHdr) {
        VfxChain::Config c;
        c.fg = vfxFg; c.fgMode = o_.vfgMode;
        c.sr = vfxSr; c.srQuality = o_.srQuality;
        c.hdr = vfxHdr; c.hdrBeforeSr = o_.hdrBeforeSr;
        c.srcW = srcW_; c.srcH = srcH_;
        c.outW = vfxSr ? outW_ : srcW_; c.outH = vfxSr ? outH_ : srcH_;
        vfx_ = std::make_unique<VfxChain>();
        if (!vfx_->Init(dev_, c, store_.data(), kStore)) { LOGE("VFX chain init failed"); return false; }
    }

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
    {
        int64_t t0 = Qpc::Now();
        // Positive frame numbers mapping to slots 0/1 (n % kStore must never be negative).
        store_[0].n = (int64_t)kStore * 100000; store_[1].n = store_[0].n + 1;
        for (int i = 0; i < 4; i++) {
            if (vfx_) { vfx_->Ingest(store_[0]); vfx_->Ingest(store_[1]); vfx_->Process(store_[0], &store_[1], 0.5f, true); }
            if (rtx_ && rtxInW_ == srcW_ && rtxInH_ == srcH_) rtx_->Process(store_[0].rgb.tex.Get(), vpOut_.tex.Get());
            if (interp_) { interp_->Prepare(store_[0], store_[1]); interp_->Interpolate(store_[0], store_[1], 0.5f, interpOut_); }
            dev_.ctx()->Flush();
        }
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        if (SUCCEEDED(dev_.dev()->CreateQuery(&qd, &q))) {
            dev_.ctx()->End(q.Get());
            BOOL done = FALSE;
            while (dev_.ctx()->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) Sleep(0);
        }
        if (vfx_) vfx_->PopTimings();
        store_[0].n = store_[1].n = -1;
        LOGI("warm-up of processing stages: %.0f ms", Qpc::ToMs(Qpc::Now() - t0));
    }

    // ---- pacing
    PacerConfig pc;
    pc.mode = o_.mode;
    pc.vrrMultiplier = o_.vrrK;
    pc.snapEps = o_.snapEps;
    pc.marginMs = o_.marginMs;
    // Start near the expected steady state (VRR k-grid needs (k-1)/k of a source period,
    // arbitrary-phase fixed refresh up to a full period) so the servo converges quickly.
    pc.initialDelayMs = o_.initialDelayMs > 0 ? o_.initialDelayMs
                                               : 1000.0 / o_.capFps * (o_.mode == PaceMode::Vrr || o_.autoMode ? (double)(o_.vrrK - 1) / o_.vrrK : 1.0) + 7.0;
    pacer_.Configure(pc, o_.capFps);
    vsync_.Start(pres_.output(), pres_.refreshHz());

    if (!o_.csvPath.empty()) {
        csv_ = fopen(o_.csvPath.c_str(), "w");
        if (csv_) fprintf(csv_, "idx,present_id,render_start_ms,present_call_ms,predicted_v_ms,actual_v_ms,nA,nB,t,delay_ms,slack_ms,late,"
                                "newest_sample_ms,newest_arrival_ms,newest_uploaded_ms,content_sample_ms,render_cpu_ms,"
                                "arrival_to_scanout_ms,content_to_scanout_ms\n");
    }
    LOGI("pipeline: mode=%s interp=%s sr=%s hdr=%s src=%ux%u out=%ux%u display=%ux%u@%.0fHz",
         PaceModeName(o_.mode), o_.interp.c_str(), o_.sr.c_str(), o_.hdr.c_str(), srcW_, srcH_, outW_, outH_,
         pres_.width(), pres_.height(), pres_.refreshHz());
    return true;
}

SourceFrame* App::Frame(int64_t n) {
    if (n < 0) return nullptr;
    SourceFrame& f = store_[(size_t)(n % kStore)];
    return f.n == n ? &f : nullptr;
}

void App::IngestCaptured(const RawFrame& rf) {
    const int64_t n = rf.sourceIndex;
    SourceFrame& f = store_[(size_t)(n % kStore)];
    if (!cap_->LockSlot(rf)) { stats_.Add("cap.overwritten", 1); return; }
    conv_.Convert(cap_->PlaneSrv(rf.slot, 0), cap_->PlaneCount() > 1 ? cap_->PlaneSrv(rf.slot, 1) : nullptr, f.rgb);
    cap_->UnlockSlot(rf);
    f.n = n; f.seq = rf.seq; f.generation++;
    f.sampleQpc = rf.sampleTimeQpc; f.arrivalQpc = rf.arrivalQpc; f.uploadedQpc = rf.uploadedQpc;
    f.readyQpc = rf.uploadedQpc;
    pacer_.OnSourceFrame(n, rf.uploadedQpc);

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

    // Precompute for the new pair.
    if (vfx_) vfx_->Ingest(f);
    if (interp_) if (SourceFrame* prev = Frame(n - 1)) interp_->Prepare(*prev, f);
}

void App::Ingest(int64_t now) {
    if (o_.synthetic) {
        const int64_t per = (int64_t)((double)Qpc::Freq() / o_.capFps);
        if (!synthT0_) { synthT0_ = now; synthNext_ = now; }
        while (now >= synthNext_) {
            int64_t n = synthN_++;
            SourceFrame& f = store_[(size_t)(n % kStore)];
            synth_.Render(f.rgb, (double)n / o_.capFps);
            f.n = n; f.seq = (uint64_t)n + 1; f.generation++;
            // A clock-driven source: frame n exists from its tick time on (like a capture
            // card delivering at exactly 60 Hz); rendering it lazily here does not change that.
            f.sampleQpc = synthNext_; f.arrivalQpc = synthNext_; f.uploadedQpc = synthNext_; f.readyQpc = synthNext_;
            pacer_.OnSourceFrame(n, synthNext_);
            if (vfx_) vfx_->Ingest(f);
            if (interp_) if (SourceFrame* prev = Frame(n - 1)) interp_->Prepare(*prev, f);
            synthNext_ += per;
        }
        return;
    }
    for (const RawFrame& rf : cap_->Drain()) IngestCaptured(rf);
}

bool App::Render(const PaceDecision& d, int64_t renderStart, int64_t V, UINT sync, bool tearing, int64_t presentAt) {
    auto* ctx = dev_.ctx();
    SourceFrame* A = Frame(d.nA);
    SourceFrame* B = d.nB != d.nA ? Frame(d.nB) : nullptr;
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
    const SourceFrame* newest = (doInterp && B) ? B : A;
    r.newestArrival = newest->arrivalQpc; r.newestSample = newest->sampleQpc; r.newestUploaded = newest->uploadedQpc;
    r.contentSample = doInterp ? (int64_t)((double)A->sampleQpc + (double)(B->sampleQpc - A->sampleQpc) * d.t) : A->sampleQpc;
    r.renderCpuMs = Qpc::ToMs(submitDone - renderStart);   // excludes the deliberate present hold
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
    stats_.Add("lat.arrival_to_present_ms", Qpc::ToMs(presentCall - newest->arrivalQpc));
    stats_.Add("pace.delay_ms", d.delayMs);
    if (doInterp) stats_.Add("pace.t", d.t);
    prof_.Collect(ctx, [&](const char* name, double ms) { stats_.Add(std::string("gpu.") + name, ms); });
    return true;
}

void App::FinalizeRecord(OutRecord& r) {
    r.finalized = true;
    finalizedCount_++;
    if (o_.mode == PaceMode::Vrr || vrrActive_ || (o_.mode == PaceMode::Passthrough && o_.passSync == 0)) {
        // VRR / immediate: the flip (scan-out start) happens at Present() once the GPU is done;
        // in VRR mode the present is held until the scheduled time, after the GPU work.
        stats_.Add("lat.arrival_to_flip_ms", Qpc::ToMs(r.presentCall - r.newestArrival));
        double cs = Qpc::ToMs(r.presentCall - r.contentSample);
        if (cs > 0 && cs < 500) stats_.Add("lat.content_to_flip_ms", cs);
    }
    if (r.actualV) {
        stats_.Add("lat.arrival_to_scanout_ms", Qpc::ToMs(r.actualV - r.newestArrival));
        double cs = Qpc::ToMs(r.actualV - r.contentSample);
        if (cs > 0 && cs < 500) stats_.Add("lat.content_to_scanout_ms", cs);
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
        fprintf(csv_, "%llu,%u,%.3f,%.3f,%.3f,%.3f,%lld,%lld,%.4f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                (unsigned long long)r.idx, r.presentId, ms(r.renderStart), ms(r.presentCall), ms(r.predictedV), ms(r.actualV),
                (long long)r.nA, (long long)r.nB, r.t, r.delayMs, r.slackMs, r.late ? 1 : 0, ms(r.newestSample), ms(r.newestArrival),
                ms(r.newestUploaded), ms(r.contentSample), r.renderCpuMs,
                r.actualV ? Qpc::ToMs(r.actualV - r.newestArrival) : 0.0, r.actualV ? Qpc::ToMs(r.actualV - r.contentSample) : 0.0);
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
            const bool vrrActive = vrrRatio_ < 1.3;
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

void App::Report(bool final) {
    const int64_t now = Qpc::Now();
    if (!warmupDone_ && Qpc::ToSec(now - startQpc_) > o_.warmupSec) {
        warmupDone_ = true;
        stats_.Clear();
        LOGI("warm-up (%.0f s) done: statistics reset", o_.warmupSec);
    }
    if (!final && now - lastReport_ < Qpc::FromMs(o_.reportSec * 1000.0)) return;
    lastReport_ = now;
    char head[512];
    snprintf(head, sizeof(head),
             "---- %.1fs  mode=%s interp=%s%s sr=%s%s hdr=%s | src %.3f Hz (jit %.2f ms) disp %.3f Hz (jit %.2f ms) | "
             "D=%.2f ms minslack=%.2f lead=%.2f late=%llu stall=%llu missed=%llu gpubusy=%llu drops=%llu%s",
             Qpc::ToSec(now - startQpc_), PaceModeName(o_.mode), o_.interp.c_str(), interpOn_ ? "" : "(off)", o_.sr.c_str(),
             srOn_ ? "" : "(off)", o_.hdr.c_str(), pacer_.SourceHz(), pacer_.SourceJitterMs(), vsync_.MeasuredHz(),
             vsync_.JitterMs(), pacer_.DelayMs(), pacer_.MinSlackMs(), leadMs_, (unsigned long long)pacer_.LateCount(),
             (unsigned long long)pacer_.StallCount(), (unsigned long long)missed_, (unsigned long long)gpuBusyAtPresent_, (unsigned long long)(cap_ ? cap_->DroppedFrames() : 0),
             o_.decodeTimecode ? (timecodeHits_ ? " tc=ok" : " tc=none") : "");
    LOGI("%s\n%s", head, stats_.Report().c_str());
    char title[256];
    double p50 = 0, p99 = 0;
    const char* metric = "arrival->scanout";
    if (!stats_.Get("lat.arrival_to_scanout_ms", p50, p99)) { stats_.Get("lat.arrival_to_flip_ms", p50, p99); metric = "arrival->flip"; }
    snprintf(title, sizeof(title), "camss | %s | D %.1f ms | %s p50 %.1f p99 %.1f ms", PaceModeName(o_.mode),
             pacer_.DelayMs(), metric, p50, p99);
    pres_.SetTitle(title);
}

void App::HandleKeys() {
    int k = pres_.PopKey();
    switch (k) {
    case 'I': interpOn_ = !interpOn_; LOGI("interpolation %s", interpOn_ ? "on" : "off"); break;
    case 'S': srOn_ = !srOn_; LOGI("super resolution %s", srOn_ ? "on" : "off"); break;
    case 'R': stats_.Clear(); LOGI("stats reset"); break;
    case 'T': o_.timecode = !o_.timecode; LOGI("timecode %s", o_.timecode ? "on" : "off"); break;
    default: break;
    }
}

bool g_quitRequested = false;

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
        int64_t n = pacer_.NewestIndex();
        if (n < 0 || n == shown) continue;
        PaceDecision d;
        d.valid = true; d.nA = d.nB = n; d.t = 0; d.delayMs = 0;
        int64_t V = sync ? vsync_.NextVblankAfter(s + Qpc::FromMs(1.0)) : s + Qpc::FromMs(1.0);
        Render(d, s, V, (UINT)sync, sync == 0);
        shown = n;
        PollPresentStats();
        Report(false);
    }
}

int App::Run() {
    startQpc_ = lastReport_ = Qpc::Now();
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
        LOGI("auto mode: refreshes/present %.2f -> %s pacing", vrrRatio_, PaceModeName(o_.mode));
        stats_.Clear();
        records_.clear();
    }
    switch (o_.mode) {
    case PaceMode::Fixed: RunFixed(); break;
    case PaceMode::Vrr: RunVrr(); break;
    case PaceMode::Passthrough: RunPassthrough(); break;
    }
    while (!records_.empty()) { if (!records_.front().finalized) FinalizeRecord(records_.front()); records_.pop_front(); }
    Report(true);
    timeEndPeriod(1);
    return 0;
}

} // namespace camss

// ------------------------------------------------------------------ entry
static BOOL WINAPI CtrlHandler(DWORD) { camss::g_quitRequested = true; return TRUE; }

// Log the faulting module + offset on a crash (no dump files, no system settings touched).
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HR_CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HR_CHECK(MFStartup(MF_VERSION));
    int rc = 0;
    try {
        Options o;
        bool list = false;
        for (int i = 1; i < argc; i++) if (std::wstring(argv[i]) == L"--list") list = true;
        if (list) { MfCapture::ListDevices(); MFShutdown(); return 0; }
        if (!ParseOptions(argc, argv, o)) { MFShutdown(); return 0; }
        if (!o.logPath.empty()) SetLogFile(o.logPath.c_str());
        if (o.debug) SetLogLevel(LogLevel::Debug);
        if (o.selftest) { rc = RunSelfTest(o); MFShutdown(); CoUninitialize(); return rc; }
        App app(o);
        if (!app.Init(GetModuleHandleW(nullptr))) { LOGE("init failed"); rc = 1; }
        else rc = app.Run();
    } catch (const std::exception& e) {
        LOGE("fatal: %s", e.what());
        rc = 2;
    }
    MFShutdown();
    CoUninitialize();
    return rc;
}
