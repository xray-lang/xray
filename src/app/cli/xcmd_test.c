/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_test.c - Run each entry module's selected tests in one instance
 */
#include "xcli.h"
#include "xcli_fs.h"
#include "xcli_lir.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include "shared/xr_utf8_core.h"
#include "shared/xr_value_format_core.h"
#include "vm/register/xrbc_lir.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

#include "xcmd_test_report.inc.c"

typedef struct TestOptions {
    const char *filter;
    bool quiet, verbose, fail_fast;
} TestOptions;

typedef struct TestResults {
    uint64_t passed, failed, skipped, executed;
} TestResults;

typedef struct TestRoleRange {
    uint32_t start, count;
} TestRoleRange;

typedef struct TestFile {
    const char *path;
    XrLirTestPlan *plan;
    XrRbcTestSession *session;
    XrLirTestPlanRole *actions;
    TestRoleRange roles[XR_XIR_TEST_ROLE_AFTER_EACH + 1];
    TestReportFile report;
    bool initialization_failed;
} TestFile;

static const char *test_role_name(XrXirTestRole role) {
    switch (role) {
    case XR_XIR_TEST_ROLE_TEST: return "test";
    case XR_XIR_TEST_ROLE_SKIP: return "skip";
    case XR_XIR_TEST_ROLE_BEFORE_ALL: return "before_all";
    case XR_XIR_TEST_ROLE_AFTER_ALL: return "after_all";
    case XR_XIR_TEST_ROLE_BEFORE_EACH: return "before_each";
    case XR_XIR_TEST_ROLE_AFTER_EACH: return "after_each";
    default: return "none";
    }
}

static bool test_collect_roles(TestFile *file, const TestOptions *options, TestResults *results) {
    uint32_t count = xr_lir_test_plan_role_count(file->plan);
    uint32_t selected = 0, skipped = 0, hooks = 0;
    for (uint32_t i = 0; i < count; ++i) {
        XrLirTestPlanRole view;
        if (xr_lir_test_plan_role_at(file->plan, i, &view) != XR_LIR_OK ||
            view.role <= XR_XIR_TEST_ROLE_NONE || view.role > XR_XIR_TEST_ROLE_AFTER_EACH)
            return false;
        if (view.function.id) ++file->roles[view.role].count;
        bool is_test = view.role == XR_XIR_TEST_ROLE_TEST || view.role == XR_XIR_TEST_ROLE_SKIP;
        if (!is_test) ++hooks;
        else if (view.function.id) ++selected;
        else ++skipped;
        if (options->verbose && (view.role != XR_XIR_TEST_ROLE_TEST || view.function.id)) {
            fprintf(stderr, "  %-11s ", test_role_name(view.role));
            fwrite(view.name, 1, view.name_length, stderr);
            fputc('\n', stderr);
        }
    }
    if (options->verbose)
        fprintf(stderr, "xray test: %s: %u test(s), %u skipped, %u hook(s)\n",
            file->path, selected, skipped, hooks);
    results->skipped += skipped;
    return true;
}

