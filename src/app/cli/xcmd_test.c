/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_test.c - 'xray test' command implementation
 *
 * KEY CONCEPT:
 *   Unified execution model: @test functions run on the same main_coro
 *   as top-level code (via xr_coro_reset_for_call), ensuring identical
 *   semantics with direct execution (xray file.xr). This makes JIT
 *   behavior consistent between test and direct modes.
 *
 *   Parallel execution: test files run concurrently on a thread pool
 *   (each file gets its own isolate). -j N controls parallelism.
 *
 * WHY THIS DESIGN:
 *   - Previous design created a new coroutine per @test, causing JIT
 *     context mismatch and making JIT diff testing impossible.
 *   - Reusing main_coro matches xr_execute() semantics exactly.
 *   - File-level parallelism is natural (each isolate is independent).
 */

#include "xcli.h"
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../api/xtest_runner.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xexec_state.h"
#include "../../module/xmodule.h"
#include "../../vm/xvm_internal.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xworker.h"
#include "../../frontend/parser/xparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../../base/xmalloc.h"

#define TEST_FILE_TIMEOUT_SEC 120

/* ========== Per-File Result (thread-safe, no shared state) ========== */

typedef struct {
    char filepath[1024];

    // Counters
    int test_count;
    int passed, failed, errors, skipped, timeout;
    double duration_ms;

    // Failure details (owned copies)
    XrTestFailureRecord *failures;
    int failure_count;
    int failure_cap;

    // Compilation/execution error
    bool has_error;
    char error_msg[256];
} XrTestFileResult;

static void file_result_add_failure(XrTestFileResult *r, const char *test_name,
                                    const char *message, XrTestStatus status) {
    if (r->failure_count >= r->failure_cap) {
        r->failure_cap = r->failure_cap == 0 ? 8 : r->failure_cap * 2;
        r->failures = realloc(r->failures, r->failure_cap * sizeof(XrTestFailureRecord));
    }
    XrTestFailureRecord *rec = &r->failures[r->failure_count++];
    rec->file = strdup(r->filepath);
    rec->test_name = strdup(test_name ? test_name : "<anonymous>");
    rec->message = strdup(message ? message : "");
    rec->status = status;
}

static void file_result_free(XrTestFileResult *r) {
    for (int i = 0; i < r->failure_count; i++) {
        free(r->failures[i].file);
        free(r->failures[i].test_name);
        free(r->failures[i].message);
    }
    free(r->failures);
}

/* ========== Display Helpers ========== */

// Extract filename without .xr extension into caller-provided buffer
static void get_display_name(const char *filepath, char *buf, size_t bufsz) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    strncpy(buf, base, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot && strcmp(dot, ".xr") == 0) *dot = '\0';
}

#define get_time_ms() cli_get_time_ms()

/* ========== Closure Lookup ========== */

// Find runtime closure in shared array that matches the given proto.
// After top-level execution, closures with upvalue bindings are stored
// in the shared array via OP_SETSHARED. This lets us recover the live
// closure (with proper upvalue pointers) for each @test function.
static XrClosure *find_closure_for_proto(XrayIsolate *X, XrProto *target_proto) {
    XrVMState *vm = xr_isolate_get_vm_state(X);
    XrSharedArray *shared = &vm->shared;
    for (int i = 0; i < shared->count; i++) {
        XrValue val = shared->data[i];
        if (xr_value_is_closure(val)) {
            XrClosure *closure = xr_value_to_closure(val);
            if (closure && closure->proto == target_proto)
                return closure;
        }
    }
    return NULL;
}

// Get or create closure for a test/hook proto
static XrClosure *get_test_closure(XrayIsolate *X, XrProto *proto) {
    XrClosure *closure = find_closure_for_proto(X, proto);
    if (!closure)
        closure = xr_closure_new(X, proto, xr_isolate_get_main_coro(X));
    return closure;
}

/* ========== Error Extraction ========== */

static const char *extract_coro_error(XrCoroutine *coro) {
    if (!coro) return "unknown error";
    XrValue err = coro->error;
    if (XR_IS_STRING(err)) {
        XrString *s = (XrString *)XR_TO_PTR(err);
        if (s && s->data[0] != '\0') return s->data;
    }
    return "test failed";
}

