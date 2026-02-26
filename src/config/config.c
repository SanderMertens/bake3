#include "bake/config.h"
#include "bake/common.h"
#include "bake/os.h"

#include <ctype.h>

#include "parson.h"

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults);
static void bake_project_cfg_init_impl(bake_project_cfg_t *cfg, bool init_dependee, bool set_defaults);
static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee);
static int bake_parse_project_cfg_object(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_meta,
    bool parse_dependee);
static int bake_parse_rules(
    const JSON_Array *rules,
    bake_rule_list_t *rules_out);

static const char *bake_cfg_eval_mode = "debug";
static const char *bake_cfg_eval_target = NULL;

void bake_project_cfg_set_eval_context(const char *mode, const char *target) {
    bake_cfg_eval_mode = (mode && mode[0]) ? mode : "debug";
    bake_cfg_eval_target = (target && target[0]) ? target : NULL;
}

static char* bake_ltrim(char *str) {
    while (str && *str && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

static void bake_rtrim(char *str) {
    if (!str) {
        return;
    }

    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
}

static int bake_json_conditional_key_matches(const char *key) {
    if (!key) {
        return 0;
    }

    char *raw = bake_strdup(key);
    if (!raw) {
        return -1;
    }

    size_t len = strlen(raw);
    if (len < 4 || raw[0] != '$' || raw[1] != '{' || raw[len - 1] != '}') {
        ecs_os_free(raw);
        return 0;
    }

    raw[len - 1] = '\0';
    char *expr = bake_ltrim(raw + 2);
    bake_rtrim(expr);

    char *space = expr;
    while (*space && !isspace((unsigned char)*space)) {
        space++;
    }

    if (!*space) {
        ecs_os_free(raw);
        return 0;
    }

    *space = '\0';
    char *kind = expr;
    char *value = bake_ltrim(space + 1);
    bake_rtrim(value);

    int match = 0;
    if (value[0]) {
        if (!strcmp(kind, "os")) {
            match = !strcasecmp(value, bake_host_os());
        } else if (!strcmp(kind, "cfg")) {
            const char *mode = bake_cfg_eval_mode ? bake_cfg_eval_mode : "debug";
            match = !strcasecmp(value, mode);
        } else if (!strcmp(kind, "target")) {
            match = bake_cfg_eval_target && !strcasecmp(value, bake_cfg_eval_target);
        }
    }

    ecs_os_free(raw);
    return match;
}

static char* bake_json_strdup_value(const JSON_Value *value) {
    if (!value) {
        return NULL;
    }

    JSON_Value_Type type = json_value_get_type(value);
    if (type == JSONString) {
        const char *str = json_value_get_string(value);
        return str ? bake_strdup(str) : NULL;
    }

    if (type == JSONBoolean) {
        return bake_strdup(json_value_get_boolean(value) ? "true" : "false");
    }

    if (type == JSONNull) {
        return bake_strdup("null");
    }

    char *serialized = json_serialize_to_string(value);
    if (!serialized) {
        return NULL;
    }

    char *out = bake_strdup(serialized);
    json_free_serialized_string(serialized);
    return out;
}

static int bake_json_parse_strlist(const JSON_Array *array, bake_strlist_t *list) {
    size_t count = json_array_get_count(array);
    for (size_t i = 0; i < count; i++) {
        JSON_Value *value = json_array_get_value(array, i);
        JSON_Value_Type type = json_value_get_type(value);
        if (type == JSONArray || type == JSONObject) {
            return -1;
        }

        char *str = bake_json_strdup_value(value);
        if (!str) {
            return -1;
        }

        if (bake_strlist_append_owned(list, str) != 0) {
            ecs_os_free(str);
            return -1;
        }
    }

    return 0;
}

static int bake_json_get_string(const JSON_Object *object, const char *key, char **out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    JSON_Value_Type type = json_value_get_type(value);
    if (type == JSONObject || type == JSONArray) {
        return -1;
    }

    char *str = bake_json_strdup_value(value);
    if (!str) {
        return -1;
    }

    ecs_os_free(*out);
    *out = str;
    return 0;
}

static int bake_json_get_bool(const JSON_Object *object, const char *key, bool *out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    if (json_value_get_type(value) != JSONBoolean) {
        return -1;
    }

    *out = json_value_get_boolean(value) != 0;
    return 0;
}

static int bake_json_get_array(const JSON_Object *object, const char *key, bake_strlist_t *out) {
    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 1;
    }

    if (json_value_get_type(value) != JSONArray) {
        return -1;
    }

    return bake_json_parse_strlist(json_value_get_array(value), out);
}

static int bake_json_get_array_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    bake_strlist_t *out)
{
    int rc = bake_json_get_array(object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_array(object, alias, out);
    }
    return rc;
}

