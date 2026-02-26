#if defined(_WIN32)

#include "bake/os.h"

int bake_setenv(const char *name, const char *value) {
    if (!name || !name[0] || !value) {
        return -1;
    }
    return _putenv_s(name, value);
}

int bake_unsetenv(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    return _putenv_s(name, "");
}

char* bake_get_home(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = getenv("USERPROFILE");
    }
    return home ? ecs_os_strdup(home) : NULL;
}

const char* bake_executable_name(void) {
    return "bake.exe";
}

int32_t bake_default_threads(void) {
    return 4;
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_platform_dummy_t;
#endif