/* ========== File-Level Watchdog ========== */

typedef struct {
    XrayIsolate *X;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool done;
    int timeout_sec;
} FileWatchdog;

static void *file_watchdog_thread(void *arg) {
    FileWatchdog *wd = (FileWatchdog *)arg;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += wd->timeout_sec;

    pthread_mutex_lock(&wd->mutex);
    while (!wd->done) {
        if (pthread_cond_timedwait(&wd->cond, &wd->mutex, &deadline) == ETIMEDOUT) {
            if (!wd->done) {
                XrRuntime *runtime = (XrRuntime *)xr_isolate_get_vm_state(wd->X)->runtime;
                if (runtime) xr_runtime_force_stop(runtime);
            }
            break;
        }
    }
    pthread_mutex_unlock(&wd->mutex);
    return NULL;
}

static void watchdog_start(FileWatchdog *wd, XrayIsolate *X, int timeout_sec, pthread_t *tid) {
    wd->X = X;
    wd->done = false;
    wd->timeout_sec = timeout_sec;
    pthread_mutex_init(&wd->mutex, NULL);
    pthread_cond_init(&wd->cond, NULL);
    pthread_create(tid, NULL, file_watchdog_thread, wd);
}

static void watchdog_stop(FileWatchdog *wd, pthread_t tid) {
    pthread_mutex_lock(&wd->mutex);
    wd->done = true;
    pthread_cond_signal(&wd->cond);
    pthread_mutex_unlock(&wd->mutex);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&wd->mutex);
    pthread_cond_destroy(&wd->cond);
}

/* ========== Unified Test Execution (reuse main_coro) ========== */

// Run a closure on main_coro (identical semantics to xr_execute).
// Returns 0 on success, -1 on failure.
static int run_inline(XrayIsolate *X, XrClosure *closure) {
    XrCoroutine *main_coro = xr_isolate_get_main_coro(X);
    xr_coro_reset_for_call(main_coro, X, closure);
    xr_main_thread_run(X, main_coro);

    if (xr_coro_flags_has(main_coro, XR_CORO_FLG_DONE)
        && !xr_coro_flags_has(main_coro, XR_CORO_FLG_CANCELLED)
        && !XR_IS_STRING(main_coro->error)) {
        return 0;
    }
    return -1;
}

// Run hook functions (before_all, after_all, before_each, after_each).
// Returns 0 if all hooks succeeded, -1 on first failure.
static int run_hooks(XrayIsolate *X, XrTestFunc *hooks, int count) {
    for (int i = 0; i < count; i++) {
        XrClosure *closure = get_test_closure(X, hooks[i].proto);
        if (!closure) return -1;
        if (run_inline(X, closure) != 0) return -1;
    }
    return 0;
}

/* ========== Run Single Test File ========== */

