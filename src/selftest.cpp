// Offline self-test: renders synthetic frames with known motion, runs each frame
// generator at several time steps and compares against the ground-truth frame
// rendered at exactly that time (PSNR). Also benchmarks VFX VSR / TrueHDR.
#include "app.h"
#include "vfx/vfx_chain.h"
#include <cmath>
#include <vector>

namespace camss {

static bool Readback(Device& dev, ID3D11Texture2D* tex, std::vector<uint8_t>& rgba, UINT& w, UINT& h) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev.dev()->CreateTexture2D(&d, nullptr, &st))) return false;
    dev.ctx()->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(dev.ctx()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    w = d.Width; h = d.Height;
    rgba.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; y++) memcpy(&rgba[(size_t)y * w * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    dev.ctx()->Unmap(st.Get(), 0);
    return true;
}

static double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
        for (int c = 0; c < 3; c++) { double d = (double)a[i + c] - (double)b[i + c]; se += d * d; n++; }
    double mse = se / std::max<size_t>(1, n);
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

static void WaitIdle(Device& dev) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> q;
    dev.dev()->CreateQuery(&qd, &q);
    dev.ctx()->End(q.Get());
    BOOL done = FALSE;
    while (dev.ctx()->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
}

int RunSelfTest(const Options& o) {
    Device dev;
    if (!dev.Create(false)) return 1;
    const UINT W = o.capW, H = o.capH;
    const double fps = o.capFps;
    SyntheticSource synth;
    synth.Init(dev, W, H);
    std::array<SourceFrame, 8> store;
    for (auto& f : store) f.rgb = dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    Device::Tex gt = dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    Device::Tex out = dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    std::vector<uint8_t> bufGt, bufOut, bufA, bufB;
    UINT rw, rh;
    const float ts[] = {0.25f, 0.5f, 0.75f};
    const int pairs = 12;

    // Each generator owns its frame store: a D3D texture can be registered with CUDA only once.
    using Store = std::array<SourceFrame, 8>;
    auto makeStore = [&] {
        auto st = std::make_unique<Store>();
        for (auto& f : *st) f.rgb = dev.CreateTex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
        return st;
    };
    struct Gen { std::string name; std::unique_ptr<Interpolator> interp; std::unique_ptr<VfxChain> vfx; std::unique_ptr<Store> store; };
    std::vector<Gen> gens;
    gens.push_back({"blend", CreateBlendInterpolator(), nullptr, makeStore()});
    for (int q = 0; q <= 2; q++)
        if (auto f = CreateFlowInterpolator(q)) gens.push_back({"nvof-q" + std::to_string(q), std::move(f), nullptr, makeStore()});
    for (int mode = 0; mode <= 2; mode++) {
        Gen g; g.name = "vfx-fg-mode" + std::to_string(mode);
        g.vfx = std::make_unique<VfxChain>();
        g.store = makeStore();
        VfxChain::Config c; c.fg = true; c.fgMode = mode; c.srcW = W; c.srcH = H; c.outW = W; c.outH = H;
        if (!g.vfx->Init(dev, c, g.store->data(), (int)g.store->size())) { LOGW("VFX mode %d unavailable", mode); continue; }
        gens.push_back(std::move(g));
    }
    for (auto& g : gens) if (g.interp && !g.interp->Init(dev, W, H)) g.interp.reset();

    printf("\n== Frame generation quality vs ground truth (synthetic %ux%u, %d pairs) ==\n", W, H, pairs);
    printf("%-16s %8s %8s %8s %10s %12s\n", "generator", "t=0.25", "t=0.50", "t=0.75", "mean dB", "ms/frame");
    for (auto& g : gens) {
        if (!g.interp && !g.vfx) continue;
        double sum[3] = {}, totalMs = 0; int runs = 0;
        Store& gs = *g.store;
        for (int p = 0; p < pairs; p++) {
            int64_t n = 100 + p * 7;
            SourceFrame& A = gs[n % 8];
            SourceFrame& B = gs[(n + 1) % 8];
            synth.Render(A.rgb, n / fps); A.n = n; A.generation++;
            synth.Render(B.rgb, (n + 1) / fps); B.n = n + 1; B.generation++;
            if (g.vfx) { g.vfx->Ingest(A); g.vfx->Ingest(B); }
            if (g.interp) g.interp->Prepare(A, B);
            WaitIdle(dev);
            for (int k = 0; k < 3; k++) {
                float t = ts[k];
                synth.Render(gt, (n + t) / fps);
                int64_t t0 = Qpc::Now();
                ID3D11Texture2D* res = out.tex.Get();
                if (g.vfx) {
                    const Device::Tex* o2 = g.vfx->Process(A, &B, t, false);
                    if (o2) res = o2->tex.Get();
                } else {
                    g.interp->Interpolate(A, B, t, out);
                }
                WaitIdle(dev);
                totalMs += Qpc::ToMs(Qpc::Now() - t0); runs++;
                Readback(dev, res, bufOut, rw, rh);
                Readback(dev, gt.tex.Get(), bufGt, rw, rh);
                sum[k] += Psnr(bufOut, bufGt);
            }
        }
        printf("%-16s %8.2f %8.2f %8.2f %10.2f %12.3f\n", g.name.c_str(), sum[0] / pairs, sum[1] / pairs, sum[2] / pairs,
               (sum[0] + sum[1] + sum[2]) / (3 * pairs), totalMs / runs);
    }
    // Reference: how different are consecutive real frames (repeat-frame baseline at t=0.5).
    {
        double s = 0;
        for (int p = 0; p < pairs; p++) {
            int64_t n = 100 + p * 7;
            synth.Render(store[0].rgb, n / fps);
            synth.Render(gt, (n + 0.5) / fps);
            Readback(dev, store[0].rgb.tex.Get(), bufA, rw, rh);
            Readback(dev, gt.tex.Get(), bufGt, rw, rh);
            s += Psnr(bufA, bufGt);
        }
        printf("%-16s %8s %8.2f %8s   (repeat previous frame)\n", "repeat", "", s / pairs, "");
    }

    // ---- VSR / TrueHDR benchmarks (VFX), 1080p -> display-size.
    printf("\n== VFX VSR / TrueHDR timing (GPU, CUDA events) ==\n");
    struct SrCase { const char* name; bool sr; int q; bool hdr; bool hdrBefore; };
    SrCase cases[] = {
        {"VSR q21 (streaming-medium)", true, 21, false, true},
        {"VSR q23 (streaming-ultra)", true, 23, false, true},
        {"VSR q1 (low)", true, 1, false, true},
        {"VSR q4 (ultra)", true, 4, false, true},
        {"TrueHDR 1080p", false, 0, true, true},
        {"TrueHDR@1080p + VSR q21 (10-bit)", true, 21, true, true},
        {"VSR q21 + TrueHDR@4K", true, 21, true, false},
        {"FG low + TrueHDR + VSR q21", true, 21, true, true},
    };
    const UINT OW = o.outW ? o.outW : 3840, OH = o.outH ? o.outH : 2160;
    gens.clear();   // release CUDA registrations of the generator stores
    for (auto& c : cases) {
        auto cstore = makeStore();
        Store& store = *cstore;
        VfxChain chain;
        VfxChain::Config cfg;
        cfg.fg = strstr(c.name, "FG") != nullptr; cfg.fgMode = 0;
        cfg.sr = c.sr; cfg.srQuality = c.q; cfg.hdr = c.hdr; cfg.hdrBeforeSr = c.hdrBefore;
        cfg.srcW = W; cfg.srcH = H; cfg.outW = OW; cfg.outH = OH;
        if (!chain.Init(dev, cfg, store.data(), (int)store.size())) { printf("%-36s  init failed\n", c.name); continue; }
        StatBook sb;
        for (int i = 0; i < 80; i++) {
            int64_t n = 500 + i;
            SourceFrame& A = store[n % 8];
            SourceFrame& B = store[(n + 1) % 8];
            synth.Render(A.rgb, n / fps); A.n = n; A.generation++;
            synth.Render(B.rgb, (n + 1) / fps); B.n = n + 1; B.generation++;
            chain.Ingest(A); chain.Ingest(B);
            int64_t t0 = Qpc::Now();
            chain.Process(A, cfg.fg ? &B : nullptr, 0.5f, true);
            WaitIdle(dev);
            if (i >= 10) sb.Add("wall", Qpc::ToMs(Qpc::Now() - t0));
            for (auto& kv : chain.PopTimings()) if (i >= 10) sb.Add(kv.first, kv.second);
        }
        if (c.hdr && !c.sr) {
            // Channel-order check: average HDR output over yellow (R,G high, B low) and blue input pixels.
            int64_t n = 600;
            SourceFrame& A = store[n % 8];
            synth.Render(A.rgb, n / fps); A.n = n; A.generation++;
            chain.Ingest(A);
            const Device::Tex* res = chain.Process(A, nullptr, 0.f, true);
            WaitIdle(dev);
            std::vector<uint8_t> in8, outp;
            UINT w1, h1, w2, h2;
            Readback(dev, A.rgb.tex.Get(), in8, w1, h1);
            if (res) {
                Readback(dev, res->tex.Get(), outp, w2, h2);
                double yel[3] = {}, blu[3] = {}; int ny = 0, nb = 0;
                for (size_t i = 0; i < (size_t)w1 * h1; i++) {
                    const uint8_t* q = &in8[i * 4];
                    uint32_t v; memcpy(&v, &outp[i * 4], 4);
                    double ch[3] = {(double)(v & 1023) / 1023, (double)((v >> 10) & 1023) / 1023, (double)((v >> 20) & 1023) / 1023};
                    if (q[0] > 230 && q[1] > 200 && q[2] < 80) { for (int k = 0; k < 3; k++) yel[k] += ch[k]; ny++; }
                    if (q[0] < 80 && q[1] > 130 && q[1] < 170 && q[2] > 230) { for (int k = 0; k < 3; k++) blu[k] += ch[k]; nb++; }
                }
                printf("  channel check (PQ codes, R10G10B10A2 unpacked): yellow -> R %.3f G %.3f B %.3f (n=%d) | blue -> R %.3f G %.3f B %.3f (n=%d)\n",
                       ny ? yel[0] / ny : 0, ny ? yel[1] / ny : 0, ny ? yel[2] / ny : 0, ny, nb ? blu[0] / nb : 0, nb ? blu[1] / nb : 0,
                       nb ? blu[2] / nb : 0, nb);
            }
        }
        double fg50, fg99, h50, h99, s50, s99, o50, o99, t50, t99, w50, w99;
        sb.Get("vfx.fg", fg50, fg99); sb.Get("vfx.hdr", h50, h99); sb.Get("vfx.sr", s50, s99);
        sb.Get("vfx.out", o50, o99); sb.Get("vfx.gpu_total", t50, t99); sb.Get("wall", w50, w99);
        printf("%-36s fg %.2f  hdr %.2f  sr %.2f  out %.2f | gpu total p50 %.2f p99 %.2f | wall p50 %.2f ms\n", c.name, fg50, h50,
               s50, o50, t50, t99, w50);
    }
    return 0;
}

} // namespace camss