static bool test_prepare_session(TestFile *file, const TestOptions *options) {
    /* Group roots by role while preserving Checked order within each group. */
    uint32_t next[XR_XIR_TEST_ROLE_AFTER_EACH + 1] = {0};
    uint32_t actions = 0;
    for (size_t i = 0; i < sizeof(next) / sizeof(*next); ++i) {
        file->roles[i].start = next[i] = actions;
        actions += file->roles[i].count;
    }
    file->actions = xr_calloc(actions, sizeof(*file->actions));
    XrLirFunction *roots = xr_calloc(actions, sizeof(*roots));
    bool ready = false;
    if (!file->actions || !roots) {
        if (!options->quiet) xr_cli_error("test", "%s: cannot allocate test roles", file->path);
        test_report_suite_error(&file->report, "cannot allocate test roles");
        goto done;
    }
    uint32_t count = xr_lir_test_plan_role_count(file->plan);
    for (uint32_t i = 0; i < count; ++i) {
        XrLirTestPlanRole view;
        if (xr_lir_test_plan_role_at(file->plan, i, &view) != XR_LIR_OK) goto invalid;
        if (!view.function.id) continue;
        if (xr_lir_test_plan_check_role(file->plan, i, view.function) != XR_LIR_OK) goto invalid;
        uint32_t action = next[view.role]++;
        file->actions[action] = view;
        roots[action] = view.function;
    }
    XrLirFunction initializer = xr_lir_test_plan_initializer(file->plan);
    XrRbcCompileDiagnostic diagnostic = {0};
    ready = xr_rbc_test_session_compile(initializer.module, initializer, roots, actions,
        &file->session, &diagnostic) == XR_RBC_COMPILE_OK;
    if (!ready) {
        const char *path = diagnostic.path[0] ? diagnostic.path : file->path;
        if (!options->quiet)
            fprintf(stderr, "%s:%u:%u: %s\n", path, diagnostic.line, diagnostic.column, diagnostic.message);
        if (file->report.owner) {
            ++file->report.errors;
            if (!test_report_format(&file->report.error, "%s:%u:%u: %s", path, diagnostic.line,
                    diagnostic.column, diagnostic.message)) file->report.owner->failed = true;
        }
    }
    goto done;
invalid:
    if (!options->quiet) xr_cli_error("test", "%s: invalid test plan", file->path);
    test_report_suite_error(&file->report, "invalid test plan");
done:
    xr_free(roots);
    return ready;
}

static int test_panic_write(void *context, const void *bytes, size_t length) {
    (void)context;
    return !xrt_io_write(2, bytes, length).failed;
}

static void test_report_outcome(XrRbcStatus status, XrRbcOutcome *outcome) {
    const XrtLocationOwner *locations;
    const XrtErrorSnapshot *error = xr_rbc_outcome_uncaught_error(outcome);
    const XrtTryAssertFailure *assertion = xr_rbc_outcome_try_assert(outcome);
    const char *message;
    size_t length;
    uint32_t code, source;
    XrtArithmeticPanic panic;
    XrXirValueStatus value;
    XrtHostClockFailure clock;
    if (error) {
        if (xrt_uncaught_error_report(error)) return;
    } else if (assertion) {
        if (xrt_try_assert_report(assertion)) return;
    } else if (xr_rbc_outcome_message_panic(outcome, &code, &message, &length, &source, &locations)) {
        if (xrt_message_panic_report(code, message, length, xrt_locations_get(locations, source))) return;
    } else if (xr_rbc_outcome_arithmetic_panic(outcome, &panic, &locations)) {
        XrValueFormatSink sink = {NULL, test_panic_write};
        if (xr_value_format_panic(sink, panic.code, 0, 0, 0, 0, NULL, 0, 0)) return;
    } else if (xr_rbc_outcome_null_unwrap(outcome, &source, &locations)) {
        xrt_null_unwrap_report(xrt_locations_get(locations, source));
        return;
    } else if (xr_rbc_outcome_host_clock_failure(outcome, &clock, &locations)) {
        const XrtSourceLocation *site = xrt_locations_get(locations, clock.source_id);
        fprintf(stderr, "host clock %u failed at %s:%u:%u\n", clock.kind,
            site ? site->path : "unknown source", site ? site->line : 0, site ? site->column : 0);
        return;
    } else if (xr_rbc_outcome_value_failure(outcome, &value, &source, &locations)) {
        uint32_t close = xr_rbc_outcome_directory_close_source(outcome);
        xrt_value_failure_report_with_directory_close(value, xrt_locations_get(locations, source),
            xrt_locations_get(locations, close));
        return;
    }
    fprintf(stderr, "register VM execution failed (%u)\n", (unsigned)status);
}

