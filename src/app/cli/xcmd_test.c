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
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "../../api/xisolate_profile.h"
#include "xcli_output.h"
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
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../runtime/object/xexception.h"
#include <stdio.h>
#include <string.h>
#include "../../os/os_fs.h"
#include "../../os/os_dir.h"
#include "../../os/os_thread.h"
#include "../../os/os_time.h"
#include <stdatomic.h>

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

static void file_result_add_failure(XrTestFileResult *r, const char *test_name, const char *message,
                                    XrTestStatus status) {
    if (r->failure_count >= r->failure_cap) {
        r->failure_cap = r->failure_cap == 0 ? 8 : r->failure_cap * 2;
        XR_REALLOC_OR_ABORT(r->failures, r->failure_cap * sizeof(XrTestFailureRecord),
                            "xcmd_test failures grow");
    }
    XrTestFailureRecord *rec = &r->failures[r->failure_count++];
    rec->file = xr_strdup(r->filepath);
    rec->test_name = xr_strdup(test_name ? test_name : "<anonymous>");
    rec->message = xr_strdup(message ? message : "");
    rec->status = status;
}

static void file_result_free(XrTestFileResult *r) {
    for (int i = 0; i < r->failure_count; i++) {
        xr_free(r->failures[i].file);
        xr_free(r->failures[i].test_name);
        xr_free(r->failures[i].message);
    }
    xr_free(r->failures);
}

/* ========== Display Helpers ========== */

// Extract filename without .xr extension into caller-provided buffer
static void get_display_name(const char *filepath, char *buf, size_t bufsz) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    strncpy(buf, base, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot && strcmp(dot, ".xr") == 0)
        *dot = '\0';
}

#define get_time_ms() xr_cli_get_time_ms()

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
    if (!coro)
        return "unknown error";
    XrValue err = coro->error;
    if (XR_IS_STRING(err)) {
        XrString *s = (XrString *) XR_TO_PTR(err);
        if (s && s->data[0] != '\0')
            return s->data;
    } else if (coro->isolate && xr_value_is_exception(coro->isolate, err)) {
        // Coroutine errors now preserve the original Exception instance
        // (so linked-scope rethrow surfaces the right object — see F026).
        const char *m = xr_exception_get_message(coro->isolate, err);
        if (m && m[0] != '\0')
            return m;
    }
    return "test failed";
}

/* ========== File-Level Watchdog ========== */

typedef struct {
    XrayIsolate *X;
    xr_mutex_t mutex;
    xr_cond_t cond;
    bool done;
    int timeout_sec;
} FileWatchdog;

static void *file_watchdog_thread(void *arg) {
    FileWatchdog *wd = (FileWatchdog *) arg;
    // Track an absolute monotonic deadline so spurious wake-ups
    // do not extend the watchdog window.
    uint64_t deadline_ns = xr_time_monotonic_ns() + (uint64_t) wd->timeout_sec * 1000000000ULL;

    xr_mutex_lock(&wd->mutex);
    while (!wd->done) {
        uint64_t now_ns = xr_time_monotonic_ns();
        if (now_ns >= deadline_ns)
            break;
        bool signalled = xr_cond_wait_for_ns(&wd->cond, &wd->mutex, deadline_ns - now_ns);
        if (!signalled && !wd->done) {
            XrRuntime *runtime = (XrRuntime *) xr_isolate_get_vm_state(wd->X)->runtime;
            if (runtime)
                xr_runtime_force_stop(runtime);
            break;
        }
    }
    xr_mutex_unlock(&wd->mutex);
    return NULL;
}

static void watchdog_start(FileWatchdog *wd, XrayIsolate *X, int timeout_sec, xr_thread_t *tid) {
    wd->X = X;
    wd->done = false;
    wd->timeout_sec = timeout_sec;
    xr_mutex_init(&wd->mutex);
    xr_cond_init(&wd->cond);
    xr_thread_create(tid, file_watchdog_thread, wd);
}

static void watchdog_stop(FileWatchdog *wd, xr_thread_t tid) {
    xr_mutex_lock(&wd->mutex);
    wd->done = true;
    xr_cond_signal(&wd->cond);
    xr_mutex_unlock(&wd->mutex);
    xr_thread_join(tid, NULL);
    xr_mutex_destroy(&wd->mutex);
    xr_cond_destroy(&wd->cond);
}

/* ========== Unified Test Execution (reuse main_coro) ========== */

