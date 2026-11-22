#include "check.h"
#include "capture/frame_diff.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using namespace cameravfx;

using check::Expect;

RepeatConfig DefaultRepeat() {
    RepeatConfig cfg;
    cfg.levelThreshold = 6;
    cfg.maxChangedFrac = 2e-5;
    cfg.maxTileChanged = 16;
    return cfg;
}

constexpr int kSceneMargin = 64;
constexpr int kLatticeCell = 16;
constexpr int kCheckerCellPixels = 4;
constexpr int kGlitchLevel = 100;

static uint32_t Hash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t Next() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return (uint32_t)(state >> 16); }
    int Noise(int amplitude) { return amplitude ? (int)(Next() % (uint32_t)(2 * amplitude + 1)) - amplitude : 0; }
};

enum class Fmt { YUY2, NV12, P010, RGB32, RGB24 };

struct Checker { int x, y, size; };

struct Params {
    int panX = 0;
    int brightnessOffset = 0;
    bool darkLowContrast = false;
    bool sceneCut = false;
    std::vector<Checker> checkers;
    int noiseAmplitude = 0;
    uint32_t noiseSeed = 1;
    std::vector<std::pair<int, int>> glitches;
};

struct Scene {
    int width, height, paddedWidth;
    std::vector<uint8_t> luma;
    std::vector<uint8_t> chromaU, chromaV;
    Scene(int w, int h) : width(w), height(h), paddedWidth(w + kSceneMargin) {
        const int chromaWidth = paddedWidth / 2;
        luma.resize((size_t)paddedWidth * height);
        chromaU.resize((size_t)chromaWidth * height);
        chromaV.resize((size_t)chromaWidth * height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < paddedWidth; x++) {
                const int gx = x / kLatticeCell, gy = y / kLatticeCell;
                const float fx = (float)(x % kLatticeCell) / kLatticeCell, fy = (float)(y % kLatticeCell) / kLatticeCell;
                auto lattice = [&](int a, int b) { return (float)(Hash(a, b, 7) & 255) / 255.f; };
                const float smooth = (lattice(gx, gy) * (1 - fx) + lattice(gx + 1, gy) * fx) * (1 - fy) +
                                     (lattice(gx, gy + 1) * (1 - fx) + lattice(gx + 1, gy + 1) * fx) * fy;
                float v = 40.f + 110.f * x / paddedWidth + 50.f * y / height + 50.f * smooth + (float)(Hash(x, y, 3) % 25) - 12.f;
                for (int k = 0; k < 5; k++) {
                    const int cx = (int)(Hash(k, 0, 11) % (uint32_t)paddedWidth), cy = (int)(Hash(k, 1, 11) % (uint32_t)height);
                    const int r = 40 + (int)(Hash(k, 2, 11) % 120);
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < r * r) v += (k & 1) ? 45.f : -45.f;
                }
                if (y > height / 3 && y < height / 3 + 40) v = 0.6f * v + 30.f;
                luma[(size_t)y * paddedWidth + x] = (uint8_t)std::clamp((int)v, 16, 235);
            }
            for (int x = 0; x < chromaWidth; x++) {
                chromaU[(size_t)y * chromaWidth + x] = (uint8_t)std::clamp(128 + (int)(30 * std::sin(x * 0.011)) + (int)(Hash(x, y, 5) % 7) - 3, 16, 240);
                chromaV[(size_t)y * chromaWidth + x] = (uint8_t)std::clamp(128 + (int)(30 * std::cos(y * 0.013)) + (int)(Hash(x, y, 9) % 7) - 3, 16, 240);
            }
        }
    }
    int Luma(int x, int y, const Params& p) const {
        int v = luma[(size_t)y * paddedWidth + std::clamp(x + p.panX, 0, paddedWidth - 1)];
        if (p.sceneCut) v = 251 - v;
        if (p.darkLowContrast) v = 16 + (v - 16) / 9;
        v += p.brightnessOffset;
        for (const Checker& c : p.checkers)
            if (x >= c.x && x < c.x + c.size && y >= c.y && y < c.y + c.size)
                v = (((x - c.x) / kCheckerCellPixels + (y - c.y) / kCheckerCellPixels) & 1) ? 220 : 30;
        for (const auto& g : p.glitches)
            if (g.first == x && g.second == y) v += kGlitchLevel;
        return std::clamp(v, 0, 255);
    }
    int ChromaU(int x, int y, const Params& p) const { return Chroma(chromaU, x, y, p); }
    int ChromaV(int x, int y, const Params& p) const { return Chroma(chromaV, x, y, p); }