static bool test_report_action_message(TestReportBytes *out, XrRbcStatus status,
    XrRbcOutcome *outcome) {
    uint32_t code, source;
    const char *message;
    size_t length;
    const XrtLocationOwner *locations;
    XrtArithmeticPanic panic;
    XrXirValueStatus value;
    XrtHostClockFailure clock;
    XrtOutputFailure output;
    XrValueFormatSink sink = {out, test_report_write};
    const XrtErrorSnapshot *error = xr_rbc_outcome_uncaught_error(outcome);
    const XrtTryAssertFailure *assertion = xr_rbc_outcome_try_assert(outcome);
    if (error) return xrt_uncaught_error_format(error, &sink);
    if (assertion) return xrt_try_assert_format(assertion, &sink);
    if (xr_rbc_outcome_message_panic(outcome, &code, &message, &length, &source, &locations))
        return xr_value_format_panic(sink, code, 0, 0, 0, 1, (const uint8_t *)message, length, 0);
    if (xr_rbc_outcome_arithmetic_panic(outcome, &panic, &locations))
        return xr_value_format_panic(sink, panic.code, 0, 0, 0, 0, NULL, 0, 0);
    if (xr_rbc_outcome_null_unwrap(outcome, &source, &locations))
        return test_report_text(out, "[Uncaught Panic] E0413: null value unwrapped");
    if (xr_rbc_outcome_value_failure(outcome, &value, &source, &locations)) {
        const XrtSourceLocation *site = xrt_locations_get(locations, source);
        const XrtSourceLocation *close = xrt_locations_get(locations, xr_rbc_outcome_directory_close_source(outcome));
        if (!test_report_format(out, "XR_RUN_6002: value operation rejected (status %u)", (unsigned)value))
            return false;
        if (site && !test_report_format(out, " at %s:%u:%u", site->path, site->line, site->column)) return false;
        return !close || test_report_format(out, "\nnote: directory cleanup rejected its token at %s:%u:%u",
            close->path, close->line, close->column);
    }
    if (xr_rbc_outcome_host_clock_failure(outcome, &clock, &locations)) {
        const XrtSourceLocation *site = xrt_locations_get(locations, clock.source_id);
        return test_report_format(out, "host clock %u failed at %s:%u:%u", clock.kind,
            site ? site->path : "unknown source", site ? site->line : 0, site ? site->column : 0);
    }
    if (xr_rbc_outcome_output_failure(outcome, &output, &locations))
        return test_report_format(out, "test output failed (operation %u, host error %u)",
            (unsigned)output.operation, output.io.code);
    return test_report_format(out, "register VM execution failed (%u)", (unsigned)status);
}

static void test_report_action_failure(TestFile *file, const XrLirTestPlanRole *role, XrRbcStatus status,
    XrRbcOutcome *outcome, TestReportBytes *cause) {
    TestReport *report = file->report.owner;
    if (!report || report->failed) return;
    if (file->initialization_failed) {
        ++file->report.errors;
        cause = &file->report.error;
    }
    if (!cause) return;
    bool written = !cause->length || test_report_text(cause, "\n");
    if (written && file->initialization_failed)
        written = test_report_text(cause, "module initialization failed: ");
    else if (written && role->role != XR_XIR_TEST_ROLE_TEST)
        written = test_report_format(cause, "hook @%s '%s': ", test_role_name(role->role), role->name);
    if (!written || !test_report_action_message(cause, status, outcome)) {
        report->failed = true;
    } else if (cause->length && cause->data[cause->length - 1] == '\n') {
        --cause->length;
    }
}

static bool test_run_action(TestFile *file, uint32_t action, const TestOptions *options,
    TestResults *results, TestReportBytes *cause, TestReportStatus *case_status) {
    const XrLirTestPlanRole *role = &file->actions[action];
    XrRbcOutcome *outcome = NULL;
    XrRbcTestActionStage stage;
    XrRbcStatus status = xr_rbc_test_session_run_detailed(file->session, action, &outcome, &stage);
    if (stage == XR_RBC_TEST_ACTION_INVOKE && role->role == XR_XIR_TEST_ROLE_TEST)
        ++results->executed;
    if (status != XR_RBC_OK) {
        file->initialization_failed = stage == XR_RBC_TEST_ACTION_INITIALIZATION;
        if (case_status && stage == XR_RBC_TEST_ACTION_INVOKE &&
            (status == XR_RBC_TYPED_PANIC || status == XR_RBC_MESSAGE_PANIC || status == XR_RBC_TRY_ASSERT ||
             status == XR_RBC_NULL_UNWRAP || status == XR_RBC_UNCAUGHT_ERROR))
            *case_status = TEST_REPORT_FAILED;
        test_report_action_failure(file, role, status, outcome, cause);
        if (!options->quiet) {
            fprintf(stderr, "xray test: %s: ", file->path);
            if (file->initialization_failed) fputs("module initialization failed\n", stderr);
            else {
                fprintf(stderr, "@%s ", test_role_name(role->role));
                fwrite(role->name, 1, role->name_length, stderr);
                fputs(" failed\n", stderr);
            }
            test_report_outcome(status, outcome);
        }
    }
    xr_rbc_outcome_free(&outcome);
    return status == XR_RBC_OK;
}

