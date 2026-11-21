// Repeat-frame detector test + benchmark (CPU only, synthetic frames).
//   repeat_test            run the tests and the benchmark
//   repeat_test --no-bench run the tests only
// Prints PASS/FAIL; exit code 0 = all tests passed.
#include "capture/frame_diff.h"
#include "util/common.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace camss;

static int g_fail = 0, g_checks = 0;
#define EXPECT(cond, ...) do { g_checks++; const bool _ok = (cond); if (!_ok) g_fail++; \
    printf("  %s ", _ok ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

// ------------------------------------------------------------------ synthetic content

static uint32_t Hash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    int Noise(int amp) { return amp ? (int)(Next() % (uint32_t)(2 * amp + 1)) - amp : 0; }
};

enum class Fmt { YUY2, NV12, P010, RGB32, RGB24 };

struct Obj { int x, y, size; };

struct Params {
    int panX = 0;            // horizontal pan in pixels
    int bright = 0;          // added to luma (8-bit levels)
    bool dark = false;       // dark low-contrast scene (luma ~16..40)
    bool alt = false;        // a completely different picture (scene cut)
    std::vector<Obj> objs;   // high-contrast checker objects (4 px cells)
    int noiseAmp = 0;        // +-noise per written byte (P010: per 10-bit sample)
    uint32_t noiseSeed = 1;
    std::vector<std::pair<int, int>> spikes; // isolated single-sample glitches (x, y): luma += 100
};

// Background: gradient + smooth value noise + fine noise texture + a few shapes. Precomputed
// with a margin so pans read real content.
struct Scene {
    int W, H, TW;
    std::vector<uint8_t> lum;        // TW x H
    std::vector<uint8_t> cu, cv;     // TW/2 x H
    Scene(int w, int h) : W(w), H(h), TW(w + 64) {
        lum.resize((size_t)TW * H);
        cu.resize((size_t)(TW / 2) * H);
        cv.resize((size_t)(TW / 2) * H);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < TW; x++) {
                // smooth noise: bilinear interpolation of a 16 px lattice
                const int gx = x / 16, gy = y / 16;
                const float fx = (x % 16) / 16.f, fy = (y % 16) / 16.f;
                auto g = [&](int a, int b) { return (float)(Hash(a, b, 7) & 255) / 255.f; };
                const float sm = (g(gx, gy) * (1 - fx) + g(gx + 1, gy) * fx) * (1 - fy) + (g(gx, gy + 1) * (1 - fx) + g(gx + 1, gy + 1) * fx) * fy;
                float v = 40.f + 110.f * x / TW + 50.f * y / H + 50.f * sm + (float)(Hash(x, y, 3) % 25) - 12.f;
                // shapes: a few discs and a bar
                for (int k = 0; k < 5; k++) {
                    const int cx = (int)(Hash(k, 0, 11) % (uint32_t)TW), cy = (int)(Hash(k, 1, 11) % (uint32_t)H);
                    const int r = 40 + (int)(Hash(k, 2, 11) % 120);
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r) v += (k & 1) ? 45.f : -45.f;
                }
                if (y > H / 3 && y < H / 3 + 40) v = 0.6f * v + 30.f;
                lum[(size_t)y * TW + x] = (uint8_t)std::clamp((int)v, 16, 235);
            }
            for (int x = 0; x < TW / 2; x++) {
                cu[(size_t)y * (TW / 2) + x] = (uint8_t)std::clamp(128 + (int)(30 * std::sin(x * 0.011)) + (int)(Hash(x, y, 5) % 7) - 3, 16, 240);
                cv[(size_t)y * (TW / 2) + x] = (uint8_t)std::clamp(128 + (int)(30 * std::cos(y * 0.013)) + (int)(Hash(x, y, 9) % 7) - 3, 16, 240);
            }
        }
    }
    int Luma(int x, int y, const Params& p) const {
        int v = lum[(size_t)y * TW + std::clamp(x + p.panX, 0, TW - 1)];
        if (p.alt) v = 251 - v;
        if (p.dark) v = 16 + (v - 16) / 9;
        v += p.bright;
        for (const Obj& o : p.objs)
            if (x >= o.x && x < o.x + o.size && y >= o.y && y < o.y + o.size)
                v = (((x - o.x) / 4 + (y - o.y) / 4) & 1) ? 220 : 30;
        for (auto& s : p.spikes)
            if (s.first == x && s.second == y) v += 100;
        return std::clamp(v, 0, 255);
    }
    int ChromaU(int x, int y, const Params& p) const {
        int v = cu[(size_t)y * (TW / 2) + std::clamp((x + p.panX) / 2, 0, TW / 2 - 1)];
        return p.alt ? 256 - v : v;
    }
    int ChromaV(int x, int y, const Params& p) const {
        int v = cv[(size_t)y * (TW / 2) + std::clamp((x + p.panX) / 2, 0, TW / 2 - 1)];
        return p.alt ? 256 - v : v;
    }
};

