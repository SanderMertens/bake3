#include "bake/os.h"
#include <flecs.h>

#include <stdlib.h>
#include <time.h>

#define BAKE_LOCK_POLL_MS (200)
#define BAKE_LOCK_STALE_SEC (2 * 60 * 60)

static char* bake_lock_owner_path(const char *dir) {
    return bake_path_join(dir, "owner");
}

static bool bake_lock_is_stale(const char *dir) {
    char *owner_path = bake_lock_owner_path(dir);
    char *owner = bake_file_read_trimmed(owner_path);
    ecs_os_free(owner_path);

    bool stale = false;
    if (owner && owner[0]) {
        int64_t pid = (int64_t)strtoll(owner, NULL, 10);
        if (pid > 0 && !bake_os_pid_alive(pid)) {
            stale = true;
        }
    }
    ecs_os_free(owner);

    if (!stale) {
        int64_t mtime = bake_os_file_mtime(dir);
        if (mtime < 0) {
            return false;
        }
        int64_t age_sec = (int64_t)(time(NULL)) - (mtime / 1000000000LL);
        if (age_sec > BAKE_LOCK_STALE_SEC) {
            stale = true;
        }
    }

    return stale;
}

int bake_os_lock_acquire(
    const char *path,
    int32_t timeout_sec,
    bake_lock_t *lock_out)
{
    if (!path || !path[0] || !lock_out) {
        return -1;
    }

    lock_out->path = NULL;
    lock_out->held = false;

    char *parent = bake_path_dirname(path);
    if (parent && parent[0] && bake_os_mkdirs(parent) != 0) {
        ecs_os_free(parent);
        return -1;
    }
    ecs_os_free(parent);

    int64_t waited_ms = 0;
    int64_t timeout_ms = (int64_t)timeout_sec * 1000;
    bool reported = false;

    while (true) {
        if (bake_os_mkdir(path) == 0) {
            char *owner_path = bake_lock_owner_path(path);
            char *owner = flecs_asprintf("%lld\n", (long long)bake_os_pid());
            bake_file_write(owner_path, owner);
            ecs_os_free(owner);
            ecs_os_free(owner_path);
            lock_out->path = ecs_os_strdup(path);
            lock_out->held = true;
            return 0;
        }

        if (!bake_path_exists(path)) {
            return -1;
        }

        if (bake_lock_is_stale(path)) {
            ecs_warn("removing stale lock %s", path);
            if (bake_os_rmtree(path) != 0) {
                return -1;
            }
            continue;
        }

        if (timeout_ms > 0 && waited_ms >= timeout_ms) {
            ecs_err("timed out waiting for lock %s", path);
            return -1;
        }

        if (!reported) {
            reported = true;
            ecs_trace("waiting for lock %s", path);
        }

        ecs_os_sleep(0, BAKE_LOCK_POLL_MS * 1000 * 1000);
        waited_ms += BAKE_LOCK_POLL_MS;
    }
}

void bake_os_lock_release(bake_lock_t *lock) {
    if (!lock || !lock->held || !lock->path) {
        return;
    }

    bake_os_rmtree(lock->path);
    ecs_os_free(lock->path);
    lock->path = NULL;
    lock->held = false;
}
