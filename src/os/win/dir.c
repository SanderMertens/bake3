#if defined(_WIN32)

#include "bake/os.h"

#include <windows.h>

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out) {
    char *pattern = bake_join_path(path, "*");
    if (!pattern) {
        return -1;
    }

    WIN32_FIND_DATAA ffd;
    HANDLE handle = FindFirstFileA(pattern, &ffd);
    ecs_os_free(pattern);

    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    int32_t count = 0;
    int32_t capacity = 16;
    bake_dir_entry_t *entries = ecs_os_calloc_n(bake_dir_entry_t, capacity);
    if (!entries) {
        FindClose(handle);
        return -1;
    }

    do {
        if (count == capacity) {
            int32_t next = capacity * 2;
            bake_dir_entry_t *next_entries = ecs_os_realloc_n(entries, bake_dir_entry_t, next);
            if (!next_entries) {
                bake_dir_entries_free(entries, count);
                FindClose(handle);
                return -1;
            }
            entries = next_entries;
            capacity = next;
        }

        bake_dir_entry_t *entry = &entries[count++];
        entry->name = bake_strdup(ffd.cFileName);
        entry->path = bake_join_path(path, ffd.cFileName);
        entry->is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileA(handle, &ffd) != 0);

    FindClose(handle);

    *entries_out = entries;
    *count_out = count;
    return 0;
}

#endif

#if !defined(_WIN32)
typedef int bake_dir_win_dummy_t;
#endif
