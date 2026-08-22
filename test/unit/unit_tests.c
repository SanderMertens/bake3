#include "bake/os.h"
#include "build/build_internal.h"
#include "bake/bundle.h"
#include "bake/strlist.h"
#include "bake/common.h"
#include "common/strutil.h"

#include <flecs.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *expr, const char *file, int line) {
    checks ++;
    if (!cond) {
        failures ++;
        printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

static void check_str(
    const char *got,
    const char *expect,
    const char *expr,
    const char *file,
    int line)
{
    checks ++;
    if (!got || !expect || strcmp(got, expect)) {
        failures ++;
        printf("FAIL %s:%d: %s\n  got:    %s\n  expect: %s\n",
            file, line, expr, got ? got : "(null)", expect);
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)
#define CHECK_STR(got, expect) check_str(got, expect, #got, __FILE__, __LINE__)

static void test_trim(void) {
    char buf[] = "  \t value \n ";
    char *trimmed = bake_ltrim(buf);
    bake_rtrim(trimmed);
    CHECK_STR(trimmed, "value");

    char only_space[] = "   ";
    char *empty = bake_ltrim(only_space);
    bake_rtrim(empty);
    CHECK_STR(empty, "");

    char none[] = "value";
    CHECK_STR(bake_ltrim(none), "value");

    bake_rtrim(NULL);
    CHECK(bake_ltrim(NULL) == NULL);
}

static void test_path_join(void) {
    char sep = bake_path_sep();
    char expect[64];

    char *joined = bake_path_join("a", "b");
    snprintf(expect, sizeof(expect), "a%cb", sep);
    CHECK_STR(joined, expect);
    ecs_os_free(joined);

    joined = bake_path_join("a/", "b");
    CHECK_STR(joined, "a/b");
    ecs_os_free(joined);

    joined = bake_path_join("", "b");
    CHECK_STR(joined, "b");
    ecs_os_free(joined);

    joined = bake_path_join("a", "");
    CHECK_STR(joined, "a");
    ecs_os_free(joined);

    joined = bake_path_join3("a", "b", "c");
    snprintf(expect, sizeof(expect), "a%cb%cc", sep, sep);
    CHECK_STR(joined, expect);
    ecs_os_free(joined);
}

static void test_path_compare(void) {
    CHECK(bake_path_equal_normalized("a/b", "a\\b"));
    CHECK(bake_path_equal_normalized("a/b/", "a/b"));
    CHECK(bake_path_equal_normalized("a/b///", "a/b"));
    CHECK(!bake_path_equal_normalized("a/b", "a/c"));
    CHECK(!bake_path_equal_normalized("a/b", "a/bc"));
    CHECK(!bake_path_equal_normalized(NULL, "a"));

    size_t prefix_len = 0;
    CHECK(bake_path_has_prefix_normalized("a/b/c", "a/b", &prefix_len));
    CHECK(prefix_len == 3);
    CHECK(bake_path_has_prefix_normalized("a/b", "a/b/", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/bc", "a/b", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/b", "a/b/c", NULL));
    CHECK(!bake_path_has_prefix_normalized("a/b", "", NULL));
}

static void test_path_is_abs(void) {
#if defined(_WIN32)
    CHECK(bake_path_is_abs("C:\\x"));
    CHECK(bake_path_is_abs("C:/x"));
#endif
    CHECK(bake_path_is_abs("/x"));
    CHECK(!bake_path_is_abs("x"));
    CHECK(!bake_path_is_abs("./x"));
    CHECK(!bake_path_is_abs(""));
}

static void test_project_id_as_macro(void) {
    char *macro = bake_project_id_as_macro("my.project-name");
    CHECK_STR(macro, "my_project_name");
    ecs_os_free(macro);

    macro = bake_project_id_as_macro("Mixed.Case9");
    CHECK_STR(macro, "mixed_case9");
    ecs_os_free(macro);

    CHECK(bake_project_id_as_macro(NULL) == NULL);
}

static void test_shell_quote(void) {
    char *quoted = bake_shell_quote_arg("plain");
    CHECK_STR(quoted, "\"plain\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("with space");
    CHECK_STR(quoted, "\"with space\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("has\"quote");
    CHECK_STR(quoted, "\"has\\\"quote\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg("back\\slash");
    CHECK_STR(quoted, "\"back\\\\slash\"");
    ecs_os_free(quoted);

    quoted = bake_shell_quote_arg(NULL);
    CHECK_STR(quoted, "\"\"");
    ecs_os_free(quoted);
}

static void test_strlist(void) {
    bake_strlist_t list;
    bake_strlist_init(&list);

    CHECK(bake_strlist_append(&list, "a") == 0);
    CHECK(bake_strlist_append(&list, "b") == 0);
    CHECK(list.count == 2);
    CHECK(bake_strlist_contains(&list, "a"));
    CHECK(!bake_strlist_contains(&list, "c"));

    CHECK(bake_strlist_append_unique(&list, "a") == 0);
    CHECK(list.count == 2);
    CHECK(bake_strlist_append_unique(&list, "c") == 0);
    CHECK(list.count == 3);

    char *joined = bake_strlist_join(&list, ",");
    CHECK_STR(joined, "a,b,c");
    ecs_os_free(joined);

    bake_strlist_t copy;
    bake_strlist_init(&copy);
    CHECK(bake_strlist_copy(&copy, &list) == 0);
    CHECK(copy.count == 3);
    CHECK(copy.items[0] != list.items[0]);
    CHECK_STR(copy.items[0], "a");

    bake_strlist_t merged;
    bake_strlist_init(&merged);
    CHECK(bake_strlist_append(&merged, "b") == 0);
    CHECK(bake_strlist_merge_unique(&merged, &list) == 0);
    CHECK(merged.count == 3);
    CHECK_STR(merged.items[0], "b");

    bake_strlist_fini(&merged);
    bake_strlist_fini(&copy);
    bake_strlist_fini(&list);
}

static void test_bundle_cargo_native_paths(void) {
    bake_set_build_target(NULL);

    char *command = bake_bundle_cargo_command(
        "source dir", "build dir", "debug");
    char *manifest = bake_path_join("source dir", "Cargo.toml");
    char *expected = flecs_asprintf(
        "cargo build --manifest-path \"%s\" --target-dir \"build dir\"",
        manifest);
    CHECK_STR(command, expected);
    ecs_os_free(expected);
    ecs_os_free(manifest);
    ecs_os_free(command);

    char *profile = bake_bundle_cargo_profile_dir("debug");
    CHECK_STR(profile, "debug");
    ecs_os_free(profile);
}

static void test_bundle_cargo_emscripten_paths(void) {
    bake_set_build_target("em");

    char *command = bake_bundle_cargo_command(
        "source dir", "build dir", "profile");
    char *manifest = bake_path_join("source dir", "Cargo.toml");
    char *expected_command = flecs_asprintf(
        "cargo rustc --release --target wasm32-unknown-emscripten "
        "--crate-type staticlib "
        "--manifest-path \"%s\" --target-dir \"build dir\"",
        manifest);
    CHECK_STR(command, expected_command);
    ecs_os_free(expected_command);
    ecs_os_free(manifest);
    ecs_os_free(command);

    char *profile = bake_bundle_cargo_profile_dir("profile");
    char *expected_profile = bake_path_join(
        "wasm32-unknown-emscripten", "release");
    CHECK_STR(profile, expected_profile);
    ecs_os_free(expected_profile);
    ecs_os_free(profile);

    bake_set_build_target(NULL);
}

static bool strlist_has(const bake_strlist_t *list, const char *value) {
    for (int32_t i = 0; i < list->count; i++) {
        if (!strcmp(list->items[i], value)) {
            return true;
        }
    }
    return false;
}

static void mode_flags(
    const char *mode,
    bake_strlist_t *cflags,
    bake_strlist_t *ldflags)
{
    bake_strlist_t cxxflags;
    bake_strlist_init(cflags);
    bake_strlist_init(&cxxflags);
    bake_strlist_init(ldflags);
    bake_add_mode_flags(mode, BAKE_COMPILER_CLANG, cflags, &cxxflags, ldflags);
    bake_strlist_fini(&cxxflags);
}

static void test_mode_flags_native(void) {
    bake_set_build_target(NULL);

    bake_strlist_t cflags, ldflags;

    mode_flags("release", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-flto"));
    CHECK(!strlist_has(&ldflags, "-O3"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("debug", &cflags, &ldflags);
    CHECK(ldflags.count == 0);
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("profile", &cflags, &ldflags);
    CHECK(strlist_has(&cflags, "-pg"));
    CHECK(strlist_has(&ldflags, "-pg"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);
}

static void test_mode_flags_emscripten(void) {
    bake_set_build_target("em");

    bake_strlist_t cflags, ldflags;

    mode_flags("release", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O3"));
    CHECK(strlist_has(&ldflags, "-flto"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("debug", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O0"));
    CHECK(strlist_has(&ldflags, "-g"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("profile", &cflags, &ldflags);
    CHECK(strlist_has(&cflags, "-O2"));
    CHECK(!strlist_has(&cflags, "-pg"));
    CHECK(strlist_has(&ldflags, "-O2"));
    CHECK(!strlist_has(&ldflags, "-pg"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    mode_flags("sanitize", &cflags, &ldflags);
    CHECK(strlist_has(&ldflags, "-O0"));
    bake_strlist_fini(&cflags); bake_strlist_fini(&ldflags);

    bake_set_build_target(NULL);
}

static void test_em_serve_first_port(void) {
    CHECK(bake_em_serve_first_port(0) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(-1) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(65536) == BAKE_EM_SERVE_PORT_DEFAULT);
    CHECK(bake_em_serve_first_port(1) == 1);
    CHECK(bake_em_serve_first_port(8123) == 8123);
    CHECK(bake_em_serve_first_port(65535) == 65535);
    CHECK(BAKE_EM_SERVE_PORT_SCAN > 0);
}

int main(void) {
    ecs_os_init();

    test_trim();
    test_path_join();
    test_path_compare();
    test_path_is_abs();
    test_project_id_as_macro();
    test_shell_quote();
    test_strlist();
    test_bundle_cargo_native_paths();
    test_bundle_cargo_emscripten_paths();
    test_mode_flags_native();
    test_mode_flags_emscripten();
    test_em_serve_first_port();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
