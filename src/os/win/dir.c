#if defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <windows.h>

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out) {
    char *pattern = bake_path_join(path, "*");
    if (!pattern) {
        return -1;
    }

    WIN32_FIND_DATAA ffd;
    HANDLE handle = FindFirstFileA(pattern, &ffd);
    ecs_os_free(pattern);

    if (handle == INVALID_HANDLE_VALUE) {
        bake_log_last_win_error("open directory", path);
        return -1;
    }

    ecs_vec_t vec = {0};
    ecs_vec_init_t(NULL, &vec, bake_dir_entry_t, 0);

    do {
        bake_dir_entry_t *entry = ecs_vec_append_t(NULL, &vec, bake_dir_entry_t);
        entry->name = ecs_os_strdup(ffd.cFileName);
        entry->path = bake_path_join(path, ffd.cFileName);
        if (!entry->name || !entry->path) {
            bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
            FindClose(handle);
            return -1;
        }
        entry->is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileA(handle, &ffd) != 0);

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        bake_log_win_error("read directory", path, err);
        bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
        FindClose(handle);
        return -1;
    }

    if (!FindClose(handle)) {
        bake_log_last_win_error("close directory", path);
        bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
        return -1;
    }

    *entries_out = ecs_vec_first_t(&vec, bake_dir_entry_t);
    *count_out = ecs_vec_count(&vec);
    return 0;
}

#endif

#if !defined(_WIN32)
typedef int bake_dir_win_dummy_t;
#endif