struct Frame {
    std::vector<uint8_t> data;
    size_t stride = 0;     // bytes (positive)
    int rowBytes = 0;      // compared bytes per row (luma plane for NV12 / P010)
    int rows = 0;
    bool bottomUp = false;
    DiffLayout layout = DiffLayout::Bytes8;
    const uint8_t* Top() const { return bottomUp ? data.data() + stride * (rows - 1) : data.data(); }
    ptrdiff_t Pitch() const { return bottomUp ? -(ptrdiff_t)stride : (ptrdiff_t)stride; }
};

static uint8_t Clamp8(int v) { return (uint8_t)std::clamp(v, 0, 255); }

static Frame Render(const Scene& sc, Fmt fmt, const Params& p, int extraStride = 0) {
    const int W = sc.W, H = sc.H;
    Frame f;
    Rng rng(p.noiseSeed);
    const int a = p.noiseAmp;
    switch (fmt) {
    case Fmt::YUY2: f.rowBytes = W * 2; break;
    case Fmt::NV12: f.rowBytes = W; break;
    case Fmt::P010: f.rowBytes = W * 2; f.layout = DiffLayout::HighBytes16; break;
    case Fmt::RGB32: f.rowBytes = W * 4; break;
    case Fmt::RGB24: f.rowBytes = W * 3; break;
    }
    f.rows = H;
    f.stride = (size_t)f.rowBytes + extraStride;
    const size_t planeRows = (fmt == Fmt::NV12 || fmt == Fmt::P010) ? (size_t)H * 3 / 2 : (size_t)H;
    f.data.assign(f.stride * planeRows, 0);
    for (int y = 0; y < H; y++) {
        uint8_t* row = f.data.data() + f.stride * y;
        for (int x = 0; x < W; x++) {
            const int Y = sc.Luma(x, y, p), U = sc.ChromaU(x, y, p), V = sc.ChromaV(x, y, p);
            switch (fmt) {
            case Fmt::YUY2:
                row[x * 2] = Clamp8(Y + rng.Noise(a));
                row[x * 2 + 1] = Clamp8(((x & 1) ? V : U) + rng.Noise(a));
                break;
            case Fmt::NV12:
                row[x] = Clamp8(Y + rng.Noise(a));
                break;
            case Fmt::P010: {
                const int v10 = std::clamp(Y * 4 + rng.Noise(a), 0, 1023);
                const uint16_t s = (uint16_t)(v10 << 6);
                row[x * 2] = (uint8_t)(s & 0xFF);
                row[x * 2 + 1] = (uint8_t)(s >> 8);
                break;
            }
            case Fmt::RGB32:
            case Fmt::RGB24: {
                const int bpp = fmt == Fmt::RGB32 ? 4 : 3;
                const float c = 1.164f * (Y - 16), d = (float)(U - 128), e = (float)(V - 128);
                row[x * bpp + 0] = Clamp8((int)(c + 2.018f * d) + rng.Noise(a));
                row[x * bpp + 1] = Clamp8((int)(c - 0.391f * d - 0.813f * e) + rng.Noise(a));
                row[x * bpp + 2] = Clamp8((int)(c + 1.596f * e) + rng.Noise(a));
                if (bpp == 4) row[x * 4 + 3] = 255;
                break;
            }
            }
        }
    }
    // Chroma planes (not compared, present for a realistic memory layout).
    if (fmt == Fmt::NV12 || fmt == Fmt::P010) {
        for (int y = 0; y < H / 2; y++) {
            uint8_t* row = f.data.data() + f.stride * (H + y);
            for (int x = 0; x < W / 2; x++) {
                const int U = sc.ChromaU(x * 2, y * 2, p), V = sc.ChromaV(x * 2, y * 2, p);
                if (fmt == Fmt::NV12) { row[x * 2] = (uint8_t)U; row[x * 2 + 1] = (uint8_t)V; }
                else { row[x * 4 + 1] = (uint8_t)(U >> 2); row[x * 4] = (uint8_t)(U << 6); row[x * 4 + 3] = (uint8_t)(V >> 2); row[x * 4 + 2] = (uint8_t)(V << 6); }
            }
        }
    }
    return f;
}