static int bake_json_get_string_alias(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    char **out)
{
    int rc = bake_json_get_string(object, key, out);
    if (rc == 1 && alias) {
        rc = bake_json_get_string(object, alias, out);
    }
    return rc;
}

static int bake_json_get_object_optional(
    const JSON_Object *object,
    const char *key,
    const JSON_Object **out)
{
    *out = NULL;

    JSON_Value *value = json_object_get_value(object, key);
    if (!value) {
        return 0;
    }

    if (json_value_get_type(value) != JSONObject) {
        return -1;
    }

    *out = json_value_get_object(value);
    return 0;
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

char* bake_project_cfg_artefact_name(const bake_project_cfg_t *cfg) {
    if (!cfg || !cfg->output_name ||
        (cfg->kind != BAKE_PROJECT_PACKAGE &&
         cfg->kind != BAKE_PROJECT_APPLICATION &&
         cfg->kind != BAKE_PROJECT_TEST))
    {
        return NULL;
    }

#if defined(_WIN32)
    const char *exe_ext = ".exe";
    const char *lib_ext = ".lib";
    const char *lib_prefix = "";
#else
    const char *exe_ext = "";
    const char *lib_ext = ".a";
    const char *lib_prefix = "lib";
#endif

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        return bake_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    }

    return bake_asprintf("%s%s", cfg->output_name, exe_ext);
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

static int bake_parse_lang_cfg(const JSON_Object *object, bake_lang_cfg_t *cfg) {
    if (!object) {
        return 0;
    }

    if (bake_json_get_array_alias(object, "cflags", NULL, &cfg->cflags) < 0) return -1;
    if (bake_json_get_array_alias(object, "cxxflags", NULL, &cfg->cxxflags) < 0) return -1;
    if (bake_json_get_array_alias(object, "defines", NULL, &cfg->defines) < 0) return -1;
    if (bake_json_get_array_alias(object, "ldflags", NULL, &cfg->ldflags) < 0) return -1;
    if (bake_json_get_array_alias(object, "lib", "libs", &cfg->libs) < 0) return -1;
    if (bake_json_get_array_alias(object, "static-lib", "static_lib", &cfg->static_libs) < 0) return -1;
    if (bake_json_get_array_alias(object, "libpath", "libpaths", &cfg->libpaths) < 0) return -1;
    if (bake_json_get_array_alias(object, "link", "links", &cfg->links) < 0) return -1;
    if (bake_json_get_array_alias(object, "include", NULL, &cfg->include_paths) < 0) return -1;

    if (bake_json_get_string(object, "c-standard", &cfg->c_standard) < 0) return -1;
    if (bake_json_get_string(object, "cpp-standard", &cfg->cpp_standard) < 0) return -1;

    if (bake_json_get_bool(object, "static", &cfg->static_lib) < 0) return -1;
    if (bake_json_get_bool(object, "export-symbols", &cfg->export_symbols) < 0) return -1;
    if (bake_json_get_bool(object, "precompile-header", &cfg->precompile_header) < 0) return -1;

    size_t key_count = json_object_get_count(object);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(object, i);
        int cond = bake_json_conditional_key_matches(key);
        if (cond < 0) {
            return -1;
        }

        if (cond > 0) {
            JSON_Value *val = json_object_get_value_at(object, i);
            const JSON_Object *nested = json_value_get_object(val);
            if (!nested || bake_parse_lang_cfg(nested, cfg) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int bake_parse_project_value_lang_array(
    const JSON_Object *object,
    const char *key,
    const char *alias,
    bake_strlist_t *c_list,
    bake_strlist_t *cpp_list)
{
    if (bake_json_get_array_alias(object, key, alias, c_list) < 0) {
        return -1;
    }
    if (bake_json_get_array_alias(object, key, alias, cpp_list) < 0) {
        return -1;
    }
    return 0;
}

static int bake_parse_project_value_lang_bool(
    const JSON_Object *object,
    const char *key,
    bool *c_value,
    bool *cpp_value)
{
    bool value = false;
    int rc = bake_json_get_bool(object, key, &value);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0) {
        *c_value = value;
        *cpp_value = value;
    }
    return 0;
}

static int bake_parse_project_value_cfg(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    if (!object) {
        return 0;
    }

    if (bake_json_get_string(object, "language", &cfg->language) < 0) return -1;
    if (bake_json_get_string(object, "output", &cfg->output_name) < 0) return -1;
    if (bake_json_get_string(object, "artefact", &cfg->output_name) < 0) return -1;

    if (bake_json_get_bool(object, "private", &cfg->private_project) < 0) return -1;
    if (bake_json_get_bool(object, "public", &cfg->public_project) < 0) return -1;
    if (bake_json_get_bool(object, "standalone", &cfg->standalone) < 0) return -1;
    if (bake_json_get_bool(object, "amalgamate", &cfg->amalgamate) < 0) return -1;
    if (bake_json_get_string_alias(
        object,
        "amalgamate-path",
        "amalgamate_path",
        &cfg->amalgamate_path) < 0)
    {
        return -1;
    }

    if (bake_json_get_array_alias(object, "use", NULL, &cfg->use) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-private", "use_private", &cfg->use_private) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-build", "use_build", &cfg->use_build) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-runtime", "use_runtime", &cfg->use_runtime) < 0) return -1;

    if (bake_parse_project_value_lang_array(
        object, "cflags", NULL,
        &cfg->c_lang.cflags, &cfg->cpp_lang.cflags) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "cxxflags", NULL,
        &cfg->c_lang.cxxflags, &cfg->cpp_lang.cxxflags) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "defines", NULL,
        &cfg->c_lang.defines, &cfg->cpp_lang.defines) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "ldflags", NULL,
        &cfg->c_lang.ldflags, &cfg->cpp_lang.ldflags) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "lib", "libs",
        &cfg->c_lang.libs, &cfg->cpp_lang.libs) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "static-lib", "static_lib",
        &cfg->c_lang.static_libs, &cfg->cpp_lang.static_libs) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "libpath", "libpaths",
        &cfg->c_lang.libpaths, &cfg->cpp_lang.libpaths) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "link", "links",
        &cfg->c_lang.links, &cfg->cpp_lang.links) != 0) return -1;
    if (bake_parse_project_value_lang_array(
        object, "include", NULL,
        &cfg->c_lang.include_paths, &cfg->cpp_lang.include_paths) != 0) return -1;

    if (bake_json_get_string(object, "c-standard", &cfg->c_lang.c_standard) < 0) return -1;
    if (bake_json_get_string(object, "cpp-standard", &cfg->cpp_lang.cpp_standard) < 0) return -1;

    if (bake_parse_project_value_lang_bool(
        object, "static",
        &cfg->c_lang.static_lib, &cfg->cpp_lang.static_lib) != 0) return -1;
    if (bake_parse_project_value_lang_bool(
        object, "export-symbols",
        &cfg->c_lang.export_symbols, &cfg->cpp_lang.export_symbols) != 0) return -1;
    if (bake_parse_project_value_lang_bool(
        object, "precompile-header",
        &cfg->c_lang.precompile_header, &cfg->cpp_lang.precompile_header) != 0) return -1;

    return 0;
}

