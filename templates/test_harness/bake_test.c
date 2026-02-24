#include "bake_test.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#define BAKE_TEST_PARAM_MAX (128)
#define BAKE_JMP_FAIL (1)
#define BAKE_JMP_ABORT (2)
#define BAKE_JMP_QUARANTINE (3)
#define BAKE_TEST_EMPTY (2)
#define BAKE_TEST_QUARANTINED (3)

static bake_test_suite *g_current_suite = NULL;
static bake_test_case *g_current_case = NULL;
static bool g_expect_abort = false;
static bool g_observed_abort = false;
static bool g_failed = false;
static bool g_flaky = false;
static const char *g_quarantine_date = NULL;
static int g_jmp_active = 0;
static jmp_buf g_jmp;
static const char *g_cli_params[BAKE_TEST_PARAM_MAX];
static int g_cli_param_count = 0;
static volatile sig_atomic_t g_interrupted = 0;
static void bake_abort_handler(int sig);
static void bake_interrupt_handler(int sig);

static void bake_interrupt_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static const char* bake_debug_exec(const char *exec) {
    const char *test_path = strstr(exec, "/test/");
    if (test_path) {
        return test_path + 1;
    }
    test_path = strstr(exec, "\\test\\");
    if (test_path) {
        return test_path + 1;
    }
    return exec;
}

static void bake_append_suite_params(char *buf, size_t size, bake_test_suite *suite) {
    if (!suite || !suite->param_count || !size) {
        return;
    }

    size_t off = 0;
    int n = snprintf(buf + off, size - off, " [ ");
    if (n < 0 || (size_t)n >= (size - off)) {
        return;
    }
    off += (size_t)n;

    for (uint32_t i = 0; i < suite->param_count; i++) {
        bake_test_param *param = &suite->params[i];
        const char *value = "";
        if (param->value_cur >= 0 && param->value_cur < param->value_count) {
            value = param->values[param->value_cur];
        }
        n = snprintf(buf + off, size - off, "%s%s: %s",
            i ? ", " : "", param->name ? param->name : "", value ? value : "");
        if (n < 0 || (size_t)n >= (size - off)) {
            return;
        }
        off += (size_t)n;
    }

    snprintf(buf + off, size - off, " ]");
}

static void bake_print_debug_command(const char *exec, bake_test_suite *suite, bake_test_case *tc) {
    printf("To run/debug your test, do:\n");
    printf("export $(bake env)\n");
    printf("%s %s.%s", bake_debug_exec(exec), suite->id, tc->id);
    for (int p = 0; p < g_cli_param_count; p++) {
        printf(" --param %s", g_cli_params[p]);
    }
    for (uint32_t p = 0; p < suite->param_count; p++) {
        bake_test_param *param = &suite->params[p];
        bool cli_override = false;
        for (int i = 0; i < g_cli_param_count; i++) {
            size_t len = strlen(param->name);
            if (!strncmp(g_cli_params[i], param->name, len) && g_cli_params[i][len] == '=') {
                cli_override = true;
                break;
            }
        }
        if (cli_override) {
            continue;
        }
        if (param->value_cur < 0 || param->value_cur >= param->value_count) {
            continue;
        }
        printf(" --param %s=%s", param->name, param->values[param->value_cur]);
    }
    printf("\n\n");
}

static void bake_print_report(const char *test_id, const char *suite_id, const char *param_str, int pass, int fail, int empty) {
    printf("PASS:%3d, FAIL:%3d, EMPTY:%3d (%s.%s%s)\n", pass, fail, empty, test_id, suite_id, param_str ? param_str : "");
}