private:
    int Chroma(const std::vector<uint8_t>& plane, int x, int y, const Params& p) const {
        const int chromaWidth = paddedWidth / 2;
        const int v = plane[(size_t)y * chromaWidth + std::clamp((x + p.panX) / 2, 0, chromaWidth - 1)];
        return p.sceneCut ? 256 - v : v;
    }
};

struct Frame {
    std::vector<uint8_t> data;
    size_t stride = 0;
    int rowBytes = 0;
    int rows = 0;
    bool bottomUp = false;
    DiffLayout layout = DiffLayout::Bytes8;
    const uint8_t* Top() const { return bottomUp ? data.data() + stride * (rows - 1) : data.data(); }
    ptrdiff_t Pitch() const { return bottomUp ? -(ptrdiff_t)stride : (ptrdiff_t)stride; }
};

static uint8_t Clamp8(int v) { return (uint8_t)std::clamp(v, 0, 255); }

static Frame Render(const Scene& sc, Fmt fmt, const Params& p, int extraStride = 0) {
    const int w = sc.width, h = sc.height;
    Frame f;
    Rng rng(p.noiseSeed);
    const int a = p.noiseAmplitude;
    switch (fmt) {
    case Fmt::YUY2: f.rowBytes = w * 2; break;
    case Fmt::NV12: f.rowBytes = w; break;
    case Fmt::P010: f.rowBytes = w * 2; f.layout = DiffLayout::HighBytes16; break;
    case Fmt::RGB32: f.rowBytes = w * 4; break;
    case Fmt::RGB24: f.rowBytes = w * 3; break;
    }
    f.rows = h;
    f.stride = (size_t)f.rowBytes + extraStride;
    const bool twoPlanes = fmt == Fmt::NV12 || fmt == Fmt::P010;
    const size_t planeRows = twoPlanes ? (size_t)h * 3 / 2 : (size_t)h;
    f.data.assign(f.stride * planeRows, 0);
    for (int y = 0; y < h; y++) {
        uint8_t* row = f.data.data() + f.stride * y;
        for (int x = 0; x < w; x++) {
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
    if (twoPlanes) {
        for (int y = 0; y < h / 2; y++) {
            uint8_t* row = f.data.data() + f.stride * (h + y);
            for (int x = 0; x < w / 2; x++) {
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

struct ScalarStats { uint64_t changed = 0; uint32_t maxTile = 0; };

static ScalarStats ScalarReference(const Frame& prev, const Frame& cur, const RepeatConfig& cfg) {
    ScalarStats r;
    const bool high16 = cur.layout == DiffLayout::HighBytes16;
    const int n = high16 ? cur.rowBytes / 2 : cur.rowBytes;
    const int bandRows = std::max(1, 16 / cfg.rowStep);
    std::vector<uint32_t> tiles((n + 63) / 64, 0);
    int comparedRow = 0;
    for (int y = 0; y < cur.rows; y += cfg.rowStep, comparedRow++) {
        if (comparedRow % bandRows == 0) std::fill(tiles.begin(), tiles.end(), 0u);
        const uint8_t* a = cur.Top() + (ptrdiff_t)y * cur.Pitch();
        const uint8_t* b = prev.Top() + (ptrdiff_t)y * prev.Pitch();
        for (int i = 0; i < n; i++) {
            const int va = high16 ? a[2 * i + 1] : a[i], vb = high16 ? b[2 * i + 1] : b[i];
            if (std::abs(va - vb) > cfg.levelThreshold) { r.changed++; tiles[i / 64]++; r.maxTile = std::max(r.maxTile, tiles[i / 64]); }
        }
    }
    return r;
}

static double ChangedFrac(const RepeatCheck& c) { return (double)c.changed / (double)c.compared; }

static void Configure(RepeatDetector& d, const Frame& f, const RepeatConfig& cfg = DefaultRepeat()) {
    d.Configure(f.layout, f.rowBytes, f.rows, cfg);
}

static RepeatCheck Check(RepeatDetector& d, const Frame& f) { return d.Check(f.Top(), f.Pitch()); }

static void PrintStats(const char* what, const RepeatCheck& c) {
    printf("         %-34s changed %7llu (frac %.2e) maxTile %4u cost %.1f us\n", what, (unsigned long long)c.changed, ChangedFrac(c),
           c.maxTileChanged, c.costMs * 1000.0);
}

static RepeatCheck Pair(RepeatDetector& d, const Frame& a, const Frame& b, const char* what, const RepeatConfig& cfg = DefaultRepeat()) {
    Configure(d, a, cfg);
    Expect(!Check(d, a).repeat, "first frame of '%s' is not a repeat", what);
    const RepeatCheck r = Check(d, b);
    PrintStats(what, r);
    return r;
}

static void TestYuy2(const Scene& sc) {
    printf("YUY2 1920x1080 (rowStep 2, threshold 6, maxChangedFrac 2e-5, maxTileChanged 16)\n");
    RepeatDetector d;
    Params base;
    base.checkers = {{900, 500, 32}};
    const Frame f0 = Render(sc, Fmt::YUY2, base);
    Configure(d, f0);
    Expect(!Check(d, f0).repeat, "first frame is never a repeat");
    const RepeatCheck same = Check(d, f0);
    PrintStats("identical", same);
    Expect(same.repeat, "identical frame -> repeat");
    Expect(same.changed == 0, "identical frame: 0 changed");

    {
        Params a = base, b = base;
        a.noiseAmplitude = 2; a.noiseSeed = 11; b.noiseAmplitude = 2; b.noiseSeed = 12;
        Expect(Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "+-2 noise (|d|<=4)").repeat, "+-2 level noise everywhere -> repeat");
    }
    {
        Params b = base; b.checkers = {{904, 500, 32}};
        Expect(!Pair(d, f0, Render(sc, Fmt::YUY2, b), "32x32 object moved 4 px").repeat, "32x32 moving object -> not repeat");
    }
    {
        Params a = base, b = base;
        a.checkers.push_back({1203, 301, 16}); b.checkers.push_back({1207, 301, 16});
        Expect(!Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "16x16 object moved 4 px").repeat, "16x16 moving object -> not repeat");
    }
    {
        Params a = base, b = base;
        a.checkers.push_back({400, 700, 30}); b.checkers.push_back({406, 703, 30});
        RepeatConfig loose = DefaultRepeat(); loose.maxChangedFrac = 1e-3;
        const Frame fa = Render(sc, Fmt::YUY2, a), fb = Render(sc, Fmt::YUY2, b);
        const RepeatCheck tiled = Pair(d, fa, fb, "30x30 cursor, maxChangedFrac 1e-3", loose);
        Expect(!tiled.repeat, "30x30 cursor with maxChangedFrac 1e-3 -> not repeat (tile check)");
        Expect(ChangedFrac(tiled) < loose.maxChangedFrac, "  ... global fraction %.2e alone would have hidden it", ChangedFrac(tiled));
        RepeatConfig noTile = loose; noTile.maxTileChanged.reset();
        Expect(Pair(d, fa, fb, "same, tile check off", noTile).repeat, "  ... and with the tile check off it is (wrongly) a repeat");
    }
    {
        Params b = base;
        Rng r(5);
        for (int k = 0; k < 20; k++) b.glitches.push_back({(int)(r.Next() % 1920), (int)(r.Next() % 540) * 2});
        Expect(Pair(d, f0, Render(sc, Fmt::YUY2, b), "20 isolated glitch samples").repeat, "20 isolated strong single-sample glitches -> repeat");
    }
    {
        Params b = base; b.panX = 1;
        Expect(!Pair(d, f0, Render(sc, Fmt::YUY2, b), "global pan 1 px").repeat, "1 px global pan -> not repeat");
    }
    {
        Params a = base, b = base;
        a.darkLowContrast = true; b.darkLowContrast = true; b.brightnessOffset = 1; a.checkers.clear(); b.checkers.clear();
        Expect(Pair(d, Render(sc, Fmt::YUY2, a), Render(sc, Fmt::YUY2, b), "dark scene +1 level").repeat,
               "dark low-contrast scene, +1 level everywhere -> repeat (below threshold, by design)");
    }
    {
        Params b = base; b.sceneCut = true;
        const RepeatCheck cut = Pair(d, f0, Render(sc, Fmt::YUY2, b), "scene cut");
        Expect(!cut.repeat, "full-frame change -> not repeat");
        Expect(ChangedFrac(cut) > 0.5, "  ... changed fraction %.3f", ChangedFrac(cut));
    }
    {
        Configure(d, f0);
        Check(d, f0);
        Expect(Check(d, f0).repeat, "identical before Reset -> repeat");
        d.Reset();
        Expect(!Check(d, f0).repeat, "identical right after Reset() -> not repeat");
        Expect(Check(d, f0).repeat, "identical after that -> repeat");
    }
    {
        Params p = base; p.checkers.clear(); p.darkLowContrast = true;
        Configure(d, Render(sc, Fmt::YUY2, p));
        Check(d, Render(sc, Fmt::YUY2, p));
        int firstNew = -1;
        for (int k = 1; k <= 10 && firstNew < 0; k++) {
            p.brightnessOffset = k;
            if (!Check(d, Render(sc, Fmt::YUY2, p)).repeat) firstNew = k;
        }
        Expect(firstNew == 7, "+1 level per frame drift: first non-repeat after %d frames (expected 7 = threshold 6 + 1)", firstNew);
    }
    {
        Configure(d, f0);
        int good = 0;
        const int frames = 20;
        for (int i = 0; i < frames; i++) {
            Params p = base; p.panX = (i / 2) * 3; p.checkers = {{100 + (i / 2) * 8, 200, 32}};
            p.noiseAmplitude = 1; p.noiseSeed = 100 + i;
            if (Check(d, Render(sc, Fmt::YUY2, p)).repeat == (i % 2 == 1)) good++;
        }
        Expect(good == frames, "30-in-60 cadence: %d/%d frames classified correctly (new, repeat, new, ...)", good, frames);
    }
}

static void TestExactness(const Scene& sc) {
    printf("Kernel vs scalar reference (all layouts, odd widths, padded and negative pitch)\n");
    struct Case { Fmt fmt; int w, h; int step; int extra; bool flip; const char* name; bool cut = false; };
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
        const Scene scene = (c.w == sc.width && c.h == sc.height) ? sc : Scene(c.w, c.h);
        Params a, b;
        a.checkers = {{c.w / 3, c.h / 3, 40}}; b.checkers = {{c.w / 3 + 4, c.h / 3 + 2, 40}, {c.w - 20, c.h - 30, 18}};
        a.noiseAmplitude = 5; a.noiseSeed = 1; b.noiseAmplitude = 5; b.noiseSeed = 2;
        b.sceneCut = c.cut;
        Frame fa = Render(scene, c.fmt, a, c.extra), fb = Render(scene, c.fmt, b, c.extra);
        if (c.flip) { fa = FlipToBottomUp(fa); fb = FlipToBottomUp(fb); }
        RepeatConfig cfg = DefaultRepeat(); cfg.rowStep = c.step;
        RepeatDetector d;
        Configure(d, fa, cfg);
        Check(d, fa);
        const RepeatCheck k = Check(d, fb);
        const ScalarStats r = ScalarReference(fa, fb, cfg);
        Expect(k.changed == r.changed && k.maxTileChanged == r.maxTile, "%-36s changed %llu/%llu maxTile %u/%u", c.name, (unsigned long long)k.changed,
               (unsigned long long)r.changed, k.maxTileChanged, r.maxTile);
    }
}

static void TestOtherFormats(const Scene& sc) {
    printf("Other formats\n");
    RepeatDetector d;
    Params base; base.checkers = {{900, 500, 32}};
    Params moved = base; moved.checkers = {{904, 500, 32}};
    {
        Params a = base, b = base;
        a.noiseAmplitude = 3; a.noiseSeed = 21; b.noiseAmplitude = 3; b.noiseSeed = 22;
        const Frame fa = Render(sc, Fmt::P010, a), fb = Render(sc, Fmt::P010, b);
        Expect(Pair(d, fa, fb, "P010 +-3 (10-bit) noise").repeat, "P010 (HighBytes16) low-bit noise -> repeat");
        Params c = moved; c.noiseAmplitude = 3; c.noiseSeed = 23;
        Expect(!Pair(d, fa, Render(sc, Fmt::P010, c), "P010 object moved").repeat, "P010 moving object -> not repeat");
    }
    {
        const Frame fa = Render(sc, Fmt::NV12, base);
        Expect(Pair(d, fa, fa, "NV12 identical").repeat, "NV12 luma identical -> repeat");
        Expect(!Pair(d, fa, Render(sc, Fmt::NV12, moved), "NV12 object moved").repeat, "NV12 luma moving object -> not repeat");
    }
    {
        const Frame fa = Render(sc, Fmt::RGB32, base);
        Params n = base; n.noiseAmplitude = 2; n.noiseSeed = 31;
        Expect(Pair(d, fa, Render(sc, Fmt::RGB32, n), "RGB32 +-2 noise").repeat, "RGB32 +-2 noise -> repeat");
        Expect(!Pair(d, fa, Render(sc, Fmt::RGB32, moved), "RGB32 object moved").repeat, "RGB32 moving object -> not repeat");
    }
    {
        const Frame ta = Render(sc, Fmt::RGB24, base), tb = Render(sc, Fmt::RGB24, moved);
        const Frame ba = FlipToBottomUp(ta), bb = FlipToBottomUp(tb);
        const RepeatCheck topDown = Pair(d, ta, tb, "RGB24 top-down moved");
        const RepeatCheck bottomUp = Pair(d, ba, bb, "RGB24 bottom-up moved");
        Expect(!topDown.repeat && !bottomUp.repeat && topDown.changed == bottomUp.changed, "negative pitch: same decision and changed count as top-down");
        Expect(Pair(d, ba, ba, "RGB24 bottom-up identical").repeat, "negative pitch identical -> repeat");
    }
}

int main() {
    const Scene sc(1920, 1080);
    TestYuy2(sc);
    TestOtherFormats(sc);
    TestExactness(sc);
    return check::Summary();
}
