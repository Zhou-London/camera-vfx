#pragma once
#include "telemetry.h"
#include "../util/common.h"
#include <memory>
#include <string>
#include <thread>

namespace cameravfx {

struct DashboardState;

void RestoreConsoleAfterCrash();

class Dashboard {
public:
    Dashboard();
    ~Dashboard();
    void Start(Telemetry* tel);
    void Stop();
    void OnLog(LogLevel lvl, double tSec, const char* msg, size_t len);

    void Pump(Telemetry* tel, int64_t nowQpc);
    std::string RenderText(int width, int height, bool ansi, bool popupOpen) const;
private:
    std::unique_ptr<DashboardState> state_;
    std::thread uiThread_;
};

}
