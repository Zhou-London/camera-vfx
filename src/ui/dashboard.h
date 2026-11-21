// Terminal dashboard (FTXUI) and the per-setting latency record.
//
// Runs on its own below-normal-priority thread; reads only from Telemetry (lock-free frame ring +
// status snapshot) and from the log sink, so it can never stall the render thread.
//  * TUI mode: full-screen dashboard (settings, live latency with p50/p99, 60 s history graphs,
//    per-setting comparison table, counters, audio, recent log lines, key hints). Keys typed into
//    the console are forwarded to the app through Telemetry::PushKey.
//  * Record: every period with unchanged settings is a "segment"; its latency / GPU / fps statistics
//    are shown in the comparison table and appended to historyPath (CSV) when the segment ends.
#pragma once
#include "telemetry.h"
#include "../util/common.h"
#include <memory>
#include <string>

namespace camss {

struct DashboardOptions {
    bool tui = true;              // interactive full-screen dashboard; false = record only (plain console)
    std::string historyPath;      // CSV file to append segments to ("" = do not write)
    double refreshHz = 10;        // UI refresh rate
};

struct DashboardImpl;

class Dashboard {
public:
    Dashboard();
    ~Dashboard();
    bool Start(Telemetry* tel, const DashboardOptions& o);
    void Stop();                              // restores the terminal, flushes the history file, joins
    bool Running() const;
    // Log sink: called by the logger's writer thread for every message (any thread; never blocks long).
    void OnLog(LogLevel lvl, double tSec, const char* msg, size_t len);
    // Plain-text table of the recorded segments (for the console after Stop()).
    std::string Summary() const;
private:
    std::unique_ptr<DashboardImpl> p_;
};

} // namespace camss