static void run_test_file(const char *filepath, XrTestConfig *config,
                          bool jitless, bool jit_force, XrTestFileResult *result) {
    memset(result, 0, sizeof(*result));
    strncpy(result->filepath, filepath, sizeof(result->filepath) - 1);

    // Create fresh isolate
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    params.enable_jit = !jitless;
    if (jit_force) params.jit_threshold = 1;
    XrayIsolate *X = xray_isolate_new(&params);
    if (!X) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "failed to create isolate");
        result->errors = 1;
        return;
    }
    xr_isolate_set_suppress_exception_print(X, true);
    xr_multicore_init(X, 0);
    xray_isolate_set_script_info(X, filepath, 0, NULL);
    xr_module_system_init_with_script(X, filepath);

    // Read + Parse + Compile
    char *source = cli_read_file(filepath);
    if (!source) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "cannot read file");
        result->errors = 1;
        goto cleanup_isolate;
    }

    AstNode *ast = xr_parse_with_source(X, source, filepath);
    if (!ast) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "parse failed");
        result->errors = 1;
        goto cleanup_source;
    }

    XrProto *proto = xr_compile_ast_with_source(X, ast, filepath);
    if (!proto) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "compile failed");
        result->errors = 1;
        goto cleanup_ast;
    }

    // Discover @test functions
    XrTestSuite *suite = xr_test_discover(proto, filepath);
    if (suite->test_count == 0) {
        result->test_count = 0;
        goto cleanup_suite;
    }
    result->test_count = suite->test_count;

    // Start file-level watchdog
    FileWatchdog wd;
    pthread_t wd_tid;
    watchdog_start(&wd, X, TEST_FILE_TIMEOUT_SEC, &wd_tid);

    double file_start = get_time_ms();

    // Execute top-level code (imports, shared vars, function definitions)
    int exec_result = xr_execute(X, proto);
    if (exec_result != 0) {
        result->has_error = true;
        snprintf(result->error_msg, sizeof(result->error_msg), "top-level execution failed");
        result->errors = 1;
        watchdog_stop(&wd, wd_tid);
        goto cleanup_suite;
    }

    // Run @before_all hooks
    if (suite->before_all_count > 0) {
        if (run_hooks(X, suite->before_all, suite->before_all_count) != 0) {
            for (int i = 0; i < suite->test_count; i++) {
                result->errors++;
                const char *tname = suite->tests[i].proto->name
                    ? suite->tests[i].proto->name->data : "<anonymous>";
                file_result_add_failure(result, tname, "@before_all failed", TEST_ERROR);
            }
            watchdog_stop(&wd, wd_tid);
            result->duration_ms = get_time_ms() - file_start;
            goto cleanup_suite;
        }
    }

    // Run each @test
    for (int i = 0; i < suite->test_count; i++) {
        XrTestFunc *test = &suite->tests[i];
        const char *tname = test->proto->name ? test->proto->name->data : "<anonymous>";

        // @test(skip)
        if (test->attr == ATTR_TEST_SKIP) {
            result->skipped++;
            continue;
        }

        // Filter
        if (config->filter && !strstr(tname, config->filter)) {
            result->skipped++;
            continue;
        }

        // Run @before_each
        if (suite->before_each_count > 0) {
            if (run_hooks(X, suite->before_each, suite->before_each_count) != 0) {
                result->errors++;
                file_result_add_failure(result, tname, "@before_each failed", TEST_ERROR);
                if (config->fail_fast) break;
                continue;
            }
        }

        // Run @test (unified model: reuse main_coro)
        double test_start = get_time_ms();
        XrClosure *closure = get_test_closure(X, test->proto);
        if (!closure) {
            result->errors++;
            file_result_add_failure(result, tname, "failed to create closure", TEST_ERROR);
            if (config->fail_fast) break;
            continue;
        }

        int rc = run_inline(X, closure);
        double test_dur = get_time_ms() - test_start;

        // Determine result
        if (test->timeout > 0 && test_dur > test->timeout * 1000.0) {
            result->timeout++;
            file_result_add_failure(result, tname, "exceeded timeout", TEST_TIMEOUT);
        } else if (rc == 0) {
            result->passed++;
        } else {
            result->failed++;
            XrCoroutine *main_coro = xr_isolate_get_main_coro(X);
            file_result_add_failure(result, tname, extract_coro_error(main_coro), TEST_FAILED);
        }

        // Run @after_each (even if test failed)
        if (suite->after_each_count > 0) {
            run_hooks(X, suite->after_each, suite->after_each_count);
        }

        if (config->fail_fast && (result->failed + result->errors + result->timeout) > 0)
            break;
    }

    // Run @after_all hooks
    if (suite->after_all_count > 0) {
        run_hooks(X, suite->after_all, suite->after_all_count);
    }

    watchdog_stop(&wd, wd_tid);
    result->duration_ms = get_time_ms() - file_start;

cleanup_suite:
    xr_test_suite_free(suite);
    xr_free_code(X, proto);
cleanup_ast:
    xr_ast_free(X, ast);
cleanup_source:
    free(source);
cleanup_isolate:
    xr_multicore_destroy(X);
    xray_isolate_delete(X);
}

/* ========== File Collection ========== */

typedef struct {
    char **paths;
    int count;
    int capacity;
} XrFileList;

static void filelist_add(XrFileList *fl, const char *path) {
    if (fl->count >= fl->capacity) {
        fl->capacity = fl->capacity == 0 ? 64 : fl->capacity * 2;
        fl->paths = realloc(fl->paths, fl->capacity * sizeof(char *));
    }
    fl->paths[fl->count++] = strdup(path);
}

static void filelist_free(XrFileList *fl) {
    for (int i = 0; i < fl->count; i++) free(fl->paths[i]);
    free(fl->paths);
}