static Frame FlipToBottomUp(const Frame& in) {
    Frame f = in;
    f.bottomUp = true;
    for (int y = 0; y < in.rows; y++)
        memcpy(f.data.data() + f.stride * (in.rows - 1 - y), in.data.data() + in.stride * y, in.stride);
    return f;
}

// Scalar reference of the detector's statistics for (prev -> cur).
struct RefStats { uint64_t changed = 0, sad = 0; uint32_t maxTile = 0; };
static RefStats Reference(const Frame& prev, const Frame& cur, const RepeatConfig& cfg) {
    RefStats r;
    const bool hi = cur.layout == DiffLayout::HighBytes16;
    const int n = hi ? cur.rowBytes / 2 : cur.rowBytes;
    const int bandRows = std::max(1, 16 / cfg.rowStep);
    std::vector<uint32_t> tiles((n + 63) / 64, 0);
    int cr = 0;
    for (int y = 0; y < cur.rows; y += cfg.rowStep, cr++) {
        if (cr % bandRows == 0) std::fill(tiles.begin(), tiles.end(), 0u);
        const uint8_t* a = cur.Top() + (ptrdiff_t)y * cur.Pitch();
        const uint8_t* b = prev.Top() + (ptrdiff_t)y * prev.Pitch();
        for (int i = 0; i < n; i++) {
            const int va = hi ? a[2 * i + 1] : a[i], vb = hi ? b[2 * i + 1] : b[i];
            const int d = std::abs(va - vb);
            r.sad += d;
            if (d > cfg.levelThreshold) { r.changed++; tiles[i / 64]++; r.maxTile = std::max(r.maxTile, tiles[i / 64]); }
        }
    }
    return r;
}

static void Configure(RepeatDetector& d, const Frame& f, const RepeatConfig& cfg = {}) {
    d.Configure(f.layout, f.rowBytes, f.rows, cfg);
}
static bool Check(RepeatDetector& d, const Frame& f) { return d.Check(f.Top(), f.Pitch()); }

static void Stats(const char* what, const RepeatDetector& d) {
    printf("         %-34s changed %7llu (frac %.2e) maxTile %4d mean|d| %.3f cost %.1f us\n", what,
           (unsigned long long)d.LastChangedCount(), d.LastChangedFrac(), d.LastMaxTileChanged(), d.LastMeanAbsDiff(),
           d.LastCostMs() * 1000.0);
}

// Configure, check `a` (first frame, never a repeat), then `b`; returns Check(b).
static bool Pair(RepeatDetector& d, const Frame& a, const Frame& b, const char* what, const RepeatConfig& cfg = {}) {
    Configure(d, a, cfg);
    const bool first = Check(d, a);
    if (first) { g_fail++; printf("  FAIL first frame reported as repeat (%s)\n", what); }
    const bool r = Check(d, b);
    Stats(what, d);
    return r;
}

// ------------------------------------------------------------------ tests

