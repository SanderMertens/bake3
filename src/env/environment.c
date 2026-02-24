#include "bake2/environment.h"

#include "../config/jsmn.h"

static char* bake_get_home(void) {
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !home[0]) {
        home = getenv("USERPROFILE");
    }
#endif
    return home ? bake_strdup(home) : NULL;
}

int bake_environment_init_paths(bake_context_t *ctx) {
    const char *env_home = getenv("BAKE_HOME");
    if (env_home && env_home[0]) {
        ctx->bake_home = bake_strdup(env_home);
    } else {
        char *home = bake_get_home();
        if (!home) {
            return -1;
        }
        ctx->bake_home = bake_join_path(home, "bake");
        ecs_os_free(home);
    }

    if (!ctx->bake_home) {
        return -1;
    }

    if (bake_mkdirs(ctx->bake_home) != 0) {
        return -1;
    }

    ctx->env_path = bake_join_path(ctx->bake_home, "bake2_env.json");
    if (!ctx->env_path) {
        return -1;
    }

    return 0;
}

static int bake_json_skip(const jsmntok_t *toks, int count, int index) {
    if (index < 0 || index >= count) {
        return index + 1;
    }

    int next = index + 1;
    if (toks[index].type == JSMN_OBJECT || toks[index].type == JSMN_ARRAY) {
        for (int i = 0; i < toks[index].size; i++) {
            next = bake_json_skip(toks, count, next);
        }
    }

    return next;
}

static int bake_json_eq(const char *json, const jsmntok_t *tok, const char *str) {
    size_t len = strlen(str);
    size_t tok_len = (size_t)(tok->end - tok->start);
    return tok_len == len && strncmp(json + tok->start, str, len) == 0;
}

static char* bake_json_strdup(const char *json, const jsmntok_t *tok) {
    size_t len = (size_t)(tok->end - tok->start);
    char *str = ecs_os_malloc(len + 1);
    if (!str) {
        return NULL;
    }
    memcpy(str, json + tok->start, len);
    str[len] = '\0';
    return str;
}

static int bake_json_find_object_key(const char *json, const jsmntok_t *toks, int count, int object, const char *key) {
    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    int i = object + 1;
    int end = bake_json_skip(toks, count, object);
    while (i < end) {
        int key_tok = i;
        int val_tok = key_tok + 1;
        if (toks[key_tok].type == JSMN_STRING && bake_json_eq(json, &toks[key_tok], key)) {
            return val_tok;
        }
        i = bake_json_skip(toks, count, val_tok);
    }

    return -1;
}

static ecs_entity_t bake_env_project_ensure_entity(ecs_world_t *world, const char *id) {
    ecs_entity_t entity = ecs_lookup_path_w_sep(world, 0, id, "::", NULL, false);
    if (entity) {
        return entity;
    }

    ecs_entity_desc_t desc = {0};
    desc.name = id;
    desc.sep = "::";
    return ecs_entity_init(world, &desc);
}

static void bake_env_project_upsert(ecs_world_t *world, const char *id, const char *path, const char *kind) {
    ecs_entity_t e = bake_env_project_ensure_entity(world, id);
    const BakeEnvProject *existing = ecs_get(world, e, BakeEnvProject);
    if (existing) {
        ecs_os_free((char*)existing->id);
        ecs_os_free((char*)existing->path);
        ecs_os_free((char*)existing->kind);
    }

    BakeEnvProject value = {
        .id = bake_strdup(id),
        .path = bake_strdup(path),
        .kind = bake_strdup(kind)
    };

    ecs_set_ptr(world, e, BakeEnvProject, &value);
}

const BakeEnvProject* bake_environment_find_project(const ecs_world_t *world, const char *id, ecs_entity_t *entity_out) {
    ecs_entity_t e = ecs_lookup_path_w_sep(world, 0, id, "::", NULL, false);
    if (e) {
        const BakeEnvProject *project = ecs_get(world, e, BakeEnvProject);
        if (project) {
            if (entity_out) {
                *entity_out = e;
            }
            return project;
        }
    }

    ecs_iter_t it = ecs_each_id(world, ecs_id(BakeEnvProject));
    while (ecs_each_next(&it)) {
        const BakeEnvProject *projects = ecs_field(&it, BakeEnvProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            if (!projects[i].id) {
                continue;
            }
            if (!strcmp(projects[i].id, id)) {
                if (entity_out) {
                    *entity_out = it.entities[i];
                }
                return &projects[i];
            }
        }
    }

    return NULL;
}

