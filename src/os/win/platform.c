#if defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <windows.h>

int bake_os_setenv(const char *name, const char *value) {
    if (!name || !name[0] || !value) {
        return -1;
    }
    return _putenv_s(name, value);
}

int bake_os_unsetenv(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    return _putenv_s(name, "");
}

char* bake_os_home_path(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = getenv("USERPROFILE");
    }
    return home ? ecs_os_strdup(home) : NULL;
}

const char* bake_host_executable_name(void) {
    return "bake.exe";
}

int32_t bake_host_threads(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD cpu = si.dwNumberOfProcessors;
    if (cpu < 1) {
        return 1;
    }
    if (cpu > 128) {
        return 128;
    }
    return (int32_t)cpu;
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_platform_dummy_t;
#endif
