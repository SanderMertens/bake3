#ifndef BAKE2_OS_H
#define BAKE2_OS_H

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

char bake_path_sep(void);
const char* bake_path_last_sep(const char *path);
bool bake_path_is_abs(const char *path);
char* bake_path_join(const char *lhs, const char *rhs);
char* bake_path_join3(const char *a, const char *b, const char *c);
bool bake_path_is_sep(char ch);
int bake_path_is_dir(const char *path);
bool bake_path_equal_normalized(const char *lhs, const char *rhs);
bool bake_path_has_prefix_normalized(const char *path, const char *prefix, size_t *prefix_len_out);
int bake_path_exists(const char *path);

int bake_setenv(const char *name, const char *value);
int bake_unsetenv(const char *name);
char* bake_get_home(void);
const char* bake_executable_name(void);
int32_t bake_default_threads(void);

const char* bake_host_os(void);
const char* bake_host_arch(void);
char* bake_host_platform(void);
char* bake_host_triplet(const char *mode);

int64_t bake_os_file_mtime(const char *path);
int bake_os_mkdir(const char *path);
char* bake_os_getcwd(void);
int bake_os_rmdir(const char *path);

char* bake_file_read(const char *path, size_t *len_out);
int bake_file_write(const char *path, const char *content);
int bake_mkdirs(const char *path);
int bake_rmtree(const char *path);
int64_t bake_file_mtime(const char *path);
int bake_file_copy(const char *src, const char *dst);
int bake_is_dir(const char *path);
char* bake_dirname(const char *path);
char* bake_basename(const char *path);
char* bake_stem(const char *path);
char* bake_getcwd(void);

int bake_proc_run(
    const char *const *argv,
    const bake_process_stdio_t *stdio_cfg,
    bake_process_result_t *result);
int bake_proc_run_argv(const char *const *argv, bake_process_result_t *result);
bool bake_proc_was_interrupted(void);
void bake_proc_clear_interrupt(void);

#endif
