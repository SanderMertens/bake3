#if !defined(_WIN32)

#include "bake/os.h"

#include <dirent.h>
#include <sys/stat.h>

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out) {
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    int32_t count = 0;
    int32_t capacity = 16;
    bake_dir_entry_t *entries = ecs_os_calloc_n(bake_dir_entry_t, capacity);
    if (!entries) {
        closedir(dir);
        return -1;
    }

    struct dirent *de = NULL;
    while ((de = readdir(dir))) {
        if (count == capacity) {
            int32_t next = capacity * 2;
            bake_dir_entry_t *next_entries = ecs_os_realloc_n(entries, bake_dir_entry_t, next);
            if (!next_entries) {
                bake_dir_entries_free(entries, count);
                closedir(dir);
                return -1;
            }
            entries = next_entries;
            capacity = next;
        }

        bake_dir_entry_t *entry = &entries[count++];
        entry->name = bake_strdup(de->d_name);
        entry->path = bake_join_path(path, de->d_name);

        struct stat st;
        entry->is_dir = false;
        if (stat(entry->path, &st) == 0) {
            entry->is_dir = S_ISDIR(st.st_mode);
        }
    }

    closedir(dir);

    *entries_out = entries;
    *count_out = count;
    return 0;
}

#endif

#if defined(_WIN32)
typedef int bake_dir_posix_dummy_t;
#endif
