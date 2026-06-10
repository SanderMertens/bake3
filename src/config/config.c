#include "bake/config.h"
#include "bake/common.h"
#include "bake/os.h"

#include <ctype.h>

#include "parson.h"
#include "common/json_helpers.h"
#include "common/strutil.h"

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
static int bake_parse_bundles_object(
    const JSON_Object *bundles_obj,
    bake_bundle_list_t *bundles);

static const char *bake_cfg_eval_mode = "debug";
static const char *bake_cfg_eval_target = NULL;

void bake_project_cfg_set_eval_context(const char *mode, const char *target) {
    bake_cfg_eval_mode = (mode && mode[0]) ? mode : "debug";
    bake_cfg_eval_target = (target && target[0]) ? target : NULL;
}

static int bake_json_conditional_key_matches(const char *key) {
    if (!key) {
        return 0;
    }

    char *raw = ecs_os_strdup(key);
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
            match = !strcasecmp(value, bake_target_os());
        } else if (!strcmp(kind, "arch")) {
            match = !strcasecmp(value, bake_target_arch());
        } else if (!strcmp(kind, "cfg")) {
            const char *mode = bake_cfg_eval_mode ? bake_cfg_eval_mode : "debug";
            match = !strcasecmp(value, mode);
        } else if (!strcmp(kind, "target")) {
            match = bake_cfg_eval_target && !strcasecmp(value, bake_cfg_eval_target);
        } else {
            ecs_warn("unknown conditional key kind '%s' in '%s'", kind, key);
        }
    }

    ecs_os_free(raw);
    return match;
}

bool bake_language_is_cpp(const bake_project_cfg_t *cfg) {
    return cfg->language &&
        (!strcmp(cfg->language, "cpp") || !strcmp(cfg->language, "c++"));
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

    const char *target_exe_ext = bake_target_exe_ext();
    if (target_exe_ext && target_exe_ext[0]) {
        exe_ext = target_exe_ext;
    }

    if (cfg->kind == BAKE_PROJECT_PACKAGE) {
        return flecs_asprintf("%s%s%s", lib_prefix, cfg->output_name, lib_ext);
    }

    return flecs_asprintf("%s%s", cfg->output_name, exe_ext);
}

void bake_rule_list_init(bake_rule_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_rule_list_fini(bake_rule_list_t *list) {
    bake_rule_t *items = ecs_vec_first_t(&list->vec, bake_rule_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].ext);
        ecs_os_free(items[i].command);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_rule_t);
}

int bake_rule_list_append(bake_rule_list_t *list, const char *ext, const char *command) {
    bake_rule_t *rule = ecs_vec_append_t(NULL, &list->vec, bake_rule_t);
    rule->ext = ecs_os_strdup(ext);
    rule->command = ecs_os_strdup(command);
    return (rule->ext && rule->command) ? 0 : -1;
}

void bake_bundle_list_init(bake_bundle_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_bundle_list_fini(bake_bundle_list_t *list) {
    bake_bundle_t *items = ecs_vec_first_t(&list->vec, bake_bundle_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].id);
        ecs_os_free(items[i].repository);
        ecs_os_free(items[i].branch);
        ecs_os_free(items[i].tag);
        ecs_os_free(items[i].commit);
        ecs_os_free(items[i].subdir);
        ecs_os_free(items[i].library);
        ecs_os_free(items[i].build_system);
        bake_strlist_fini(&items[i].includes);
        bake_strlist_fini(&items[i].sources);
        bake_strlist_fini(&items[i].cmake_args);
        bake_strlist_fini(&items[i].libs);
        bake_strlist_fini(&items[i].ldflags);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_bundle_t);
}

bake_bundle_t* bake_bundle_list_append(bake_bundle_list_t *list) {
    bake_bundle_t *item = ecs_vec_append_t(NULL, &list->vec, bake_bundle_t);
    if (!item) {
        return NULL;
    }
    memset(item, 0, sizeof(*item));
    bake_strlist_init(&item->includes);
    bake_strlist_init(&item->sources);
    bake_strlist_init(&item->cmake_args);
    bake_strlist_init(&item->libs);
    bake_strlist_init(&item->ldflags);
    return item;
}

int32_t bake_bundle_list_count(const bake_bundle_list_t *list) {
    return ecs_vec_count(&list->vec);
}

bake_bundle_t* bake_bundle_list_get(const bake_bundle_list_t *list, int32_t index) {
    if (index < 0 || index >= ecs_vec_count(&list->vec)) {
        return NULL;
    }
    return &ecs_vec_first_t(&list->vec, bake_bundle_t)[index];
}