static int bake_env_project_is_stale(const BakeEnvProject *project) {
    if (!project || !project->path || !project->path[0]) {
        return 1;
    }

    if (!bake_path_exists(project->path) || !bake_is_dir(project->path)) {
        return 1;
    }

    char *project_json = bake_join_path(project->path, "project.json");
    if (!project_json) {
        return 1;
    }

    int stale = !bake_path_exists(project_json);
    ecs_os_free(project_json);
    return stale;
}

int bake_environment_load(bake_context_t *ctx) {
    if (!bake_path_exists(ctx->env_path)) {
        return 0;
    }

    size_t len = 0;
    char *json = bake_read_file(ctx->env_path, &len);
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

    int projects_tok = bake_json_find_object_key(json, tokens, parsed, 0, "projects");
    if (projects_tok >= 0 && tokens[projects_tok].type == JSMN_ARRAY) {
        int i = projects_tok + 1;
        for (int32_t n = 0; n < tokens[projects_tok].size; n++) {
            if (tokens[i].type == JSMN_OBJECT) {
                int id_tok = bake_json_find_object_key(json, tokens, parsed, i, "id");
                int path_tok = bake_json_find_object_key(json, tokens, parsed, i, "path");
                int kind_tok = bake_json_find_object_key(json, tokens, parsed, i, "kind");

                if (id_tok >= 0 && path_tok >= 0 && kind_tok >= 0) {
                    char *id = bake_json_strdup(json, &tokens[id_tok]);
                    char *path = bake_json_strdup(json, &tokens[path_tok]);
                    char *kind = bake_json_strdup(json, &tokens[kind_tok]);
                    if (id && path && kind) {
                        bake_env_project_upsert(ctx->world, id, path, kind);
                    }
                    ecs_os_free(id);
                    ecs_os_free(path);
                    ecs_os_free(kind);
                }
            }
            i = bake_json_skip(tokens, parsed, i);
        }
    }

    ecs_os_free(tokens);
    ecs_os_free(json);
    return 0;
}

static void bake_environment_sync_from_projects(bake_context_t *ctx) {
    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeProject));
    while (ecs_each_next(&it)) {
        const BakeProject *projects = ecs_field(&it, BakeProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const bake_project_cfg_t *cfg = projects[i].cfg;
            if (!cfg || !cfg->id || !cfg->path) {
                continue;
            }

            bake_env_project_upsert(
                ctx->world,
                cfg->id,
                cfg->path,
                bake_project_kind_str(cfg->kind));
        }
    }
}

int bake_environment_save(bake_context_t *ctx) {
    bake_environment_sync_from_projects(ctx);

    ecs_strbuf_t buf = ECS_STRBUF_INIT;
    ecs_strbuf_appendstr(&buf, "{\n  \"projects\": [\n");

    int32_t count = 0;
    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeEnvProject));
    while (ecs_each_next(&it)) {
        const BakeEnvProject *projects = ecs_field(&it, BakeEnvProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            if (!projects[i].id || !projects[i].path || !projects[i].kind) {
                continue;
            }

            if (count) {
                ecs_strbuf_appendstr(&buf, ",\n");
            }

            ecs_strbuf_append(&buf,
                "    {\"id\":\"%s\",\"path\":\"%s\",\"kind\":\"%s\"}",
                projects[i].id, projects[i].path, projects[i].kind);
            count++;
        }
    }

    ecs_strbuf_appendstr(&buf, "\n  ]\n}\n");
    char *json = ecs_strbuf_get(&buf);

    int rc = bake_write_file(ctx->env_path, json);
    ecs_os_free(json);
    return rc;
}

int bake_environment_import_projects(bake_context_t *ctx) {
    int imported = 0;

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeEnvProject));
    while (ecs_each_next(&it)) {
        const BakeEnvProject *projects = ecs_field(&it, BakeEnvProject, 0);
        for (int32_t i = 0; i < it.count; i++) {
            const BakeEnvProject *env_project = &projects[i];
            if (bake_env_project_is_stale(env_project)) {
                continue;
            }

            ecs_entity_t existing_entity = 0;
            const BakeProject *existing = bake_model_find_project(
                ctx->world, env_project->id, &existing_entity);
            if (existing && existing->cfg && existing->cfg->path) {
                continue;
            }

            if (env_project->path && env_project->path[0]) {
                const BakeProject *path_hit = bake_model_find_project_by_path(
                    ctx->world, env_project->path, NULL);
                if (path_hit) {
                    continue;
                }
            }

            char *project_json = bake_join_path(env_project->path, "project.json");
            if (!project_json) {
                continue;
            }

            bake_project_cfg_t *cfg = ecs_os_calloc_t(bake_project_cfg_t);
            if (!cfg) {
                ecs_os_free(project_json);
                return -1;
            }

            bake_project_cfg_init(cfg);
            if (bake_project_cfg_load_file(project_json, cfg) != 0) {
                bake_project_cfg_fini(cfg);
                ecs_os_free(cfg);
                ecs_os_free(project_json);
                continue;
            }

            ecs_os_free(cfg->id);
            cfg->id = bake_strdup(env_project->id);
            if (!cfg->id) {
                bake_project_cfg_fini(cfg);
                ecs_os_free(cfg);
                ecs_os_free(project_json);
                return -1;
            }

            bake_model_add_project(ctx->world, cfg, true);
            imported++;
            ecs_os_free(project_json);
        }
    }

    return imported;
}