static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

// Build clean path without double slashes
static void build_path(char *buf, size_t size, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    while (dlen > 0 && dir[dlen - 1] == '/') dlen--;
    snprintf(buf, size, "%.*s/%s", (int)dlen, dir, name);
}

static void collect_files_recursive(const char *path, XrFileList *fl) {
    DIR *dir = opendir(path);
    if (!dir) return;

    // Collect entries first for sorted order
    char **subdirs = NULL;
    int ndir = 0, dcap = 0;
    char **xrfiles = NULL;
    int nfile = 0, fcap = 0;

    struct dirent *entry;
    char filepath[1024];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || entry->d_name[0] == '_') continue;
        build_path(filepath, sizeof(filepath), path, entry->d_name);
        struct stat st;
        if (stat(filepath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (ndir >= dcap) {
                dcap = dcap == 0 ? 16 : dcap * 2;
                subdirs = realloc(subdirs, dcap * sizeof(char *));
            }
            subdirs[ndir++] = strdup(filepath);
        } else if (S_ISREG(st.st_mode) && cli_is_xr_file(entry->d_name)) {
            if (nfile >= fcap) {
                fcap = fcap == 0 ? 16 : fcap * 2;
                xrfiles = realloc(xrfiles, fcap * sizeof(char *));
            }
            xrfiles[nfile++] = strdup(filepath);
        }
    }
    closedir(dir);

    if (nfile > 1) qsort(xrfiles, nfile, sizeof(char *), cmp_strings);
    if (ndir > 1) qsort(subdirs, ndir, sizeof(char *), cmp_strings);

    for (int i = 0; i < nfile; i++) { filelist_add(fl, xrfiles[i]); free(xrfiles[i]); }
    free(xrfiles);

    for (int i = 0; i < ndir; i++) { collect_files_recursive(subdirs[i], fl); free(subdirs[i]); }
    free(subdirs);
}

/* ========== Parallel Execution ========== */

typedef struct {
    char **files;
    int file_count;
    _Atomic int next_idx;
    XrTestFileResult *results;
    XrTestConfig config;
    bool jitless;
    bool jit_force;
} XrTestParallelCtx;

static void *test_worker_thread(void *arg) {
    XrTestParallelCtx *ctx = (XrTestParallelCtx *)arg;
    while (1) {
        int idx = atomic_fetch_add(&ctx->next_idx, 1);
        if (idx >= ctx->file_count) break;
        run_test_file(ctx->files[idx], &ctx->config, ctx->jitless, ctx->jit_force,
                      &ctx->results[idx]);
    }
    return NULL;
}

/* ========== Output Formatting ========== */

// Compute max display name length for dot-padding alignment
static int compute_align_width(char **files, int count) {
    int max_w = 0;
    for (int i = 0; i < count; i++) {
        char buf[256];
        get_display_name(files[i], buf, sizeof(buf));
        int len = (int)strlen(buf);
        if (len > max_w) max_w = len;
    }
    return max_w;
}

