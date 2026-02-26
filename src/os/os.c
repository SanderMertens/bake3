#include "bake/os.h"

const char* bake_host_os(void) {
#if defined(__APPLE__)
    return "Darwin";
#elif defined(__linux__)
    return "Linux";
#elif defined (_WIN32)
#else
    return "Windows";
#endif
}

const char* bake_host_arch(void) {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

char* bake_host_triplet(const char *mode) {
    const char *cfg = mode && mode[0] ? mode : "debug";
    return flecs_asprintf("%s-%s-%s", bake_host_arch(), bake_host_os(), cfg);
}

char* bake_host_platform(void) {
    return flecs_asprintf("%s-%s", bake_host_arch(), bake_host_os());
}
