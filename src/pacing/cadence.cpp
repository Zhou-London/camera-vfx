#include "cadence.h"
// STUB: replaced by the cadence work item.
namespace camss {
void CadenceDetector::Reset() { active_ = 1.0; estimate_ = 0; pendingFrames_ = 0; unique_.clear(); seen_.clear(); lastN_ = -1; }
bool CadenceDetector::OnFrame(int64_t, bool) { return false; }
double CadenceDetector::UniqueFraction() const { return 1.0; }
} // namespace camss