static void TestYuy2(const Scene& sc) {
    printf("YUY2 1920x1080 (rowStep 2, threshold 6, maxChangedFrac 2e-5, maxTileChanged 16)\n");
    RepeatDetector d;
    Params base;
    base.objs = {{900, 500, 32}};
    const Frame f0 = Render(sc, Fmt::YUY2, base);
    // first frame
    Configure(d, f0);
    EXPECT(!Check(d, f0), "first frame is never a repeat");
    EXPECT(Check(d, f0), "identical frame -> repeat");
    Stats("identical", d);
    EXPECT(d.LastChangedCount() == 0 && d.LastMeanAbsDiff() == 0.0, "identical frame: 0 changed, mean 0");

    {   // +-2 noise everywhere on both frames (independent)
        Params a = base, b = base;
        a.noiseAmp = 2; a.noiseSeed = 11; b.noiseAmp = 2; b.noiseSeed = 12;
        EXPECT(Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "+-2 noise (|d|<=4)"), "+-2 level noise everywhere -> repeat");
    }
    {   // 32x32 object moves by 4 px
        Params b = base; b.objs = {{904, 500, 32}};
        EXPECT(!Pair(d, f0, Render(sc, Fmt::YUY2, b), "32x32 object moved 4 px"), "32x32 moving object -> not repeat");
    }
    {   // 16x16 object appears/moves by 4 px (smaller than one tile, may straddle tiles)
        Params a = base, b = base;
        a.objs.push_back({1203, 301, 16}); b.objs.push_back({1207, 301, 16});
        EXPECT(!Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "16x16 object moved 4 px"), "16x16 moving object -> not repeat");
    }
    {   // a 30x30 cursor on a NOISY-source configuration where the global fraction alone would hide it
        Params a = base, b = base;
        a.objs.push_back({400, 700, 30}); b.objs.push_back({406, 703, 30});
        RepeatConfig loose; loose.maxChangedFrac = 1e-3;   // 2070 samples allowed
        const Frame fa = Render(sc, Fmt::YUY2, a), fb = Render(sc, Fmt::YUY2, b);
        EXPECT(!Pair(d, fa, fb, "30x30 cursor, maxChangedFrac 1e-3", loose), "30x30 cursor with maxChangedFrac 1e-3 -> not repeat (tile check)");
        EXPECT(d.LastChangedFrac() < loose.maxChangedFrac, "  ... global fraction %.2e alone would have hidden it", d.LastChangedFrac());
        RepeatConfig noTile = loose; noTile.maxTileChanged = -1;
        EXPECT(Pair(d, fa, fb, "same, tile check off", noTile), "  ... and with the tile check off it is (wrongly) a repeat");
    }
    {   // isolated single-sample glitches spread over the frame: still a repeat
        Params b = base;
        Rng r(5);
        for (int k = 0; k < 20; k++) b.spikes.push_back({(int)(r.Next() % 1920), (int)(r.Next() % 540) * 2});
        EXPECT(Pair(d, f0, Render(sc, Fmt::YUY2, b), "20 isolated glitch samples"), "20 isolated strong single-sample glitches -> repeat");
    }
    {   // slow global pan of 1 px
        Params b = base; b.panX = 1;
        EXPECT(!Pair(d, f0, Render(sc, Fmt::YUY2, b), "global pan 1 px"), "1 px global pan -> not repeat");
    }
    {   // dark low-contrast scene, +1 level brightness everywhere
        Params a = base, b = base;
        a.dark = true; b.dark = true; b.bright = 1; a.objs.clear(); b.objs.clear();
        EXPECT(Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "dark scene +1 level"),
               "dark low-contrast scene, +1 level everywhere -> repeat (below threshold, by design)");
    }
    {   // full-frame change (scene cut)
        Params b = base; b.alt = true;
        EXPECT(!Pair(d, f0, Render(sc, Fmt::YUY2, b), "scene cut"), "full-frame change -> not repeat");
        EXPECT(d.LastChangedFrac() > 0.5, "  ... changed fraction %.3f", d.LastChangedFrac());
    }
    {   // Reset(): the next frame is never a repeat
        Configure(d, f0);
        Check(d, f0);
        EXPECT(Check(d, f0), "identical before Reset -> repeat");
        d.Reset();
        EXPECT(!Check(d, f0), "identical right after Reset() -> not repeat");
        EXPECT(Check(d, f0), "identical after that -> repeat");
    }
    {   // reference = last content frame: a sub-threshold per-frame drift accumulates
        Params p = base; p.objs.clear(); p.dark = true;
        Configure(d, Render(sc, Fmt::YUY2, p));
        Check(d, Render(sc, Fmt::YUY2, p));
        int firstNew = -1;
        for (int k = 1; k <= 10 && firstNew < 0; k++) {
            p.bright = k;
            if (!Check(d, Render(sc, Fmt::YUY2, p))) firstNew = k;
        }
        EXPECT(firstNew == 7, "+1 level per frame drift: first non-repeat after %d frames (expected 7 = threshold 6 + 1)", firstNew);
    }
    {   // 30 fps content in 60 Hz: every frame sent twice (independent +-1 transmission noise); pan 3 px per content frame
        Configure(d, f0);
        int good = 0;
        const int N = 20;
        double maxCost = 0;
        for (int i = 0; i < N; i++) {
            Params p = base; p.panX = (i / 2) * 3; p.objs = {{100 + (i / 2) * 8, 200, 32}};
            p.noiseAmp = 1; p.noiseSeed = 100 + i;
            const bool rep = Check(d, Render(sc, Fmt::YUY2, p));
            maxCost = std::max(maxCost, d.LastCostMs());
            if (rep == (i % 2 == 1)) good++;
        }
        EXPECT(good == N, "30-in-60 cadence: %d/%d frames classified correctly (new, repeat, new, ...)", good, N);
    }
}

