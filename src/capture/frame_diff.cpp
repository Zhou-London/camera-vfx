#include "frame_diff.h"
#include "../util/common.h"
#include <immintrin.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace cameravfx {

namespace {

constexpr int kTileSamples = 64;
constexpr int kTileImageRows = 16;
constexpr size_t kSimdAlign = 64;
constexpr size_t kLaneBytesPerTile = 32;

struct Job {
    const uint8_t* row;
    ptrdiff_t rowAdvance;
    const uint8_t* reference;
    uint8_t* copy;
    size_t copyStride;
    int samples;
    int rows;
    int bandRows;
    bool prefetchNextRow;
    int threshold;
    int tileCols;
    uint8_t* lanes;
};

struct Counts {
    uint64_t changed = 0;
    uint32_t maxTile = 0;
};

inline void AddTile(uint32_t changed, Counts& c) {
    c.changed += changed;
    c.maxTile = std::max(c.maxTile, changed);
}

template <bool High16>
inline void PrefetchRow(const char* next, int i) {
    _mm_prefetch(next + (High16 ? 2 : 1) * (size_t)i, _MM_HINT_T0);
    if constexpr (High16) _mm_prefetch(next + 2 * (size_t)i + 64, _MM_HINT_T0);
}

template <bool High16>
inline uint32_t ScalarTail(const uint8_t* cur, const uint8_t* ref, uint8_t* copy, int i, int n, int threshold) {
    uint32_t changed = 0;
    for (; i < n; i++) {
        const int a = High16 ? cur[2 * (size_t)i + 1] : cur[i];
        copy[i] = (uint8_t)a;
        changed += std::abs(a - (int)ref[i]) > threshold ? 1u : 0u;
    }
    return changed;
}

inline __m256i ChangedMask(__m256i a, __m256i b, __m256i thresholdPlusOne) {
    const __m256i d = _mm256_or_si256(_mm256_subs_epu8(a, b), _mm256_subs_epu8(b, a));
    return _mm256_cmpeq_epi8(_mm256_max_epu8(d, thresholdPlusOne), d);
}

template <bool High16>
inline __m256i Load32Samples(const uint8_t* cur, int i) {
    if constexpr (High16) {
        const uint8_t* p = cur + 2 * (size_t)i;
        const __m256i lo = _mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)p), 8);
        const __m256i hi = _mm256_srli_epi16(_mm256_loadu_si256((const __m256i*)(p + 32)), 8);
        constexpr int kRestoreLaneOrder = 0xD8;
        return _mm256_permute4x64_epi64(_mm256_packus_epi16(lo, hi), kRestoreLaneOrder);
    } else {
        return _mm256_loadu_si256((const __m256i*)(cur + i));
    }
}

inline uint32_t SumBytes(__m256i v) {
    const __m256i s = _mm256_sad_epu8(v, _mm256_setzero_si256());
    const __m128i t = _mm_add_epi64(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
    return (uint32_t)_mm_cvtsi128_si32(t) + (uint32_t)_mm_extract_epi32(t, 2);
}

template <bool High16>
Counts Compare(const Job& j) {
    Counts c;
    const __m256i thresholdPlusOne = _mm256_set1_epi8((char)(uint8_t)(j.threshold + 1));
    const __m256i one = _mm256_set1_epi8(1);
    __m256i* lanes = (__m256i*)j.lanes;
    uint32_t tailChanged = 0;
    const uint8_t* row = j.row;
    const uint8_t* ref = j.reference;
    uint8_t* copy = j.copy;
    for (int y = 0; y < j.rows; y++, row += j.rowAdvance, ref += j.copyStride, copy += j.copyStride) {
        int i = 0;
        __m256i* lane = lanes;
        const char* next = (const char*)(y + 1 < j.rows && j.prefetchNextRow ? row + j.rowAdvance : row);
        for (; i + kTileSamples <= j.samples; i += kTileSamples, lane++) {
            PrefetchRow<High16>(next, i);
            const __m256i a0 = Load32Samples<High16>(row, i), a1 = Load32Samples<High16>(row, i + 32);
            const __m256i m0 = ChangedMask(a0, _mm256_load_si256((const __m256i*)(ref + i)), thresholdPlusOne);
            const __m256i m1 = ChangedMask(a1, _mm256_load_si256((const __m256i*)(ref + i + 32)), thresholdPlusOne);
            _mm256_store_si256((__m256i*)(copy + i), a0);
            _mm256_store_si256((__m256i*)(copy + i + 32), a1);
            _mm256_store_si256(lane, _mm256_sub_epi8(_mm256_load_si256(lane), _mm256_add_epi8(m0, m1)));
        }
        for (; i + 32 <= j.samples; i += 32) {
            const __m256i a = Load32Samples<High16>(row, i);
            const __m256i m = ChangedMask(a, _mm256_load_si256((const __m256i*)(ref + i)), thresholdPlusOne);
            _mm256_store_si256((__m256i*)(copy + i), a);
            tailChanged += 32 - SumBytes(_mm256_add_epi8(m, one));
        }
        tailChanged += ScalarTail<High16>(row, ref, copy, i, j.samples, j.threshold);
        if ((y + 1) % j.bandRows == 0 || y + 1 == j.rows) {
            for (int t = 0; t < j.tileCols; t++) {
                AddTile(SumBytes(_mm256_load_si256(lanes + t)), c);
                _mm256_store_si256(lanes + t, _mm256_setzero_si256());
            }
            AddTile(tailChanged, c);
            tailChanged = 0;
        }
    }
    _mm256_zeroupper();
    return c;
}

}

