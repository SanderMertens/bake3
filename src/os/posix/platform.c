#if !defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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

char* bake_os_executable_path(void) {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *buf = ecs_os_malloc((ecs_size_t)size + 1);
    if (!buf) {
        return NULL;
    }
    if (_NSGetExecutablePath(buf, &size) != 0) {
        ecs_os_free(buf);
        return NULL;
    }
    char *resolved = bake_path_resolve(buf);
    ecs_os_free(buf);
    return resolved;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return NULL;
    }
    buf[n] = '\0';
    return ecs_os_strdup(buf);
#endif
}

int32_t bake_os_cpu_count(void) {
    return (int32_t)sysconf(_SC_NPROCESSORS_ONLN);
}

#endif

#if defined(_WIN32)
typedef int bake_os_posix_platform_dummy_t;
#endif
