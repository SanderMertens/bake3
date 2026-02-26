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

#define BAKE_UNUSED(x) (void)(x)

char* bake_strdup(const char *str);
char* bake_join_path(const char *lhs, const char *rhs);
char* bake_join3_path(const char *a, const char *b, const char *c);
char* bake_asprintf(const char *fmt, ...);
char* bake_read_file(const char *path, size_t *len_out);
int bake_write_file(const char *path, const char *content);
int bake_mkdirs(const char *path);
int bake_path_exists(const char *path);
int64_t bake_file_mtime(const char *path);
int bake_is_dir(const char *path);
char* bake_dirname(const char *path);
char* bake_basename(const char *path);
char* bake_stem(const char *path);
char* bake_getcwd(void);
int bake_remove_tree(const char *path);
int bake_copy_file(const char *src, const char *dst);
int bake_run_command(const char *cmd);
int bake_run_command_quiet(const char *cmd);
char* bake_host_triplet(const char *mode);
char* bake_host_platform(void);
char* bake_project_build_root(const char *project_path, const char *mode);
bool bake_path_has_prefix_normalized(const char *path, const char *prefix, size_t *prefix_len_out);
bool bake_path_equal_normalized(const char *lhs, const char *rhs);
int bake_entity_list_append_unique(
    ecs_entity_t **entities,
    int32_t *count,
    int32_t *capacity,
    ecs_entity_t entity);

#endif
