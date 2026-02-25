#include "bake2/config.h"
#include "bake2/common.h"

#include <ctype.h>

#include "jsmn.h"

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults);
static void bake_project_cfg_init_impl(bake_project_cfg_t *cfg, bool init_dependee, bool set_defaults);
static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee);
static int bake_parse_rules(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int rules_tok,
    bake_rule_list_t *rules);

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

static int bake_json_get_string_alias(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    const char *key,
    const char *alias,
    char **out)
{
    int rc = bake_json_get_string(json, toks, count, object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_string(json, toks, count, object, alias, out);
    }
    return rc;
}

const char* bake_project_kind_str(bake_project_kind_t kind) {
    switch (kind) {
    case BAKE_PROJECT_APPLICATION: return "application";
    case BAKE_PROJECT_PACKAGE: return "package";
    case BAKE_PROJECT_CONFIG: return "config";
    case BAKE_PROJECT_TEST: return "test";
    case BAKE_PROJECT_TEMPLATE: return "template";
    default: return "application";
    }
}

bake_project_kind_t bake_project_kind_parse(const char *value) {
    if (!value) {
        return BAKE_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "application") || !strcmp(value, "app")) {
        return BAKE_PROJECT_APPLICATION;
    }
    if (!strcmp(value, "package") || !strcmp(value, "lib")) {
        return BAKE_PROJECT_PACKAGE;
    }
    if (!strcmp(value, "config")) {
        return BAKE_PROJECT_CONFIG;
    }
    if (!strcmp(value, "test")) {
        return BAKE_PROJECT_TEST;
    }
    if (!strcmp(value, "template")) {
        return BAKE_PROJECT_TEMPLATE;
    }
    return BAKE_PROJECT_APPLICATION;
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

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults) {
    bake_strlist_init(&cfg->cflags);
    bake_strlist_init(&cfg->cxxflags);
    bake_strlist_init(&cfg->defines);
    bake_strlist_init(&cfg->ldflags);
    bake_strlist_init(&cfg->libs);
    bake_strlist_init(&cfg->static_libs);
    bake_strlist_init(&cfg->libpaths);
    bake_strlist_init(&cfg->links);
    bake_strlist_init(&cfg->include_paths);
    cfg->c_standard = set_defaults ? bake_strdup("c99") : NULL;
    cfg->cpp_standard = set_defaults ? bake_strdup("c++17") : NULL;
    cfg->static_lib = false;
    cfg->export_symbols = false;
    cfg->precompile_header = set_defaults;
}

void bake_lang_cfg_init(bake_lang_cfg_t *cfg) {
    bake_lang_cfg_init_impl(cfg, true);
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
    cfg->cfg = ecs_os_calloc_t(bake_project_cfg_t);
    if (cfg->cfg) {
        bake_project_cfg_init_impl(cfg->cfg, false, false);
    }
    cfg->json = NULL;
}

void bake_dependee_cfg_fini(bake_dependee_cfg_t *cfg) {
    if (cfg->cfg) {
        bake_project_cfg_fini_impl(cfg->cfg, false);
        ecs_os_free(cfg->cfg);
    }
    ecs_os_free(cfg->json);
    memset(cfg, 0, sizeof(*cfg));
}

static void bake_project_cfg_init_impl(bake_project_cfg_t *cfg, bool init_dependee, bool set_defaults) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = BAKE_PROJECT_APPLICATION;
    cfg->has_test_spec = false;
    cfg->public_project = set_defaults;
    cfg->language = set_defaults ? bake_strdup("c") : NULL;
    cfg->amalgamate = false;
    cfg->amalgamate_path = NULL;

    bake_strlist_init(&cfg->use);
    bake_strlist_init(&cfg->use_private);
    bake_strlist_init(&cfg->use_build);
    bake_strlist_init(&cfg->use_runtime);
    bake_strlist_init(&cfg->drivers);
    bake_strlist_init(&cfg->plugins);

    if (init_dependee) {
        bake_dependee_cfg_init(&cfg->dependee);
    } else {
        memset(&cfg->dependee, 0, sizeof(cfg->dependee));
    }
    bake_lang_cfg_init_impl(&cfg->c_lang, set_defaults);
    bake_lang_cfg_init_impl(&cfg->cpp_lang, set_defaults);
    bake_rule_list_init(&cfg->rules);
}

void bake_project_cfg_init(bake_project_cfg_t *cfg) {
    bake_project_cfg_init_impl(cfg, true, true);
}