// Run a closure on main_coro (identical semantics to xr_execute).
// Returns 0 on success, -1 on failure.
static int run_inline(XrayIsolate *X, XrClosure *closure) {
    XrCoroutine *main_coro = xr_isolate_get_main_coro(X);
    xr_coro_reset_for_call(main_coro, X, closure);
    xr_main_thread_run(X, main_coro);

    if (xr_coro_flags_has(main_coro, XR_CORO_FLG_DONE) &&
        !xr_coro_flags_has(main_coro, XR_CORO_FLG_CANCELLED) && XR_IS_NULL(main_coro->error)) {
        return 0;
    }
    return -1;
}

// Run hook functions (before_all, after_all, before_each, after_each).
// Returns 0 if all hooks succeeded, -1 on first failure.
static int run_hooks(XrayIsolate *X, XrTestFunc *hooks, int count) {
    for (int i = 0; i < count; i++) {
        XrClosure *closure = get_test_closure(X, hooks[i].proto);
        if (!closure)
            return -1;
        if (run_inline(X, closure) != 0)
            return -1;
    }
    return 0;
}

/* ========== Run Single Test File ========== */

static void run_test_file(const char *filepath, XrTestConfig *config, bool jitless, bool jit_force,
                          XrTestFileResult *result) {
    memset(result, 0, sizeof(*result));
    strncpy(result->filepath, filepath, sizeof(result->filepath) - 1);

    // Create fresh isolate via profile factory
    XrayIsolateParams params;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_TEST, &params);
    params.enable_jit = !jitless;
    if (jit_force)
        params.jit_threshold = 1;
    XrayIsolate *X = xr_isolate_profile_create(&params);
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
    char *source = xr_cli_read_file(filepath);
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
    xr_thread_t wd_tid;
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
                const char *tname =
                    suite->tests[i].proto->name ? suite->tests[i].proto->name->data : "<anonymous>";
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
                if (config->fail_fast)
                    break;
                continue;
            }
        }

        // Run @test (unified model: reuse main_coro)
        double test_start = get_time_ms();
        XrClosure *closure = get_test_closure(X, test->proto);
        if (!closure) {
            result->errors++;
            file_result_add_failure(result, tname, "failed to create closure", TEST_ERROR);
            if (config->fail_fast)
                break;
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
    xr_program_destroy(ast);
cleanup_source:
    xr_free(source);
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
        XR_REALLOC_OR_ABORT(fl->paths, fl->capacity * sizeof(char *), "xcmd_test filelist grow");
    }
    fl->paths[fl->count++] = xr_strdup(path);
}

static void filelist_free(XrFileList *fl) {
    for (int i = 0; i < fl->count; i++)
        xr_free(fl->paths[i]);
    xr_free(fl->paths);
}

static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **) a, *(const char **) b);
}

// Build clean path without double slashes
static void build_path(char *buf, size_t size, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    while (dlen > 0 && dir[dlen - 1] == '/')
        dlen--;
    snprintf(buf, size, "%.*s/%s", (int) dlen, dir, name);
}

static void collect_files_recursive(const char *path, XrFileList *fl) {
    XrDirIter *it = xr_dir_open(path);
    if (!it)
        return;

    // Collect entries first for sorted order
    char **subdirs = NULL;
    int ndir = 0, dcap = 0;
    char **xrfiles = NULL;
    int nfile = 0, fcap = 0;

    char filepath[1024];
    XrDirEntry e;
    while (xr_dir_next(it, &e)) {
        if (e.name[0] == '.' || e.name[0] == '_')
            continue;
        build_path(filepath, sizeof(filepath), path, e.name);
        if (e.is_dir) {
            if (ndir >= dcap) {
                dcap = dcap == 0 ? 16 : dcap * 2;
                XR_REALLOC_OR_ABORT(subdirs, dcap * sizeof(char *), "xcmd_test subdirs grow");
            }
            subdirs[ndir++] = xr_strdup(filepath);
        } else if (xr_cli_is_xr_file(e.name)) {
            if (nfile >= fcap) {
                fcap = fcap == 0 ? 16 : fcap * 2;
                XR_REALLOC_OR_ABORT(xrfiles, fcap * sizeof(char *), "xcmd_test xrfiles grow");
            }
            xrfiles[nfile++] = xr_strdup(filepath);
        }
    }
    xr_dir_close(it);

    if (nfile > 1)
        qsort(xrfiles, nfile, sizeof(char *), cmp_strings);
    if (ndir > 1)
        qsort(subdirs, ndir, sizeof(char *), cmp_strings);

    for (int i = 0; i < nfile; i++) {
        filelist_add(fl, xrfiles[i]);
        xr_free(xrfiles[i]);
    }
    xr_free(xrfiles);

    for (int i = 0; i < ndir; i++) {
        collect_files_recursive(subdirs[i], fl);
        xr_free(subdirs[i]);
    }
    xr_free(subdirs);
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
    XrTestParallelCtx *ctx = (XrTestParallelCtx *) arg;
    while (1) {
        int idx = atomic_fetch_add(&ctx->next_idx, 1);
        if (idx >= ctx->file_count)
            break;
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
        int len = (int) strlen(buf);
        if (len > max_w)
            max_w = len;
    }
    return max_w;
}

