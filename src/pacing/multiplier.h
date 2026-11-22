#pragma once
#include <optional>

namespace cameravfx {

enum class MultiplierLimit { None, Off, Threshold, Refresh, Gpu };

struct MultiplierChoice {
    int multiplier = 1;
    MultiplierLimit limit = MultiplierLimit::None;
};

struct MultiplierInputs {
    bool fgActive = false;
    bool vrr = false;
    double contentFps = 0;
    int requested = 1;
    double thresholdFps = 0;
    double maxRefreshHz = 0;
    std::optional<double> generatedFrameMs;
    double leadMs = 0;
    int current = 1;
};

MultiplierChoice ChooseMultiplier(const MultiplierInputs& in);

}
