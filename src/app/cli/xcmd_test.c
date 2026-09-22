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
 *   Each file builds one detached Program and runs tests and hooks in one
 *   module instance. File workers share no execution state.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_output.h"
#include "xcli_canonical_source.h"
#include "xcli_program_vm.h"
#include "../toolchain/xtc_target_profile.h"
#include "../../api/xisolate_profile.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../os/os_fs.h"
#include "../../os/os_dir.h"
#include "../../os/os_thread.h"
#include "../../os/os_time.h"
#include "../../plan/target/xr_target_profile.h"
#include "xray_vm.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define TEST_FILE_TIMEOUT_SEC 120
#define TEST_WORKER_STACK_SIZE (8u * 1024u * 1024u)

typedef enum {
    TEST_PASSED,
    TEST_FAILED,
    TEST_ERROR,
    TEST_SKIPPED,
    TEST_TIMEOUT
} XrTestStatus;

typedef struct XrTestConfig {
    bool verbose;
    bool fail_fast;
    const char *filter;  // NULL = run all
} XrTestConfig;

// Failure record for end-of-run summary
typedef struct XrTestFailureRecord {
    char *file;       // owned copy
    char *test_name;  // owned copy
    char *message;    // owned copy
    XrTestStatus status;
} XrTestFailureRecord;


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

static bool test_entry_is_case(const XrProgramSourceTestEntry *entry) {
    return entry->kind == XR_PROGRAM_TEST_CASE || entry->kind == XR_PROGRAM_TEST_SKIP;
}

static bool test_entry_selected(const XrProgramSourceTestEntry *entry, const XrTestConfig *config) {
    return entry->kind == XR_PROGRAM_TEST_CASE &&
           (!config->filter || strstr(entry->name, config->filter));
}

typedef struct XrTestMessageSink {
    char *message;
    size_t size;
    size_t used;
} XrTestMessageSink;

static int test_message_write(void *context, const void *bytes, size_t size) {
    XrTestMessageSink *sink = context;
    if (!sink->size)
        return 0;
    size_t room = sink->size - sink->used - 1u;
    size_t copied = size < room ? size : room;
    if (copied)
        memcpy(sink->message + sink->used, bytes, copied);
    sink->used += copied;
    sink->message[sink->used] = '\0';
    if (copied != size && sink->size >= 4u)
        memcpy(sink->message + sink->size - 4u, "...", 4u);
    return 1;
}

static XrTestStatus invoke_test_entry(XrCliProgramVm *vm, uint32_t function_id, double deadline,
                                      char *message, size_t message_size) {
    XrTestMessageSink capture = {message, message_size, 0u};
    XrValueFormatSink errors = {&capture, test_message_write};
    XrCliVmResult result = xr_cli_program_vm_invoke(vm, function_id, deadline, &errors);
    if (result.timed_out) {
        snprintf(message, message_size, "exceeded timeout");
        return TEST_TIMEOUT;
    }
    if (result.kind == XR_VM_OUTCOME_RETURN)
        return TEST_PASSED;
    if (result.error_reported || result.panic_reported)
        return TEST_FAILED;
    snprintf(message, message_size, "execution failed (outcome=%u trap=%u panic=%u)",
             (unsigned) result.kind, (unsigned) result.trap, result.panic_info.code);
    return TEST_FAILED;
}

static bool run_hooks(XrCliProgramVm *vm, const XrProgramSourceProduct *product,
                      XrProgramSourceTestKind kind, double deadline, char *message,
                      size_t message_size) {
    bool passed = true;
    for (uint32_t index = 0u; index < product->test_entry_count; ++index) {
        const XrProgramSourceTestEntry *hook = &product->tests[index];
        if (hook->kind != kind)
            continue;
        char failure[256] = {0};
        if (invoke_test_entry(vm, hook->function_id, deadline, failure, sizeof(failure)) !=
            TEST_PASSED) {
            if (passed)
                snprintf(message, message_size, "hook '%s': %s", hook->name, failure);
            passed = false;
            if (kind == XR_PROGRAM_TEST_BEFORE_ALL || kind == XR_PROGRAM_TEST_BEFORE_EACH)
                break;
        }
    }
    return passed;
}

