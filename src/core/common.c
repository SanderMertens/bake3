#include "bake/common.h"
#include "bake/os.h"

char* bake_strdup(const char *str) {
    if (!str) {
        return NULL;
    }
    size_t len = strlen(str);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, str, len + 1);
    return out;
}

char* bake_join_path(const char *lhs, const char *rhs) {
    if (!lhs || !lhs[0]) {
        return bake_strdup(rhs);
    }
    if (!rhs || !rhs[0]) {
        return bake_strdup(lhs);
    }

    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    char path_sep = bake_os_path_sep();
    bool has_sep = lhs[lhs_len - 1] == path_sep;

    char *out = ecs_os_malloc(lhs_len + rhs_len + (has_sep ? 1 : 2));
    if (!out) {
        return NULL;
    }

    memcpy(out, lhs, lhs_len);
    out[lhs_len] = '\0';
    if (!has_sep) {
        out[lhs_len] = path_sep;
        out[lhs_len + 1] = '\0';
    }
    strcat(out, rhs);
    return out;
}

char* bake_join3_path(const char *a, const char *b, const char *c) {
    char *ab = bake_join_path(a, b);
    if (!ab) {
        return NULL;
    }
    char *abc = bake_join_path(ab, c);
    ecs_os_free(ab);
    return abc;
}

char* bake_asprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        return NULL;
    }

    char *out = ecs_os_malloc((size_t)needed + 1);
    if (!out) {
        return NULL;
    }

    va_start(args, fmt);
    vsnprintf(out, (size_t)needed + 1, fmt, args);
    va_end(args);

    return out;
}

static const char* bake_host_arch(void) {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

static bool bake_common_path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

static size_t bake_common_trim_path_len(const char *path) {
    size_t len = strlen(path);
    while (len > 0 && bake_common_path_is_sep(path[len - 1])) {
        len--;
    }
    return len;
}

bool bake_path_has_prefix_normalized(const char *path, const char *prefix, size_t *prefix_len_out) {
    if (!path || !prefix) {
        return false;
    }

    size_t prefix_len = bake_common_trim_path_len(prefix);
    if (!prefix_len) {
        return false;
    }

    if (strncmp(path, prefix, prefix_len)) {
        return false;
    }

    if (path[prefix_len] && !bake_common_path_is_sep(path[prefix_len])) {
        return false;
    }

    if (prefix_len_out) {
        *prefix_len_out = prefix_len;
    }
    return true;
}

bool bake_path_equal_normalized(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    size_t lhs_len = bake_common_trim_path_len(lhs);
    size_t rhs_len = bake_common_trim_path_len(rhs);
    if (lhs_len != rhs_len) {
        return false;
    }

    return strncmp(lhs, rhs, lhs_len) == 0;
}

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

char* bake_host_triplet(const char *mode) {
    const char *cfg = mode && mode[0] ? mode : "debug";
    return bake_asprintf("%s-%s-%s", bake_host_arch(), bake_os_host(), cfg);
}

char* bake_host_platform(void) {
    return bake_asprintf("%s-%s", bake_host_arch(), bake_os_host());
}

char* bake_project_build_root(const char *project_path, const char *mode) {
    if (!project_path || !project_path[0]) {
        return NULL;
    }

    char *triplet = bake_host_triplet(mode);
    if (!triplet) {
        return NULL;
    }

    char *bake_dir = bake_join_path(project_path, ".bake");
    if (!bake_dir) {
        ecs_os_free(triplet);
        return NULL;
    }

    char *root = bake_join_path(bake_dir, triplet);
    ecs_os_free(triplet);
    ecs_os_free(bake_dir);
    return root;
}

char* bake_read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    char *buf = ecs_os_malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_len = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_len != (size_t)len) {
        ecs_os_free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (len_out) {
        *len_out = (size_t)len;
    }
    return buf;
}

int bake_write_file(const char *path, const char *content) {
    char *dir = bake_dirname(path);
    if (!dir) {
        return -1;
    }

    if (bake_mkdirs(dir) != 0) {
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

int bake_path_exists(const char *path) {
    return bake_os_path_exists(path);
}

int64_t bake_file_mtime(const char *path) {
    return bake_os_file_mtime(path);
}

int bake_is_dir(const char *path) {
    return bake_os_path_is_dir(path);
}

int bake_mkdirs(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    if (bake_path_exists(path)) {
        return 0;
    }

    char *tmp = bake_strdup(path);
    if (!tmp) {
        return -1;
    }

    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char prev = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && !bake_path_exists(tmp) && bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
                ecs_os_free(tmp);
                return -1;
            }
            tmp[i] = prev;
        }
    }

    if (!bake_path_exists(tmp) && bake_os_mkdir(tmp) != 0 && errno != EEXIST) {
        ecs_os_free(tmp);
        return -1;
    }

    ecs_os_free(tmp);
    return 0;
}

char* bake_dirname(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = bake_os_path_last_sep(path);

    if (!slash) {
        return bake_strdup(".");
    }

    size_t len = (size_t)(slash - path);
    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

char* bake_basename(const char *path) {
    if (!path) {
        return NULL;
    }

    const char *slash = bake_os_path_last_sep(path);

    if (!slash) {
        return bake_strdup(path);
    }

    return bake_strdup(slash + 1);
}

char* bake_stem(const char *path) {
    char *base = bake_basename(path);
    if (!base) {
        return NULL;
    }

    char *dot = strrchr(base, '.');
    if (dot) {
        dot[0] = '\0';
    }

    return base;
}

char* bake_getcwd(void) {
    return bake_os_getcwd();
}

int bake_remove_tree(const char *path) {
    if (!bake_path_exists(path)) {
        return 0;
    }

    if (!bake_is_dir(path)) {
        return remove(path);
    }

    bake_dir_entry_t *entries = NULL;
    int32_t count = 0;
    if (bake_dir_list(path, &entries, &count) != 0) {
        return -1;
    }

    for (int32_t i = 0; i < count; i++) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) {
            continue;
        }
        if (bake_remove_tree(entries[i].path) != 0) {
            bake_dir_entries_free(entries, count);
            return -1;
        }
    }
    bake_dir_entries_free(entries, count);

    return bake_os_rmdir(path);
}

int bake_copy_file(const char *src, const char *dst) {
    size_t len = 0;
    char *content = bake_read_file(src, &len);
    if (!content) {
        return -1;
    }

    char *dir = bake_dirname(dst);
    if (!dir || bake_mkdirs(dir) != 0) {
        ecs_os_free(content);
        ecs_os_free(dir);
        return -1;
    }
    ecs_os_free(dir);

    FILE *f = fopen(dst, "wb");
    if (!f) {
        ecs_os_free(content);
        return -1;
    }

    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    ecs_os_free(content);
    return written == len ? 0 : -1;
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

    char *str = bake_strdup(path);
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

    cmd->argv[cmd->argc] = bake_strdup(arg);
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
