#pragma once
#include "../util/common.h"
#include <d3d11.h>
#include <array>

namespace cameravfx {

class GpuProfiler {
public:
    void Init(ID3D11Device* dev);
    void Begin(ID3D11DeviceContext* ctx);
    void Mark(ID3D11DeviceContext* ctx, const char* key);
    void End(ID3D11DeviceContext* ctx);
    template <typename F> void Collect(ID3D11DeviceContext* ctx, F&& onTiming) {
        for (int i = 0; i < kFrames; i++) {
            Frame& f = frames_[(cur_ + 1 + i) % kFrames];
            if (!f.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if (ctx->GetData(f.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            f.pending = false;
            if (disjoint.Disjoint) continue;
            std::array<UINT64, kMarks> ticks{};
            bool resolved = true;
            for (int m = 0; m < f.count && resolved; m++)
                resolved = ctx->GetData(f.stamps[m].Get(), &ticks[m], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!resolved) continue;
            const double msPerTick = 1000.0 / (double)disjoint.Frequency;
            for (int m = 1; m < f.count; m++) onTiming(f.keys[m], (double)(ticks[m] - ticks[m - 1]) * msPerTick);
            onTiming(kTotalKey, (double)(ticks[f.count - 1] - ticks[0]) * msPerTick);
        }
    }

private:
    static constexpr int kFrames = 6;
    static constexpr int kMarks = 8;
    static constexpr const char* kTotalKey = "gpu.total";
    struct Frame {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>, kMarks> stamps;
        std::array<const char*, kMarks> keys{};
        int count = 0;
        bool pending = false;
    };
    std::array<Frame, kFrames> frames_;
    int cur_ = 0;
    bool active_ = false;
};

}
