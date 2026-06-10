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

char* bake_os_executable_path(void) {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return NULL;
    }
    return ecs_os_strdup(buf);
}

int32_t bake_os_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int32_t)si.dwNumberOfProcessors;
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_platform_dummy_t;
#endif
