#include "bake/common.h"
#include "bake/os.h"

int bake_entity_list_append_unique(
    ecs_entity_t **entities,
    int32_t *count,
    int32_t *capacity,
    ecs_entity_t entity)
{
    for (int32_t i = 0; i < *count; i++) {
        if ((*entities)[i] == entity) {
            return 0;
        }
    }

    if (*count == *capacity) {
        int32_t next = *capacity ? *capacity * 2 : 32;
        ecs_entity_t *next_entities = ecs_os_realloc_n(*entities, ecs_entity_t, next);
        if (!next_entities) {
            return -1;
        }
        *entities = next_entities;
        *capacity = next;
    }

    (*entities)[(*count)++] = entity;
    return 1;
}

char* bake_project_build_root(const char *project_path, const char *mode) {
    if (!project_path || !project_path[0]) {
        return NULL;
    }

    char *triplet = bake_host_triplet(mode);
    if (!triplet) {
        return NULL;
    }

    char *bake_dir = bake_path_join(project_path, ".bake");
    if (!bake_dir) {
        ecs_os_free(triplet);
        return NULL;
    }

    char *root = bake_path_join(bake_dir, triplet);
    ecs_os_free(triplet);
    ecs_os_free(bake_dir);
    return root;
}

typedef enum bake_cmd_redir_t {
    BAKE_CMD_REDIR_NONE = 0,
    BAKE_CMD_REDIR_STDIN,
    BAKE_CMD_REDIR_STDOUT,
    BAKE_CMD_REDIR_STDERR
} bake_cmd_redir_t;

typedef struct bake_cmd_line_t {
    char **argv;
    int argc;
    int argv_cap;
    bake_process_stdio_t stdio_cfg;
} bake_cmd_line_t;

static bool bake_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void bake_cmd_line_fini(bake_cmd_line_t *cmd) {
    if (!cmd) {
        return;
    }

    if (cmd->argv) {
        for (int i = 0; i < cmd->argc; i++) {
            ecs_os_free(cmd->argv[i]);
        }
        ecs_os_free(cmd->argv);
    }

    ecs_os_free((char*)cmd->stdio_cfg.stdin_path);
    ecs_os_free((char*)cmd->stdio_cfg.stdout_path);
    ecs_os_free((char*)cmd->stdio_cfg.stderr_path);
    memset(cmd, 0, sizeof(*cmd));
}

static int bake_cmd_set_redirect(char **dst, const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    char *str = ecs_os_strdup(path);
    if (!str) {
        return -1;
    }

    ecs_os_free(*dst);
    *dst = str;
    return 0;
}

static bool bake_cmd_parse_redirection_token(
    const char *token,
    bake_cmd_redir_t *target_out,
    bool *append_out,
    const char **path_inline_out)
{
    if (!token || !target_out || !append_out || !path_inline_out) {
        return false;
    }

    *target_out = BAKE_CMD_REDIR_NONE;
    *append_out = false;
    *path_inline_out = NULL;

    if (!strncmp(token, "2>>", 3)) {
        *target_out = BAKE_CMD_REDIR_STDERR;
        *append_out = true;
        *path_inline_out = token + 3;
        return true;
    }

    if (!strncmp(token, "2>", 2)) {
        *target_out = BAKE_CMD_REDIR_STDERR;
        *path_inline_out = token + 2;
        return true;
    }

    if (!strncmp(token, "1>>", 3)) {
        *target_out = BAKE_CMD_REDIR_STDOUT;
        *append_out = true;
        *path_inline_out = token + 3;
        return true;
    }

    if (!strncmp(token, "1>", 2)) {
        *target_out = BAKE_CMD_REDIR_STDOUT;
        *path_inline_out = token + 2;
        return true;
    }

    if (!strncmp(token, ">>", 2)) {
        *target_out = BAKE_CMD_REDIR_STDOUT;
        *append_out = true;
        *path_inline_out = token + 2;
        return true;
    }

    if (token[0] == '>') {
        *target_out = BAKE_CMD_REDIR_STDOUT;
        *path_inline_out = token + 1;
        return true;
    }

    if (!strncmp(token, "0<", 2)) {
        *target_out = BAKE_CMD_REDIR_STDIN;
        *path_inline_out = token + 2;
        return true;
    }

    if (token[0] == '<') {
        *target_out = BAKE_CMD_REDIR_STDIN;
        *path_inline_out = token + 1;
        return true;
    }

    return false;
}

static int bake_cmd_append_arg(bake_cmd_line_t *cmd, const char *arg) {
    if (!cmd->argv) {
        cmd->argv_cap = 8;
        cmd->argv = ecs_os_calloc_n(char*, cmd->argv_cap);
        if (!cmd->argv) {
            return -1;
        }
    }

    if ((cmd->argc + 2) > cmd->argv_cap) {
        int next = cmd->argv_cap ? cmd->argv_cap * 2 : 8;
        if (next < (cmd->argc + 2)) {
            next = cmd->argc + 2;
        }

        char **next_argv = ecs_os_realloc_n(cmd->argv, char*, next);
        if (!next_argv) {
            return -1;
        }

        memset(
            &next_argv[cmd->argv_cap],
            0,
            (size_t)(next - cmd->argv_cap) * sizeof(char*));
        cmd->argv = next_argv;
        cmd->argv_cap = next;
    }

    cmd->argv[cmd->argc] = ecs_os_strdup(arg);
    if (!cmd->argv[cmd->argc]) {
        return -1;
    }
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return 0;
}

