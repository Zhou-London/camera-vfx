#include "frame_diff.h"
#include "../util/common.h"
#include <immintrin.h>
#include <intrin.h>
#include <cstring>
#include <cstdlib>

// AVX2 intrinsics are used in selected functions only (runtime dispatch); MSVC accepts them
// without /arch:AVX2, clang/gcc need a per-function target attribute.
#if defined(__clang__) || defined(__GNUC__)
#define CAMSS_AVX2_FN __attribute__((target("avx2")))
#else
#define CAMSS_AVX2_FN
#endif

namespace camss {
namespace {

// The kernel is compute bound even when the frame comes from L3 (about 40 SIMD ops per 64
// samples with SSE2); AVX2 halves that. Both produce identical statistics.

struct Job {
    const uint8_t* row;      // first compared row of the current frame
    ptrdiff_t rowAdv;        // bytes between compared rows (rowStep * pitch)
    const uint8_t* ref;      // reference copy (compact rows, stride bytes apart, 64-byte aligned)
    uint8_t* out;            // copy being written
    size_t stride;
    int n;                   // samples per row
    int rows;                // compared rows
    int bandRows;            // compared rows per tile band
    bool prefetch;           // software-prefetch the next compared row (rowStep > 1)
    int thr;
    int tileCols;            // full 64-sample tile columns
    uint8_t* lanes;          // tileCols * 32 byte lane counters (zeroed on entry)
};

struct Result {
    uint64_t changed = 0, sad = 0;
    uint32_t maxTile = 0;
};

inline void TileDone(uint32_t c, Result& r) {
    r.changed += c;
    r.maxTile = c > r.maxTile ? c : r.maxTile;
}

// With rowStep > 1 the next compared row is rowStep rows away, which the hardware prefetchers
// do not anticipate; prefetch it one row ahead (halves the cost when the frame is not cached:
// 1080p YUY2 rowStep 2 from DRAM 134 -> 71 us). With rowStep 1, `next` is the current row
// (already in flight), so the prefetch is a no-op hint.
template <bool High16>
inline void Prefetch(const char* next, int i) {
    _mm_prefetch(next + (High16 ? 2 : 1) * (size_t)i, _MM_HINT_T0);
    if constexpr (High16) _mm_prefetch(next + 2 * (size_t)i + 64, _MM_HINT_T0);
}

template <bool High16>
inline uint32_t ScalarTail(const uint8_t* cur, const uint8_t* ref, uint8_t* out, int i, int n, int thr, uint64_t& sad) {
    uint32_t c = 0;
    for (; i < n; i++) {
        const int a = High16 ? cur[2 * (size_t)i + 1] : cur[i];
        const int d = std::abs(a - (int)ref[i]);
        out[i] = (uint8_t)a;
        sad += (uint64_t)d;
        c += d > thr ? 1u : 0u;
    }
    return c;
}

// ------------------------------------------------------------------ SSE2

// |a - b| of 16 samples: accumulates the SAD into `sad` (2 x u64) and returns 0xFF where
// |a - b| > thr (thr1 = thr + 1 broadcast: d >= thr1  <=>  max(d, thr1) == d).
inline __m128i Diff16(__m128i a, __m128i b, __m128i thr1, __m128i& sad) {
    const __m128i d = _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
    sad = _mm_add_epi64(sad, _mm_sad_epu8(d, _mm_setzero_si128()));
    return _mm_cmpeq_epi8(_mm_max_epu8(d, thr1), d);
}

// 16 samples starting at sample index i. HighBytes16: the high byte of 16 little-endian
// 16-bit samples (32 source bytes).
template <bool High16>
inline __m128i Load16(const uint8_t* cur, int i) {
    if constexpr (High16) {
        const uint8_t* p = cur + 2 * (size_t)i;
        const __m128i lo = _mm_srli_epi16(_mm_loadu_si128((const __m128i*)p), 8);
        const __m128i hi = _mm_srli_epi16(_mm_loadu_si128((const __m128i*)(p + 16)), 8);
        return _mm_packus_epi16(lo, hi);
    } else {
        return _mm_loadu_si128((const __m128i*)(cur + i));
    }
}

inline uint32_t HSum8(__m128i v) {   // sum of 16 unsigned bytes
    const __m128i s = _mm_sad_epu8(v, _mm_setzero_si128());
    return (uint32_t)_mm_cvtsi128_si32(s) + (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(s, 8));
}

// Changed samples of each 64-sample block are counted per byte lane into the tile column's
// lane counters (mask bytes are 0xFF = -1, so subtracting their sum adds the count); counters
// are reduced once per tile band. The tail (< 64 samples) is counted as its own tile.
template <bool High16>
Result RunSse2(const Job& j) {
    Result r;
    const __m128i thr1 = _mm_set1_epi8((char)(uint8_t)(j.thr + 1));
    __m128i* lanes = (__m128i*)j.lanes;   // 2 x 16 counters per tile column (only the first is used here)
    __m128i sad = _mm_setzero_si128();
    uint64_t sadTail = 0;
    uint32_t tail = 0;
    const uint8_t* row = j.row;
    const uint8_t* ref = j.ref;
    uint8_t* out = j.out;
    for (int y = 0; y < j.rows; y++, row += j.rowAdv, ref += j.stride, out += j.stride) {
        int i = 0;
        __m128i* ln = lanes;
        const char* next = (const char*)(y + 1 < j.rows && j.prefetch ? row + j.rowAdv : row);
        for (; i + 64 <= j.n; i += 64, ln += 2) {
            Prefetch<High16>(next, i);
            const __m128i a0 = Load16<High16>(row, i), a1 = Load16<High16>(row, i + 16);
            const __m128i a2 = Load16<High16>(row, i + 32), a3 = Load16<High16>(row, i + 48);
            const __m128i m0 = Diff16(a0, _mm_load_si128((const __m128i*)(ref + i)), thr1, sad);
            const __m128i m1 = Diff16(a1, _mm_load_si128((const __m128i*)(ref + i + 16)), thr1, sad);
            const __m128i m2 = Diff16(a2, _mm_load_si128((const __m128i*)(ref + i + 32)), thr1, sad);
            const __m128i m3 = Diff16(a3, _mm_load_si128((const __m128i*)(ref + i + 48)), thr1, sad);
            _mm_store_si128((__m128i*)(out + i), a0);
            _mm_store_si128((__m128i*)(out + i + 16), a1);
            _mm_store_si128((__m128i*)(out + i + 32), a2);
            _mm_store_si128((__m128i*)(out + i + 48), a3);
            const __m128i m = _mm_add_epi8(_mm_add_epi8(m0, m1), _mm_add_epi8(m2, m3));   // -(0..4) per lane
            _mm_store_si128(ln, _mm_sub_epi8(_mm_load_si128(ln), m));
        }
        if (i < j.n) {
            for (; i + 16 <= j.n; i += 16) {
                const __m128i a = Load16<High16>(row, i);
                const __m128i m = Diff16(a, _mm_load_si128((const __m128i*)(ref + i)), thr1, sad);
                _mm_store_si128((__m128i*)(out + i), a);
                tail += (uint32_t)(16 - HSum8(_mm_add_epi8(m, _mm_set1_epi8(1))));   // 0xFF + 1 = 0
            }
            tail += ScalarTail<High16>(row, ref, out, i, j.n, j.thr, sadTail);
        }
        if ((y + 1) % j.bandRows == 0 || y + 1 == j.rows) {
            for (int t = 0; t < j.tileCols; t++) {
                TileDone(HSum8(_mm_load_si128(lanes + 2 * t)), r);
                _mm_store_si128(lanes + 2 * t, _mm_setzero_si128());
            }
            TileDone(tail, r);
            tail = 0;
        }
    }
    r.sad = (uint64_t)_mm_cvtsi128_si64(sad) + (uint64_t)_mm_cvtsi128_si64(_mm_srli_si128(sad, 8)) + sadTail;
    return r;
}

// ------------------------------------------------------------------ AVX2

CAMSS_AVX2_FN inline __m256i Diff32(__m256i a, __m256i b, __m256i thr1, __m256i& sad) {
    const __m256i d = _mm256_or_si256(_mm256_subs_epu8(a, b), _mm256_subs_epu8(b, a));
    sad = _mm256_add_epi64(sad, _mm256_sad_epu8(d, _mm256_setzero_si256()));
    return _mm256_cmpeq_epi8(_mm256_max_epu8(d, thr1), d);
}

template <bool High16>
CAMSS_AVX2_FN inline __m256i Load32(const uint8_t* cur, int i) {
    if constexpr (High16) {
        const uint8_t* p = cur + 2 * (size_t)i;
        const __m256i lo = _mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)p), 8);
        const __m256i hi = _mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)(p + 32)), 8);
        // packus works per 128-bit lane: [lo0 hi0 lo1 hi1] -> restore sample order
        return _mm256_permute4x64_epi64(_mm256_packus_epi16(lo, hi), 0xD8);
    } else {
        return _mm256_loadu_si256((const __m256i*)(cur + i));
    }
}