const bake_bundle_t* bake_bundle_list_find(const bake_bundle_list_t *list, const char *id) {
    if (!id) {
        return NULL;
    }
    int32_t count = ecs_vec_count(&list->vec);
    bake_bundle_t *items = ecs_vec_first_t(&list->vec, bake_bundle_t);
    for (int32_t i = 0; i < count; i++) {
        if (items[i].id && !strcmp(items[i].id, id)) {
            return &items[i];
        }
    }
    return NULL;
}

void bake_amalgamate_list_init(bake_amalgamate_list_t *list) {
    memset(&list->vec, 0, sizeof(list->vec));
}

void bake_amalgamate_list_fini(bake_amalgamate_list_t *list) {
    bake_amalgamate_cfg_t *items = ecs_vec_first_t(&list->vec, bake_amalgamate_cfg_t);
    int32_t count = ecs_vec_count(&list->vec);
    for (int32_t i = 0; i < count; i++) {
        ecs_os_free(items[i].path);
        ecs_os_free(items[i].prefix);
        bake_strlist_fini(&items[i].disable_flags);
    }
    ecs_vec_fini_t(NULL, &list->vec, bake_amalgamate_cfg_t);
}

bake_amalgamate_cfg_t* bake_amalgamate_list_append(bake_amalgamate_list_t *list) {
    bake_amalgamate_cfg_t *item = ecs_vec_append_t(NULL, &list->vec, bake_amalgamate_cfg_t);
    if (!item) {
        return NULL;
    }
    memset(item, 0, sizeof(*item));
    bake_strlist_init(&item->disable_flags);
    return item;
}

int32_t bake_amalgamate_list_count(const bake_amalgamate_list_t *list) {
    return ecs_vec_count(&list->vec);
}

bake_amalgamate_cfg_t* bake_amalgamate_list_get(const bake_amalgamate_list_t *list, int32_t index) {
    if (index < 0 || index >= ecs_vec_count(&list->vec)) {
        return NULL;
    }
    return &ecs_vec_first_t(&list->vec, bake_amalgamate_cfg_t)[index];
}

static void bake_lang_cfg_init_impl(bake_lang_cfg_t *cfg, bool set_defaults) {
#define F(n) bake_strlist_init(&cfg->n)
    F(cflags); F(cxxflags); F(defines); F(ldflags); F(libs);
    F(static_libs); F(libpaths); F(links); F(include_paths); F(embed);
#undef F
    cfg->c_standard = set_defaults ? ecs_os_strdup("c99") : NULL;
    cfg->cpp_standard = set_defaults ? ecs_os_strdup("c++17") : NULL;
    cfg->static_lib = false;
    cfg->export_symbols = false;
    cfg->precompile_header = set_defaults;
}

void bake_lang_cfg_init(bake_lang_cfg_t *cfg) {
    bake_lang_cfg_init_impl(cfg, true);
}

void bake_lang_cfg_fini(bake_lang_cfg_t *cfg) {
#define F(n) bake_strlist_fini(&cfg->n)
    F(cflags); F(cxxflags); F(defines); F(ldflags); F(libs);
    F(static_libs); F(libpaths); F(links); F(include_paths); F(embed);
#undef F
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
    cfg->language = set_defaults ? ecs_os_strdup("c") : NULL;

#define F(n) bake_strlist_init(&cfg->n)
    F(use); F(use_private); F(use_build); F(use_runtime); F(drivers); F(plugins);
    F(bundle_includes); F(bundle_libpaths); F(bundle_libs); F(bundle_ldflags);
    F(bundle_sources);
#undef F

    if (init_dependee) {
        bake_dependee_cfg_init(&cfg->dependee);
    } else {
        memset(&cfg->dependee, 0, sizeof(cfg->dependee));
    }
    bake_lang_cfg_init_impl(&cfg->c_lang, set_defaults);
    bake_lang_cfg_init_impl(&cfg->cpp_lang, set_defaults);
    bake_rule_list_init(&cfg->rules);
    bake_bundle_list_init(&cfg->bundles);
    bake_amalgamate_list_init(&cfg->amalgamate);
}

void bake_project_cfg_init(bake_project_cfg_t *cfg) {
    bake_project_cfg_init_impl(cfg, true, true);
}