void RepeatDetector::Configure(DiffLayout layout, int rowBytes, int rows, const RepeatConfig& cfg) {
    layout_ = layout;
    cfg_ = cfg;
    samplesPerRow_ = layout == DiffLayout::HighBytes16 ? rowBytes / 2 : rowBytes;
    comparedRows_ = (rows + cfg.rowStep - 1) / cfg.rowStep;
    bandRows_ = std::max(1, kTileImageRows / cfg.rowStep);
    copyStride_ = ((size_t)samplesPerRow_ + kSimdAlign - 1) & ~(kSimdAlign - 1);
    tileCols_ = samplesPerRow_ / kTileSamples;
    laneBytes_ = (size_t)tileCols_ * kLaneBytesPerTile;
    buffer_.assign(laneBytes_ + 2 * copyStride_ * (size_t)comparedRows_ + kSimdAlign, 0);
    alignOffset_ = (kSimdAlign - ((uintptr_t)buffer_.data() & (kSimdAlign - 1))) & (kSimdAlign - 1);
    referenceCopy_ = 0;
    haveReference_ = false;
}

RepeatCheck RepeatDetector::Check(const uint8_t* top, ptrdiff_t pitch) {
    const int64_t t0 = Qpc::Now();
    uint8_t* base = buffer_.data() + alignOffset_;
    uint8_t* copies = base + laneBytes_;
    const size_t copyBytes = copyStride_ * (size_t)comparedRows_;
    Job j;
    j.row = top;
    j.rowAdvance = (ptrdiff_t)cfg_.rowStep * pitch;
    j.reference = copies + (size_t)referenceCopy_ * copyBytes;
    j.copy = copies + (size_t)(referenceCopy_ ^ 1) * copyBytes;
    j.copyStride = copyStride_;
    j.samples = samplesPerRow_;
    j.rows = comparedRows_;
    j.bandRows = bandRows_;
    j.prefetchNextRow = cfg_.rowStep > 1;
    j.threshold = cfg_.levelThreshold;
    j.tileCols = tileCols_;
    j.lanes = base;
    memset(base, 0, laneBytes_);
    const Counts counts = layout_ == DiffLayout::HighBytes16 ? Compare<true>(j) : Compare<false>(j);

    RepeatCheck r;
    r.compared = (uint64_t)samplesPerRow_ * (uint64_t)comparedRows_;
    if (haveReference_) {
        r.changed = counts.changed;
        r.maxTileChanged = counts.maxTile;
        r.repeat = (double)counts.changed <= cfg_.maxChangedFrac * (double)r.compared &&
                   (!cfg_.maxTileChanged || counts.maxTile <= *cfg_.maxTileChanged);
    } else {
        r.changed = r.compared;
    }
    if (!r.repeat) referenceCopy_ ^= 1;
    haveReference_ = true;
    r.costMs = Qpc::ToMs(Qpc::Now() - t0);
    return r;
}

}