static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee) {
    ecs_os_free(cfg->id);
    ecs_os_free(cfg->path);
    ecs_os_free(cfg->language);
    ecs_os_free(cfg->output_name);
    ecs_os_free(cfg->amalgamate_path);

    bake_strlist_fini(&cfg->use);
    bake_strlist_fini(&cfg->use_private);
    bake_strlist_fini(&cfg->use_build);
    bake_strlist_fini(&cfg->use_runtime);
    bake_strlist_fini(&cfg->drivers);
    bake_strlist_fini(&cfg->plugins);

    if (fini_dependee) {
        bake_dependee_cfg_fini(&cfg->dependee);
    } else {
        if (cfg->dependee.cfg) {
            bake_project_cfg_fini_impl(cfg->dependee.cfg, false);
            ecs_os_free(cfg->dependee.cfg);
        }
        ecs_os_free(cfg->dependee.json);
    }
    bake_lang_cfg_fini(&cfg->c_lang);
    bake_lang_cfg_fini(&cfg->cpp_lang);
    bake_rule_list_fini(&cfg->rules);

    memset(cfg, 0, sizeof(*cfg));
}

void bake_project_cfg_fini(bake_project_cfg_t *cfg) {
    bake_project_cfg_fini_impl(cfg, true);
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

    if (bake_json_get_array_alias(json, toks, count, object, "cflags", NULL, &cfg->cflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "cxxflags", NULL, &cfg->cxxflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "defines", NULL, &cfg->defines) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "ldflags", NULL, &cfg->ldflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "lib", "libs", &cfg->libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "static-lib", "static_lib", &cfg->static_libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "libpath", "libpaths", &cfg->libpaths) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "link", "links", &cfg->links) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "include", NULL, &cfg->include_paths) < 0) return -1;

    if (bake_json_get_string(json, toks, count, object, "c-standard", &cfg->c_standard) < 0) return -1;
    if (bake_json_get_string(json, toks, count, object, "cpp-standard", &cfg->cpp_standard) < 0) return -1;

    if (bake_json_get_bool(json, toks, count, object, "static", &cfg->static_lib) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "export-symbols", &cfg->export_symbols) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "precompile-header", &cfg->precompile_header) < 0) return -1;

    return 0;
}

static int bake_parse_project_value_cfg(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_project_cfg_t *cfg)
{
    if (object < 0) {
        return 0;
    }

    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    if (bake_json_get_string(json, toks, count, object, "language", &cfg->language) < 0) return -1;
    if (bake_json_get_string(json, toks, count, object, "output", &cfg->output_name) < 0) return -1;
    if (bake_json_get_string(json, toks, count, object, "artefact", &cfg->output_name) < 0) return -1;

    if (bake_json_get_bool(json, toks, count, object, "private", &cfg->private_project) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "public", &cfg->public_project) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "standalone", &cfg->standalone) < 0) return -1;
    if (bake_json_get_bool(json, toks, count, object, "amalgamate", &cfg->amalgamate) < 0) return -1;
    if (bake_json_get_string_alias(
        json, toks, count, object,
        "amalgamate-path", "amalgamate_path",
        &cfg->amalgamate_path) < 0)
    {
        return -1;
    }

    if (bake_json_get_array_alias(json, toks, count, object, "use", NULL, &cfg->use) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "use-private", "use_private", &cfg->use_private) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "use-build", "use_build", &cfg->use_build) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "use-runtime", "use_runtime", &cfg->use_runtime) < 0) return -1;

    if (bake_json_get_array_alias(json, toks, count, object, "cflags", NULL, &cfg->c_lang.cflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "cflags", NULL, &cfg->cpp_lang.cflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "cxxflags", NULL, &cfg->c_lang.cxxflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "cxxflags", NULL, &cfg->cpp_lang.cxxflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "defines", NULL, &cfg->c_lang.defines) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "defines", NULL, &cfg->cpp_lang.defines) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "ldflags", NULL, &cfg->c_lang.ldflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "ldflags", NULL, &cfg->cpp_lang.ldflags) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "lib", "libs", &cfg->c_lang.libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "lib", "libs", &cfg->cpp_lang.libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "static-lib", "static_lib", &cfg->c_lang.static_libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "static-lib", "static_lib", &cfg->cpp_lang.static_libs) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "libpath", "libpaths", &cfg->c_lang.libpaths) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "libpath", "libpaths", &cfg->cpp_lang.libpaths) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "link", "links", &cfg->c_lang.links) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "link", "links", &cfg->cpp_lang.links) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "include", NULL, &cfg->c_lang.include_paths) < 0) return -1;
    if (bake_json_get_array_alias(json, toks, count, object, "include", NULL, &cfg->cpp_lang.include_paths) < 0) return -1;

    if (bake_json_get_string(json, toks, count, object, "c-standard", &cfg->c_lang.c_standard) < 0) return -1;
    if (bake_json_get_string(json, toks, count, object, "cpp-standard", &cfg->cpp_lang.cpp_standard) < 0) return -1;

    bool value = false;
    int bool_rc = bake_json_get_bool(json, toks, count, object, "static", &value);
    if (bool_rc < 0) return -1;
    if (bool_rc == 0) {
        cfg->c_lang.static_lib = value;
        cfg->cpp_lang.static_lib = value;
    }

    bool_rc = bake_json_get_bool(json, toks, count, object, "export-symbols", &value);
    if (bool_rc < 0) return -1;
    if (bool_rc == 0) {
        cfg->c_lang.export_symbols = value;
        cfg->cpp_lang.export_symbols = value;
    }

    bool_rc = bake_json_get_bool(json, toks, count, object, "precompile-header", &value);
    if (bool_rc < 0) return -1;
    if (bool_rc == 0) {
        cfg->c_lang.precompile_header = value;
        cfg->cpp_lang.precompile_header = value;
    }

    return 0;
}

