#include "app.h"
#include "vfx/vfx_chain.h"
#include "vfx/vfx_quality.h"
#include <cmath>
#include <format>

namespace cameravfx {

namespace {
constexpr double kLoadNoteExtraSec = 0.1;
constexpr int kVolumeStepPercent = 10;
constexpr int kVolumeSteps = 20;
constexpr double kThresholdChoicesFps[] = {0, 30, 60, 90, 120};
constexpr double kSameThresholdTolerance = 0.01;
constexpr double kRefreshMatchTolerance = 1;
constexpr std::string_view kTimesSign = "×";
constexpr std::optional<ColorMatrix> kMatrixChoices[] = {std::nullopt, ColorMatrix::Bt709, ColorMatrix::Bt601, ColorMatrix::Bt2020};
constexpr std::optional<bool> kRangeChoices[] = {std::nullopt, false, true};

bool SameSizeAndRate(const CaptureMode& a, const CaptureMode& b) { return SameSize(a, b) && SameRate(a, b); }

std::string CaptureModeLabel(const CaptureMode& m) {
    return SameRate(m.rateHz, std::round(m.rateHz)) ? std::format("{}×{} @ {:.0f}", m.width, m.height, m.rateHz)
                                                     : std::format("{}×{} @ {:.2f}", m.width, m.height, m.rateHz);
}

std::string DeliveryNote(double measuredMs, PixFmt format, PixFmt current) {
    if (measuredMs < 0) return {};
    if (format == current) return std::format("交付 {:.1f} ms（实测）", measuredMs);
    return std::format("交付≈{:.1f} ms", measuredMs * PixFmtBytesPerPixel(format) / PixFmtBytesPerPixel(current));
}

std::string LoadTimeNote(double loadMs) { return std::format("切换约 {:.1f} s", loadMs / 1000.0 + kLoadNoteExtraSec); }

std::string FileName(const std::string& path) { return path.substr(path.find_last_of("\\/") + 1); }

template <class Choices, class Value> int ChoiceIndex(const Choices& choices, const Value& value) {
    return (int)(std::find(std::begin(choices), std::end(choices), value) - std::begin(choices));
}

std::optional<int> SettingValue(const SettingItem& item, std::string_view text) {
    switch (item.kind) {
    case SettingItem::Kind::Action: return 0;
    case SettingItem::Kind::Toggle: {
        const std::optional<bool> on = ParseBool(text);
        return on ? std::optional<int>(*on ? 1 : 0) : std::nullopt;
    }
    case SettingItem::Kind::Choice:
        for (size_t i = 0; i < item.choices.size(); i++) {
            std::string choice = Lower(item.choices[i]);
            if (choice.starts_with(kTimesSign)) choice.erase(0, kTimesSign.size());
            if (!text.empty() && choice.starts_with(text)) return (int)i;
        }
        return std::nullopt;
    }
    return std::nullopt;
}
}

SettingItem& App::SettingsMenu::Add(SettingId id, SettingItem::Kind kind, const char* group, const char* label) {
    SettingItem& item = items.emplace_back();
    item.id = id;
    item.kind = kind;
    item.group = group;
    item.label = label;
    return item;
}

void App::HandleKeys() {
    RunKeyScript();
    for (SettingCmd c; !g_quitRequested && telemetry_.PopSetting(c);) ApplySetting(c.id, c.value);
    if (const int key = presenter_.PopKey()) PressKey(key);
    if (const int key = telemetry_.PopKey()) PressKey(key);
}

void App::PressKey(int key) {
    if (key == VK_ESCAPE) g_quitRequested = true;
    else if (key == 'M' && audio_.Running()) ApplySetting(SettingId::Mute, audio_.Muted() ? 0 : 1);
}

void App::RunKeyScript() {
    const double elapsedSec = Qpc::ToSec(Qpc::Now() - startQpc_);
    while (!g_quitRequested && keyScriptPos_ < options_.keyScript.size() && elapsedSec >= options_.keyScript[keyScriptPos_].atSec) {
        const KeyScriptStep step = options_.keyScript[keyScriptPos_++];
        if (step.key) {
            PressKey(step.key);
            continue;
        }
        if (settingsDirty_) PublishSettings();
        const auto item = std::find_if(menu_.items.begin(), menu_.items.end(), [&](const SettingItem& it) { return step.setting == SettingName(it.id); });
        if (item == menu_.items.end()) {
            LOGW("key-script: no setting '%s' here", step.setting.c_str());
            continue;
        }
        const std::optional<int> value = SettingValue(*item, step.value);
        if (!value) {
            LOGW("key-script: '%s' is not a value of %s", step.value.c_str(), step.setting.c_str());
            continue;
        }
        ApplySetting(item->id, *value);
    }
}

void App::PublishSettings() {
    settingsDirty_ = false;
    publishedCaptureMs_ = stages_.captureMs;
    SettingsMenu menu;
    AddCaptureSettings(menu);
    AddFgSettings(menu);
    AddSrSettings(menu);
    AddAudioSettings(menu);
    AddToolSettings(menu);
    telemetry_.PublishSettings(menu.items);
    menu_ = std::move(menu);
}

void App::AddCaptureSettings(SettingsMenu& menu) const {
    const CaptureInfo* capture = source_.Capture();
    if (!capture) return;
    const CaptureMode& current = capture->mode;
    for (const CaptureMode& m : capture->modes)
        if (SameSizeAndRate(m, current)) menu.formats.push_back(m);
    std::sort(menu.formats.begin(), menu.formats.end(),
              [](const CaptureMode& a, const CaptureMode& b) { return PixFmtBytesPerPixel(a.format) < PixFmtBytesPerPixel(b.format); });
    SettingItem& format = menu.Add(SettingId::CapFormat, SettingItem::Kind::Choice, "采集", "格式");
    format.pausesPlayback = true;
    format.saved = true;
    for (const CaptureMode& m : menu.formats) {
        if (m.format == current.format) format.value = (int)format.choices.size();
        format.choices.push_back(PixFmtLabel(m.format));
        format.choiceNotes.push_back(DeliveryNote(publishedCaptureMs_, m.format, current.format));
    }

    for (const CaptureMode& m : capture->modes)
        if (m.format == current.format) menu.modes.push_back(m);
    std::sort(menu.modes.begin(), menu.modes.end(), [](const CaptureMode& a, const CaptureMode& b) {
        return a.width * a.height != b.width * b.height ? a.width * a.height > b.width * b.height : a.rateHz > b.rateHz;
    });
    SettingItem& mode = menu.Add(SettingId::CapMode, SettingItem::Kind::Choice, "采集", "分辨率");
    mode.pausesPlayback = true;
    mode.saved = true;
    for (const CaptureMode& m : menu.modes) {
        if (SameSizeAndRate(m, current)) mode.value = (int)mode.choices.size();
        mode.choices.push_back(CaptureModeLabel(m));
        mode.choiceNotes.push_back(SameSize(m, current) ? "" : "重建处理链");
    }

    const ColorSpec automatic = ResolveColor(ColorOptions{}, capture->tagged, current.format);
    SettingItem& matrix = menu.Add(SettingId::Matrix, SettingItem::Kind::Choice, "采集", "矩阵");
    matrix.saved = true;
    for (const std::optional<ColorMatrix>& m : kMatrixChoices)
        matrix.choices.push_back(m ? std::string(MatrixName(*m)) : std::string("自动（") + MatrixName(automatic.matrix) + "）");
    matrix.value = ChoiceIndex(kMatrixChoices, options_.color.matrix);
    if (IsRgb(current.format)) {
        matrix.enabled = false;
        matrix.note = "RGB 输入不需要";
    }
    SettingItem& range = menu.Add(SettingId::Range, SettingItem::Kind::Choice, "采集", "范围");
    range.saved = true;
    for (const std::optional<bool>& full : kRangeChoices)
        range.choices.push_back(!full ? std::string("自动（") + (automatic.fullRange ? "全" : "有限") + "）" : *full ? "全 0–255" : "有限 16–235");
    range.value = ChoiceIndex(kRangeChoices, options_.color.fullRange);
}

void App::AddFgSettings(SettingsMenu& menu) const {
    if (!options_.fg) return;
    menu.Add(SettingId::Fg, SettingItem::Kind::Toggle, "补帧", "开启").value = fgOn_;
    SettingItem& quality = menu.Add(SettingId::FgQuality, SettingItem::Kind::Choice, "补帧", "档位");
    quality.pausesPlayback = true;
    quality.saved = true;
    for (const QualityLevel& level : FgQualityLevels()) {
        if (level.value == options_.fgQuality) quality.value = (int)quality.choices.size();
        quality.choices.push_back(level.name);
    }
    if (vfx_->FgLoadMs() > 0) quality.note = LoadTimeNote(vfx_->FgLoadMs());
    if (mode_ != PaceMode::Vrr) return;

    const double content = ContentFps();
    auto outcome = [&](int requested, double thresholdFps) {
        const MultiplierChoice c = MultiplierFor(requested, thresholdFps);
        switch (c.limit) {
        case MultiplierLimit::Off: return std::string("补帧已关");
        case MultiplierLimit::Threshold: return std::format("{:.0f} 帧已到门槛：不补", content);
        case MultiplierLimit::Refresh:
        case MultiplierLimit::Gpu:
            return std::format("{:.0f} → {:.0f} fps（{}限制为 ×{}）", content, content * c.multiplier, c.limit == MultiplierLimit::Gpu ? "GPU " : "刷新率",
                               c.multiplier);
        case MultiplierLimit::None: break;
        }
        return std::format("{:.0f} → {:.0f} fps", content, content * c.multiplier);
    };
    SettingItem& multiplier = menu.Add(SettingId::FgMultiplier, SettingItem::Kind::Choice, "补帧", "倍数");
    multiplier.saved = true;
    for (int k = 1; k <= kMaxFgMultiplier; k++) {
        multiplier.choices.push_back(std::format("{}{}", kTimesSign, k));
        multiplier.choiceNotes.push_back(outcome(k, options_.fgThresholdFps));
    }
    multiplier.value = options_.fgMultiplier - 1;

    const double refreshHz = presenter_.RefreshHz();
    menu.fgThresholds.assign(std::begin(kThresholdChoicesFps), std::end(kThresholdChoicesFps));
    for (double extra : {std::round(refreshHz), options_.fgThresholdFps}) {
        const bool listed =
            std::any_of(menu.fgThresholds.begin(), menu.fgThresholds.end(), [&](double t) { return std::abs(t - extra) < kSameThresholdTolerance; });
        if (!listed) menu.fgThresholds.push_back(extra);
    }
    std::sort(menu.fgThresholds.begin(), menu.fgThresholds.end());
    SettingItem& threshold = menu.Add(SettingId::FgThreshold, SettingItem::Kind::Choice, "补帧", "门槛");
    threshold.saved = true;
    for (double t : menu.fgThresholds) {
        if (std::abs(t - options_.fgThresholdFps) < kSameThresholdTolerance) threshold.value = (int)threshold.choices.size();
        threshold.choices.push_back(t <= 0                                      ? std::string("不限")
                                    : std::abs(t - refreshHz) < kRefreshMatchTolerance ? std::format("{:.0f}（刷新率）", t)
                                                                                      : std::format("{:.0f}", t));
        threshold.choiceNotes.push_back(outcome(options_.fgMultiplier, t));
    }
}

void App::AddSrSettings(SettingsMenu& menu) const {
    if (!options_.sr) return;
    menu.Add(SettingId::Sr, SettingItem::Kind::Toggle, "超分", "开启").value = srOn_;
    SettingItem& quality = menu.Add(SettingId::SrQuality, SettingItem::Kind::Choice, "超分", "档位");
    quality.pausesPlayback = true;
    quality.saved = true;
    const bool upscaling = outputSize_.width != source_.Mode().width || outputSize_.height != source_.Mode().height;
    for (const QualityLevel& level : SrQualityLevels()) {
        if (upscaling && SrQualityNeedsSameSize(level.value)) continue;
        if (level.value == options_.srQuality) quality.value = (int)quality.choices.size();
        menu.srQualities.push_back(level.value);
        quality.choices.push_back(level.name);
    }
    if (vfx_->SrLoadMs() > 0) quality.note = LoadTimeNote(vfx_->SrLoadMs());
}

void App::AddAudioSettings(SettingsMenu& menu) const {
    if (!audio_.Running()) return;
    menu.Add(SettingId::Mute, SettingItem::Kind::Toggle, "声音", "静音").value = audio_.Muted();
    SettingItem& volume = menu.Add(SettingId::AudioVolume, SettingItem::Kind::Choice, "声音", "音量");
    volume.saved = true;
    for (int step = 0; step <= kVolumeSteps; step++) volume.choices.push_back(std::format("{}%", step * kVolumeStepPercent));
    volume.value = (int)std::lround(options_.audioVolumePercent / kVolumeStepPercent);
}

void App::AddToolSettings(SettingsMenu& menu) const {
    SettingItem& timecode = menu.Add(SettingId::Timecode, SettingItem::Kind::Toggle, "其他", "时间码");
    timecode.value = options_.timecode;
    timecode.note = "回环测延迟用";
    menu.Add(SettingId::PrintStats, SettingItem::Kind::Action, "其他", "打印统计").note = "分段耗时写进日志";
    menu.Add(SettingId::ResetStats, SettingItem::Kind::Action, "其他", "重置统计");
    menu.Add(SettingId::Save, SettingItem::Kind::Action, "其他", "保存为默认").note = "写入 " + FileName(options_.config);
}

void App::ApplySetting(SettingId id, int value) {
    settingsDirty_ = true;
    auto valid = [&](size_t count) { return value >= 0 && (size_t)value < count; };
    switch (id) {
    case SettingId::CapFormat:
        if (valid(menu_.formats.size()) && menu_.formats[value].format != source_.Mode().format) RestartCapture(CaptureMode(menu_.formats[value]));
        break;
    case SettingId::CapMode:
        if (valid(menu_.modes.size()) && !SameSizeAndRate(menu_.modes[value], source_.Mode())) RestartCapture(CaptureMode(menu_.modes[value]));
        break;
    case SettingId::Matrix:
        if (valid(std::size(kMatrixChoices))) SetMatrix(value);
        break;
    case SettingId::Range:
        if (valid(std::size(kRangeChoices))) SetRange(value);
        break;
    case SettingId::Fg: SetFgOn(value != 0); break;
    case SettingId::FgQuality:
        if (valid(FgQualityLevels().size())) SwitchModels(FgQualityLevels()[value].value, options_.srQuality);
        break;
    case SettingId::FgMultiplier:
        if (valid(kMaxFgMultiplier)) SetFgMultiplier(value + 1);
        break;
    case SettingId::FgThreshold:
        if (valid(menu_.fgThresholds.size())) SetFgThreshold(menu_.fgThresholds[value]);
        break;
    case SettingId::Sr: SetSrOn(value != 0); break;
    case SettingId::SrQuality:
        if (valid(menu_.srQualities.size())) SwitchModels(options_.fgQuality, menu_.srQualities[value]);
        break;
    case SettingId::Mute:
        audio_.SetMuted(value != 0);
        LOGI("audio %s", value ? "muted" : "unmuted");
        break;
    case SettingId::AudioVolume:
        if (valid(kVolumeSteps + 1)) SetAudioVolume(value * kVolumeStepPercent);
        break;
    case SettingId::Timecode:
        options_.timecode = value != 0;
        LOGI("timecode %s", options_.timecode ? "on" : "off");
        break;
    case SettingId::PrintStats: PrintDetail(); break;
    case SettingId::ResetStats:
        ResetStats();
        LOGI("stats reset");
        break;
    case SettingId::Save: SaveSettings(); break;
    case SettingId::Count: break;
    }
}

void App::SetMatrix(int choice) {
    options_.color.matrix = kMatrixChoices[choice];
    source_.SetColor(options_.color);
    LOGI("colour matrix %s", options_.color.matrix ? MatrixName(*options_.color.matrix) : "auto");
}

void App::SetRange(int choice) {
    options_.color.fullRange = kRangeChoices[choice];
    source_.SetColor(options_.color);
    LOGI("colour range %s", !options_.color.fullRange ? "auto" : *options_.color.fullRange ? "full" : "limited");
}

void App::SetFgOn(bool on) {
    if (fgOn_ == on) return;
    fgOn_ = on;
    pacer_.SetFg(FgActive());
    UpdateMultiplier();
    statsEpoch_++;
    LOGI("frame generation %s: x%d", fgOn_ ? "on" : "off", pacer_.Multiplier());
}

void App::SetFgMultiplier(int multiplier) {
    options_.fgMultiplier = multiplier;
    UpdateMultiplier();
    LOGI("frame generation x%d below %.0f fps: now x%d", options_.fgMultiplier, options_.fgThresholdFps, pacer_.Multiplier());
}

void App::SetFgThreshold(double thresholdFps) {
    options_.fgThresholdFps = thresholdFps;
    UpdateMultiplier();
    LOGI("frame generation x%d below %.0f fps: now x%d", options_.fgMultiplier, options_.fgThresholdFps, pacer_.Multiplier());
}

void App::SetSrOn(bool on) {
    if (srOn_ == on) return;
    srOn_ = on;
    statsEpoch_++;
    LOGI("super resolution %s", srOn_ ? "on" : "off");
}

void App::SetAudioVolume(int percent) {
    options_.audioVolumePercent = percent;
    audio_.SetVolume((float)(percent / 100.0));
    LOGI("audio volume %d%%", percent);
}

void App::SaveSettings() {
    std::vector<std::string> keys;
    for (const SettingItem& item : menu_.items)
        if (item.saved) keys.push_back(SettingName(item.id));
    if (SaveOptions(options_, keys)) LOGI("settings saved to %s", options_.config.c_str());
    else LOGE("settings: cannot write %s", options_.config.c_str());
}

}