// Print results for a single file
static void print_file_result(XrTestFileResult *r, int align_width, bool verbose) {
    if (r->test_count == 0)
        return;

    char name[256];
    get_display_name(r->filepath, name, sizeof(name));

    if (r->has_error) {
        printf("   " XR_CLR_RED "x" XR_CLR_RESET " %s: %s\n", name, r->error_msg);
        return;
    }

    int problems = r->failed + r->errors + r->timeout;
    int ran = r->passed + r->failed + r->errors + r->timeout;

    if (verbose) {
        const char *plural = (r->test_count == 1) ? "" : "s";
        printf("   " XR_CLR_CYAN "%s" XR_CLR_RESET " " XR_CLR_DIM "(%d test%s)" XR_CLR_RESET "\n",
               name, r->test_count, plural);

        // Print individual failures
        for (int i = 0; i < r->failure_count; i++) {
            XrTestFailureRecord *f = &r->failures[i];
            const char *color = (f->status == TEST_TIMEOUT) ? XR_CLR_YELLOW : XR_CLR_RED;
            printf("     %sx" XR_CLR_RESET " %s: %s\n", color, f->test_name, f->message);
        }

        // Summary line
        if (problems > 0) {
            printf("     " XR_CLR_DIM "%d passed," XR_CLR_RESET " " XR_CLR_RED
                   "%d failed" XR_CLR_RESET,
                   r->passed, problems);
        } else {
            printf("     " XR_CLR_GREEN "%d passed" XR_CLR_RESET, r->passed);
        }
        if (r->skipped > 0)
            printf(XR_CLR_DIM ", %d skipped" XR_CLR_RESET, r->skipped);
        printf("  " XR_CLR_DIM "(%.0fms)" XR_CLR_RESET "\n", r->duration_ms);
    } else {
        if (problems == 0) {
            int name_len = (int) strlen(name);
            int aw = align_width > name_len ? align_width : name_len;
            int dots = aw - name_len + 4;
            if (dots < 3)
                dots = 3;
            if (dots > 255)
                dots = 255;
            char dot_buf[256];
            dot_buf[0] = ' ';
            for (int d = 1; d < dots; d++)
                dot_buf[d] = '.';
            dot_buf[dots] = '\0';

            printf("   " XR_CLR_GREEN "+" XR_CLR_RESET " %s" XR_CLR_DIM "%s" XR_CLR_RESET " %d/%d",
                   name, dot_buf, r->passed, ran + r->skipped);
            if (r->skipped > 0)
                printf("  " XR_CLR_DIM "%d skipped" XR_CLR_RESET, r->skipped);
            printf("  " XR_CLR_DIM "(%.0fms)" XR_CLR_RESET "\n", r->duration_ms);
        } else {
            printf("   " XR_CLR_RED "x" XR_CLR_RESET " %s " XR_CLR_DIM "(%d test%s)" XR_CLR_RESET
                   "\n",
                   name, r->test_count, r->test_count == 1 ? "" : "s");
            for (int i = 0; i < r->failure_count; i++) {
                XrTestFailureRecord *f = &r->failures[i];
                const char *color = (f->status == TEST_TIMEOUT) ? XR_CLR_YELLOW : XR_CLR_RED;
                printf("       %sx" XR_CLR_RESET " %s: %s\n", color, f->test_name, f->message);
            }
            printf("     " XR_CLR_DIM "%d passed," XR_CLR_RESET " " XR_CLR_RED
                   "%d failed" XR_CLR_RESET,
                   r->passed, problems);
            if (r->skipped > 0)
                printf(", %d skipped", r->skipped);
            printf("  " XR_CLR_DIM "(%.0fms)" XR_CLR_RESET "\n", r->duration_ms);
        }
    }
}

