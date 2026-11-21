#include "dashboard.h"
// STUB: replaced by the dashboard work item.
namespace camss {
struct DashboardImpl {};
Dashboard::Dashboard() = default;
Dashboard::~Dashboard() = default;
bool Dashboard::Start(Telemetry*, const DashboardOptions&) { return false; }
void Dashboard::Stop() {}
bool Dashboard::Running() const { return false; }
void Dashboard::OnLog(LogLevel, double, const char*, size_t) {}
std::string Dashboard::Summary() const { return {}; }
} // namespace camss
