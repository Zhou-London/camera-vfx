#pragma once
#include "config.h"
#include "audio/audio_monitor.h"
#include "capture/frame_source.h"
#include "gfx/blit.h"
#include "gfx/device.h"
#include "gfx/gpu_profiler.h"
#include "pacing/cadence.h"
#include "pacing/clocks.h"
#include "pacing/lead.h"
#include "pacing/multiplier.h"
#include "pacing/pacer.h"
#include "present/present_monitor.h"
#include "present/presenter.h"
#include "ui/dashboard.h"
#include "ui/telemetry.h"
#include "util/stats.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cameravfx {

class VfxChain;

extern std::atomic<bool> g_quitRequested;

class App {
public:
    explicit App(const Options& options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    bool Init(HINSTANCE inst);
    int Run();

private:
    struct OutputSlot {
        PaceDecision decision;
        int64_t renderStart = 0;
        int64_t vblank = 0;
        UINT syncInterval = 1;
        int64_t presentAt = 0;
    };
    struct SettingsMenu {
        std::vector<SettingItem> items;
        std::vector<CaptureMode> formats;
        std::vector<CaptureMode> modes;
        std::vector<int> srQualities;
        std::vector<double> fgThresholds;
        SettingItem& Add(SettingId id, SettingItem::Kind kind, const char* group, const char* label);
    };
    struct MeasuredLatency {
        std::string metric;
        Percentiles arrival;
        std::optional<Percentiles> content;
    };
    struct Loading {
        std::string label;
        bool fgModel = false, srModel = false;
        int64_t since = 0;
    };
    enum class LoadingIngest { None, KeepSourceClock };

    static constexpr double kStatusPublishMs = 100;

    bool SetupProcessing();
    bool BuildVfx();
    void WarmUpChain();
    bool FgActive() const { return options_.fg && fgOn_; }

    void ResetPacing(double contentPeriod);
    CaptureAnalysis Analysis() const;
    void OnContentRateChanged();
    MultiplierChoice MultiplierFor(int requested, double thresholdFps) const;
    bool UpdateMultiplier();
    void RecheckMultiplier(int64_t now);
    double SignalHz() const;
    double ContentFps() const;
    double OutputPeriodMs() const;
    std::string ModeLabel() const;

    void RunLoop(double untilSec);
    bool KeepRunning(double untilSec) const;
    std::optional<OutputSlot> NextSlot();
    std::optional<OutputSlot> NextFixedSlot();
    std::optional<OutputSlot> NextVrrSlot();
    std::optional<OutputSlot> NextPassthroughSlot();
    PaceDecision ShowNewest() const;
    void Ingest(int64_t now);
    void Render(const OutputSlot& slot);
    PresentedFrame DescribePresented(const OutputSlot& slot, const SourceFrame& a, const SourceFrame* b, bool generated, int64_t submitted,
                                     int64_t presentCall) const;
    void HoldPresent(const OutputSlot& slot);
    void AfterPresents(int64_t now);

    void BeginPause(Loading loading);
    void EndPause(int64_t t0, double settleSec);
    bool WhileLoading(const std::function<bool()>& work, LoadingIngest ingest);
    void ShowLoading(std::optional<float> progress);
    void SwitchModels(int fgQuality, int srQuality);
    void RestartCapture(const CaptureMode& mode);
    std::unique_ptr<MfCapture> ReopenCapture(const CaptureMode& mode);
    void StartAudio();

    void HandleKeys();
    void PressKey(int key);
    void RunKeyScript();
    void PublishSettings();
    void AddCaptureSettings(SettingsMenu& menu) const;
    void AddFgSettings(SettingsMenu& menu) const;
    void AddSrSettings(SettingsMenu& menu) const;
    void AddAudioSettings(SettingsMenu& menu) const;
    void AddToolSettings(SettingsMenu& menu) const;
    void ApplySetting(SettingId id, int value);
    void SetMatrix(int choice);
    void SetRange(int choice);
    void SetFgOn(bool on);
    void SetFgMultiplier(int multiplier);
    void SetFgThreshold(double thresholdFps);
    void SetSrOn(bool on);
    void SetAudioVolume(int percent);
    void SaveSettings();

    void Report(int64_t now, bool final);
    void ResetStats();
    void ShowNotice(std::string text, double seconds);
    void UpdateStageTimes();
    std::optional<MeasuredLatency> Latency() const;
    std::string StatusLine(const std::optional<MeasuredLatency>& latency) const;
    void WarnGpuOverBudget();
    void WarnSlowOutput();
    void PrintDetail();
    void PublishStatus(int64_t now);
    void StartDashboard();
    void StopDashboard();

    Options options_;
    Device dev_;
    StatBook stats_;
    FrameSource source_{dev_, stats_};
    Presenter presenter_;
    VsyncClock vsync_;
    Pacer pacer_;
    CadenceDetector cadence_;
    LeadController lead_;
    Blitter blit_;
    GpuProfiler profiler_;
    std::unique_ptr<VfxChain> vfx_;
    AudioMonitor audio_;
    Telemetry telemetry_;
    Dashboard dash_;
    PresentMonitor monitor_{presenter_, stats_, telemetry_};
    PreciseSleeper sleeper_;
    ComPtr<ID3D11Query> frameDone_;

    PaceMode mode_ = PaceMode::Fixed;
    bool probing_ = false;
    bool detectRepeats_ = false;
    bool fgOn_ = true;
    bool srOn_ = true;
    MultiplierLimit multiplierLimit_ = MultiplierLimit::None;
    FrameSize outputSize_;
    bool vfxReloading_ = false;
    bool haveSlot_ = false;
    int64_t shownSignal_ = -1;
    double lastVfxGpuMs_ = 0;
    uint64_t gpuBusyAtPresent_ = 0;

    Device::Tex pauseTex_;
    bool pauseValid_ = false;
    std::optional<Loading> loading_;

    bool tui_ = false;
    uint64_t statsEpoch_ = 0;
    bool settingsDirty_ = true;
    SettingsMenu menu_;
    size_t keyScriptPos_ = 0;
    StageTimes stages_;
    double publishedCaptureMs_ = kUnknownMs;
    AudioStats audioStats_;
    std::string notice_;
    int64_t noticeUntil_ = 0;
    int64_t startQpc_ = 0;
    int64_t lastReport_ = 0;
    int64_t lastStatusPublish_ = 0;
    int64_t lastMultiplierCheck_ = 0;
    int64_t statsResetAt_ = 0;
    uint64_t lastReportPresented_ = 0;
    double outFps_ = 0;
    int slowReports_ = 0;
    uint64_t slowReportsEpoch_ = 0;
    bool warmupDone_ = false;
};

}