static int bake_parse_project_lang_cfg(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    const JSON_Object *lang_c = NULL;
    if (bake_json_get_object_optional(object, "lang.c", &lang_c) != 0) return -1;
    if (bake_parse_lang_cfg(lang_c, &cfg->c_lang) != 0) return -1;

    const JSON_Object *lang_cpp = NULL;
    if (bake_json_get_object_optional(object, "lang.cpp", &lang_cpp) != 0) return -1;
    if (bake_parse_lang_cfg(lang_cpp, &cfg->cpp_lang) != 0) return -1;

    const JSON_Object *lang = NULL;
    if (bake_json_get_object_optional(object, "lang", &lang) != 0) return -1;
    if (lang) {
        const JSON_Object *lang_c_obj = NULL;
        if (bake_json_get_object_optional(lang, "c", &lang_c_obj) != 0) return -1;
        if (bake_parse_lang_cfg(lang_c_obj, &cfg->c_lang) != 0) return -1;

        const JSON_Object *lang_cpp_obj = NULL;
        if (bake_json_get_object_optional(lang, "cpp", &lang_cpp_obj) != 0) return -1;
        if (bake_parse_lang_cfg(lang_cpp_obj, &cfg->cpp_lang) != 0) return -1;
    }

    return 0;
}

static int bake_parse_dependee_cfg(
    const JSON_Object *object,
    bake_dependee_cfg_t *cfg);

