#pragma once
#include "../pacing/multiplier.h"
#include "../util/common.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cameravfx {

constexpr float kUnknownMs = -1;

struct FrameSample {
    int64_t qpc = 0;
    float latencyMs = kUnknownMs;
    float contentMs = kUnknownMs;
    float delayMs = 0;
    float gpuMs = 0;
    uint8_t flags = 0;
    static constexpr uint8_t kLate = 1, kHold = 2;
};

struct StageTimes {
    double captureMs = kUnknownMs, uploadMs = kUnknownMs, fgMs = kUnknownMs, srMs = kUnknownMs, hdrMs = kUnknownMs;
};

struct AppStatus {
    uint64_t statsEpoch = 0;
    double uptimeSec = 0;
    bool warmupDone = false;
    std::string configPath;
    std::string captureName;
    std::string captureFormat;
    FrameSize sourceSize, outputSize, displaySize;
    double displayHz = 0;
    bool vrrActive = false;
    std::string mode;
    bool probing = false;
    bool fg = false, sr = false, hdr = false;
    std::string fgQuality, srQuality;
    bool fgOn = true, srOn = true;
    bool fgActive = false;
    bool timecode = false;
    bool detectRepeats = false;
    int fgMultiplier = 1;
    int fgMultiplierRequested = 2;
    double fgThresholdFps = 0;
    MultiplierLimit fgMultiplierLimit = MultiplierLimit::None;
    double signalHz = 0, contentFps = 0, outFps = 0;
    bool contentStatic = false;
    double leadMs = 0, gpuBudgetMs = 0;
    uint64_t late = 0, drops = 0, missed = 0;
    bool audioEnabled = false, audioRunning = false, audioMuted = false;
    float audioVolume = 1;
    double audioLatencyMs = 0, audioSyncMs = 0;
    uint64_t audioUnderruns = 0;
    std::string audioIn, audioOut;
    StageTimes stages;
    std::string notice;
    std::string loading;
    bool fgLoading = false, srLoading = false;
};

enum class SettingId : uint16_t {
    CapFormat, CapMode, Matrix, Range,
    Fg, FgQuality, FgMultiplier, FgThreshold,
    Sr, SrQuality,
    Mute, AudioVolume,
    Timecode, PrintStats, ResetStats, Save,
    Count
};
inline const char* SettingName(SettingId id) {
    static const char* const kNames[] = {"format", "capture", "matrix", "range", "fg", "fg-quality", "fg-multiplier", "fg-threshold",
                                         "sr", "sr-quality", "mute", "audio-volume", "timecode", "print-stats", "reset-stats", "save"};
    static_assert(std::size(kNames) == (size_t)SettingId::Count);
    return kNames[(size_t)id];
}

struct SettingItem {
    enum class Kind : uint8_t { Toggle, Choice, Action };
    SettingId id = SettingId::Count;
    Kind kind = Kind::Choice;
    std::string group, label;
    std::vector<std::string> choices;
    std::vector<std::string> choiceNotes;
    int value = 0;
    bool pausesPlayback = false;
    bool saved = false;
    bool enabled = true;
    std::string note;
};

struct SettingCmd {
    SettingId id = SettingId::Count;
    int value = 0;
};

template <typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0);
public:
    bool Push(const T& v) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return false;
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
private:
    std::array<T, N> buf_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

class Telemetry {
public:
    void PushFrame(const FrameSample& s) noexcept { frames_.Push(s); }
    void PublishStatus(AppStatus s) {
        std::unique_lock<std::mutex> lk(m_, std::try_to_lock);
        if (!lk) return;
        status_ = std::move(s);
        statusSeq_++;
    }
    int PopKey() noexcept { int vk = 0; keys_.Pop(vk); return vk; }
    bool PopSetting(SettingCmd& c) noexcept { return cmds_.Pop(c); }
    void PublishSettings(std::vector<SettingItem> items) {
        std::lock_guard<std::mutex> lk(m_);
        settings_ = std::move(items);
        settingsSeq_++;
    }

    bool PopFrame(FrameSample& s) noexcept { return frames_.Pop(s); }
    bool Status(uint64_t& seq, AppStatus& out) const {
        std::lock_guard<std::mutex> lk(m_);
        if (statusSeq_ == seq) return false;
        out = status_;
        seq = statusSeq_;
        return true;
    }
    void PushKey(int vk) noexcept { keys_.Push(vk); }
    void PushSetting(const SettingCmd& c) noexcept { cmds_.Push(c); }
    bool Settings(uint64_t& seq, std::vector<SettingItem>& out) const {
        std::lock_guard<std::mutex> lk(m_);
        if (settingsSeq_ == seq) return false;
        out = settings_;
        seq = settingsSeq_;
        return true;
    }
private:
    SpscRing<FrameSample, 4096> frames_;
    SpscRing<int, 64> keys_;
    SpscRing<SettingCmd, 64> cmds_;
    mutable std::mutex m_;
    AppStatus status_;
    uint64_t statusSeq_ = 0;
    std::vector<SettingItem> settings_;
    uint64_t settingsSeq_ = 0;
};

}
