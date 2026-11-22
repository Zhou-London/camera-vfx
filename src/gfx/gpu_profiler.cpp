#include "gpu_profiler.h"

namespace cameravfx {

void GpuProfiler::Init(ID3D11Device* dev) {
    for (Frame& f : frames_) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HR_CHECK(dev->CreateQuery(&qd, &f.disjoint));
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& q : f.stamps) HR_CHECK(dev->CreateQuery(&qd, &q));
    }
}

void GpuProfiler::Begin(ID3D11DeviceContext* ctx) {
    cur_ = (cur_ + 1) % kFrames;
    Frame& f = frames_[cur_];
    active_ = !f.pending;
    if (!active_) return;
    f.count = 0;
    ctx->Begin(f.disjoint.Get());
    Mark(ctx, nullptr);
}

void GpuProfiler::Mark(ID3D11DeviceContext* ctx, const char* key) {
    if (!active_) return;
    Frame& f = frames_[cur_];
    f.keys[f.count] = key;
    ctx->End(f.stamps[f.count].Get());
    f.count++;
}

void GpuProfiler::End(ID3D11DeviceContext* ctx) {
    if (!active_) return;
    ctx->End(frames_[cur_].disjoint.Get());
    frames_[cur_].pending = true;
    active_ = false;
}

}