CAMSS_AVX2_FN inline uint32_t HSum8x32(__m256i v) {   // sum of 32 unsigned bytes
    const __m256i s = _mm256_sad_epu8(v, _mm256_setzero_si256());
    const __m128i t = _mm_add_epi64(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    return (uint32_t)_mm_cvtsi128_si32(t) + (uint32_t)_mm_extract_epi32(t, 2);
}

template <bool High16>
CAMSS_AVX2_FN Result RunAvx2(const Job& j) {
    Result r;
    const __m256i thr1 = _mm256_set1_epi8((char)(uint8_t)(j.thr + 1));
    const __m256i one = _mm256_set1_epi8(1);
    __m256i* lanes = (__m256i*)j.lanes;   // 32 counters per tile column
    __m256i sad = _mm256_setzero_si256();
    uint64_t sadTail = 0;
    uint32_t tail = 0;
    const uint8_t* row = j.row;
    const uint8_t* ref = j.ref;
    uint8_t* out = j.out;
    for (int y = 0; y < j.rows; y++, row += j.rowAdv, ref += j.stride, out += j.stride) {
        int i = 0;
        __m256i* ln = lanes;
        const char* next = (const char*)(y + 1 < j.rows && j.prefetch ? row + j.rowAdv : row);
        for (; i + 64 <= j.n; i += 64, ln++) {
            Prefetch<High16>(next, i);
            const __m256i a0 = Load32<High16>(row, i), a1 = Load32<High16>(row, i + 32);
            const __m256i m0 = Diff32(a0, _mm256_load_si256((const __m256i*)(ref + i)), thr1, sad);
            const __m256i m1 = Diff32(a1, _mm256_load_si256((const __m256i*)(ref + i + 32)), thr1, sad);
            _mm256_store_si256((__m256i*)(out + i), a0);
            _mm256_store_si256((__m256i*)(out + i + 32), a1);
            _mm256_store_si256(ln, _mm256_sub_epi8(_mm256_load_si256(ln), _mm256_add_epi8(m0, m1)));   // += 0..2 per lane
        }
        if (i < j.n) {
            for (; i + 32 <= j.n; i += 32) {
                const __m256i a = Load32<High16>(row, i);
                const __m256i m = Diff32(a, _mm256_load_si256((const __m256i*)(ref + i)), thr1, sad);
                _mm256_store_si256((__m256i*)(out + i), a);
                tail += 32 - HSum8x32(_mm256_add_epi8(m, one));   // 0xFF + 1 = 0
            }
            tail += ScalarTail<High16>(row, ref, out, i, j.n, j.thr, sadTail);
        }
        if ((y + 1) % j.bandRows == 0 || y + 1 == j.rows) {
            for (int t = 0; t < j.tileCols; t++) {
                TileDone(HSum8x32(_mm256_load_si256(lanes + t)), r);
                _mm256_store_si256(lanes + t, _mm256_setzero_si256());
            }
            TileDone(tail, r);
            tail = 0;
        }
    }
    const __m128i s = _mm_add_epi64(_mm256_castsi256_si128(sad), _mm256_extracti128_si256(sad, 1));
    r.sad = (uint64_t)_mm_cvtsi128_si64(s) + (uint64_t)_mm_extract_epi64(s, 1) + sadTail;
    _mm256_zeroupper();
    return r;
}

bool DetectAvx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;   // OS saves XMM + YMM state
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
}

} // namespace

