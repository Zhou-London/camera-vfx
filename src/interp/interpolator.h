// Frame interpolation interface. All implementations run entirely on the GPU and
// generate a frame at an explicit time step t in (0,1) between two source frames.
#pragma once
#include "../gfx/device.h"
#include <memory>

namespace camss {

struct SourceFrame {
    int64_t n = -1;              // source timeline index (accounts for dropped frames)
    uint64_t seq = 0;
    Device::Tex rgb;             // B8G8R8A8_UNORM, source resolution
    int64_t sampleQpc = 0, arrivalQpc = 0, uploadedQpc = 0, readyQpc = 0;
    uint64_t generation = 0;     // bumps when the slot is re-filled
};

class Interpolator {
public:
    virtual ~Interpolator() = default;
    virtual const char* Name() const = 0;
    virtual bool Init(Device& dev, UINT w, UINT h) = 0;
    // Optional precomputation for a new consecutive pair (e.g. optical flow). Called
    // once per pair, as soon as frame b is available. May be called again for the
    // same pair (must be cheap if cached).
    virtual void Prepare(const SourceFrame& a, const SourceFrame& b) { (void)a; (void)b; }
    // Generates the frame at time t between a and b into `out` (same size as source, UAV+SRV).
    virtual void Interpolate(const SourceFrame& a, const SourceFrame& b, float t, const Device::Tex& out) = 0;
    // Output texture format this interpolator writes.
    virtual DXGI_FORMAT OutFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM; }
};

std::unique_ptr<Interpolator> CreateBlendInterpolator();
std::unique_ptr<Interpolator> CreateFlowInterpolator(int quality);   // NVOF hardware optical flow + own warp/blend
std::unique_ptr<Interpolator> CreateVfxInterpolator();               // Maxine VFX SDK frame generation (if present)

} // namespace camss
