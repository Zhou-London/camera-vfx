// NVIDIA Maxine Video Effects (VFX SDK 1.3) chain, fully CUDA-resident:
//   Video Frame Generation (explicit timestep)  @ source resolution
//   TrueHDR (SDR RGBA8 -> HDR10 RGB10A2/PQ)     @ source (default) or output resolution
//   Video Super Resolution                      source -> output resolution
// D3D11 <-> CUDA interop happens only twice: once per *source* frame (ingest) and
// once per *output* frame (final transfer into a D3D texture for presentation).
#pragma once
#include "../gfx/device.h"
#include "../interp/interpolator.h"
#include <memory>
#include <vector>
#include <string>

namespace camss {

struct VfxImpl;

class VfxChain {
public:
    struct Config {
        bool fg = false; int fgMode = 0;
        bool sr = false; int srQuality = 21; float srStrength = 1.0f;
        bool hdr = false; bool hdrBeforeSr = true;
        unsigned hdrPeakNits = 650, hdrContrast = 100, hdrSaturation = 100, hdrMiddleGray = 50;
        UINT srcW = 0, srcH = 0, outW = 0, outH = 0;
        std::wstring runtimeDir;   // empty = auto
        bool profile = true;       // CUDA event timing per stage
    };
    VfxChain();
    ~VfxChain();
    bool Init(Device& dev, const Config& cfg, SourceFrame* store, int storeCount);
    bool HasFg() const;
    bool OutputIsPq() const;
    // Copies a freshly converted source frame (D3D) into its CUDA slot.
    void Ingest(const SourceFrame& f);
    // Runs the chain for frame A (and B,t if B != null) and returns the D3D output texture.
    // `external` (optional): an already interpolated D3D frame (source size, RGBA8) to run
    // HDR/SR on instead of A (used when a D3D interpolator is combined with VFX SR/HDR).
    const Device::Tex* Process(const SourceFrame& A, const SourceFrame* B, float t, bool srOn,
                               const Device::Tex* external = nullptr);
    bool LastFgRan() const;       // did the last Process() actually generate a frame?
    bool LastOutputPq() const;    // is the last output HDR10 (PQ) or SDR?
    // Stage GPU times (ms) of completed frames since the last call.
    std::vector<std::pair<const char*, double>> PopTimings();
private:
    std::unique_ptr<VfxImpl> p_;
};

} // namespace camss
