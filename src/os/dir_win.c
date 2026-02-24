#if defined(_WIN32)

#include "bake2/os.h"

#include <windows.h>

int b2_dir_list(const char *path, b2_dir_entry_t **entries_out, int32_t *count_out) {
    char *pattern = b2_join_path(path, "*");
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
    b2_dir_entry_t *entries = ecs_os_calloc_n(b2_dir_entry_t, capacity);
    if (!entries) {
        FindClose(handle);
        return -1;
    }

    do {
        if (count == capacity) {
            int32_t next = capacity * 2;
            b2_dir_entry_t *next_entries = ecs_os_realloc_n(entries, b2_dir_entry_t, next);
            if (!next_entries) {
                b2_dir_entries_free(entries, count);
                FindClose(handle);
                return -1;
            }
            entries = next_entries;
            capacity = next;
        }

        b2_dir_entry_t *entry = &entries[count++];
        entry->name = b2_strdup(ffd.cFileName);
        entry->path = b2_join_path(path, ffd.cFileName);
        entry->is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileA(handle, &ffd) != 0);

    FindClose(handle);

    *entries_out = entries;
    *count_out = count;
    return 0;
}

#endif

#if !defined(_WIN32)
typedef int b2_dir_win_dummy_t;
#endif