static void bake_project_cfg_fini_impl(bake_project_cfg_t *cfg, bool fini_dependee) {
#define F(n) ecs_os_free(cfg->n)
    F(id); F(path); F(language); F(output_name);
#undef F

#define F(n) bake_strlist_fini(&cfg->n)
    F(use); F(use_private); F(use_build); F(use_runtime); F(drivers); F(plugins);
    F(bundle_includes); F(bundle_libpaths); F(bundle_libs); F(bundle_ldflags);
    F(bundle_sources);
#undef F
    bake_amalgamate_list_fini(&cfg->amalgamate);

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
    bake_bundle_list_fini(&cfg->bundles);

    memset(cfg, 0, sizeof(*cfg));
}

void bake_project_cfg_fini(bake_project_cfg_t *cfg) {
    bake_project_cfg_fini_impl(cfg, true);
}

static int bake_parse_lang_cfg(const JSON_Object *object, bake_lang_cfg_t *cfg) {
    if (!object) {
        return 0;
    }

#define G(key, alias, field) if (bake_json_get_array_alias(object, key, alias, &cfg->field) < 0) return -1
    G("cflags", NULL, cflags); G("cxxflags", NULL, cxxflags); G("defines", NULL, defines);
    G("ldflags", NULL, ldflags); G("lib", "libs", libs); G("static-lib", "static_lib", static_libs);
    G("libpath", "libpaths", libpaths); G("link", "links", links); G("include", NULL, include_paths);
    G("embed", NULL, embed);
#undef G

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

static int bake_parse_amalgamate_item(
    const JSON_Object *item,
    bake_amalgamate_cfg_t *amalg)
{
    if (bake_json_get_string(item, "path", &amalg->path) < 0) return -1;
    if (bake_json_get_string(item, "prefix", &amalg->prefix) < 0) return -1;
    if (bake_json_get_array_alias(item, "disable-flags", "disable_flags", &amalg->disable_flags) < 0) return -1;
    return 0;
}

static int bake_parse_amalgamate(
    const JSON_Object *object,
    bake_project_cfg_t *cfg)
{
    JSON_Value *value = json_object_get_value(object, "amalgamate");
    if (!value) {
        return 0;
    }

    JSON_Value_Type type = json_value_get_type(value);

    if (type == JSONBoolean) {
        if (json_value_get_boolean(value)) {
            bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
            if (!amalg) return -1;
            if (bake_json_get_string_alias(object, "amalgamate-path", "amalgamate_path", &amalg->path) < 0) return -1;
        }
        return 0;
    }

    if (type == JSONObject) {
        bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
        if (!amalg) return -1;
        return bake_parse_amalgamate_item(json_value_get_object(value), amalg);
    }

    if (type == JSONArray) {
        JSON_Array *array = json_value_get_array(value);
        size_t count = json_array_get_count(array);
        for (size_t i = 0; i < count; i++) {
            const JSON_Object *item = json_array_get_object(array, i);
            if (!item) {
                ecs_err("each 'amalgamate' entry must be an object");
                return -1;
            }
            bake_amalgamate_cfg_t *amalg = bake_amalgamate_list_append(&cfg->amalgamate);
            if (!amalg) return -1;
            if (bake_parse_amalgamate_item(item, amalg) != 0) return -1;
        }
        return 0;
    }

    ecs_err("'amalgamate' must be a boolean, object or array");
    return -1;
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
    if (bake_parse_amalgamate(object, cfg) < 0) return -1;

    if (bake_json_get_array_alias(object, "use", NULL, &cfg->use) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-private", "use_private", &cfg->use_private) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-build", "use_build", &cfg->use_build) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-runtime", "use_runtime", &cfg->use_runtime) < 0) return -1;
    if (bake_json_get_array_alias(object, "use-bundle", "use_bundle", &cfg->use_build) < 0) return -1;

#define ARR(key, alias, field) \
    if (bake_json_get_array_alias(object, key, alias, &cfg->c_lang.field) < 0) return -1; \
    if (bake_json_get_array_alias(object, key, alias, &cfg->cpp_lang.field) < 0) return -1
    ARR("cflags", NULL, cflags);
    ARR("cxxflags", NULL, cxxflags);
    ARR("defines", NULL, defines);
    ARR("ldflags", NULL, ldflags);
    ARR("lib", "libs", libs);
    ARR("static-lib", "static_lib", static_libs);
    ARR("libpath", "libpaths", libpaths);
    ARR("link", "links", links);
    ARR("include", NULL, include_paths);
    ARR("embed", NULL, embed);
#undef ARR

    if (bake_json_get_string(object, "c-standard", &cfg->c_lang.c_standard) < 0) return -1;
    if (bake_json_get_string(object, "cpp-standard", &cfg->cpp_lang.cpp_standard) < 0) return -1;

#define LBOOL(key, field) do { \
    bool _v = false; int _rc = bake_json_get_bool(object, key, &_v); \
    if (_rc < 0) return -1; \
    if (_rc == 0) { cfg->c_lang.field = _v; cfg->cpp_lang.field = _v; } \
} while (0)
    LBOOL("static", static_lib);
    LBOOL("export-symbols", export_symbols);
    LBOOL("precompile-header", precompile_header);
#undef LBOOL

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

    {
        const JSON_Object *bundles_obj = NULL;
        if (bake_json_get_object_optional(object, "bundle", &bundles_obj) != 0) return -1;
        if (bake_parse_bundles_object(bundles_obj, &cfg->bundles) != 0) return -1;

        const JSON_Object *bundles_alias = NULL;
        if (bake_json_get_object_optional(object, "bundles", &bundles_alias) != 0) return -1;
        if (bake_parse_bundles_object(bundles_alias, &cfg->bundles) != 0) return -1;
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

    char *dependee_json = ecs_os_strdup(serialized);
    json_free_serialized_string(serialized);
    if (!dependee_json) {
        return -1;
    }

    ecs_os_free(cfg->json);
    cfg->json = dependee_json;
    return 0;
}

static int bake_parse_bundle_entry(
    const char *id,
    const JSON_Object *object,
    bake_bundle_list_t *bundles)
{
    if (!id || !id[0] || !object) {
        return -1;
    }

    bake_bundle_t *bundle = bake_bundle_list_append(bundles);
    if (!bundle) {
        return -1;
    }

    bundle->id = ecs_os_strdup(id);
    if (!bundle->id) {
        return -1;
    }

    if (bake_json_get_string_alias(object, "repository", "repo", &bundle->repository) < 0) return -1;
    if (bake_json_get_string(object, "branch", &bundle->branch) < 0) return -1;
    if (bake_json_get_string(object, "tag", &bundle->tag) < 0) return -1;
    if (bake_json_get_string(object, "commit", &bundle->commit) < 0) return -1;
    if (bake_json_get_string_alias(object, "subdir", "path", &bundle->subdir) < 0) return -1;
    if (bake_json_get_string(object, "library", &bundle->library) < 0) return -1;
    if (bake_json_get_string_alias(object, "build-system", "build_system", &bundle->build_system) < 0) return -1;

    if (bake_json_get_bool(object, "header-only", &bundle->header_only) < 0) {
        if (bake_json_get_bool(object, "header_only", &bundle->header_only) < 0) return -1;
    }
    if (bake_json_get_array_alias(object, "include", "includes", &bundle->includes) < 0) return -1;
    if (bake_json_get_array(object, "sources", &bundle->sources) < 0) return -1;
    if (bake_json_get_array_alias(object, "cmake-args", "cmake_args", &bundle->cmake_args) < 0) return -1;
    if (bake_json_get_array_alias(object, "lib", "libs", &bundle->libs) < 0) return -1;
    if (bake_json_get_array(object, "ldflags", &bundle->ldflags) < 0) return -1;

    return 0;
}

static int bake_parse_bundles_object(
    const JSON_Object *bundles_obj,
    bake_bundle_list_t *bundles)
{
    if (!bundles_obj) {
        return 0;
    }

    size_t key_count = json_object_get_count(bundles_obj);
    for (size_t i = 0; i < key_count; i++) {
        const char *key = json_object_get_name(bundles_obj, i);
        JSON_Value *val = json_object_get_value_at(bundles_obj, i);
        if (!val || json_value_get_type(val) != JSONObject) {
            return -1;
        }
        if (bake_parse_bundle_entry(key, json_value_get_object(val), bundles) != 0) {
            return -1;
        }
    }

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
        cfg->path = bake_path_dirname(project_json_path);
    }

    if (!cfg->id) {
        cfg->id = ecs_os_strdup(cfg->path);
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
        cfg->output_name = bake_path_stem(cfg->id);
    }

    return cfg->path && cfg->id && cfg->output_name ? 0 : -1;
}

int bake_project_cfg_load_file(const char *project_json_path, bake_project_cfg_t *cfg) {
    size_t len = 0;
    char *json = bake_file_read(project_json_path, &len);
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

    if (!cfg->path) {
        cfg->path = bake_path_dirname(project_json_path);
    }

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