static int bake_cmd_next_token(const char **cursor, char **token_out) {
    const char *p = *cursor;
    while (*p && bake_char_is_space(*p)) {
        p++;
    }

    if (!*p) {
        *cursor = p;
        *token_out = NULL;
        return 0;
    }

    ecs_strbuf_t token = ECS_STRBUF_INIT;
    while (*p && !bake_char_is_space(*p)) {
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1] && quote == '"') {
                    p++;
                }
                ecs_strbuf_appendch(&token, *p++);
            }
            if (*p == quote) {
                p++;
            }
            continue;
        }

        if (*p == '\\' && p[1]) {
            p++;
        }
        ecs_strbuf_appendch(&token, *p++);
    }

    *cursor = p;
    *token_out = ecs_strbuf_get(&token);
    if (!*token_out) {
        return -1;
    }

    return 0;
}

static int bake_parse_command_line(const char *line, bake_cmd_line_t *cmd) {
    memset(cmd, 0, sizeof(*cmd));

    const char *cursor = line;
    bake_cmd_redir_t pending = BAKE_CMD_REDIR_NONE;
    bool pending_append = false;

    while (true) {
        char *token = NULL;
        if (bake_cmd_next_token(&cursor, &token) != 0) {
            bake_cmd_line_fini(cmd);
            return -1;
        }

        if (!token) {
            break;
        }

        if (!token[0]) {
            ecs_os_free(token);
            continue;
        }

        if (pending != BAKE_CMD_REDIR_NONE) {
            int rc = 0;
            if (pending == BAKE_CMD_REDIR_STDIN) {
                rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stdin_path, token);
            } else if (pending == BAKE_CMD_REDIR_STDOUT) {
                rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stdout_path, token);
                cmd->stdio_cfg.stdout_append = pending_append;
            } else {
                rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stderr_path, token);
                cmd->stdio_cfg.stderr_append = pending_append;
            }

            ecs_os_free(token);
            pending = BAKE_CMD_REDIR_NONE;
            pending_append = false;
            if (rc != 0) {
                bake_cmd_line_fini(cmd);
                return -1;
            }
            continue;
        }

        bake_cmd_redir_t target = BAKE_CMD_REDIR_NONE;
        bool append = false;
        const char *inline_path = NULL;
        if (bake_cmd_parse_redirection_token(token, &target, &append, &inline_path)) {
            if (inline_path && inline_path[0]) {
                int rc = 0;
                if (target == BAKE_CMD_REDIR_STDIN) {
                    rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stdin_path, inline_path);
                } else if (target == BAKE_CMD_REDIR_STDOUT) {
                    rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stdout_path, inline_path);
                    cmd->stdio_cfg.stdout_append = append;
                } else {
                    rc = bake_cmd_set_redirect((char**)&cmd->stdio_cfg.stderr_path, inline_path);
                    cmd->stdio_cfg.stderr_append = append;
                }

                ecs_os_free(token);
                if (rc != 0) {
                    bake_cmd_line_fini(cmd);
                    return -1;
                }
                continue;
            }

            pending = target;
            pending_append = append;
            ecs_os_free(token);
            continue;
        }

        if (bake_cmd_append_arg(cmd, token) != 0) {
            ecs_os_free(token);
            bake_cmd_line_fini(cmd);
            return -1;
        }

        ecs_os_free(token);
    }

    if (pending != BAKE_CMD_REDIR_NONE || !cmd->argc) {
        bake_cmd_line_fini(cmd);
        return -1;
    }

    return 0;
}

static int bake_run_command_impl(const char *cmd, bool log_command) {
    if (!cmd || !cmd[0]) {
        return -1;
    }

    bake_cmd_line_t parsed;
    if (bake_parse_command_line(cmd, &parsed) != 0) {
        ecs_err("invalid command line: %s", cmd);
        return -1;
    }

    if (log_command) {
        ecs_trace("$ %s", cmd);
    }

    bake_process_result_t result = {0};
    int rc = bake_proc_run(
        (const char *const*)parsed.argv,
        &parsed.stdio_cfg,
        &result);
    if (rc != 0) {
        ecs_err("failed to start command: %s", cmd);
        bake_cmd_line_fini(&parsed);
        return -1;
    }

    if (result.interrupted) {
        ecs_err("command interrupted: %s", cmd);
        bake_cmd_line_fini(&parsed);
        return -1;
    }

    if (result.term_signal) {
        ecs_err("command terminated by signal %d: %s", result.term_signal, cmd);
        bake_cmd_line_fini(&parsed);
        return -1;
    }

    if (result.exit_code != 0) {
        ecs_err("command failed with exit code %d: %s", result.exit_code, cmd);
        bake_cmd_line_fini(&parsed);
        return -1;
    }

    bake_cmd_line_fini(&parsed);
    return 0;
}

int bake_run_command(const char *cmd) {
    return bake_run_command_impl(cmd, true);
}

int bake_run_command_quiet(const char *cmd) {
    return bake_run_command_impl(cmd, false);
}
