#ifndef BAKE3_PS_H
#define BAKE3_PS_H

#include "bake/context.h"

#define BAKE_PS_ENV_LOCAL "local"
#define BAKE_PS_ENV_GLOBAL "global"
#define BAKE_PS_ENV_UNKNOWN "?"

typedef enum bake_ps_env_kind_t {
    BakePsEnvUnknown = 0,
    BakePsEnvLocal = 1,
    BakePsEnvGlobal = 2
} bake_ps_env_kind_t;

typedef struct bake_ps_info_t {
    const char *project;
    const char *cfg;
    const char *env;
    bake_ps_env_kind_t env_kind;
    const char *bake_home;
    const char *workspace;
    const char *kind;
    bool announce;
} bake_ps_info_t;

typedef struct bake_ps_entry_t {
    int64_t pid;
    int64_t parent_pid;
    int64_t start_time;
    int64_t elapsed_sec;
    char *project;
    char *cfg;
    char *env;
    bake_ps_env_kind_t env_kind;
    char *bake_home;
    char *workspace;
    char *cmd;
    char *kind;
    const char *state;
    const char *source;
} bake_ps_entry_t;

typedef struct bake_ps_list_t {
    bake_ps_entry_t *items;
    int32_t count;
} bake_ps_list_t;

typedef struct bake_ps_path_info_t {
    char *env;
    char *workspace;
    char *cfg;
    char *project;
} bake_ps_path_info_t;

char* bake_ps_registry_dir(void);

int bake_ps_register(
    int64_t pid,
    const char *const *argv,
    const bake_ps_info_t *info);

void bake_ps_unregister(int64_t pid);

int bake_ps_collect(bake_ps_list_t *list_out, bool all_users);

void bake_ps_list_fini(bake_ps_list_t *list);

int64_t bake_ps_parse_etime(const char *etime);

void bake_ps_format_elapsed(int64_t seconds, char *buf, size_t size);

char* bake_ps_shorten_path(const char *path, int32_t max_len);

char* bake_ps_render_workspace(const char *workspace, bool full);

char* bake_ps_render_cmd(const char *cmd, bool full);

int bake_ps_parse_local_env_path(const char *path, bake_ps_path_info_t *info_out);

void bake_ps_path_info_fini(bake_ps_path_info_t *info);

bake_ps_env_kind_t bake_ps_env_from_home(const char *bake_home, char **name_out);

const char* bake_ps_env_kind_str(bake_ps_env_kind_t kind);

bake_ps_env_kind_t bake_ps_env_kind_from_str(const char *kind);

char* bake_ps_env_label(bake_ps_env_kind_t kind, const char *name);

bool bake_ps_entry_matches(const bake_ps_entry_t *entry, const char *target);

int bake_ps_command(bake_context_t *ctx);

#endif
