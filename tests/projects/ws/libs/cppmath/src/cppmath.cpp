#include "ws/cppmath.hpp"

extern "C" {
#include "ws/core.h"
}

int ws_cppmath_accumulate(int value) {
    return ws_core_add(value, 10);
}
