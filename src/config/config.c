#include "bake2/config.h"
#include "bake2/common.h"

#include <ctype.h>

#include "jsmn.h"

static int bake_json_skip(const jsmntok_t *toks, int count, int index) {
    if (index < 0 || index >= count) {
        return index + 1;
    }

    int next = index + 1;
    const jsmntok_t *tok = &toks[index];

    if (tok->type == JSMN_OBJECT) {
        for (int i = 0; i < tok->size; i++) {
            next = bake_json_skip(toks, count, next);
        }
    } else if (tok->type == JSMN_ARRAY) {
        for (int i = 0; i < tok->size; i++) {
            next = bake_json_skip(toks, count, next);
        }
    }

    return next;
}

static int bake_json_eq(const char *json, const jsmntok_t *tok, const char *str) {
    size_t len = strlen(str);
    size_t tok_len = (size_t)(tok->end - tok->start);
    if (tok_len != len) {
        return 0;
    }
    return strncmp(json + tok->start, str, len) == 0;
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

static char* bake_json_strip_comments(const char *json, size_t len, size_t *len_out) {
    if (!json) {
        return NULL;
    }

    char *out = ecs_os_malloc(len + 1);
    if (!out) {
        return NULL;
    }

    size_t r = 0;
    size_t w = 0;
    bool in_string = false;
    bool escaped = false;

    while (r < len) {
        char ch = json[r];

        if (in_string) {
            out[w++] = ch;
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            r++;
            continue;
        }

        if (ch == '"') {
            in_string = true;
            out[w++] = ch;
            r++;
            continue;
        }

        if (ch == '/' && (r + 1) < len) {
            char next = json[r + 1];

            if (next == '/') {
                r += 2;
                while (r < len && json[r] != '\n') {
                    r++;
                }
                continue;
            }

            if (next == '*') {
                r += 2;
                while ((r + 1) < len) {
                    if (json[r] == '\n') {
                        out[w++] = '\n';
                    }
                    if (json[r] == '*' && json[r + 1] == '/') {
                        r += 2;
                        break;
                    }
                    r++;
                }
                continue;
            }
        }

        out[w++] = ch;
        r++;
    }

    out[w] = '\0';
    if (len_out) {
        *len_out = w;
    }
    return out;
}

static int bake_json_find_object_key(const char *json, const jsmntok_t *toks, int count, int object, const char *key) {
    if (object < 0 || object >= count || toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    int i = object + 1;
    int end = bake_json_skip(toks, count, object);
    while (i < end) {
        int key_tok = i;
        int val_tok = key_tok + 1;
        if (val_tok >= end) {
            break;
        }

        if (toks[key_tok].type == JSMN_STRING && bake_json_eq(json, &toks[key_tok], key)) {
            return val_tok;
        }

        i = bake_json_skip(toks, count, val_tok);
    }

    return -1;
}

static int bake_json_parse_bool(const char *json, const jsmntok_t *tok, bool *value_out) {
    if (tok->type != JSMN_PRIMITIVE) {
        return -1;
    }

    if (bake_json_eq(json, tok, "true")) {
        *value_out = true;
        return 0;
    }
    if (bake_json_eq(json, tok, "false")) {
        *value_out = false;
        return 0;
    }

    return -1;
}

static int bake_json_parse_strlist(const char *json, const jsmntok_t *toks, int count, int token, bake_strlist_t *list) {
    if (token < 0 || token >= count || toks[token].type != JSMN_ARRAY) {
        return -1;
    }

    int i = token + 1;
    for (int32_t n = 0; n < toks[token].size; n++) {
        if (toks[i].type != JSMN_STRING && toks[i].type != JSMN_PRIMITIVE) {
            return -1;
        }

        char *value = bake_json_strdup(json, &toks[i]);
        if (!value) {
            return -1;
        }
        if (bake_strlist_append_owned(list, value) != 0) {
            ecs_os_free(value);
            return -1;
        }

        i = bake_json_skip(toks, count, i);
    }

    return 0;
}

static int bake_json_get_string(const char *json, const jsmntok_t *toks, int count, int object, const char *key, char **out) {
    int tok = bake_json_find_object_key(json, toks, count, object, key);
    if (tok < 0) {
        return 1;
    }

    if (toks[tok].type != JSMN_STRING && toks[tok].type != JSMN_PRIMITIVE) {
        return -1;
    }

    char *str = bake_json_strdup(json, &toks[tok]);
    if (!str) {
        return -1;
    }

    ecs_os_free(*out);
    *out = str;
    return 0;
}

static int bake_json_get_bool(const char *json, const jsmntok_t *toks, int count, int object, const char *key, bool *out) {
    int tok = bake_json_find_object_key(json, toks, count, object, key);
    if (tok < 0) {
        return 1;
    }
    return bake_json_parse_bool(json, &toks[tok], out);
}

static int bake_json_get_array(const char *json, const jsmntok_t *toks, int count, int object, const char *key, bake_strlist_t *out) {
    int tok = bake_json_find_object_key(json, toks, count, object, key);
    if (tok < 0) {
        return 1;
    }
    return bake_json_parse_strlist(json, toks, count, tok, out);
}

static int bake_json_get_array_alias(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    const char *key,
    const char *alias,
    bake_strlist_t *out)
{
    int rc = bake_json_get_array(json, toks, count, object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_array(json, toks, count, object, alias, out);
    }
    return rc;
}

const char* bake_project_kind_str(bake_project_kind_t kind) {
    switch (kind) {
    case B2_PROJECT_APPLICATION: return "application";
    case B2_PROJECT_PACKAGE: return "package";
    case B2_PROJECT_CONFIG: return "config";
    case B2_PROJECT_TEST: return "test";
    case B2_PROJECT_TEMPLATE: return "template";
    default: return "application";
    }
}

bake_project_kind_t bake_project_kind_parse(const char *value) {
    if (!value) {
        return B2_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "application") || !strcmp(value, "app")) {
        return B2_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "package") || !strcmp(value, "lib")) {
        return B2_PROJECT_PACKAGE;
    }
    if (!strcmp(value, "config")) {
        return B2_PROJECT_CONFIG;
    }
    if (!strcmp(value, "test")) {
        return B2_PROJECT_TEST;
    }
    if (!strcmp(value, "template")) {
        return B2_PROJECT_TEMPLATE;
    }
    return B2_PROJECT_APPLICATION;
}

