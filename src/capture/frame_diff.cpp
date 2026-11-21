#include "frame_diff.h"
// STUB: replaced by the repeat-detection work item.
namespace camss {
void RepeatDetector::Configure(DiffLayout l, int rb, int r, const RepeatConfig& c) { layout_ = l; rowBytes_ = rb; rows_ = r; cfg_ = c; Reset(); }
bool RepeatDetector::Check(const uint8_t*, ptrdiff_t) { return false; }
void RepeatDetector::Reset() { havePrev_ = false; }
} // namespace camss
