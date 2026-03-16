#if !defined(_WIN32)

#include "bake/os.h"

#include <dirent.h>
#include <sys/stat.h>

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out) {
    DIR *dir = opendir(path);
    if (!dir) {
        bake_log_last_errno("open directory", path);
        return -1;
    }

    ecs_vec_t vec = {0};
    ecs_vec_init_t(NULL, &vec, bake_dir_entry_t, 0);

    struct dirent *de = NULL;
    errno = 0;
    while ((de = readdir(dir))) {
        bake_dir_entry_t *entry = ecs_vec_append_t(NULL, &vec, bake_dir_entry_t);
        entry->name = ecs_os_strdup(de->d_name);
        entry->path = bake_path_join(path, de->d_name);
        if (!entry->name || !entry->path) {
            bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
            closedir(dir);
            return -1;
        }

        struct stat st;
        entry->is_dir = false;
        if (stat(entry->path, &st) != 0) {
            bake_log_last_errno("stat directory entry", entry->path);
            bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
            closedir(dir);
            return -1;
        }
        entry->is_dir = S_ISDIR(st.st_mode);
    }

    if (errno != 0) {
        bake_log_last_errno("read directory", path);
        bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
        closedir(dir);
        return -1;
    }

    if (closedir(dir) != 0) {
        bake_log_last_errno("close directory", path);
        bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
        return -1;
    }

    *entries_out = ecs_vec_first_t(&vec, bake_dir_entry_t);
    *count_out = ecs_vec_count(&vec);
    return 0;
}

#endif

#if defined(_WIN32)
typedef int bake_dir_posix_dummy_t;
#endif