static void TestExactness(const Scene& sc, bool avx2) {
    printf("%s kernel vs scalar reference (all layouts, odd widths, padded and negative pitch)\n", avx2 ? "AVX2" : "SSE2");
    struct Case { Fmt fmt; int W, H; int step; int extra; bool flip; const char* name; bool cut = false; };
    const Case cases[] = {
        {Fmt::YUY2, 1920, 1080, 2, 0, false, "YUY2 1920x1080 step2"},
        {Fmt::YUY2, 1920, 1080, 1, 0, false, "YUY2 1920x1080 step1"},
        {Fmt::NV12, 1920, 1080, 2, 0, false, "NV12 luma 1920x1080"},
        {Fmt::P010, 1920, 1080, 2, 0, false, "P010 luma 1920x1080"},
        {Fmt::RGB32, 1920, 1080, 2, 0, true, "RGB32 1920x1080 bottom-up"},
        {Fmt::RGB24, 1366, 768, 3, 6, true, "RGB24 1366x768 step3 pad+bottom-up"},
        {Fmt::YUY2, 1366, 769, 2, 64, false, "YUY2 1366x769 padded"},
        {Fmt::P010, 1366, 768, 1, 4, true, "P010 1366x768 bottom-up"},
        {Fmt::NV12, 77, 33, 1, 3, false, "NV12 77x33 (tail only)"},
        {Fmt::NV12, 1023, 100, 1, 1, false, "NV12 1023x100 (63-sample tail)"},
        {Fmt::RGB32, 1000, 200, 2, 0, true, "RGB32 1000x200 (32-sample tail)"},
        {Fmt::YUY2, 1920, 1080, 1, 0, false, "YUY2 scene cut step1 (counter range)", true},
        {Fmt::P010, 1920, 1080, 1, 0, true, "P010 scene cut step1 bottom-up", true},
    };
    for (const Case& c : cases) {
        Scene s2 = (c.W == sc.W && c.H == sc.H) ? sc : Scene(c.W, c.H);
        Params a, b;
        a.objs = {{c.W / 3, c.H / 3, 40}}; b.objs = {{c.W / 3 + 4, c.H / 3 + 2, 40}, {c.W - 20, c.H - 30, 18}};
        a.noiseAmp = 5; a.noiseSeed = 1; b.noiseAmp = 5; b.noiseSeed = 2;
        b.alt = c.cut;
        Frame fa = Render(s2, c.fmt, a, c.extra), fb = Render(s2, c.fmt, b, c.extra);
        if (c.flip) { fa = FlipToBottomUp(fa); fb = FlipToBottomUp(fb); }
        RepeatConfig cfg; cfg.rowStep = c.step;
        RepeatDetector d;
        d.SetAllowAvx2(avx2);
        Configure(d, fa, cfg);
        Check(d, fa);
        Check(d, fb);
        const RefStats r = Reference(fa, fb, cfg);
        const uint64_t sad = (uint64_t)std::llround(d.LastMeanAbsDiff() * (double)d.ComparedSamples());
        EXPECT(d.LastChangedCount() == r.changed && sad == r.sad && (uint32_t)d.LastMaxTileChanged() == r.maxTile,
               "%-36s changed %llu/%llu sad %llu/%llu maxTile %d/%u", c.name, (unsigned long long)d.LastChangedCount(),
               (unsigned long long)r.changed, (unsigned long long)sad, (unsigned long long)r.sad, d.LastMaxTileChanged(), r.maxTile);
    }
}

