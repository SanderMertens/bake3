#ifndef BAKE3_OS_H
#define BAKE3_OS_H

#include "bake/common.h"

typedef struct bake_dir_entry_t {
    char *name;
    char *path;
    bool is_dir;
} bake_dir_entry_t;

typedef struct bake_process_result_t {
    int exit_code;
    int term_signal;
    bool interrupted;
} bake_process_result_t;

typedef struct bake_process_stdio_t {
    const char *stdin_path;
    const char *stdout_path;
    bool stdout_append;
    const char *stderr_path;
    bool stderr_append;
} bake_process_stdio_t;

typedef int (*bake_dir_walk_cb)(const bake_dir_entry_t *entry, void *ctx);

int bake_dir_list(const char *path, bake_dir_entry_t **entries_out, int32_t *count_out);
void bake_dir_entries_free(bake_dir_entry_t *entries, int32_t count);
int bake_dir_walk_recursive(const char *root, bake_dir_walk_cb cb, void *ctx);
bool bake_is_dot_dir(const char *name);

char bake_path_sep(void);
const char* bake_path_last_sep(const char *path);
bool bake_path_is_abs(const char *path);
char* bake_path_join(const char *lhs, const char *rhs);
char* bake_path_join3(const char *a, const char *b, const char *c);
bool bake_path_is_sep(char ch);
void bake_path_normalize(char *path);
char* bake_path_resolve(const char *path);
int bake_path_is_dir(const char *path);
bool bake_path_equal_normalized(const char *lhs, const char *rhs);
bool bake_path_has_prefix_normalized(const char *path, const char *prefix, size_t *prefix_len_out);
int bake_path_exists(const char *path);
void bake_log_errno(const char *action, const char *path, int err);
void bake_log_errno_last(const char *action, const char *path);
int bake_remove_file(const char *path);
int bake_remove_file_if_exists(const char *path);
int bake_rename_file(const char *src, const char *dst);
#if defined(_WIN32)
void bake_log_win_error(const char *action, const char *path, unsigned long err);
void bake_log_win_error_last(const char *action, const char *path);
#endif

int bake_os_setenv(const char *name, const char *value);
int bake_os_unsetenv(const char *name);
char* bake_os_home_path(void);
const char* bake_host_executable_name(void);
int32_t bake_host_threads(void);

const char* bake_host_os(void);
const char* bake_host_arch(void);
char* bake_host_platform(void);
char* bake_host_triplet(const char *mode);

int64_t bake_os_file_mtime(const char *path); /* nanoseconds since unix epoch, -1 on error */
int bake_os_mkdir(const char *path);
char* bake_os_getcwd(void);
int bake_os_rmdir(const char *path);

char* bake_file_read(const char *path, size_t *len_out);
char* bake_file_read_trimmed(const char *path);
int bake_file_write(const char *path, const char *content);
int bake_os_mkdirs(const char *path);
int bake_os_rmtree(const char *path);
int bake_os_file_copy(const char *src, const char *dst);
int bake_file_sync_mode(const char *src, const char *dst);
char* bake_path_dirname(const char *path);
char* bake_path_basename(const char *path);
char* bake_path_stem(const char *path);

int bake_proc_run(
    const char *const *argv,
    const bake_process_stdio_t *stdio_cfg,
    bake_process_result_t *result);
int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result);

#endif