bool RepeatDetector::Avx2Supported() {
    static const bool has = DetectAvx2();
    return has;
}

void RepeatDetector::Configure(DiffLayout layout, int rowBytes, int rows, const RepeatConfig& cfg) {
    layout_ = layout;
    rowBytes_ = std::max(0, rowBytes);
    rows_ = std::max(0, rows);
    cfg_ = cfg;
    cfg_.rowStep = std::max(1, cfg_.rowStep);
    cfg_.levelThreshold = std::clamp(cfg_.levelThreshold, 0, 254);
    samplesPerRow_ = layout_ == DiffLayout::HighBytes16 ? rowBytes_ / 2 : rowBytes_;
    cmpRows_ = rows_ > 0 ? (rows_ + cfg_.rowStep - 1) / cfg_.rowStep : 0;
    if (samplesPerRow_ <= 0 || cmpRows_ <= 0) { samplesPerRow_ = 0; cmpRows_ = 0; }
    // Tile band: ~16 image rows. A byte lane counter gains <= 4 per compared row -> <= 64 per band.
    bandRows_ = std::max(1, 16 / cfg_.rowStep);
    stride_ = ((size_t)samplesPerRow_ + 63) & ~(size_t)63;
    tileCols_ = samplesPerRow_ / 64;
    lanesBytes_ = (size_t)tileCols_ * 32;
    prev_.assign(lanesBytes_ + 2 * stride_ * (size_t)cmpRows_ + 64, 0);
    prevOff_ = (64 - ((uintptr_t)prev_.data() & 63)) & 63;
    refIdx_ = 0;
    (void)Avx2Supported();   // CPUID once, outside the hot path
    Reset();
}

