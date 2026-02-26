#include "bake/os.h"

const char* bake_os_host(void) {
#if defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#elif defined (_WIN32)
#else
    return "Windows";
#endif
}
