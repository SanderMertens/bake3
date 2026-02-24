#include <stdio.h>

#include "ws/cppmath.hpp"

#ifndef WS_CORE_DEP
#error "dependee defines were not propagated"
#endif

int main(void) {
    int result = ws_cppmath_accumulate(32);
    if (result != 42) {
        return 1;
    }

    printf("hello %d\n", result);
    return 0;
}