static bool bake_char_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void bake_free_argv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int bake_parse_cmd(const char *cmd, char ***argv_out, int *argc_out) {
    int argc = 0;
    int cap = 8;
    char **argv = calloc((size_t)cap, sizeof(char*));
    if (!argv) {
        return -1;
    }

    const char *p = cmd;
    while (*p) {
        while (*p && bake_char_is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        int tcap = 128;
        int tlen = 0;
        char *tok = malloc((size_t)tcap);
        if (!tok) {
            bake_free_argv(argv, argc);
            return -1;
        }

        while (*p && !bake_char_is_space(*p)) {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    if (*p == '\\' && p[1] && quote == '"') {
                        p++;
                    }
                    if ((tlen + 2) >= tcap) {
                        tcap *= 2;
                        char *tmp = realloc(tok, (size_t)tcap);
                        if (!tmp) {
                            free(tok);
                            bake_free_argv(argv, argc);
                            return -1;
                        }
                        tok = tmp;
                    }
                    tok[tlen++] = *p++;
                }
                if (*p == quote) {
                    p++;
                }
                continue;
            }

            if (*p == '\\' && p[1]) {
                p++;
            }

            if ((tlen + 2) >= tcap) {
                tcap *= 2;
                char *tmp = realloc(tok, (size_t)tcap);
                if (!tmp) {
                    free(tok);
                    bake_free_argv(argv, argc);
                    return -1;
                }
                tok = tmp;
            }
            tok[tlen++] = *p++;
        }

        tok[tlen] = '\0';
        if ((argc + 2) >= cap) {
            cap *= 2;
            char **tmp = realloc(argv, (size_t)cap * sizeof(char*));
            if (!tmp) {
                free(tok);
                bake_free_argv(argv, argc);
                return -1;
            }
            argv = tmp;
        }
        argv[argc++] = tok;
    }

    if (!argc) {
        bake_free_argv(argv, argc);
        return -1;
    }
    argv[argc] = NULL;
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static int bake_run_subprocess(const char *cmd, int *sig_out) {
    if (sig_out) {
        *sig_out = 0;
    }

#if defined(_WIN32)
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    char *cmd_copy = _strdup(cmd);
    if (!cmd_copy) {
        return -1;
    }

    BOOL ok = CreateProcessA(NULL, cmd_copy, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd_copy);
    if (!ok) {
        return -1;
    }

    for (;;) {
        DWORD wait_rc = WaitForSingleObject(pi.hProcess, 20);
        if (wait_rc == WAIT_OBJECT_0) {
            break;
        }
        if (wait_rc != WAIT_TIMEOUT) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return -1;
        }
        if (g_interrupted) {
            TerminateProcess(pi.hProcess, 130);
        }
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exit_code;
#else
    char **argv = NULL;
    int argc = 0;
    if (bake_parse_cmd(cmd, &argv, &argc) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        bake_free_argv(argv, argc);
        return -1;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }

    bake_free_argv(argv, argc);

    int status = 0;
    for (;;) {
        if (g_interrupted) {
            kill(pid, SIGINT);
        }

        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            break;
        }
        if (rc == 0) {
            usleep(10000);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    if (WIFSIGNALED(status)) {
        if (sig_out) {
            *sig_out = WTERMSIG(status);
        }
        return 128 + WTERMSIG(status);
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

static void bake_set_failure(const char *file, int line, const char *msg, bool jump) {
    g_failed = true;
    if (g_current_suite && g_current_case) {
        printf("FAIL: %s.%s:%d: %s\n", g_current_suite->id, g_current_case->id, line, msg);
    } else {
        printf("FAIL: %s:%d: %s\n", file, line, msg);
    }
    if (jump && g_jmp_active) {
        longjmp(g_jmp, BAKE_JMP_FAIL);
    }
}

static const char* bake_lookup_param(const char *name) {
    size_t len = strlen(name);
    for (int i = 0; i < g_cli_param_count; i++) {
        const char *p = g_cli_params[i];
        if (!strncmp(p, name, len) && p[len] == '=') {
            return p + len + 1;
        }
    }

    if (g_current_suite) {
        for (uint32_t i = 0; i < g_current_suite->param_count; i++) {
            bake_test_param *param = &g_current_suite->params[i];
            if (!strcmp(param->name, name)) {
                if (param->value_cur >= 0 && param->value_cur < param->value_count) {
                    return param->values[param->value_cur];
                }
            }
        }
    }

    return NULL;
}

const char* test_param(const char *name) {
    return bake_lookup_param(name);
}

void test_is_flaky(void) {
    g_flaky = true;
}

void test_quarantine(const char *date) {
    g_quarantine_date = date ? date : "unknown";
    if (g_jmp_active) {
        longjmp(g_jmp, BAKE_JMP_QUARANTINE);
    }
}

void test_expect_abort(void) {
    g_expect_abort = true;
    signal(SIGABRT, bake_abort_handler);
}

static void bake_abort_handler(int sig) {
    (void)sig;
    if (g_expect_abort) {
        g_observed_abort = true;
        if (g_jmp_active) {
            longjmp(g_jmp, BAKE_JMP_ABORT);
        }
        exit(0);
    }

    bake_set_failure(__FILE__, __LINE__, "unexpected abort", true);
    exit(-1);
}

void test_abort(void) {
    bake_abort_handler(SIGABRT);
}

bool _if_test_assert(bool cond, const char *cond_str, const char *file, int line) {
    if (g_current_suite) {
        g_current_suite->assert_count ++;
    }
    if (!cond) {
        char msg[512];
        snprintf(msg, sizeof(msg), "assert(%s)", cond_str);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%lld) != %s (%lld)", str_v1, (long long)v1, str_v2, (long long)v2);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%llu) != %s (%llu)", str_v1, (unsigned long long)v1, str_v2, (unsigned long long)v2);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%s) != %s (%s)", str_v1, v1 ? "true" : "false", str_v2, v2 ? "true" : "false");
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    double d = fabs(v1 - v2);
    if (d > 0.000001) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%f) != %s (%f)", str_v1, v1, str_v2, v2);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    bool equal = false;
    if (!v1 && !v2) {
        equal = true;
    } else if (v1 && v2 && !strcmp(v1, v2)) {
        equal = true;
    }

    if (!equal) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "%s (%s) != %s (%s)", str_v1, v1 ? v1 : "NULL", str_v2, v2 ? v2 : "NULL");
        bake_set_failure(file, line, msg, false);
        return false;
    }

    return true;
}