static int bake_parse_project_cfg_conditionals(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_dependee)
{
    if (!object) {
        return 0;
    }

    size_t key_count = json_object_get_count(object);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(object, i);
        int cond = bake_json_conditional_key_matches(key);
        if (cond < 0) {
            return -1;
        }

        if (cond > 0) {
            JSON_Value *val = json_object_get_value_at(object, i);
            const JSON_Object *nested = json_value_get_object(val);
            if (!nested) {
                return -1;
            }
            if (bake_parse_project_cfg_object(nested, cfg, false, parse_dependee) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int bake_parse_project_cfg_object(
    const JSON_Object *object,
    bake_project_cfg_t *cfg,
    bool parse_meta,
    bool parse_dependee)
{
    if (!object) {
        return 0;
    }

    if (parse_meta) {
        if (bake_json_get_string(object, "id", &cfg->id) < 0) return -1;

        char *type = NULL;
        if (bake_json_get_string(object, "type", &type) < 0) return -1;
        if (type) {
            cfg->kind = bake_project_kind_parse(type);
            ecs_os_free(type);
        }

        if (bake_json_get_array(object, "drivers", &cfg->drivers) < 0) return -1;
        if (bake_json_get_array(object, "plugins", &cfg->plugins) < 0) return -1;

        JSON_Value *test_value = json_object_get_value(object, "test");
        if (test_value) {
            if (json_value_get_type(test_value) != JSONObject) {
                return -1;
            }
            cfg->has_test_spec = true;
            if (cfg->kind == BAKE_PROJECT_APPLICATION) {
                cfg->kind = BAKE_PROJECT_TEST;
            }
        }

        JSON_Value *rules_value = json_object_get_value(object, "rules");
        if (rules_value) {
            if (json_value_get_type(rules_value) != JSONArray) {
                return -1;
            }
            if (bake_parse_rules(json_value_get_array(rules_value), &cfg->rules) != 0) return -1;
        }
    }

    if (bake_parse_project_value_cfg(object, cfg) != 0) return -1;

    const JSON_Object *value_obj = NULL;
    if (bake_json_get_object_optional(object, "value", &value_obj) != 0) return -1;
    if (bake_parse_project_value_cfg(value_obj, cfg) != 0) return -1;

    if (bake_parse_project_lang_cfg(object, cfg) != 0) return -1;

    if (parse_dependee) {
        const JSON_Object *value_dependee_obj = NULL;
        if (value_obj && bake_json_get_object_optional(value_obj, "dependee", &value_dependee_obj) != 0) {
            return -1;
        }
        if (bake_parse_dependee_cfg(value_dependee_obj, &cfg->dependee) != 0) {
            return -1;
        }

        const JSON_Object *root_dependee_obj = NULL;
        if (bake_json_get_object_optional(object, "dependee", &root_dependee_obj) != 0) {
            return -1;
        }
        if (bake_parse_dependee_cfg(root_dependee_obj, &cfg->dependee) != 0) {
            return -1;
        }
    }

    if (bake_parse_project_cfg_conditionals(object, cfg, parse_dependee) != 0) {
        return -1;
    }

    if (bake_parse_project_cfg_conditionals(value_obj, cfg, parse_dependee) != 0) {
        return -1;
    }

    return 0;
}

static int bake_parse_dependee_cfg(
    const JSON_Object *object,
    bake_dependee_cfg_t *cfg)
{
    if (!object) {
        return 0;
    }

    if (!cfg->cfg) {
        cfg->cfg = ecs_os_calloc_t(bake_project_cfg_t);
        if (!cfg->cfg) {
            return -1;
        }
        bake_project_cfg_init_impl(cfg->cfg, false, false);
    }

    if (bake_parse_project_cfg_object(object, cfg->cfg, false, false) != 0) {
        return -1;
    }

    JSON_Value *wrapped = json_object_get_wrapping_value((JSON_Object*)object);
    char *serialized = wrapped ? json_serialize_to_string(wrapped) : NULL;
    if (!serialized) {
        return -1;
    }

    char *dependee_json = bake_strdup(serialized);
    json_free_serialized_string(serialized);
    if (!dependee_json) {
        return -1;
    }

    ecs_os_free(cfg->json);
    cfg->json = dependee_json;
    return 0;
}

static int bake_parse_rules(const JSON_Array *rules, bake_rule_list_t *rules_out) {
    if (!rules) {
        return 0;
    }

    size_t count = json_array_get_count(rules);
    for (size_t i = 0; i < count; i++) {
        JSON_Value *rule_value = json_array_get_value(rules, i);
        const JSON_Object *rule = json_value_get_object(rule_value);
        if (!rule) {
            return -1;
        }

        const char *ext = json_object_get_string(rule, "ext");
        const char *cmd = json_object_get_string(rule, "command");

        if (ext && cmd) {
            if (bake_rule_list_append(rules_out, ext, cmd) != 0) {
                return -1;
            }
        }
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
    char *json = bake_read_file(project_json_path, &len);
    if (!json) {
        return -1;
    }

    JSON_Value *root_value = json_parse_string_with_comments(json);
    const JSON_Object *root = root_value ? json_value_get_object(root_value) : NULL;
    if (!root) {
        json_value_free(root_value);
        ecs_os_free(json);
        return -1;
    }

    if (bake_parse_project_cfg_object(root, cfg, true, true) != 0) {
        goto error;
    }

    cfg->path = bake_dirname(project_json_path);

    if (bake_project_cfg_finalize_defaults(project_json_path, cfg) != 0) {
        goto error;
    }

    json_value_free(root_value);
    ecs_os_free(json);
    return 0;

error:
    json_value_free(root_value);
    ecs_os_free(json);
    return -1;
}
