#include "bake2/environment.h"

#include "../config/jsmn.h"

static char* b2_get_home(void) {
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0]) {
        home = getenv("USERPROFILE");
    }
#endif
    return home ? b2_strdup(home) : NULL;
}

int b2_environment_init_paths(b2_context_t *ctx) {
    const char *env_home = getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        ctx->bake_home = b2_strdup(env_home);
    } else {
        char *home = b2_get_home();
        if (!home) {
            return -1;
        }
        ctx->bake_home = b2_join_path(home, "bake");
        ecs_os_free(home);
    }

    if (!ctx->bake_home) {
        return -1;
    }

    if (b2_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    ctx->env_path = b2_join_path(ctx->bake_home, "bake2_env.json");
    if (!ctx->env_path) {
        return -1;
    }

    return 0;
}

static int b2_json_skip(const jsmntok_t *toks, int count, int index) {
    if (index < 0 || index >= count) {
        return index + 1;
    }

    int next = index + 1;
    if (toks[index].type == JSMN_OBJECT || toks[index].type == JSMN_ARRAY) {
        for (int i = 0; i < toks[index].size; i++) {
            next = b2_json_skip(toks, count, next);
        }
    }

    return next;
}

static int b2_json_eq(const char *json, const jsmntok_t *tok, const char *str) {
    size_t len = strlen(str);
    size_t tok_len = (size_t)(tok->end - tok->start);
    return tok_len == len && strncmp(json + tok->start, str, len) == 0;
}

static char* b2_json_strdup(const char *json, const jsmntok_t *tok) {
    size_t len = (size_t)(tok->end - tok->start);
    char *str = ecs_os_malloc(len + 1);
    if (!str) {
        return NULL;
    }
    memcpy(str, json + tok->start, len);
    str[len] = '\0';
    return str;
}

static int b2_json_find_object_key(const char *json, const jsmntok_t *toks, int count, int object, const char *key) {
    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    int i = object + 1;
    int end = b2_json_skip(toks, count, object);
    while (i < end) {
        int key_tok = i;
        int val_tok = key_tok + 1;
        if (toks[key_tok].type == JSMN_STRING && b2_json_eq(json, &toks[key_tok], key)) {
            return val_tok;
        }
        i = b2_json_skip(toks, count, val_tok);
    }

    return -1;
}

static void b2_add_env_project_entity(ecs_world_t *world, const char *id, const char *path, const char *kind) {
    ecs_entity_t e = ecs_entity(world, {
        .name = id,
        .sep = "::"
    });

    b2_env_project_t value = {
        .id = b2_strdup(id),
        .path = b2_strdup(path),
        .kind = b2_strdup(kind)
    };

    ecs_set_ptr(world, e, b2_env_project_t, &value);
}

int b2_environment_load(b2_context_t *ctx) {
    if (!b2_path_exists(ctx->env_path)) {
        return 0;
    }

    size_t len = 0;
    char *json = b2_read_file(ctx->env_path, &len);
    if (!json) {
        return -1;
    }

    int token_cap = 256;
    jsmntok_t *tokens = NULL;
    int parsed = JSMN_ERROR_NOMEM;
    while (parsed == JSMN_ERROR_NOMEM) {
        token_cap *= 2;
        ecs_os_free(tokens);
        tokens = ecs_os_calloc_n(jsmntok_t, token_cap);
        if (!tokens) {
            ecs_os_free(json);
            return -1;
        }
        jsmn_parser p;
        jsmn_init(&p);
        parsed = jsmn_parse(&p, json, len, tokens, (unsigned int)token_cap);
    }

    if (parsed < 1 || tokens[0].type != JSMN_OBJECT) {
        ecs_os_free(tokens);
        ecs_os_free(json);
        return -1;
    }

    int projects_tok = b2_json_find_object_key(json, tokens, parsed, 0, "projects");
    if (projects_tok >= 0 && tokens[projects_tok].type == JSMN_ARRAY) {
        int i = projects_tok + 1;
        for (int32_t n = 0; n < tokens[projects_tok].size; n++) {
            if (tokens[i].type == JSMN_OBJECT) {
                int id_tok = b2_json_find_object_key(json, tokens, parsed, i, "id");
                int path_tok = b2_json_find_object_key(json, tokens, parsed, i, "path");
                int kind_tok = b2_json_find_object_key(json, tokens, parsed, i, "kind");

                if (id_tok >= 0 && path_tok >= 0 && kind_tok >= 0) {
                    char *id = b2_json_strdup(json, &tokens[id_tok]);
                    char *path = b2_json_strdup(json, &tokens[path_tok]);
                    char *kind = b2_json_strdup(json, &tokens[kind_tok]);
                    if (id && path && kind) {
                        b2_add_env_project_entity(ctx->world, id, path, kind);
                    }
                    ecs_os_free(id);
                    ecs_os_free(path);
                    ecs_os_free(kind);
                }
            }
            i = b2_json_skip(tokens, parsed, i);
        }
    }

    ecs_os_free(tokens);
    ecs_os_free(json);
    return 0;
}

int b2_environment_save(b2_context_t *ctx) {
    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "{\n  \"projects\": [\n");

    int32_t count = 0;
    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(b2_project_t));
    while (ecs_each_next(&it)) {
        const b2_project_t *projects = ecs_field(&it, b2_project_t, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const b2_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || !cfg->id || !cfg->path) {
                continue;
            }

            if (count) {
                ecs_strbuf_appendstr(&buf, ",\n");
            }

            ecs_strbuf_append(&buf,
                "    {\"id\":\"%s\",\"path\":\"%s\",\"kind\":\"%s\"}",
                cfg->id, cfg->path, b2_project_kind_str(cfg->kind));
            count++;
        }
    }

    ecs_strbuf_appendstr(&buf, "\n  ]\n}\n");
    char *json = ecs_strbuf_get(&buf);

    int rc = b2_write_file(ctx->env_path, json);
    ecs_os_free(json);
    return rc;
}

int b2_environment_setup(b2_context_t *ctx, const char *argv0) {
    char *bin = b2_join_path(ctx->bake_home, "bin");
    if (!bin) {
        return -1;
    }

    if (b2_mkdirs(bin) != 0) {
        ecs_os_free(bin);
        return -1;
    }

#if defined(_WIN32)
    const char *target_name = "bake.exe";
#else
    const char *target_name = "bake";
#endif

    char *dst = b2_join_path(bin, target_name);
    ecs_os_free(bin);
    if (!dst) {
        return -1;
    }

    char *src = NULL;
    if (argv0 && b2_path_exists(argv0)) {
        src = b2_strdup(argv0);
    } else {
        char *cwd = b2_getcwd();
        if (cwd) {
            src = b2_join_path(cwd, argv0);
            ecs_os_free(cwd);
        }
    }

    if (!src) {
        ecs_os_free(dst);
        return -1;
    }

    int rc = b2_copy_file(src, dst);
    if (rc == 0) {
        B2_LOG("installed bake to %s", dst);
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}