static void TestOtherFormats(const Scene& sc) {
    printf("Other formats\n");
    RepeatDetector d;
    Params base; base.objs = {{900, 500, 32}};
    Params moved = base; moved.objs = {{904, 500, 32}};
    {   // P010: low-bit noise (+-3 10-bit levels -> high byte moves by <= 1) is a repeat; real change is not
        Params a = base, b = base;
        a.noiseAmp = 3; a.noiseSeed = 21; b.noiseAmp = 3; b.noiseSeed = 22;
        const Frame fa = Render(sc, Fmt::P010, a), fb = Render(sc, Fmt::P010, b);
        EXPECT(Pair(d, fa, fb, "P010 +-3 (10-bit) noise"), "P010 (HighBytes16) low-bit noise -> repeat");
        Params c = moved; c.noiseAmp = 3; c.noiseSeed = 23;
        EXPECT(!Pair(d, fa, Render(sc, Fmt::P010, c), "P010 object moved"), "P010 moving object -> not repeat");
    }
    {
        const Frame fa = Render(sc, Fmt::NV12, base);
        EXPECT(Pair(d, fa, fa, "NV12 identical"), "NV12 luma identical -> repeat");
        EXPECT(!Pair(d, fa, Render(sc, Fmt::NV12, moved), "NV12 object moved"), "NV12 luma moving object -> not repeat");
    }
    {
        const Frame fa = Render(sc, Fmt::RGB32, base);
        Params n = base; n.noiseAmp = 2; n.noiseSeed = 31;
        EXPECT(Pair(d, fa, Render(sc, Fmt::RGB32, n), "RGB32 +-2 noise"), "RGB32 +-2 noise -> repeat");
        EXPECT(!Pair(d, fa, Render(sc, Fmt::RGB32, moved), "RGB32 object moved"), "RGB32 moving object -> not repeat");
    }
    {   // negative pitch (bottom-up RGB24 / RGB32) gives exactly the same result as top-down
        const Frame ta = Render(sc, Fmt::RGB24, base), tb = Render(sc, Fmt::RGB24, moved);
        const Frame ba = FlipToBottomUp(ta), bb = FlipToBottomUp(tb);
        const bool rt = Pair(d, ta, tb, "RGB24 top-down moved");
        const uint64_t ct = d.LastChangedCount();
        const bool rb = Pair(d, ba, bb, "RGB24 bottom-up moved");
        EXPECT(!rt && !rb && ct == d.LastChangedCount(), "negative pitch: same decision and changed count as top-down");
        EXPECT(Pair(d, ba, ba, "RGB24 bottom-up identical"), "negative pitch identical -> repeat");
    }
    {   // not configured / null pointer: never a repeat, no crash
        RepeatDetector e;
        uint8_t px[64] = {};
        EXPECT(!e.Check(px, 64) && !e.Check(px, 64), "unconfigured detector -> never a repeat");
        Configure(e, Render(sc, Fmt::NV12, base));
        EXPECT(!e.Check(nullptr, 0), "null frame -> not a repeat");
    }
}

// ------------------------------------------------------------------ benchmark