int bake_environment_reset(bake_context_t *ctx) {
    ecs_entity_t *entities = NULL;
    int32_t count = 0;
    int32_t capacity = 16;
    entities = ecs_os_malloc_n(ecs_entity_t, capacity);
    if (!entities) {
        return -1;
    }

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeEnvProject));
    while (ecs_each_next(&it)) {
        if (count + it.count > capacity) {
            int32_t next = capacity;
            while (count + it.count > next) {
                next *= 2;
            }
            ecs_entity_t *tmp = ecs_os_realloc_n(entities, ecs_entity_t, next);
            if (!tmp) {
                ecs_os_free(entities);
                return -1;
            }
            entities = tmp;
            capacity = next;
        }
        for (int32_t i = 0; i < it.count; i++) {
            entities[count++] = it.entities[i];
        }
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeEnvProject *project = ecs_get(ctx->world, entities[i], BakeEnvProject);
        if (project) {
            ecs_os_free((char*)project->id);
            ecs_os_free((char*)project->path);
            ecs_os_free((char*)project->kind);
            ecs_remove(ctx->world, entities[i], BakeEnvProject);
        }
    }
    ecs_os_free(entities);

    if (bake_path_exists(ctx->env_path)) {
        if (remove(ctx->env_path) != 0) {
            return -1;
        }
    }

    return 0;
}

int bake_environment_cleanup(bake_context_t *ctx, int32_t *removed_out) {
    int32_t removed = 0;
    ecs_entity_t *entities = NULL;
    int32_t count = 0;
    int32_t capacity = 16;
    entities = ecs_os_malloc_n(ecs_entity_t, capacity);
    if (!entities) {
        return -1;
    }

    ecs_iter_t it = ecs_each_id(ctx->world, ecs_id(BakeEnvProject));
    while (ecs_each_next(&it)) {
        const BakeEnvProject *projects = ecs_field(&it, BakeEnvProject, 0);
        if (count + it.count > capacity) {
            int32_t next = capacity;
            while (count + it.count > next) {
                next *= 2;
            }
            ecs_entity_t *tmp = ecs_os_realloc_n(entities, ecs_entity_t, next);
            if (!tmp) {
                ecs_os_free(entities);
                return -1;
            }
            entities = tmp;
            capacity = next;
        }
        for (int32_t i = 0; i < it.count; i++) {
            if (!bake_env_project_is_stale(&projects[i])) {
                continue;
            }
            entities[count++] = it.entities[i];
        }
    }

    for (int32_t i = 0; i < count; i++) {
        const BakeEnvProject *project = ecs_get(ctx->world, entities[i], BakeEnvProject);
        if (!project) {
            continue;
        }
        ecs_os_free((char*)project->id);
        ecs_os_free((char*)project->path);
        ecs_os_free((char*)project->kind);
        ecs_remove(ctx->world, entities[i], BakeEnvProject);
        removed++;
    }
    ecs_os_free(entities);

    if (bake_environment_save(ctx) != 0) {
        return -1;
    }

    if (removed_out) {
        *removed_out = removed;
    }
    return 0;
}

int bake_environment_setup(bake_context_t *ctx, const char *argv0) {
    char *bin = bake_join_path(ctx->bake_home, "bin");
    if (!bin) {
        return -1;
    }

    if (bake_mkdirs(bin) != 0) {
        ecs_os_free(bin);
        return -1;
    }

#if defined(_WIN32)
    const char *target_name = "bake.exe";
#else
    const char *target_name = "bake";
#endif

    char *dst = bake_join_path(bin, target_name);
    ecs_os_free(bin);
    if (!dst) {
        return -1;
    }

    char *src = NULL;
    if (argv0 && bake_path_exists(argv0)) {
        src = bake_strdup(argv0);
    } else {
        char *cwd = bake_getcwd();
        if (cwd) {
            src = bake_join_path(cwd, argv0);
            ecs_os_free(cwd);
        }
    }

    if (!src) {
        ecs_os_free(dst);
        return -1;
    }

    int rc = bake_copy_file(src, dst);
    if (rc == 0) {
        BAKE_LOG("installed bake to %s", dst);
    }

    ecs_os_free(src);
    ecs_os_free(dst);
    return rc;
}