static void run_test_file(const char *filepath, XrTestConfig *config, XrTestFileResult *result) {
    memset(result, 0, sizeof(*result));
    snprintf(result->filepath, sizeof(result->filepath), "%s", filepath);
    XrProgramSourceProduct product = {0};
    XrTargetProfile *profile = NULL;
    XrCliProgramVm vm = {0};
    XrTargetCodegenFacts codegen = {0};
    char error[512] = {0};
    double file_start = get_time_ms();
    if (!xtc_target_profile_build_current_native_hosted(&codegen, &profile, error, sizeof(error)))
        goto failed;
    XrVMConfig params;
    xr_isolate_profile_params(XR_ISOLATE_PROFILE_TEST, &params);
    params.script_file = filepath;
    XrVMRuntime *compiler_host = xr_isolate_profile_create(&params);
    if (!compiler_host) {
        snprintf(error, sizeof(error), "compiler host creation failed");
        goto failed;
    }
    XrCliCanonicalSourceRequest request = {
        .schema_version = XR_CLI_CANONICAL_SOURCE_SCHEMA_VERSION,
        .compiler_host = compiler_host,
        .entry_source_path = filepath,
        .entry_kind = XR_PROGRAM_SOURCE_ENTRY_MODULE_INITIALIZER,
        .source_profile = XR_PROGRAM_SOURCE_PROFILE_DEVELOPMENT,
        .discover_tests = 1u,
        .semantic_profile_fingerprint = xr_target_profile_target_semantics_id(profile),
    };
    XrCliCanonicalSourceDiagnostic diagnostic;
    XrCliCanonicalSourceStatus build =
        xr_cli_canonical_source_build(&request, &product, &diagnostic);
    xray_vm_delete(compiler_host);
    if (build != XR_CLI_CANONICAL_SOURCE_OK) {
        snprintf(error, sizeof(error), "%s", diagnostic.message);
        goto failed;
    }
    int selected = 0;
    for (uint32_t index = 0u; index < product.test_entry_count; ++index) {
        const XrProgramSourceTestEntry *entry = &product.tests[index];
        if (test_entry_is_case(entry)) {
            ++result->test_count;
            if (test_entry_selected(entry, config))
                ++selected;
            else
                ++result->skipped;
        }
    }
    if (!selected)
        goto cleanup;
    if (!xr_cli_program_vm_open(&vm, product.program, profile, error, sizeof(error)))
        goto failed;
    double file_deadline = get_time_ms() + TEST_FILE_TIMEOUT_SEC * 1000.0;
    if (invoke_test_entry(&vm, xr_validated_program_entry_function(product.program), file_deadline,
                          error, sizeof(error)) != TEST_PASSED)
        goto failed;
    bool setup =
        run_hooks(&vm, &product, XR_PROGRAM_TEST_BEFORE_ALL, file_deadline, error, sizeof(error));
    if (!setup) {
        for (uint32_t index = 0u; index < product.test_entry_count; ++index) {
            const XrProgramSourceTestEntry *entry = &product.tests[index];
            if (test_entry_selected(entry, config)) {
                ++result->errors;
                file_result_add_failure(result, entry->name, error, TEST_ERROR);
            }
        }
    }
    for (uint32_t index = 0u; setup && index < product.test_entry_count; ++index) {
        const XrProgramSourceTestEntry *entry = &product.tests[index];
        if (!test_entry_selected(entry, config))
            continue;
        XrTestStatus status = TEST_ERROR;
        if (run_hooks(&vm, &product, XR_PROGRAM_TEST_BEFORE_EACH, file_deadline, error,
                      sizeof(error))) {
            double deadline = file_deadline;
            if (entry->timeout_seconds) {
                double test_deadline = get_time_ms() + entry->timeout_seconds * 1000.0;
                if (test_deadline < deadline)
                    deadline = test_deadline;
            }
            status = invoke_test_entry(&vm, entry->function_id, deadline, error, sizeof(error));
        }
        char hook_error[512] = {0};
        if (!run_hooks(&vm, &product, XR_PROGRAM_TEST_AFTER_EACH, file_deadline, hook_error,
                       sizeof(hook_error))) {
            if (status == TEST_PASSED) {
                status = TEST_ERROR;
                snprintf(error, sizeof(error), "%s", hook_error);
            } else {
                file_result_add_failure(result, entry->name, hook_error, TEST_ERROR);
            }
        }
        if (status == TEST_PASSED)
            ++result->passed;
        else {
            if (status == TEST_TIMEOUT)
                ++result->timeout;
            else if (status == TEST_ERROR)
                ++result->errors;
            else
                ++result->failed;
            file_result_add_failure(result, entry->name, error, status);
        }
        if (config->fail_fast && status != TEST_PASSED)
            break;
        if (get_time_ms() >= file_deadline)
            break;
    }
    if (!run_hooks(&vm, &product, XR_PROGRAM_TEST_AFTER_ALL, file_deadline, error, sizeof(error))) {
        ++result->errors;
        file_result_add_failure(result, "@after_all", error, TEST_ERROR);
    }
    goto cleanup;
failed:
    result->has_error = true;
    ++result->errors;
    snprintf(result->error_msg, sizeof(result->error_msg), "%s", error);
cleanup:
    if (!xr_cli_program_vm_close(&vm)) {
        result->has_error = true;
        ++result->errors;
        snprintf(result->error_msg, sizeof(result->error_msg), "execution instance did not retire");
    }
    xr_program_source_product_free(&product);
    xr_target_profile_free(profile);
    result->duration_ms = get_time_ms() - file_start;
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
} XrTestParallelCtx;

static void *test_worker_thread(void *arg) {
    XrTestParallelCtx *ctx = (XrTestParallelCtx *) arg;
    while (1) {
        int idx = atomic_fetch_add(&ctx->next_idx, 1);
        if (idx >= ctx->file_count)
            break;
        run_test_file(ctx->files[idx], &ctx->config, &ctx->results[idx]);
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
    if (r->test_count == 0 && !r->has_error)
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
            run_test_file(fl.paths[i], &config, &results[i]);

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
        };

        int nworkers = num_threads;
        if (nworkers > fl.count)
            nworkers = fl.count;

        if (!quiet)
            printf(" " XR_CLR_DIM "Running %d files on %d threads..." XR_CLR_RESET "\n\n", fl.count,
                   nworkers);

        xr_thread_t *threads = xr_calloc(nworkers, sizeof(xr_thread_t));
        for (int i = 0; i < nworkers; i++)
            xr_thread_create_ex(&threads[i], test_worker_thread, &pctx, TEST_WORKER_STACK_SIZE);
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
    for (int i = 0; i < fl.count; i++) {
        if (results[i].test_count > 0 || results[i].has_error)
            file_count++;
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