bool _if_test_null(void *v, const char *str_v, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v != NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s is not NULL", str_v);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_not_null(void *v, const char *str_v, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v == NULL) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s is NULL", str_v);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

bool _if_test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (g_current_suite) { g_current_suite->assert_count ++; }
    if (v1 != v2) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s (%p) != %s (%p)", str_v1, v1, str_v2, v2);
        bake_set_failure(file, line, msg, false);
        return false;
    }
    return true;
}

void _test_assert(bool cond, const char *cond_str, const char *file, int line) {
    if (!_if_test_assert(cond, cond_str, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_int(int64_t v1, int64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_int(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_uint(uint64_t v1, uint64_t v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_uint(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_bool(bool v1, bool v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_bool(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_flt(double v1, double v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_flt(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_str(const char *v1, const char *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_str(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_null(void *v, const char *str_v, const char *file, int line) {
    if (!_if_test_null(v, str_v, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_not_null(void *v, const char *str_v, const char *file, int line) {
    if (!_if_test_not_null(v, str_v, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}
void _test_ptr(const void *v1, const void *v2, const char *str_v1, const char *str_v2, const char *file, int line) {
    if (!_if_test_ptr(v1, v2, str_v1, str_v2, file, line) && g_jmp_active) { longjmp(g_jmp, BAKE_JMP_FAIL); }
}

static int bake_run_case(bake_test_suite *suite, bake_test_case *tc) {
    g_current_suite = suite;
    g_current_case = tc;
    suite->assert_count = 0;
    g_expect_abort = false;
    g_observed_abort = false;
    g_failed = false;
    g_flaky = false;
    g_quarantine_date = NULL;
    signal(SIGABRT, bake_abort_handler);

    if (suite->setup) {
        suite->setup();
    }

    g_jmp_active = 1;
    int jmp_rc = setjmp(g_jmp);
    if (jmp_rc == 0) {
        tc->function();
    }
    g_jmp_active = 0;
    signal(SIGABRT, SIG_DFL);

    if (suite->teardown) {
        suite->teardown();
    }

    if (g_quarantine_date) {
        printf("SKIP: %s.%s: test was quarantined on %s\n", suite->id, tc->id, g_quarantine_date);
        return BAKE_TEST_QUARANTINED;
    }

    if (g_expect_abort && !g_observed_abort) {
        bake_set_failure(__FILE__, __LINE__, "expected abort signal", false);
    }

    if (g_failed) {
        if (g_flaky) {
            printf("FLAKY %s.%s\n", suite->id, tc->id);
            return 0;
        }
        return -1;
    }

    return 0;
}

static const char* bake_lookup_cli_param_only(const char *name) {
    size_t len = strlen(name);
    for (int i = 0; i < g_cli_param_count; i++) {
        const char *p = g_cli_params[i];
        if (!strncmp(p, name, len) && p[len] == '=') {
            return p + len + 1;
        }
    }
    return NULL;
}

static int bake_run_suite(const char *test_id, const char *exec, bake_test_suite *suite, int *pass_out, int *fail_out, int *empty_out) {
    int rc = 0;
    int pass = 0;
    int fail = 0;
    int empty = 0;
    for (uint32_t i = 0; i < suite->testcase_count; i++) {
        if (g_interrupted) {
            return -2;
        }

        char cmd[4096];
        int written = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s.%s\"", exec, suite->id, suite->testcases[i].id);
        if (written < 0 || (size_t)written >= sizeof(cmd)) {
            fail ++;
            rc = -1;
            continue;
        }

        for (int p = 0; p < g_cli_param_count; p++) {
            size_t used = strlen(cmd);
            if ((used + strlen(g_cli_params[p]) + 12) >= sizeof(cmd)) {
                break;
            }
            strcat(cmd, " --param ");
            strcat(cmd, g_cli_params[p]);
        }

        for (uint32_t p = 0; p < suite->param_count; p++) {
            bake_test_param *param = &suite->params[p];
            if (bake_lookup_cli_param_only(param->name)) {
                continue;
            }
            if (param->value_cur < 0 || param->value_cur >= param->value_count) {
                continue;
            }
            const char *value = param->values[param->value_cur];
            size_t used = strlen(cmd);
            if ((used + strlen(param->name) + strlen(value) + 16) >= sizeof(cmd)) {
                break;
            }
            strcat(cmd, " --param ");
            strcat(cmd, param->name);
            strcat(cmd, "=");
            strcat(cmd, value);
        }

        int sig = 0;
        int test_rc = bake_run_subprocess(cmd, &sig);
        if (g_interrupted || sig == SIGINT || test_rc == 130) {
            g_interrupted = 1;
            return -2;
        }
        if (test_rc == 0) {
            pass ++;
            continue;
        }

        if (test_rc == BAKE_TEST_QUARANTINED) {
            continue;
        }

        if (test_rc == BAKE_TEST_EMPTY) {
            empty ++;
            bake_print_debug_command(exec, suite, &suite->testcases[i]);
            continue;
        }

        if (sig) {
            printf("FAIL: %s.%s exited with signal %d\n", suite->id, suite->testcases[i].id, sig);
        }
        fail ++;
        rc = -1;
        bake_print_debug_command(exec, suite, &suite->testcases[i]);
    }

    char param_str[512] = {0};
    bake_append_suite_params(param_str, sizeof(param_str), suite);
    bake_print_report(test_id, suite->id, param_str, pass, fail, empty);
    if (fail || empty) {
        printf("\n");
    }

    if (pass_out) {
        *pass_out += pass;
    }
    if (fail_out) {
        *fail_out += fail;
    }
    if (empty_out) {
        *empty_out += empty;
    }
    return rc;
}

static int bake_run_suite_for_params(const char *test_id, const char *exec, bake_test_suite *suite, uint32_t param, int *pass, int *fail, int *empty) {
    if (!suite->param_count || param >= suite->param_count) {
        return bake_run_suite(test_id, exec, suite, pass, fail, empty);
    }

    int rc = 0;
    bake_test_param *p = &suite->params[param];
    for (int32_t i = 0; i < p->value_count; i++) {
        p->value_cur = i;
        int suite_rc = bake_run_suite_for_params(test_id, exec, suite, param + 1, pass, fail, empty);
        if (suite_rc == -2) {
            return -2;
        }
        if (suite_rc != 0) {
            rc = -1;
        }
    }
    return rc;
}

#if !defined(_WIN32)
typedef struct bake_suite_jobs_t {
    const char *test_id;
    const char *exec;
    bake_test_suite *suites;
    uint32_t suite_count;
    uint32_t next_suite;
    int pass;
    int fail;
    int empty;
    int rc;
    pthread_mutex_t lock;
} bake_suite_jobs_t;

static void* bake_suite_worker(void *arg) {
    bake_suite_jobs_t *jobs = arg;
    for (;;) {
        if (g_interrupted) {
            return NULL;
        }

        pthread_mutex_lock(&jobs->lock);
        uint32_t suite_index = jobs->next_suite;
        if (suite_index < jobs->suite_count) {
            jobs->next_suite ++;
        }
        pthread_mutex_unlock(&jobs->lock);

        if (suite_index >= jobs->suite_count) {
            break;
        }

        int pass = 0;
        int fail = 0;
        int empty = 0;
        int suite_rc = bake_run_suite_for_params(jobs->test_id, jobs->exec, &jobs->suites[suite_index], 0, &pass, &fail, &empty);

        pthread_mutex_lock(&jobs->lock);
        jobs->pass += pass;
        jobs->fail += fail;
        jobs->empty += empty;
        if (suite_rc == -2) {
            g_interrupted = 1;
        } else if (suite_rc != 0) {
            jobs->rc = -1;
        }
        pthread_mutex_unlock(&jobs->lock);
    }
    return NULL;
}

static int bake_run_suites_parallel(const char *test_id, const char *exec, bake_test_suite *suites, uint32_t suite_count, int jobs_count, int *pass, int *fail, int *empty) {
    if (jobs_count < 1) {
        jobs_count = 1;
    }
    if ((uint32_t)jobs_count > suite_count) {
        jobs_count = (int)suite_count;
    }
    if (jobs_count < 2) {
        return 1;
    }

    bake_suite_jobs_t jobs = {
        .test_id = test_id,
        .exec = exec,
        .suites = suites,
        .suite_count = suite_count,
        .next_suite = 0,
        .pass = 0,
        .fail = 0,
        .empty = 0,
        .rc = 0
    };
    pthread_mutex_init(&jobs.lock, NULL);

    pthread_t *threads = calloc((size_t)jobs_count, sizeof(pthread_t));
    if (!threads) {
        pthread_mutex_destroy(&jobs.lock);
        return -1;
    }

    for (int i = 0; i < jobs_count; i++) {
        pthread_create(&threads[i], NULL, bake_suite_worker, &jobs);
    }
    for (int i = 0; i < jobs_count; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&jobs.lock);
    free(threads);

    *pass += jobs.pass;
    *fail += jobs.fail;
    *empty += jobs.empty;
    return jobs.rc;
}
#endif

static void bake_list_tests(bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {
            printf("%s.%s\n", suites[s].id, suites[s].testcases[t].id);
        }
    }
}

static void bake_list_suites(bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        printf("%s\n", suites[s].id);
    }
}

static void bake_list_commands(const char *exec, bake_test_suite *suites, uint32_t suite_count) {
    for (uint32_t s = 0; s < suite_count; s++) {
        for (uint32_t t = 0; t < suites[s].testcase_count; t++) {
            printf("%s %s.%s\n", exec, suites[s].id, suites[s].testcases[t].id);
        }
    }
}

static bake_test_suite* bake_find_suite(bake_test_suite *suites, uint32_t suite_count, const char *id) {
    for (uint32_t i = 0; i < suite_count; i++) {
        if (!strcmp(suites[i].id, id)) {
            return &suites[i];
        }
    }
    return NULL;
}

static int bake_run_single_test(bake_test_suite *suites, uint32_t suite_count, const char *id) {
    const char *dot = strchr(id, '.');
    if (!dot) {
        return -1;
    }

    char suite_id[256];
    size_t suite_len = (size_t)(dot - id);
    if (suite_len >= sizeof(suite_id)) {
        return -1;
    }
    memcpy(suite_id, id, suite_len);
    suite_id[suite_len] = '\0';

    const char *case_id = dot + 1;
    bake_test_suite *suite = bake_find_suite(suites, suite_count, suite_id);
    if (!suite) {
        printf("test suite '%s' not found\n", suite_id);
        return -1;
    }

    for (uint32_t i = 0; i < suite->testcase_count; i++) {
        if (!strcmp(suite->testcases[i].id, case_id)) {
            return bake_run_case(suite, &suite->testcases[i]);
        }
    }

    printf("testcase '%s' not found\n", id);
    return -1;
}

int bake_test_run(const char *test_id, int argc, char *argv[], bake_test_suite *suites, uint32_t suite_count) {
    if (!test_id || !test_id[0]) {
        test_id = "test";
    }

    const char *single_test = NULL;
    const char *suite_filter = NULL;
    int job_count = 1;

    g_cli_param_count = 0;
    g_interrupted = 0;
    void (*prev_sigint)(int) = signal(SIGINT, bake_interrupt_handler);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--list-tests")) {
            bake_list_tests(suites, suite_count);
            signal(SIGINT, prev_sigint);
            return 0;
        }
        if (!strcmp(arg, "--list-suites")) {
            bake_list_suites(suites, suite_count);
            signal(SIGINT, prev_sigint);
            return 0;
        }
        if (!strcmp(arg, "--list-commands")) {
            bake_list_commands(argv[0], suites, suite_count);
            signal(SIGINT, prev_sigint);
            return 0;
        }
        if (!strcmp(arg, "--param")) {
            if ((i + 1) < argc && strchr(argv[i + 1], '=')) {
                if (g_cli_param_count < BAKE_TEST_PARAM_MAX) {
                    g_cli_params[g_cli_param_count ++] = argv[i + 1];
                }
                i ++;
                continue;
            }
            printf("invalid --param argument\n");
            signal(SIGINT, prev_sigint);
            return -1;
        }
        if (!strcmp(arg, "-j")) {
            if ((i + 1) < argc) {
                int parsed_jobs = atoi(argv[i + 1]);
                if (parsed_jobs > 0) {
                    job_count = parsed_jobs;
                }
                i ++;
                continue;
            }
            printf("missing value for -j\n");
            signal(SIGINT, prev_sigint);
            return -1;
        }

        if (strchr(arg, '.')) {
            single_test = arg;
        } else {
            suite_filter = arg;
        }
    }

    if (single_test) {
        int run_rc = bake_run_single_test(suites, suite_count, single_test);
        signal(SIGINT, prev_sigint);
        return run_rc;
    }

    int pass = 0;
    int fail = 0;
    int empty = 0;
    int rc = 0;

    if (suite_filter) {
        bake_test_suite *suite = bake_find_suite(suites, suite_count, suite_filter);
        if (!suite) {
            printf("test suite '%s' not found\n", suite_filter);
            signal(SIGINT, prev_sigint);
            return -1;
        }
        int suite_rc = bake_run_suite_for_params(test_id, argv[0], suite, 0, &pass, &fail, &empty);
        if (suite_rc == -2) {
            g_interrupted = 1;
        } else if (suite_rc != 0) {
            rc = -1;
        }
    } else {
#if !defined(_WIN32)
        int parallel_rc = bake_run_suites_parallel(test_id, argv[0], suites, suite_count, job_count, &pass, &fail, &empty);
        if (parallel_rc == -1) {
            rc = -1;
        }
        if (parallel_rc == 1)
#endif
        {
            for (uint32_t s = 0; s < suite_count; s++) {
                int suite_rc = bake_run_suite_for_params(test_id, argv[0], &suites[s], 0, &pass, &fail, &empty);
                if (suite_rc == -2) {
                    g_interrupted = 1;
                    break;
                }
                if (suite_rc != 0) {
                    rc = -1;
                }
            }
        }
        if (!g_interrupted) {
            printf("-----------------------------\n");
            bake_print_report(test_id, "all", "", pass, fail, empty);
        }
    }

    signal(SIGINT, prev_sigint);
    if (g_interrupted) {
        signal(SIGINT, SIG_DFL);
        raise(SIGINT);
        return -1;
    }

    return rc;
}
