#pragma once
#include "../gfx/device.h"
#include "../gfx/encoding.h"
#include "../gfx/source_frame.h"
#include "../util/stats.h"
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cameravfx {

struct VfxImpl;

struct VfxOutput {
    const Device::Tex* tex = nullptr;
    Encoding encoding = Encoding::SdrGamma;
    bool generated = false;
};

struct VfxTimings {
    std::optional<double> fgMs, hdrMs, srMs, outMs;
    double totalMs = 0;
};

void RecordVfxTimings(StatBook& stats, const VfxTimings& timings);

class VfxChain {
public:
    struct Config {
        bool fg = false;
        int fgQuality = 0;
        bool sr = false;
        int srQuality = 0;
        bool hdr = false;
        bool hdrAfterSr = false;
        UINT srcW = 0, srcH = 0;
        UINT outW = 0, outH = 0;
        std::function<void(float loadedFraction)> onLoadProgress;
    };

    static std::unique_ptr<VfxChain> Create(Device& dev, const Config& cfg, std::span<SourceFrame> store);
    ~VfxChain();

    bool Reload(int fgQuality, int srQuality);
    bool Loaded() const;
    int FgQuality() const;
    int SrQuality() const;
    double FgLoadMs() const;
    double SrLoadMs() const;

    void Ingest(const SourceFrame& frame);
    VfxOutput Process(const SourceFrame& a, const SourceFrame* b, float t, bool srOn);
    std::vector<VfxTimings> PopTimings();

private:
    explicit VfxChain(std::unique_ptr<VfxImpl> impl);
    std::unique_ptr<VfxImpl> p_;
};

}
