#if !defined(_WIN32)

#include "bake2/os.h"

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

char* bake_os_get_home(void) {
    const char *home = getenv("HOME");
    return home ? bake_strdup(home) : NULL;
}

const char* bake_os_executable_name(void) {
    return "bake";
}

int32_t bake_os_default_threads(void) {
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu > 1 && cpu < 256) {
        return (int32_t)cpu;
    }
    return 4;
}

#endif

#if defined(_WIN32)
typedef int bake_os_posix_platform_dummy_t;
#endif