void RepeatDetector::Reset() {
    havePrev_ = false;
    changed_ = 0;
    maxTile_ = 0;
    changedFrac_ = 0;
    meanAbs_ = 0;
}

bool RepeatDetector::Check(const uint8_t* top, ptrdiff_t pitch) {
    const int64_t t0 = Qpc::Now();
    if (!top || !Configured()) {
        Reset();
        costMs_ = 0;
        return false;
    }
    uint8_t* base = prev_.data() + prevOff_;
    const size_t copyBytes = stride_ * (size_t)cmpRows_;
    Job j;
    j.row = top;
    j.rowAdv = (ptrdiff_t)cfg_.rowStep * pitch;
    j.ref = base + lanesBytes_ + (size_t)refIdx_ * copyBytes;
    j.out = base + lanesBytes_ + (size_t)(refIdx_ ^ 1) * copyBytes;
    j.stride = stride_;
    j.n = samplesPerRow_;
    j.rows = cmpRows_;
    j.bandRows = bandRows_;
    j.prefetch = cfg_.rowStep > 1;
    j.thr = cfg_.levelThreshold;
    j.tileCols = tileCols_;
    j.lanes = base;
    memset(base, 0, lanesBytes_);
    const bool high = layout_ == DiffLayout::HighBytes16;
    Result r;
    if (UsingAvx2())
        r = high ? RunAvx2<true>(j) : RunAvx2<false>(j);
    else
        r = high ? RunSse2<true>(j) : RunSse2<false>(j);
    const double total = (double)ComparedSamples();

    bool repeat = false;
    if (havePrev_) {
        changed_ = r.changed;
        maxTile_ = (int)r.maxTile;
        changedFrac_ = (double)r.changed / total;
        meanAbs_ = (double)r.sad / total;
        repeat = (double)r.changed <= cfg_.maxChangedFrac * total &&
                 (cfg_.maxTileChanged < 0 || r.maxTile <= (uint32_t)cfg_.maxTileChanged);
    } else {
        // No reference yet: report "everything changed".
        changed_ = ComparedSamples();
        maxTile_ = 0;
        changedFrac_ = 1.0;
        meanAbs_ = 0.0;
    }
    // New content becomes the reference; a repeat keeps the previous content frame as reference.
    if (!repeat) refIdx_ ^= 1;
    havePrev_ = true;
    costMs_ = Qpc::ToMs(Qpc::Now() - t0);
    return repeat;
}

} // namespace camss