static bool test_run_hooks(TestFile *file, XrXirTestRole role, const TestOptions *options,
    TestResults *results, TestReportBytes *cause) {
    if (file->initialization_failed) return false;
    TestRoleRange range = file->roles[role];
    bool passed = true;
    for (uint32_t i = 0; i < range.count; ++i) {
        if (test_run_action(file, range.start + i, options, results, cause, NULL)) continue;
        passed = false;
        if (file->initialization_failed || role == XR_XIR_TEST_ROLE_BEFORE_ALL ||
            role == XR_XIR_TEST_ROLE_BEFORE_EACH) break;
    }
    return passed;
}

static void test_run_selected(TestFile *file, const TestOptions *options, TestResults *results) {
    TestRoleRange tests = file->roles[XR_XIR_TEST_ROLE_TEST];
    if (test_run_hooks(file, XR_XIR_TEST_ROLE_BEFORE_ALL, options, results, &file->report.error)) {
        for (uint32_t i = 0; i < tests.count; ++i) {
            TestReportCase *row = test_report_next(&file->report);
            TestReportBytes *cause = row ? &row->cause : NULL;
            TestReportStatus status = TEST_REPORT_ERROR;
            bool passed = test_run_hooks(file, XR_XIR_TEST_ROLE_BEFORE_EACH, options, results, cause)
                && test_run_action(file, tests.start + i, options, results, cause, row ? &status : NULL);
            bool cleaned = test_run_hooks(file, XR_XIR_TEST_ROLE_AFTER_EACH, options, results, cause);
            if (row && !file->initialization_failed)
                row->status = passed && cleaned ? TEST_REPORT_PASSED : status;
            if (passed && cleaned) ++results->passed;
            else ++results->failed;
            if (file->initialization_failed || (options->fail_fast && (!passed || !cleaned))) break;
        }
    } else {
        ++results->failed;
        if (file->report.owner && !file->initialization_failed)
            for (uint32_t i = 0; i < file->report.tests; ++i)
                if (file->report.cases[i].status == TEST_REPORT_PENDING)
                    file->report.cases[i].status = TEST_REPORT_ERROR;
    }
    if (!file->initialization_failed &&
        !test_run_hooks(file, XR_XIR_TEST_ROLE_AFTER_ALL, options, results, &file->report.error)) {
        ++results->failed;
        ++file->report.errors;
    }
}

static void test_run_file(const char *path, const TestOptions *options, TestResults *results,
    TestReport *report) {
    XrCompileResources *resources = report ? report->json.resources : NULL;
    TestFile file = {.path = path, .report = {.owner = report, .error = {.resources = resources}}};
    XrCliTestCompileFailure cause = {.resources = resources};
    int compiled = xr_cli_lir_compile_test_plan(path, options->filter, options->quiet, &file.plan,
        report ? &cause : NULL);
    if (compiled != XR_CLI_EXIT_OK) {
        ++results->failed;
        if (report) {
            if (cause.text) test_report_suite_error(&file.report, cause.text);
            else report->failed = true;
            file.report.source_status = cause.status;
            file.report.source_stage = cause.stage;
        }
    } else if (!test_collect_roles(&file, options, results)) {
        if (!options->quiet) xr_cli_error("test", "%s: invalid test plan", path);
        test_report_suite_error(&file.report, "invalid test plan");
        ++results->failed;
    } else {
        if (report) test_report_collect(&file.report, file.plan);
        if (file.roles[XR_XIR_TEST_ROLE_TEST].count) {
            if (test_prepare_session(&file, options)) test_run_selected(&file, options, results);
            else ++results->failed;
        }
    }
    XrOsIoStatus retired = xr_rbc_test_session_free(&file.session);
    if (retired != XR_OS_IO_OK) {
        if (!options->quiet) xr_cli_error("test", "%s: register VM retirement failed (%u)",
            path, (unsigned)retired);
        fflush(stderr);
        /* The session still owns the exact handle until process exit. */
        _Exit(XR_CLI_EXIT_FAIL);
    }
    test_report_file_close(&file.report, path);
    xr_compile_resources_free(cause.text);
    xr_free(file.actions);
    xr_lir_test_plan_free(file.plan);
}