void bake_rule_list_init(bake_rule_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void bake_rule_list_fini(bake_rule_list_t *list) {
    for (int32_t i = 0; i < list->count; i++) {
        ecs_os_free(list->items[i].ext);
        ecs_os_free(list->items[i].command);
    }
    ecs_os_free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int bake_rule_list_append(bake_rule_list_t *list, const char *ext, const char *command) {
    if (list->count == list->capacity) {
        int32_t next = list->capacity ? list->capacity * 2 : 8;
        bake_rule_t *rules = ecs_os_realloc_n(list->items, bake_rule_t, next);
        if (!rules) {
            return -1;
        }
        list->items = rules;
        list->capacity = next;
    }

    bake_rule_t *rule = &list->items[list->count++];
    rule->ext = bake_strdup(ext);
    rule->command = bake_strdup(command);

    return (rule->ext && rule->command) ? 0 : -1;
}

void bake_lang_cfg_init(bake_lang_cfg_t *cfg) {
    bake_strlist_init(&cfg->cflags);
    bake_strlist_init(&cfg->cxxflags);
    bake_strlist_init(&cfg->defines);
    bake_strlist_init(&cfg->ldflags);
    bake_strlist_init(&cfg->libs);
    bake_strlist_init(&cfg->static_libs);
    bake_strlist_init(&cfg->libpaths);
    bake_strlist_init(&cfg->links);
    bake_strlist_init(&cfg->include_paths);
    cfg->c_standard = bake_strdup("c99");
    cfg->cpp_standard = bake_strdup("c++17");
    cfg->static_lib = false;
    cfg->export_symbols = false;
    cfg->precompile_header = true;
}

void bake_lang_cfg_fini(bake_lang_cfg_t *cfg) {
    bake_strlist_fini(&cfg->cflags);
    bake_strlist_fini(&cfg->cxxflags);
    bake_strlist_fini(&cfg->defines);
    bake_strlist_fini(&cfg->ldflags);
    bake_strlist_fini(&cfg->libs);
    bake_strlist_fini(&cfg->static_libs);
    bake_strlist_fini(&cfg->libpaths);
    bake_strlist_fini(&cfg->links);
    bake_strlist_fini(&cfg->include_paths);
    ecs_os_free(cfg->c_standard);
    ecs_os_free(cfg->cpp_standard);
    cfg->c_standard = NULL;
    cfg->cpp_standard = NULL;
}

void bake_dependee_cfg_init(bake_dependee_cfg_t *cfg) {
    bake_strlist_init(&cfg->use);
    bake_strlist_init(&cfg->defines);
    bake_strlist_init(&cfg->include_paths);
    bake_strlist_init(&cfg->cflags);
    bake_strlist_init(&cfg->ldflags);
    bake_strlist_init(&cfg->libs);
}

void bake_dependee_cfg_fini(bake_dependee_cfg_t *cfg) {
    bake_strlist_fini(&cfg->use);
    bake_strlist_fini(&cfg->defines);
    bake_strlist_fini(&cfg->include_paths);
    bake_strlist_fini(&cfg->cflags);
    bake_strlist_fini(&cfg->ldflags);
    bake_strlist_fini(&cfg->libs);
}

void bake_project_cfg_init(bake_project_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = B2_PROJECT_APPLICATION;
    cfg->has_test_spec = false;
    cfg->language = bake_strdup("c");

    bake_strlist_init(&cfg->use);
    bake_strlist_init(&cfg->use_private);
    bake_strlist_init(&cfg->use_build);
    bake_strlist_init(&cfg->use_runtime);
    bake_strlist_init(&cfg->drivers);
    bake_strlist_init(&cfg->plugins);

    bake_dependee_cfg_init(&cfg->dependee);
    bake_lang_cfg_init(&cfg->c_lang);
    bake_lang_cfg_init(&cfg->cpp_lang);
    bake_rule_list_init(&cfg->rules);
}

void bake_project_cfg_fini(bake_project_cfg_t *cfg) {
    ecs_os_free(cfg->id);
    ecs_os_free(cfg->path);
    ecs_os_free(cfg->language);
    ecs_os_free(cfg->output_name);

    bake_strlist_fini(&cfg->use);
    bake_strlist_fini(&cfg->use_private);
    bake_strlist_fini(&cfg->use_build);
    bake_strlist_fini(&cfg->use_runtime);
    bake_strlist_fini(&cfg->drivers);
    bake_strlist_fini(&cfg->plugins);

    bake_dependee_cfg_fini(&cfg->dependee);
    bake_lang_cfg_fini(&cfg->c_lang);
    bake_lang_cfg_fini(&cfg->cpp_lang);
    bake_rule_list_fini(&cfg->rules);

    memset(cfg, 0, sizeof(*cfg));
}

void bake_build_mode_init(bake_build_mode_t *mode, const char *id) {
    mode->id = id;
    bake_strlist_init(&mode->cflags);
    bake_strlist_init(&mode->cxxflags);
    bake_strlist_init(&mode->ldflags);
}

void bake_build_mode_fini(bake_build_mode_t *mode) {
    bake_strlist_fini(&mode->cflags);
    bake_strlist_fini(&mode->cxxflags);
    bake_strlist_fini(&mode->ldflags);
}

static int bake_parse_lang_cfg(const char *json, const jsmntok_t *toks, int count, int object, bake_lang_cfg_t *cfg) {
    if (object < 0) {
        return 0;
    }

    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    if (bake_json_get_array(json, toks, count, object, "cflags", &cfg->cflags) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "cxxflags", &cfg->cxxflags) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "defines", &cfg->defines) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "ldflags", &cfg->ldflags) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "lib", &cfg->libs) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "static-lib", &cfg->static_libs) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "libpath", &cfg->libpaths) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "link", &cfg->links) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "include", &cfg->include_paths) < 0) return -1;

    if (bake_json_get_string(json, toks, count, object, "c-standard", &cfg->c_standard) < 0) return -1;
    if (bake_json_get_string(json, toks, count, object, "cpp-standard", &cfg->cpp_standard) < 0) return -1;

    if (bake_json_get_bool(json, toks, count, object, "static", &cfg->static_lib) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "export-symbols", &cfg->export_symbols) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "precompile-header", &cfg->precompile_header) < 0) return -1;

    return 0;
}

