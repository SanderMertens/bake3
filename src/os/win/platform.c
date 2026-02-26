#if defined(_WIN32)

#include "bake/os.h"

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

char* bake_os_get_home(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        home = getenv("USERPROFILE");
    }
    return home ? bake_strdup(home) : NULL;
}

const char* bake_os_executable_name(void) {
    return "bake.exe";
}

int32_t bake_os_default_threads(void) {
    return 4;
}

#endif

#if !defined(_WIN32)
typedef int bake_os_win_platform_dummy_t;
#endif