static void Bench(const char* name, const Scene& sc, Fmt fmt, int step, int nBuf, bool afterCopy, bool avx2 = true) {
    // nBuf distinct buffers (pairs of identical content in different memory: repeat / new alternate)
    std::vector<Frame> bufs;
    bufs.reserve(nBuf);
    Params p; p.objs = {{300, 300, 32}};
    const Frame base = Render(sc, fmt, p);
    for (int i = 0; i < nBuf; i++) {
        bufs.push_back(base);
        Frame& f = bufs.back();
        // move a 32x32 block per content frame (cheap edit of the copy)
        const int pairIdx = i / 2;
        const int bpp = f.rowBytes / sc.W;
        for (int y = 100; y < 132; y++)
            for (int x = 0; x < 32 * bpp; x++)
                f.data[f.stride * y + (size_t)(100 + pairIdx * 8) * bpp + x] = (uint8_t)(Hash(x, y, pairIdx) & 255);
    }
    std::vector<uint8_t> dst(afterCopy ? bufs[0].data.size() : 0);
    RepeatDetector d;
    d.SetAllowAvx2(avx2);
    RepeatConfig cfg; cfg.rowStep = step;
    Configure(d, bufs[0], cfg);
    const int warm = 20, iters = 500;
    std::vector<double> us;
    us.reserve(iters);
    int repeats = 0;
    for (int i = 0; i < warm + iters; i++) {
        const Frame& f = bufs[i % nBuf];
        if (afterCopy) memcpy(dst.data(), f.data.data(), f.data.size());   // the capture upload memcpy reads the frame first
        const bool r = Check(d, f);
        if (i >= warm) { us.push_back(d.LastCostMs() * 1000.0); repeats += r ? 1 : 0; }
    }
    std::sort(us.begin(), us.end());
    const double mb = (double)d.ComparedSamples() / 1e6;
    printf("  %s %-42s %5.2f MB cmp  p50 %7.1f us  p99 %7.1f us  max %7.1f us  (repeats %d/%d)\n", d.UsingAvx2() ? "AVX2" : "SSE2", name, mb,
           us[us.size() / 2], us[(size_t)(us.size() * 0.99)], us.back(), repeats, iters);
}

int main(int argc, char** argv) {
    const bool bench = !(argc > 1 && strcmp(argv[1], "--no-bench") == 0);
    SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);
    const Scene sc(1920, 1080);
    TestYuy2(sc);
    TestOtherFormats(sc);
    printf("AVX2 supported: %s\n", RepeatDetector::Avx2Supported() ? "yes" : "no");
    TestExactness(sc, false);
    if (RepeatDetector::Avx2Supported()) TestExactness(sc, true);
    if (bench) {
        printf("Benchmark: Check() cost (LastCostMs, QPC) over 500 iterations, rotating distinct buffers\n");
        Bench("1080p YUY2 rowStep 2, 8 bufs (L3)", sc, Fmt::YUY2, 2, 8, false, false);
        Bench("1080p YUY2 rowStep 2, after upload memcpy", sc, Fmt::YUY2, 2, 8, true, false);
        Bench("1080p YUY2 rowStep 2, 8 bufs (L3)", sc, Fmt::YUY2, 2, 8, false);
        Bench("1080p YUY2 rowStep 1, 8 bufs (L3)", sc, Fmt::YUY2, 1, 8, false);
        Bench("1080p YUY2 rowStep 2, 48 bufs (DRAM)", sc, Fmt::YUY2, 2, 48, false);
        Bench("1080p YUY2 rowStep 1, 48 bufs (DRAM)", sc, Fmt::YUY2, 1, 48, false);
        Bench("1080p YUY2 rowStep 2, after upload memcpy", sc, Fmt::YUY2, 2, 8, true);
        Bench("1080p NV12 rowStep 2, 8 bufs", sc, Fmt::NV12, 2, 8, false);
        Bench("1080p P010 rowStep 2, 8 bufs", sc, Fmt::P010, 2, 8, false);
        const Scene sc4(3840, 2160);
        Bench("2160p YUY2 rowStep 2, 8 bufs", sc4, Fmt::YUY2, 2, 8, false);
        Bench("2160p YUY2 rowStep 1, 8 bufs", sc4, Fmt::YUY2, 1, 8, false);
        Bench("2160p YUY2 rowStep 2, after upload memcpy", sc4, Fmt::YUY2, 2, 8, true);
    }
    printf("%s (%d checks, %d failed)\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