static int bake_strlist_merge_unique(bake_strlist_t *dst, const bake_strlist_t *src) {
    for (int32_t i = 0; i < src->count; i++) {
        if (!bake_strlist_contains(dst, src->items[i])) {
            if (bake_strlist_append(dst, src->items[i]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int bake_parse_dependee_value_cfg(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_dependee_cfg_t *cfg)
{
    if (object < 0) {
        return 0;
    }

    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    if (bake_json_get_array(json, toks, count, object, "use", &cfg->use) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "defines", &cfg->defines) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "include", &cfg->include_paths) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "cflags", &cfg->cflags) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "ldflags", &cfg->ldflags) < 0) return -1;
    if (bake_json_get_array(json, toks, count, object, "lib", &cfg->libs) < 0) return -1;

    return 0;
}

static int bake_parse_dependee_lang_cfg(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_dependee_cfg_t *cfg)
{
    if (object < 0) {
        return 0;
    }

    bake_lang_cfg_t lang_cfg;
    bake_lang_cfg_init(&lang_cfg);

    int rc = bake_parse_lang_cfg(json, toks, count, object, &lang_cfg);
    if (rc == 0) {
        if (bake_strlist_merge_unique(&cfg->defines, &lang_cfg.defines) != 0 ||
            bake_strlist_merge_unique(&cfg->include_paths, &lang_cfg.include_paths) != 0 ||
            bake_strlist_merge_unique(&cfg->cflags, &lang_cfg.cflags) != 0 ||
            bake_strlist_merge_unique(&cfg->ldflags, &lang_cfg.ldflags) != 0 ||
            bake_strlist_merge_unique(&cfg->libs, &lang_cfg.libs) != 0)
        {
            rc = -1;
        }
    }

    bake_lang_cfg_fini(&lang_cfg);
    return rc;
}

static int bake_parse_dependee_cfg(const char *json, const jsmntok_t *toks, int count, int object, bake_dependee_cfg_t *cfg) {
    if (object < 0) {
        return 0;
    }

    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    if (bake_parse_dependee_value_cfg(json, toks, count, object, cfg) != 0) {
        return -1;
    }

    int value_obj = bake_json_find_object_key(json, toks, count, object, "value");
    if (bake_parse_dependee_value_cfg(json, toks, count, value_obj, cfg) != 0) {
        return -1;
    }

    int c_obj = bake_json_find_object_key(json, toks, count, object, "lang.c");
    if (bake_parse_dependee_lang_cfg(json, toks, count, c_obj, cfg) != 0) {
        return -1;
    }

    int cpp_obj = bake_json_find_object_key(json, toks, count, object, "lang.cpp");
    if (bake_parse_dependee_lang_cfg(json, toks, count, cpp_obj, cfg) != 0) {
        return -1;
    }

    return 0;
}

static int bake_parse_rules(const char *json, const jsmntok_t *toks, int count, int rules_tok, bake_rule_list_t *rules) {
    if (rules_tok < 0) {
        return 0;
    }

    if (toks[rules_tok].type != JSMN_ARRAY) {
        return -1;
    }

    int i = rules_tok + 1;
    for (int32_t n = 0; n < toks[rules_tok].size; n++) {
        if (toks[i].type != JSMN_OBJECT) {
            return -1;
        }

        int ext_tok = bake_json_find_object_key(json, toks, count, i, "ext");
        int cmd_tok = bake_json_find_object_key(json, toks, count, i, "command");

        if (ext_tok >= 0 && cmd_tok >= 0 && toks[ext_tok].type == JSMN_STRING && toks[cmd_tok].type == JSMN_STRING) {
            char *ext = bake_json_strdup(json, &toks[ext_tok]);
            char *cmd = bake_json_strdup(json, &toks[cmd_tok]);
            if (!ext || !cmd || bake_rule_list_append(rules, ext, cmd) != 0) {
                ecs_os_free(ext);
                ecs_os_free(cmd);
                return -1;
            }
            ecs_os_free(ext);
            ecs_os_free(cmd);
        }

        i = bake_json_skip(toks, count, i);
    }

    return 0;
}

static int bake_project_cfg_finalize_defaults(const char *project_json_path, bake_project_cfg_t *cfg) {
    if (!cfg->path) {
        cfg->path = bake_dirname(project_json_path);
    }

    if (!cfg->id) {
        cfg->id = bake_strdup(cfg->path);
        if (!cfg->id) {
            return -1;
        }

        for (char *p = cfg->id; *p; p++) {
            if (*p == '/' || *p == '\\' || *p == ':') {
                *p = '.';
            }
        }

        while (cfg->id[0] == '.') {
            memmove(cfg->id, cfg->id + 1, strlen(cfg->id));
        }
    }

    if (!cfg->output_name) {
        cfg->output_name = bake_stem(cfg->id);
    }

    return cfg->path && cfg->id && cfg->output_name ? 0 : -1;
}

int bake_project_cfg_load_file(const char *project_json_path, bake_project_cfg_t *cfg) {
    size_t len = 0;
    char *json_raw = bake_read_file(project_json_path, &len);
    if (!json_raw) {
        return -1;
    }

    size_t json_len = len;
    char *json = bake_json_strip_comments(json_raw, len, &json_len);
    if (!json) {
        ecs_os_free(json_raw);
        return -1;
    }

    int token_count = 512;
    jsmntok_t *tokens = NULL;
    int parsed = JSMN_ERROR_NOMEM;

    while (parsed == JSMN_ERROR_NOMEM) {
        token_count *= 2;
        ecs_os_free(tokens);
        tokens = ecs_os_calloc_n(jsmntok_t, token_count);
        if (!tokens) {
            ecs_os_free(json);
            return -1;
        }

        jsmn_parser parser;
        jsmn_init(&parser);
        parsed = jsmn_parse(&parser, json, json_len, tokens, (unsigned int)token_count);
    }

    if (parsed < 0 || parsed < 1 || tokens[0].type != JSMN_OBJECT) {
        ecs_os_free(tokens);
        ecs_os_free(json);
        return -1;
    }

    int root = 0;
    if (bake_json_get_string(json, tokens, parsed, root, "id", &cfg->id) < 0) goto error;

    char *type = NULL;
    if (bake_json_get_string(json, tokens, parsed, root, "type", &type) < 0) goto error;
    if (type) {
        cfg->kind = bake_project_kind_parse(type);
        ecs_os_free(type);
    }

    if (bake_json_get_array(json, tokens, parsed, root, "drivers", &cfg->drivers) < 0) goto error;
    if (bake_json_get_array(json, tokens, parsed, root, "plugins", &cfg->plugins) < 0) goto error;

    int test_obj = bake_json_find_object_key(json, tokens, parsed, root, "test");
    if (test_obj >= 0 && tokens[test_obj].type == JSMN_OBJECT) {
        cfg->has_test_spec = true;
        if (cfg->kind == B2_PROJECT_APPLICATION) {
            cfg->kind = B2_PROJECT_TEST;
        }
    }

    int rules_tok = bake_json_find_object_key(json, tokens, parsed, root, "rules");
    if (bake_parse_rules(json, tokens, parsed, rules_tok, &cfg->rules) != 0) goto error;

    int value_obj = bake_json_find_object_key(json, tokens, parsed, root, "value");
    if (value_obj >= 0) {
        if (tokens[value_obj].type != JSMN_OBJECT) goto error;
        if (bake_json_get_string(json, tokens, parsed, value_obj, "language", &cfg->language) < 0) goto error;
        if (bake_json_get_string(json, tokens, parsed, value_obj, "output", &cfg->output_name) < 0) goto error;
        if (bake_json_get_string(json, tokens, parsed, value_obj, "artefact", &cfg->output_name) < 0) goto error;

        if (bake_json_get_bool(json, tokens, parsed, value_obj, "private", &cfg->private_project) < 0) goto error;
        if (bake_json_get_bool(json, tokens, parsed, value_obj, "standalone", &cfg->standalone) < 0) goto error;

        if (bake_json_get_array_alias(json, tokens, parsed, value_obj, "use", NULL, &cfg->use) < 0) goto error;
        if (bake_json_get_array_alias(json, tokens, parsed, value_obj, "use-private", "use_private", &cfg->use_private) < 0) goto error;
        if (bake_json_get_array_alias(json, tokens, parsed, value_obj, "use-build", "use_build", &cfg->use_build) < 0) goto error;
        if (bake_json_get_array_alias(json, tokens, parsed, value_obj, "use-runtime", "use_runtime", &cfg->use_runtime) < 0) goto error;

        int dependee_obj = bake_json_find_object_key(json, tokens, parsed, value_obj, "dependee");
        if (bake_parse_dependee_cfg(json, tokens, parsed, dependee_obj, &cfg->dependee) != 0) goto error;
    }

    int c_obj = bake_json_find_object_key(json, tokens, parsed, root, "lang.c");
    if (bake_parse_lang_cfg(json, tokens, parsed, c_obj, &cfg->c_lang) != 0) goto error;

    int cpp_obj = bake_json_find_object_key(json, tokens, parsed, root, "lang.cpp");
    if (bake_parse_lang_cfg(json, tokens, parsed, cpp_obj, &cfg->cpp_lang) != 0) goto error;

    int root_dependee_obj = bake_json_find_object_key(json, tokens, parsed, root, "dependee");
    if (bake_parse_dependee_cfg(json, tokens, parsed, root_dependee_obj, &cfg->dependee) != 0) goto error;

    cfg->path = bake_dirname(project_json_path);

    if (bake_project_cfg_finalize_defaults(project_json_path, cfg) != 0) {
        goto error;
    }

    ecs_os_free(tokens);
    ecs_os_free(json);
    ecs_os_free(json_raw);
    return 0;

error:
    ecs_os_free(tokens);
    ecs_os_free(json);
    ecs_os_free(json_raw);
    return -1;
}
