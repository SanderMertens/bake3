#ifndef BAKE2_COMMON_H
#define BAKE2_COMMON_H

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <flecs.h>

#if defined(_WIN32)
#define B2_PATH_SEP '\\'
#define B2_PATH_SEP_STR "\\"
#else
#define B2_PATH_SEP '/'
#define B2_PATH_SEP_STR "/"
#endif

#define B2_UNUSED(x) (void)(x)

#define B2_LOG(...) \
    do { \
        fprintf(stdout, __VA_ARGS__); \
        fputc('\n', stdout); \
    } while (0)

#define B2_ERR(...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } while (0)

char* b2_strdup(const char *str);
char* b2_join_path(const char *lhs, const char *rhs);
char* b2_join3_path(const char *a, const char *b, const char *c);
char* b2_asprintf(const char *fmt, ...);
char* b2_read_file(const char *path, size_t *len_out);
int b2_write_file(const char *path, const char *content);
int b2_mkdirs(const char *path);
int b2_path_exists(const char *path);
int b2_is_dir(const char *path);
char* b2_dirname(const char *path);
char* b2_basename(const char *path);
char* b2_stem(const char *path);
char* b2_getcwd(void);
int b2_remove_tree(const char *path);
int b2_copy_file(const char *src, const char *dst);
int b2_run_command(const char *cmd);

#endif