XR_FUNC int cmd_test(const XrCliInvocation *inv) {
#if XR_OS_WINDOWS
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) return XR_CLI_EXIT_INTERNAL;
#endif
    if (!inv || inv->positional_count < 1) {
        xr_cli_error("test", "requires at least one file or directory");
        return XR_CLI_EXIT_USAGE;
    }
    int jobs = xr_cli_opt_int(&inv->options, "jobs", 1);
    if (jobs < 1) {
        xr_cli_error("test", "parallel jobs must be at least one");
        return XR_CLI_EXIT_USAGE;
    }
    if (xr_cli_opt_present(&inv->options, "sandbox")) {
        xr_cli_error("test", "sandbox execution is temporarily unavailable");
        return XR_CLI_EXIT_FAIL;
    }
    if (jobs > 1) {
        xr_cli_error("test", "parallel test execution is temporarily unavailable");
        return XR_CLI_EXIT_USAGE;
    }
    const char *report_path = xr_cli_opt_string(&inv->options, "report", NULL);
    bool quiet = xr_cli_opt_present(&inv->options, "quiet");
    TestOptions options = {
        .filter = xr_cli_opt_string(&inv->options, "filter", NULL),
        .quiet = quiet || report_path,
        .verbose = xr_cli_opt_present(&inv->options, "verbose") && !quiet,
        .fail_fast = xr_cli_opt_present(&inv->options, "fail-fast"),
    };
    XrCliFileList files;
    XrCliWalkOpts walk = xr_cli_walk_defaults();
    xr_cli_filelist_init(&files);
    for (int p = 0; p < inv->positional_count; ++p) {
        if (xr_cli_collect_files(inv->positionals[p], &walk, &files) != 0) {
            xr_cli_error("test", "cannot read %s", inv->positionals[p]);
            xr_cli_filelist_free(&files);
            return XR_CLI_EXIT_FAIL;
        }
    }
    if (files.count == 0) {
        xr_cli_error("test", "no .xr files were found");
        xr_cli_filelist_free(&files);
        return XR_CLI_EXIT_FAIL;
    }
    xr_cli_filelist_sort(&files);
    TestReport report = {0};
    if (report_path && !test_report_open(&report)) {
        xr_cli_error("test", "cannot initialize report '%s'", report_path);
        test_report_close(&report);
        xr_cli_filelist_free(&files);
        return XR_CLI_EXIT_FAIL;
    }
    TestResults results = {0};
    for (int f = 0; f < files.count; ++f) {
        test_run_file(files.paths[f], &options, &results, report_path ? &report : NULL);
        if (results.failed && options.fail_fast) {
            if (!options.quiet) fputs("xray test: stopping after the first failure\n", stderr);
            break;
        }
    }
    bool zero = !results.executed && !results.failed;
    bool published = true;
    if (report_path) {
        if (!quiet && zero) fprintf(stderr, "Error: 0 tests executed across %d file(s)\n", files.count);
        published = test_report_publish(&report, report_path);
        test_report_close(&report);
    }
    xr_cli_filelist_free(&files);
    if (!options.quiet) {
        if (zero) fputs("xray test: 0 tests executed\n", stderr);
        fprintf(stderr, "xray test: %" PRIu64 " passed, %" PRIu64 " failed, %" PRIu64 " skipped\n",
            results.passed, results.failed, results.skipped);
    }
    return results.failed || !results.executed || !published ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
