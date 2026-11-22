#include "selftest.h"
#include "capture/synthetic_source.h"
#include "capture/timecode.h"
#include "gfx/blit.h"
#include "gfx/source_frame.h"
#include "util/stats.h"
#include "vfx/vfx_chain.h"
#include "vfx/vfx_quality.h"
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace cameravfx {

namespace {

constexpr int kStoreFrames = 8;
constexpr float kTimesteps[] = {0.25f, 0.5f, 0.75f};
constexpr int kPairs = 12;
constexpr int64_t kFirstPairFrame = 100;
constexpr int64_t kPairSpacing = 7;
constexpr uint32_t kTimecodeSamples = 64;
constexpr int kBenchmarkFrames = 80;
constexpr int kBenchmarkWarmUpFrames = 10;
constexpr int64_t kBenchmarkFirstFrame = 500;
constexpr int64_t kChannelCheckFrame = 600;
constexpr FrameSize kBenchmarkOutput{3840, 2160};
constexpr double kMaxPsnr = 99.0;
constexpr double kMinMse = 1e-10;
constexpr double kLimitedBlack = 16.0;
constexpr double kLimitedSpan = 219.0;
constexpr double kBt709LumaB = 0.0722, kBt709LumaG = 0.7152, kBt709LumaR = 0.2126;
constexpr uint8_t kNeutralChroma = 128;
constexpr uint32_t kTenBitMax = 1023;

using Store = std::array<SourceFrame, kStoreFrames>;

struct Image {
    std::vector<uint8_t> bytes;
    UINT width = 0, height = 0;
    const uint8_t* Pixel(size_t x, size_t y) const { return &bytes[(y * width + x) * 4]; }
};

Image ReadBack(Device& dev, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HR_CHECK(dev.D3dDevice()->CreateTexture2D(&desc, nullptr, &staging));
    dev.Context()->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HR_CHECK(dev.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    Image image{std::vector<uint8_t>((size_t)desc.Width * desc.Height * 4), desc.Width, desc.Height};
    for (UINT y = 0; y < desc.Height; y++)
        memcpy(&image.bytes[(size_t)y * desc.Width * 4], (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch, (size_t)desc.Width * 4);
    dev.Context()->Unmap(staging.Get(), 0);
    return image;
}

double Psnr(const Image& a, const Image& b) {
    double squaredError = 0;
    size_t samples = 0;
    for (size_t i = 0; i + 3 < a.bytes.size(); i += 4)
        for (int c = 0; c < 3; c++) {
            const double d = (double)a.bytes[i + c] - (double)b.bytes[i + c];
            squaredError += d * d;
            samples++;
        }
    const double mse = squaredError / (double)samples;
    return mse <= kMinMse ? kMaxPsnr : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::unique_ptr<Store> MakeStore(Device& dev, UINT w, UINT h) {
    auto store = std::make_unique<Store>();
    for (SourceFrame& f : *store) f.rgb = dev.CreateTex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    return store;
}

struct Pair {
    SourceFrame& a;
    SourceFrame& b;
};

Pair RenderPair(SyntheticSource& synth, Store& store, int64_t n, double fps) {
    SourceFrame& a = store[n % kStoreFrames];
    SourceFrame& b = store[(n + 1) % kStoreFrames];
    synth.Render(a.rgb, n / fps);
    a.n = n;
    synth.Render(b.rgb, (n + 1) / fps);
    b.n = n + 1;
    return {a, b};
}

std::string Ms(const std::optional<Percentiles>& p) { return p ? std::format("{:.2f}", p->p50) : std::string("-"); }

uint32_t TimecodeTestValue(uint32_t k) { return 0x9E3779B9u * (k + 1) ^ (k << 7); }

std::vector<uint8_t> AsCapturedYuy2(const Image& bgra, int downscale) {
    const int w = (int)bgra.width / downscale, h = (int)bgra.height / downscale;
    std::vector<uint8_t> yuy2((size_t)w * h * 2, kNeutralChroma);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double luma = 0;
            for (int dy = 0; dy < downscale; dy++)
                for (int dx = 0; dx < downscale; dx++) {
                    const uint8_t* q = bgra.Pixel((size_t)(x * downscale + dx), (size_t)(y * downscale + dy));
                    luma += kBt709LumaB * q[0] + kBt709LumaG * q[1] + kBt709LumaR * q[2];
                }
            luma /= downscale * downscale;
            yuy2[((size_t)y * w + x) * 2] = (uint8_t)(kLimitedBlack + luma * kLimitedSpan / 255.0 + 0.5);
        }
    return yuy2;
}

bool TestFrameGeneration(Device& dev, SyntheticSource& synth, const CaptureMode& mode) {
    struct Generator {
        int fgQuality;
        std::unique_ptr<Store> store;
        std::unique_ptr<VfxChain> chain;
    };
    std::vector<Generator> generators;
    bool ok = true;
    for (const QualityLevel& level : FgQualityLevels()) {
        Generator g{level.value, MakeStore(dev, mode.width, mode.height), nullptr};
        VfxChain::Config c;
        c.fg = true;
        c.fgQuality = level.value;
        c.srcW = c.outW = mode.width;
        c.srcH = c.outH = mode.height;
        g.chain = VfxChain::Create(dev, c, *g.store);
        if (!g.chain) {
            LOGE("frame generation %s unavailable", level.name);
            ok = false;
            continue;
        }
        generators.push_back(std::move(g));
    }
    Device::Tex truth = dev.CreateTex(mode.width, mode.height, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);

    printf("\n== Frame generation quality vs ground truth (synthetic %ux%u, %d pairs) ==\n", mode.width, mode.height, kPairs);
    printf("%-16s %8s %8s %8s %10s %12s\n", "generator", "t=0.25", "t=0.50", "t=0.75", "mean dB", "ms/frame");
    for (Generator& g : generators) {
        double sum[std::size(kTimesteps)] = {}, totalMs = 0;
        int samples[std::size(kTimesteps)] = {}, runs = 0;
        for (int p = 0; p < kPairs; p++) {
            const int64_t n = kFirstPairFrame + p * kPairSpacing;
            const Pair pair = RenderPair(synth, *g.store, n, mode.rateHz);
            g.chain->Ingest(pair.a);
            g.chain->Ingest(pair.b);
            dev.WaitIdle();
            for (size_t k = 0; k < std::size(kTimesteps); k++) {
                synth.Render(truth, (n + kTimesteps[k]) / mode.rateHz);
                const int64_t t0 = Qpc::Now();
                const VfxOutput out = g.chain->Process(pair.a, &pair.b, kTimesteps[k], false);
                dev.WaitIdle();
                totalMs += Qpc::ToMs(Qpc::Now() - t0);
                runs++;
                if (!out.tex) {
                    LOGE("frame generation %s produced no frame (pair %d, t=%.2f)", FgQualityName(g.fgQuality), p, kTimesteps[k]);
                    ok = false;
                    continue;
                }
                sum[k] += Psnr(ReadBack(dev, out.tex->tex.Get()), ReadBack(dev, truth.tex.Get()));
                samples[k]++;
            }
        }
        auto mean = [](double s, int count) { return count ? s / count : 0.0; };
        const std::string name = std::string("fg-") + FgQualityName(g.fgQuality);
        printf("%-16s %8.2f %8.2f %8.2f %10.2f %12.3f\n", name.c_str(), mean(sum[0], samples[0]), mean(sum[1], samples[1]), mean(sum[2], samples[2]),
               mean(sum[0] + sum[1] + sum[2], samples[0] + samples[1] + samples[2]), totalMs / runs);
    }

    Device::Tex frame = dev.CreateTex(mode.width, mode.height, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    double repeatSum = 0;
    for (int p = 0; p < kPairs; p++) {
        const int64_t n = kFirstPairFrame + p * kPairSpacing;
        synth.Render(frame, n / mode.rateHz);
        synth.Render(truth, (n + 0.5) / mode.rateHz);
        repeatSum += Psnr(ReadBack(dev, frame.tex.Get()), ReadBack(dev, truth.tex.Get()));
    }
    printf("%-16s %8s %8.2f %8s   (repeat previous frame)\n", "repeat", "", repeatSum / kPairs, "");
    return ok;
}

bool TestTimecode(Device& dev, const CaptureMode& mode) {
    struct TimecodeCase {
        UINT width, height;
        int downscale;
        const char* name;
    };
    constexpr TimecodeCase kCases[] = {{1920, 1080, 1, "1080p output -> 1080p capture"}, {3840, 2160, 2, "4K output -> 1080p capture (2x box downscale)"}};
    printf("\n== Loopback timecode encode/decode ==\n");
    Blitter blit;
    blit.Init(dev);
    Device::Tex source = dev.CreateTex(mode.width, mode.height, DXGI_FORMAT_R8G8B8A8_UNORM, true, true);
    bool ok = true;
    for (const TimecodeCase& c : kCases) {
        Device::Tex target = dev.CreateTex(c.width, c.height, DXGI_FORMAT_B8G8R8A8_UNORM, false, true);
        uint32_t decoded = 0;
        for (uint32_t k = 0; k < kTimecodeSamples; k++) {
            BlitParams bp;
            bp.timecode = true;
            bp.timecodeValue = TimecodeTestValue(k);
            blit.Draw(source.srv.Get(), mode.width, mode.height, target.rtv.Get(), c.width, c.height, bp);
            const std::vector<uint8_t> yuy2 = AsCapturedYuy2(ReadBack(dev, target.tex.Get()), c.downscale);
            const int w = (int)c.width / c.downscale, h = (int)c.height / c.downscale;
            if (DecodeTimecode(yuy2.data(), (ptrdiff_t)w * 2, PixFmt::YUY2, w, h) == bp.timecodeValue) decoded++;
        }
        printf("  %-48s decoded %u/%u correctly\n", c.name, decoded, kTimecodeSamples);
        ok = ok && decoded == kTimecodeSamples;
    }
    return ok;
}

void PrintHdrChannels(Device& dev, const SourceFrame& input, const Device::Tex& output) {
    const Image in = ReadBack(dev, input.rgb.tex.Get());
    const Image out = ReadBack(dev, output.tex.Get());
    auto isYellow = [](const uint8_t* q) { return q[0] > 230 && q[1] > 200 && q[2] < 80; };
    auto isBlue = [](const uint8_t* q) { return q[0] < 80 && q[1] > 130 && q[1] < 170 && q[2] > 230; };
    double yellow[3] = {}, blue[3] = {};
    int yellowCount = 0, blueCount = 0;
    for (size_t i = 0; i < (size_t)in.width * in.height; i++) {
        const uint8_t* q = &in.bytes[i * 4];
        uint32_t packed;
        memcpy(&packed, &out.bytes[i * 4], 4);
        const double pq[3] = {(double)(packed & kTenBitMax) / kTenBitMax, (double)((packed >> 10) & kTenBitMax) / kTenBitMax,
                              (double)((packed >> 20) & kTenBitMax) / kTenBitMax};
        double* sum = isYellow(q) ? yellow : isBlue(q) ? blue : nullptr;
        if (!sum) continue;
        for (int c = 0; c < 3; c++) sum[c] += pq[c];
        (sum == yellow ? yellowCount : blueCount)++;
    }
    auto avg = [](double s, int n) { return n ? s / n : 0.0; };
    printf("  channel check (PQ codes, R10G10B10A2 unpacked): yellow -> R %.3f G %.3f B %.3f (n=%d) | blue -> R %.3f G %.3f B %.3f (n=%d)\n",
           avg(yellow[0], yellowCount), avg(yellow[1], yellowCount), avg(yellow[2], yellowCount), yellowCount, avg(blue[0], blueCount),
           avg(blue[1], blueCount), avg(blue[2], blueCount), blueCount);
}

bool BenchmarkChains(Device& dev, SyntheticSource& synth, const CaptureMode& mode, FrameSize output) {
    struct BenchmarkCase {
        const char* name;
        bool fg, sr;
        const char* srQuality;
        bool hdr, hdrAfterSr;
    };
    constexpr BenchmarkCase kCases[] = {
        {"sr streaming-medium", false, true, "streaming-medium", false, false},
        {"sr streaming-ultra", false, true, "streaming-ultra", false, false},
        {"sr low", false, true, "low", false, false},
        {"sr ultra", false, true, "ultra", false, false},
        {"hdr at source size", false, false, "streaming-medium", true, false},
        {"hdr + sr streaming-medium (10-bit)", false, true, "streaming-medium", true, false},
        {"sr streaming-medium + hdr-after-sr", false, true, "streaming-medium", true, true},
        {"fg low + hdr + sr streaming-medium", true, true, "streaming-medium", true, false},
    };
    printf("\n== SR / TrueHDR timing (GPU, CUDA events) ==\n");
    bool ok = true;
    for (const BenchmarkCase& c : kCases) {
        const std::unique_ptr<Store> store = MakeStore(dev, mode.width, mode.height);
        VfxChain::Config cfg;
        cfg.fg = c.fg;
        cfg.sr = c.sr;
        cfg.srQuality = *FindSrQuality(c.srQuality);
        cfg.hdr = c.hdr;
        cfg.hdrAfterSr = c.hdrAfterSr;
        cfg.srcW = mode.width;
        cfg.srcH = mode.height;
        cfg.outW = c.sr ? output.width : mode.width;
        cfg.outH = c.sr ? output.height : mode.height;
        const std::unique_ptr<VfxChain> chain = VfxChain::Create(dev, cfg, *store);
        if (!chain) {
            printf("%-36s  init failed\n", c.name);
            ok = false;
            continue;
        }
        StatBook timings;
        for (int i = 0; i < kBenchmarkFrames; i++) {
            const Pair pair = RenderPair(synth, *store, kBenchmarkFirstFrame + i, mode.rateHz);
            chain->Ingest(pair.a);
            chain->Ingest(pair.b);
            const int64_t t0 = Qpc::Now();
            chain->Process(pair.a, c.fg ? &pair.b : nullptr, 0.5f, true);
            dev.WaitIdle();
            const bool measured = i >= kBenchmarkWarmUpFrames;
            if (measured) timings.Add("wall", Qpc::ToMs(Qpc::Now() - t0));
            for (const VfxTimings& t : chain->PopTimings())
                if (measured) RecordVfxTimings(timings, t);
        }
        if (c.hdr && !c.sr) {
            SourceFrame& frame = (*store)[kChannelCheckFrame % kStoreFrames];
            synth.Render(frame.rgb, kChannelCheckFrame / mode.rateHz);
            frame.n = kChannelCheckFrame;
            chain->Ingest(frame);
            const VfxOutput out = chain->Process(frame, nullptr, 0.f, true);
            dev.WaitIdle();
            if (out.tex) PrintHdrChannels(dev, frame, *out.tex);
        }
        const std::optional<Percentiles> total = timings.Get(stat::kVfxGpuTotal);
        printf("%-36s fg %s  hdr %s  sr %s  out %s | gpu total p50 %s p99 %s | wall p50 %s ms\n", c.name, Ms(timings.Get(stat::kVfxFg)).c_str(),
               Ms(timings.Get(stat::kVfxHdr)).c_str(), Ms(timings.Get(stat::kVfxSr)).c_str(), Ms(timings.Get(stat::kVfxOut)).c_str(), Ms(total).c_str(),
               total ? std::format("{:.2f}", total->p99).c_str() : "-", Ms(timings.Get("wall")).c_str());
    }
    return ok;
}

}

bool RunSelfTest(const Options& o) {
    Device dev;
    dev.Create(o.d3dDebug);
    SyntheticSource synth;
    synth.Init(dev);
    const bool fgOk = TestFrameGeneration(dev, synth, o.capture);
    const bool timecodeOk = TestTimecode(dev, o.capture);
    const bool benchmarkOk = BenchmarkChains(dev, synth, o.capture, o.out.value_or(kBenchmarkOutput));
    const bool ok = fgOk && timecodeOk && benchmarkOk;
    printf("\nselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

}
