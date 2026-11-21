// Telemetry from the real-time render thread to the (low-priority) UI / recording thread.
//  * FrameSample: one per output frame, pushed into a lock-free single-producer single-consumer
//    ring. The render thread never blocks or allocates; when the ring is full the sample is dropped
//    (counted).
//  * AppStatus: slow-changing state (settings, rates, counters, audio), published by the render
//    thread about 10 times per second under a mutex (one struct copy).
//  * Keys: the UI forwards key presses to the app (render thread polls them like window keys).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <array>

namespace camss {

struct FrameSample {
    int64_t qpc = 0;              // Present() call time (QPC ticks)
    float latencyMs = -1;         // newest source frame arrival -> flip (VRR/immediate) or scan-out (fixed); <0 unknown
    float contentMs = -1;         // content sample time -> flip / scan-out; <0 unknown
    float delayMs = 0;            // pacer D
    float gpuMs = 0;              // VFX chain GPU time (CUDA events) of a recent frame, 0 if no VFX
    float t = 0;                  // interpolation step actually used
    uint8_t flags = 0;            // kLate | kHold | kInterp
    static constexpr uint8_t kLate = 1, kHold = 2, kInterp = 4;
};

struct AppStatus {
    uint64_t generation = 0;      // bumps whenever a setting below changes (segments in the history)
    double uptimeSec = 0;
    bool warmupDone = false;
    std::string configPath;
    // source / display
    std::string captureName;      // "UGREEN 25173" or "synthetic"
    std::string captureFormat;    // "YUY2 1920x1080@60"
    unsigned srcW = 0, srcH = 0, outW = 0, outH = 0, dispW = 0, dispH = 0;
    double dispHz = 0;
    bool vrrActive = false;
    // settings
    std::string mode;             // "vrr" | "fixed" | "passthrough" | "auto (probing)"
    std::string interp;           // none | blend | nvof | vfx
    std::string fgQuality;        // low | medium | high (vfx) or "-"
    std::string sr;               // none | rtx | vfx
    std::string srQuality;        // e.g. hb-ultra (vfx) or "-"
    std::string hdr;              // none | rtx | vfx
    bool interpOn = true, srOn = true;
    bool adaptive = false;        // repeat-aware frame generation
    int multiplier = 1;           // VRR output frames per content frame
    double targetFps = 0;         // fg-target-fps (0 = fixed multiplier)
    // rates
    double signalHz = 0, contentFps = 0, outFps = 0;
    bool contentStatic = false;
    // pacing
    double delayMs = 0, leadMs = 0, minSlackMs = 0, gpuBudgetMs = 0;
    std::string latencyMetric;    // "arrival->flip" | "arrival->scanout"
    uint64_t late = 0, hold = 0, drops = 0, missed = 0, repeats = 0, gpuBusy = 0;
    // audio
    bool audioEnabled = false, audioRunning = false, audioPlaying = false, audioMuted = false;
    float audioVolume = 1;
    double audioLatencyMs = 0, audioBufferMs = 0, audioSyncMs = 0;
    uint64_t audioUnderruns = 0;
    std::string audioIn, audioOut;
    // pipeline stage times (p50 ms over the recent window; <0 = not present)
    double captureMs = -1;        // device sample -> arrival (USB + frame server)
    double uploadMs = -1;         // arrival -> uploaded
    double fgMs = -1, srMs = -1, hdrMs = -1;   // VFX stages (GPU)
    std::string notice;           // short latest warning shown in the UI (e.g. GPU over budget), may be empty
};

// Lock-free SPSC ring (power-of-two capacity).
template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    bool Push(const T& v) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) { dropped_.fetch_add(1, std::memory_order_relaxed); return false; }
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& v) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    std::array<T, N> buf_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::atomic<uint64_t> dropped_{0};
};

class Telemetry {
public:
    // ---- render thread
    void PushFrame(const FrameSample& s) noexcept { frames_.Push(s); }
    void PublishStatus(const AppStatus& s) { std::lock_guard<std::mutex> lk(m_); status_ = s; statusSeq_++; }
    int PopKey() noexcept { return keys_.Pop(lastKey_) ? lastKey_ : 0; }   // 0 = none
    // ---- UI thread
    bool PopFrame(FrameSample& s) noexcept { return frames_.Pop(s); }
    AppStatus Status(uint64_t* seq = nullptr) const { std::lock_guard<std::mutex> lk(m_); if (seq) *seq = statusSeq_; return status_; }
    void PushKey(int vk) noexcept { keys_.Push(vk); }   // Windows virtual-key code ('F', VK_ESCAPE, VK_OEM_PLUS ...)
    uint64_t DroppedFrames() const noexcept { return frames_.Dropped(); }
private:
    SpscRing<FrameSample, 4096> frames_;
    SpscRing<int, 64> keys_;
    int lastKey_ = 0;
    mutable std::mutex m_;
    AppStatus status_;
    uint64_t statusSeq_ = 0;
};

} // namespace camss
