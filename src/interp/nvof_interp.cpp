// Placeholder: NVOF-based interpolator (being implemented separately).
#include "interpolator.h"
namespace camss {
std::unique_ptr<Interpolator> CreateFlowInterpolator(int) { LOGE("nvof interpolator not built yet"); return nullptr; }
std::unique_ptr<Interpolator> CreateVfxInterpolator() { return nullptr; }
}