// Print directory group headers and file results in order
static void print_all_results(XrTestFileResult *results, char **files, int count, int align_width,
                              bool verbose) {
    const char *last_dir = NULL;
    char dir_buf[1024];
    for (int i = 0; i < count; i++) {
        if (results[i].test_count == 0 && !results[i].has_error)
            continue;

        // Extract directory name for group headers
        strncpy(dir_buf, files[i], sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash)
            *last_slash = '\0';
        const char *dir_name = strrchr(dir_buf, '/');
        dir_name = dir_name ? dir_name + 1 : dir_buf;

        if (!last_dir || strcmp(last_dir, dir_buf) != 0) {
            last_dir = dir_buf;
            printf(" " XR_CLR_BOLD "%s" XR_CLR_RESET "\n", dir_name);
        }

        print_file_result(&results[i], align_width, verbose);
    }
}

// Print final summary report
static void print_summary(int file_count, int total_passed, int total_failed, int total_errors,
                          int total_skipped, int total_timeout, double total_time_ms,
                          const char *filter, XrTestFileResult *results, int result_count) {
    int total_problems = total_failed + total_errors + total_timeout;

    // Failures detail section
    int total_failure_records = 0;
    for (int i = 0; i < result_count; i++)
        total_failure_records += results[i].failure_count;

    if (total_failure_records > 0) {
        printf("\n " XR_CLR_RED XR_CLR_BOLD "Failed Tests" XR_CLR_RESET "\n\n");
        for (int i = 0; i < result_count; i++) {
            for (int j = 0; j < results[i].failure_count; j++) {
                XrTestFailureRecord *rec = &results[i].failures[j];
                char fname[256];
                get_display_name(rec->file, fname, sizeof(fname));
                printf("  " XR_CLR_RED "\u2717" XR_CLR_RESET " %s " XR_CLR_DIM ">" XR_CLR_RESET
                       " %s\n",
                       fname, rec->test_name);
                if (rec->message[0] != '\0')
                    printf("    " XR_CLR_DIM "%s" XR_CLR_RESET "\n", rec->message);
            }
        }
    }

    // Summary
    printf("\n " XR_CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" XR_CLR_RESET
           "\n");

    printf(" " XR_CLR_BOLD " Tests" XR_CLR_RESET "  ");
    printf("%d file%s", file_count, file_count == 1 ? "" : "s");
    printf(XR_CLR_DIM " | " XR_CLR_RESET);
    if (total_problems == 0) {
        printf(XR_CLR_GREEN XR_CLR_BOLD "%d passed" XR_CLR_RESET, total_passed);
    } else {
        printf(XR_CLR_GREEN "%d passed" XR_CLR_RESET, total_passed);
        printf(XR_CLR_DIM " | " XR_CLR_RESET);
        printf(XR_CLR_RED XR_CLR_BOLD "%d failed" XR_CLR_RESET, total_problems);
    }
    if (total_skipped > 0) {
        printf(XR_CLR_DIM " | " XR_CLR_RESET);
        printf(XR_CLR_DIM "%d skipped" XR_CLR_RESET, total_skipped);
    }
    if (filter)
        printf("  " XR_CLR_DIM "(filter: \"%s\")" XR_CLR_RESET, filter);
    printf("\n");

    printf(" " XR_CLR_BOLD "  Time" XR_CLR_RESET "  ");
    if (total_time_ms >= 1000.0)
        printf("%.2fs\n", total_time_ms / 1000.0);
    else
        printf("%.0fms\n", total_time_ms);

    printf(" " XR_CLR_DIM "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" XR_CLR_RESET
           "\n");

    if (total_problems == 0)
        printf("\n " XR_CLR_GREEN XR_CLR_BOLD "\u2713 All tests passed" XR_CLR_RESET "\n\n");
    else
        printf("\n " XR_CLR_RED XR_CLR_BOLD "\u2717 %d test%s failed" XR_CLR_RESET "\n\n",
               total_problems, total_problems == 1 ? "" : "s");
}

/* ========== CLI Entry Point ========== */

