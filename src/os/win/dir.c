#if defined(_WIN32)

#include "bake/os.h"
#include <flecs.h>

#include <windows.h>

static wchar_t* bake_utf8_to_wide(const char *s) {
    if (!s) {
        return NULL;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *out = ecs_os_malloc((int32_t)(n * (int)sizeof(wchar_t)));
    if (!out) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n) <= 0) {
        ecs_os_free(out);
        return NULL;
    }
    return out;
}

static char* bake_wide_to_utf8(const wchar_t *s) {
    if (!s) {
        return NULL;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    char *out = ecs_os_malloc((int32_t)n);
    if (!out) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL) <= 0) {
        ecs_os_free(out);
        return NULL;
    }
    return out;
}

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out) {
    char *pattern = bake_path_join(path, "*");
    if (!pattern) {
        return -1;
    }

    wchar_t *wpattern = bake_utf8_to_wide(pattern);
    ecs_os_free(pattern);
    if (!wpattern) {
        return -1;
    }

    WIN32_FIND_DATAW ffd;
    HANDLE handle = FindFirstFileW(wpattern, &ffd);
    ecs_os_free(wpattern);

    if (handle == INVALID_HANDLE_VALUE) {
        bake_log_win_error_last("open directory", path);
        return -1;
    }

    ecs_vec_t vec = {0};
    ecs_vec_init_t(NULL, &vec, bake_dir_entry_t, 0);

    do {
        bake_dir_entry_t *entry = ecs_vec_append_t(NULL, &vec, bake_dir_entry_t);
        entry->name = bake_wide_to_utf8(ffd.cFileName);
        entry->path = entry->name ? bake_path_join(path, entry->name) : NULL;
        if (!entry->name || !entry->path) {
            bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
            FindClose(handle);
            return -1;
        }
        entry->is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileW(handle, &ffd) != 0);

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        bake_log_win_error("read directory", path, err);
        bake_dir_entries_free(ecs_vec_first_t(&vec, bake_dir_entry_t), ecs_vec_count(&vec));
        FindClose(handle);
        return -1;
    }

    if (!FindClose(handle)) {
        bake_log_win_error_last("close directory", path);
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
