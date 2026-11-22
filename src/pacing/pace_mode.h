#pragma once

namespace cameravfx {

enum class PaceMode { Passthrough, Fixed, Vrr };
const char* PaceModeName(PaceMode mode);

}