XR_FUNC int cmd_test(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");

    bool verbose = xr_cli_opt_bool(&inv->options, "verbose");
    bool quiet = xr_cli_opt_bool(&inv->options, "quiet");
    bool fail_fast = xr_cli_opt_bool(&inv->options, "fail-fast");
    bool jitless = xr_cli_opt_bool(&inv->options, "no-jit");
    bool jit_force = xr_cli_opt_bool(&inv->options, "jit-force");
    const char *filter = xr_cli_opt_string(&inv->options, "filter", NULL);
    int num_threads = xr_cli_opt_int(&inv->options, "jobs", 1);
    if (num_threads < 1)
        num_threads = 1;

    if (inv->positional_count < 1) {
        xr_cli_error("test", "please specify test file or directory");
        return XR_CLI_EXIT_USAGE;
    }

    /* Collect test files from all positional args */
    XrFileList fl = {0};
    for (int i = 0; i < inv->positional_count; i++) {
        const char *test_path = inv->positionals[i];
        XrFsStat st;
        if (xr_fs_stat(test_path, &st) != 0) {
            xr_cli_error("test", "path does not exist '%s'", test_path);
            filelist_free(&fl);
            return XR_CLI_EXIT_FAIL;
        }
        if (st.kind == XR_FS_DIR) {
            collect_files_recursive(test_path, &fl);
        } else {
            filelist_add(&fl, test_path);
        }
    }

    if (fl.count == 0) {
        if (!quiet)
            fprintf(stderr, "No test files found\n");
        filelist_free(&fl);
        return XR_CLI_EXIT_OK;
    }

    XrTestConfig config = {.verbose = verbose, .fail_fast = fail_fast, .filter = filter};

    /* Allocate results */
    XrTestFileResult *results = xr_calloc(fl.count, sizeof(XrTestFileResult));

    double total_start = get_time_ms();

    if (!quiet)
        printf("\n");

    if (num_threads <= 1 || fl.count == 1) {
        /* Serial execution */
        int aw = quiet ? 0 : compute_align_width(fl.paths, fl.count);
        char last_dir[1024] = "";
        for (int i = 0; i < fl.count; i++) {
            run_test_file(fl.paths[i], &config, jitless, jit_force, &results[i]);

            if (!quiet) {
                char dir_buf[1024];
                strncpy(dir_buf, fl.paths[i], sizeof(dir_buf) - 1);
                dir_buf[sizeof(dir_buf) - 1] = '\0';
                char *ls = strrchr(dir_buf, '/');
                if (ls)
                    *ls = '\0';
                if (strcmp(last_dir, dir_buf) != 0) {
                    strncpy(last_dir, dir_buf, sizeof(last_dir) - 1);
                    const char *dn = strrchr(dir_buf, '/');
                    dn = dn ? dn + 1 : dir_buf;
                    if (results[i].test_count > 0 || results[i].has_error)
                        printf(" " XR_CLR_BOLD "%s" XR_CLR_RESET "\n", dn);
                }
                print_file_result(&results[i], aw, verbose);
            }

            if (fail_fast && (results[i].failed + results[i].errors + results[i].timeout) > 0)
                break;
        }
    } else {
        /* Parallel execution */
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
        if (nworkers > fl.count)
            nworkers = fl.count;

        if (!quiet)
            printf(" " XR_CLR_DIM "Running %d files on %d threads..." XR_CLR_RESET "\n\n", fl.count,
                   nworkers);

        xr_thread_t *threads = xr_calloc(nworkers, sizeof(xr_thread_t));
        for (int i = 0; i < nworkers; i++)
            xr_thread_create(&threads[i], test_worker_thread, &pctx);
        for (int i = 0; i < nworkers; i++)
            xr_thread_join(threads[i], NULL);
        xr_free(threads);

        if (!quiet) {
            int aw = compute_align_width(fl.paths, fl.count);
            print_all_results(results, fl.paths, fl.count, aw, verbose);
        }
    }

    double total_time = get_time_ms() - total_start;

    /* Aggregate stats */
    int file_count = 0, total_passed = 0, total_failed = 0;
    int total_errors = 0, total_skipped = 0, total_timeout = 0;
    int empty_file_count = 0;
    for (int i = 0; i < fl.count; i++) {
        if (results[i].test_count > 0 || results[i].has_error)
            file_count++;
        else
            empty_file_count++;
        total_passed += results[i].passed;
        total_failed += results[i].failed;
        total_errors += results[i].errors;
        total_skipped += results[i].skipped;
        total_timeout += results[i].timeout;
    }
    int total_executed = total_passed + total_failed + total_errors + total_timeout;

    if (!quiet) {
        print_summary(file_count, total_passed, total_failed, total_errors, total_skipped,
                      total_timeout, total_time, filter, results, fl.count);
    }

    int exit_code;
    if ((total_failed + total_errors + total_timeout) > 0) {
        exit_code = XR_CLI_EXIT_FAIL;
    } else if (total_executed == 0) {
        /* No tests were executed at all — treat as error to prevent
         * silent false-pass when test files lack @test functions. */
        if (!quiet)
            fprintf(stderr, "Error: 0 tests executed across %d file(s)\n", fl.count);
        exit_code = XR_CLI_EXIT_FAIL;
    } else {
        exit_code = XR_CLI_EXIT_OK;
    }

    for (int i = 0; i < fl.count; i++)
        file_result_free(&results[i]);
    xr_free(results);
    filelist_free(&fl);

    return exit_code;
}
