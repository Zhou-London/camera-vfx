// Terminal dashboard (FTXUI) and the per-setting latency record (CSV).
//
// Runs on its own below-normal-priority thread; reads only from Telemetry (lock-free frame ring +
// status snapshot) and from the log sink, so it can never stall the render thread.
//  * TUI mode: full-screen dashboard (live latency with p50/p99, the switches with their keys and
//    the read-only setup, 60 s history graphs, recent log lines). Keys typed into the console are
//    forwarded to the app through Telemetry::PushKey.
//  * Record: every period with unchanged settings is a "segment"; its latency / GPU / fps statistics
//    are appended to historyPath (CSV) when the segment ends.
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

// Cost of the UI thread (for benchmarks / diagnostics).
struct DashboardPerf {
    uint64_t refreshes = 0;       // model updates (drain ring + statistics)
    uint64_t frames = 0;          // frame samples consumed
    uint64_t draws = 0;           // TUI frames built
    double updateMsAvg = 0, updateMsMax = 0;   // model update wall time
    double buildMsAvg = 0, buildMsMax = 0;     // element tree build wall time (TUI)
    uint64_t uiCycles = 0;        // QueryThreadCycleTime of the UI thread so far
};

struct DashboardImpl;

class Dashboard {
public:
    Dashboard();
    ~Dashboard();
    bool Start(Telemetry* tel, const DashboardOptions& o);
    void Stop();                              // restores the terminal, flushes the history file, joins
    bool Running() const;
    // True while the full-screen TUI owns the console (false in record-only mode, or when the
    // console is not interactive and Start() fell back to record-only). While true, nothing else
    // should write to the console.
    bool TuiActive() const;
    // Log sink: called by the logger's writer thread for every message (any thread; never blocks long).
    void OnLog(LogLevel lvl, double tSec, const char* msg, size_t len);
    DashboardPerf Perf() const;

    // ---- offline use (tests, tui_demo --render); do not mix with Start()
    // Updates the model once from `tel` on the calling thread; nowQpc = "now" (simulated clocks).
    void Pump(Telemetry* tel, const DashboardOptions& o, int64_t nowQpc);
    // Ends the current segment at nowQpc (as Stop() does): written to the history if long enough.
    void Finish(int64_t nowQpc);
    // Renders the dashboard off-screen; ansi = with colour escape codes, else plain text;
    // help = with the key-help overlay.
    std::string RenderText(int width, int height, bool ansi, bool help = false) const;
private:
    std::unique_ptr<DashboardImpl> p_;
};

} // namespace camss
