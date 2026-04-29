#if !defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <unistd.h>

int bake_os_setenv(const char *name, const char *value) {
    if (!name || !name[0] || !value) {
        return -1;
    }
    return setenv(name, value, 1);
}

int bake_os_unsetenv(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    return unsetenv(name);
}

char* bake_os_home_path(void) {
    const char *home = getenv("HOME");
    return home ? ecs_os_strdup(home) : NULL;
}

const char* bake_host_executable_name(void) {
    return "bake";
}

int32_t bake_host_threads(void) {
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 1) {
        return 1;
    }
    if (cpu > 128) {
        return 128;
    }
    return (int32_t)cpu;
}

#endif

#if defined(_WIN32)
typedef int bake_os_posix_platform_dummy_t;
#endif