// Print results for a single file
static void print_file_result(XrTestFileResult *r, int align_width, bool verbose) {
    if (r->test_count == 0) return;

    char name[256];
    get_display_name(r->filepath, name, sizeof(name));

    if (r->has_error) {
        printf("   " CLR_RED "x" CLR_RESET " %s: %s\n", name, r->error_msg);
        return;
    }

    int problems = r->failed + r->errors + r->timeout;
    int ran = r->passed + r->failed + r->errors + r->timeout;

    if (verbose) {
        const char *plural = (r->test_count == 1) ? "" : "s";
        printf("   " CLR_CYAN "%s" CLR_RESET " " CLR_DIM "(%d test%s)" CLR_RESET "\n",
               name, r->test_count, plural);

        // Print individual failures
        for (int i = 0; i < r->failure_count; i++) {
            XrTestFailureRecord *f = &r->failures[i];
            const char *color = (f->status == TEST_TIMEOUT) ? CLR_YELLOW : CLR_RED;
            printf("     %sx" CLR_RESET " %s: %s\n", color, f->test_name, f->message);
        }

        // Summary line
        if (problems > 0) {
            printf("     " CLR_DIM "%d passed," CLR_RESET " " CLR_RED "%d failed" CLR_RESET,
                   r->passed, problems);
        } else {
            printf("     " CLR_GREEN "%d passed" CLR_RESET, r->passed);
        }
        if (r->skipped > 0) printf(CLR_DIM ", %d skipped" CLR_RESET, r->skipped);
        printf("  " CLR_DIM "(%.0fms)" CLR_RESET "\n", r->duration_ms);
    } else {
        if (problems == 0) {
            int name_len = (int)strlen(name);
            int aw = align_width > name_len ? align_width : name_len;
            int dots = aw - name_len + 4;
            if (dots < 3) dots = 3;
            if (dots > 255) dots = 255;
            char dot_buf[256];
            dot_buf[0] = ' ';
            for (int d = 1; d < dots; d++) dot_buf[d] = '.';
            dot_buf[dots] = '\0';

            printf("   " CLR_GREEN "+" CLR_RESET " %s" CLR_DIM "%s" CLR_RESET " %d/%d",
                   name, dot_buf, r->passed, ran + r->skipped);
            if (r->skipped > 0) printf("  " CLR_DIM "%d skipped" CLR_RESET, r->skipped);
            printf("  " CLR_DIM "(%.0fms)" CLR_RESET "\n", r->duration_ms);
        } else {
            printf("   " CLR_RED "x" CLR_RESET " %s " CLR_DIM "(%d test%s)" CLR_RESET "\n",
                   name, r->test_count, r->test_count == 1 ? "" : "s");
            for (int i = 0; i < r->failure_count; i++) {
                XrTestFailureRecord *f = &r->failures[i];
                const char *color = (f->status == TEST_TIMEOUT) ? CLR_YELLOW : CLR_RED;
                printf("       %sx" CLR_RESET " %s: %s\n", color, f->test_name, f->message);
            }
            printf("     " CLR_DIM "%d passed," CLR_RESET " " CLR_RED "%d failed" CLR_RESET,
                   r->passed, problems);
            if (r->skipped > 0) printf(", %d skipped", r->skipped);
            printf("  " CLR_DIM "(%.0fms)" CLR_RESET "\n", r->duration_ms);
        }
    }
}

// Print directory group headers and file results in order
static void print_all_results(XrTestFileResult *results, char **files, int count,
                              int align_width, bool verbose) {
    const char *last_dir = NULL;
    char dir_buf[1024];
    for (int i = 0; i < count; i++) {
        if (results[i].test_count == 0 && !results[i].has_error) continue;

        // Extract directory name for group headers
        strncpy(dir_buf, files[i], sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) *last_slash = '\0';
        const char *dir_name = strrchr(dir_buf, '/');
        dir_name = dir_name ? dir_name + 1 : dir_buf;

        if (!last_dir || strcmp(last_dir, dir_buf) != 0) {
            last_dir = dir_buf;
            printf(" " CLR_BOLD "%s" CLR_RESET "\n", dir_name);
        }

        print_file_result(&results[i], align_width, verbose);
    }
}

