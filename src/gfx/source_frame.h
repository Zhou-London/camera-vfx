#pragma once
#include "device.h"

namespace cameravfx {

struct SourceFrame {
    int64_t n = -1;
    Device::Tex rgb;
    int64_t sampleQpc = 0, arrivalQpc = 0, uploadedQpc = 0;
};

}