static int bake_parse_project_lang_cfg(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_project_cfg_t *cfg)
{
    int c_obj = bake_json_find_object_key(json, toks, count, object, "lang.c");
    if (bake_parse_lang_cfg(json, toks, count, c_obj, &cfg->c_lang) != 0) return -1;

    int cpp_obj = bake_json_find_object_key(json, toks, count, object, "lang.cpp");
    if (bake_parse_lang_cfg(json, toks, count, cpp_obj, &cfg->cpp_lang) != 0) return -1;

    int lang_obj = bake_json_find_object_key(json, toks, count, object, "lang");
    if (lang_obj >= 0) {
        if (toks[lang_obj].type != JSMN_OBJECT) {
            return -1;
        }

        int lang_c_obj = bake_json_find_object_key(json, toks, count, lang_obj, "c");
        if (bake_parse_lang_cfg(json, toks, count, lang_c_obj, &cfg->c_lang) != 0) return -1;

        int lang_cpp_obj = bake_json_find_object_key(json, toks, count, lang_obj, "cpp");
        if (bake_parse_lang_cfg(json, toks, count, lang_cpp_obj, &cfg->cpp_lang) != 0) return -1;
    }

    return 0;
}

static int bake_parse_dependee_cfg(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_dependee_cfg_t *cfg);

static int bake_parse_project_cfg_object(
    const char *json,
    const jsmntok_t *toks,
    int count,
    int object,
    bake_project_cfg_t *cfg,
    bool parse_meta,
    bool parse_dependee)
{
    if (object < 0) {
        return 0;
    }

    if (toks[object].type != JSMN_OBJECT) {
        return -1;
    }

    if (parse_meta) {
        if (bake_json_get_string(json, toks, count, object, "id", &cfg->id) < 0) return -1;

        char *type = NULL;
        if (bake_json_get_string(json, toks, count, object, "type", &type) < 0) return -1;
        if (type) {
            cfg->kind = bake_project_kind_parse(type);
            ecs_os_free(type);
        }

        if (bake_json_get_array(json, toks, count, object, "drivers", &cfg->drivers) < 0) return -1;
        if (bake_json_get_array(json, toks, count, object, "plugins", &cfg->plugins) < 0) return -1;

        int test_obj = bake_json_find_object_key(json, toks, count, object, "test");
        if (test_obj >= 0) {
            if (toks[test_obj].type != JSMN_OBJECT) return -1;
            cfg->has_test_spec = true;
            if (cfg->kind == BAKE_PROJECT_APPLICATION) {
                cfg->kind = BAKE_PROJECT_TEST;
            }
        }

        int rules_tok = bake_json_find_object_key(json, toks, count, object, "rules");
        if (bake_parse_rules(json, toks, count, rules_tok, &cfg->rules) != 0) return -1;
    }

    if (bake_parse_project_value_cfg(json, toks, count, object, cfg) != 0) return -1;

    int value_obj = bake_json_find_object_key(json, toks, count, object, "value");
    if (bake_parse_project_value_cfg(json, toks, count, value_obj, cfg) != 0) return -1;

    if (bake_parse_project_lang_cfg(json, toks, count, object, cfg) != 0) return -1;

    if (parse_dependee) {
        int value_dependee_obj = -1;
        if (value_obj >= 0) {
            value_dependee_obj = bake_json_find_object_key(
                json, toks, count, value_obj, "dependee");
        }
        if (bake_parse_dependee_cfg(
            json, toks, count, value_dependee_obj, &cfg->dependee) != 0)
        {
            return -1;
        }

        int root_dependee_obj = bake_json_find_object_key(
            json, toks, count, object, "dependee");
        if (bake_parse_dependee_cfg(
            json, toks, count, root_dependee_obj, &cfg->dependee) != 0)
        {
            return -1;
        }
    }

    return 0;
}

static int bake_parse_dependee_cfg(
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

    if (!cfg->cfg) {
        cfg->cfg = ecs_os_calloc_t(bake_project_cfg_t);
        if (!cfg->cfg) {
            return -1;
        }
        bake_project_cfg_init_impl(cfg->cfg, false, false);
    }

    if (bake_parse_project_cfg_object(
        json, toks, count, object, cfg->cfg, false, false) != 0)
    {
        return -1;
    }

    char *dependee_json = bake_json_strdup(json, &toks[object]);
    if (!dependee_json) {
        return -1;
    }

    ecs_os_free(cfg->json);
    cfg->json = dependee_json;
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
    if (cfg->kind == BAKE_PROJECT_TEST) {
        cfg->public_project = false;
    }

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
    if (bake_parse_project_cfg_object(
        json, tokens, parsed, root, cfg, true, true) != 0)
    {
        goto error;
    }

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