// Print final summary report
static void print_summary(int file_count, int total_passed, int total_failed,
                          int total_errors, int total_skipped, int total_timeout,
                          double total_time_ms, const char *filter,
                          XrTestFileResult *results, int result_count) {
    int total_problems = total_failed + total_errors + total_timeout;

    // Failures detail section
    int total_failure_records = 0;
    for (int i = 0; i < result_count; i++)
        total_failure_records += results[i].failure_count;

    if (total_failure_records > 0) {
        printf("\n " CLR_RED CLR_BOLD "Failed Tests" CLR_RESET "\n\n");
        for (int i = 0; i < result_count; i++) {
            for (int j = 0; j < results[i].failure_count; j++) {
                XrTestFailureRecord *rec = &results[i].failures[j];
                char fname[256];
                get_display_name(rec->file, fname, sizeof(fname));
                printf("  " CLR_RED "\u2717" CLR_RESET " %s " CLR_DIM ">" CLR_RESET " %s\n",
                       fname, rec->test_name);
                if (rec->message[0] != '\0')
                    printf("    " CLR_DIM "%s" CLR_RESET "\n", rec->message);
            }
        }
    }

    // Summary
    printf("\n " CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" CLR_RESET "\n");

    printf(" " CLR_BOLD " Tests" CLR_RESET "  ");
    printf("%d file%s", file_count, file_count == 1 ? "" : "s");
    printf(CLR_DIM " | " CLR_RESET);
    if (total_problems == 0) {
        printf(CLR_GREEN CLR_BOLD "%d passed" CLR_RESET, total_passed);
    } else {
        printf(CLR_GREEN "%d passed" CLR_RESET, total_passed);
        printf(CLR_DIM " | " CLR_RESET);
        printf(CLR_RED CLR_BOLD "%d failed" CLR_RESET, total_problems);
    }
    if (total_skipped > 0) {
        printf(CLR_DIM " | " CLR_RESET);
        printf(CLR_DIM "%d skipped" CLR_RESET, total_skipped);
    }
    if (filter)
        printf("  " CLR_DIM "(filter: \"%s\")" CLR_RESET, filter);
    printf("\n");

    printf(" " CLR_BOLD "  Time" CLR_RESET "  ");
    if (total_time_ms >= 1000.0)
        printf("%.2fs\n", total_time_ms / 1000.0);
    else
        printf("%.0fms\n", total_time_ms);

    printf(" " CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" CLR_RESET "\n");

    if (total_problems == 0)
        printf("\n " CLR_GREEN CLR_BOLD "\u2713 All tests passed" CLR_RESET "\n\n");
    else
        printf("\n " CLR_RED CLR_BOLD "\u2717 %d test%s failed" CLR_RESET "\n\n",
               total_problems, total_problems == 1 ? "" : "s");
}

/* ========== CLI Entry Point ========== */

#define OPT_JIT_FORCE 256

static struct option test_long_options[] = {
    {"verbose",   no_argument,       0, 'v'},
    {"fail-fast", no_argument,       0, 'F'},
    {"filter",    required_argument, 0, 'f'},
    {"no-jit",    no_argument,       0, 'J'},
    {"jit-force", no_argument,       0, OPT_JIT_FORCE},
    {"quiet",     no_argument,       0, 'q'},
    {"help",      no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

void print_test_help(void) {
    printf("Usage: xray test [OPTIONS] <file or directory>\n");
    printf("\n");
    printf("Run Xray tests\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("    -v, --verbose          Verbose output (per-test details)\n");
    printf("    -q, --quiet            Quiet mode (exit code only, for scripting)\n");
    printf("    -F, --fail-fast        Stop on first failure\n");
    printf("    -f, --filter <pattern> Only run matching tests\n");
    printf("    -j <N>                 Parallel execution (N threads, default: 1)\n");
    printf("    --no-jit               Disable JIT compiler\n");
    printf("    --jit-force            Force JIT on first call\n");
    printf("    -h, --help             Show help\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("    xray test tests/test_math.xr      # Run single file\n");
    printf("    xray test tests/                   # Run all tests in directory\n");
    printf("    xray test -j8 tests/               # 8-thread parallel\n");
    printf("    xray test --verbose tests/         # Verbose output\n");
    printf("    xray test -f basic tests/          # Filter by pattern\n");
    printf("    xray test --quiet --jit-force t.xr # JIT diff testing\n");
    printf("\n");
}

int cmd_test(int argc, char **argv) {
    bool verbose = false;
    bool quiet = false;
    bool fail_fast = false;
    bool jitless = false;
    bool jit_force = false;
    const char *filter = NULL;
    const char *test_path = NULL;
    int num_threads = 1;

    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "vqFf:j:hJ", test_long_options, NULL)) != -1) {
        switch (opt) {
            case 'v': verbose = true; break;
            case 'q': quiet = true; break;
            case 'F': fail_fast = true; break;
            case 'f': filter = optarg; break;
            case 'j':
                if (!cli_parse_int(optarg, &num_threads) || num_threads < 1) {
                    fprintf(stderr, "Error: -j requires positive integer\n");
                    return 1;
                }
                break;
            case 'J': jitless = true; break;
            case OPT_JIT_FORCE: jit_force = true; break;
            case 'h': print_test_help(); return 0;
            default: print_test_help(); return 1;
        }
    }

    if (optind < argc) test_path = argv[optind];

    if (!test_path) {
        fprintf(stderr, "Error: please specify test file or directory\n");
        fprintf(stderr, "Usage: xray test <file.xr> or <directory>\n");
        return 1;
    }

    struct stat st;
    if (stat(test_path, &st) != 0) {
        fprintf(stderr, "Error: path does not exist '%s'\n", test_path);
        return 1;
    }

    // Collect test files
    XrFileList fl = {0};
    if (S_ISDIR(st.st_mode)) {
        collect_files_recursive(test_path, &fl);
    } else {
        filelist_add(&fl, test_path);
    }

    if (fl.count == 0) {
        if (!quiet) fprintf(stderr, "No test files found\n");
        filelist_free(&fl);
        return 0;
    }

    XrTestConfig config = { .verbose = verbose, .fail_fast = fail_fast, .filter = filter };

    // Allocate results
    XrTestFileResult *results = xr_calloc(fl.count, sizeof(XrTestFileResult));

    double total_start = get_time_ms();

    if (!quiet) printf("\n");

    if (num_threads <= 1 || fl.count == 1) {
        // Serial execution
        int aw = quiet ? 0 : compute_align_width(fl.paths, fl.count);
        char last_dir[1024] = "";
        for (int i = 0; i < fl.count; i++) {
            run_test_file(fl.paths[i], &config, jitless, jit_force, &results[i]);

            // In serial + non-quiet mode, print file result immediately
            if (!quiet) {
                // Print directory header if needed
                char dir_buf[1024];
                strncpy(dir_buf, fl.paths[i], sizeof(dir_buf) - 1);
                dir_buf[sizeof(dir_buf) - 1] = '\0';
                char *ls = strrchr(dir_buf, '/');
                if (ls) *ls = '\0';
                if (strcmp(last_dir, dir_buf) != 0) {
                    strncpy(last_dir, dir_buf, sizeof(last_dir) - 1);
                    const char *dn = strrchr(dir_buf, '/');
                    dn = dn ? dn + 1 : dir_buf;
                    if (results[i].test_count > 0 || results[i].has_error)
                        printf(" " CLR_BOLD "%s" CLR_RESET "\n", dn);
                }
                print_file_result(&results[i], aw, verbose);
            }

            // Fail-fast at file level
            if (fail_fast && (results[i].failed + results[i].errors + results[i].timeout) > 0)
                break;
        }
    } else {
        // Parallel execution
        XrTestParallelCtx pctx = {
            .files = fl.paths,
            .file_count = fl.count,
            .next_idx = 0,
            .results = results,
            .config = config,
            .jitless = jitless,
            .jit_force = jit_force,
        };

        int nworkers = num_threads;
        if (nworkers > fl.count) nworkers = fl.count;

        if (!quiet)
            printf(" " CLR_DIM "Running %d files on %d threads..." CLR_RESET "\n\n",
                   fl.count, nworkers);

        pthread_t *threads = xr_calloc(nworkers, sizeof(pthread_t));
        for (int i = 0; i < nworkers; i++)
            pthread_create(&threads[i], NULL, test_worker_thread, &pctx);
        for (int i = 0; i < nworkers; i++)
            pthread_join(threads[i], NULL);
        xr_free(threads);

        // Print results in file order
        if (!quiet) {
            int aw = compute_align_width(fl.paths, fl.count);
            print_all_results(results, fl.paths, fl.count, aw, verbose);
        }
    }

    double total_time = get_time_ms() - total_start;

    // Aggregate stats
    int file_count = 0, total_passed = 0, total_failed = 0;
    int total_errors = 0, total_skipped = 0, total_timeout = 0;
    for (int i = 0; i < fl.count; i++) {
        if (results[i].test_count > 0 || results[i].has_error) file_count++;
        total_passed += results[i].passed;
        total_failed += results[i].failed;
        total_errors += results[i].errors;
        total_skipped += results[i].skipped;
        total_timeout += results[i].timeout;
    }

    if (!quiet) {
        print_summary(file_count, total_passed, total_failed, total_errors,
                      total_skipped, total_timeout, total_time, filter,
                      results, fl.count);
    }

    int exit_code = (total_failed + total_errors + total_timeout) > 0 ? 1 : 0;

    // Cleanup
    for (int i = 0; i < fl.count; i++) file_result_free(&results[i]);
    xr_free(results);
    filelist_free(&fl);

    return exit_code;
}
