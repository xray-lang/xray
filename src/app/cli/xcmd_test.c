/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_test.c - Discover and report the tests of each entry module
 *
 * KEY CONCEPT:
 *   The checked artifact already records every test role, hook and per-test
 *   timeout, so discovery is a read of that artifact and only the entry module's
 *   own roles count. Starting a test needs the backend's test-role startup,
 *   which does not exist yet: until it does, options that only make sense while
 *   running (-j, --report) are refused and the command exits non-zero instead of
 *   inventing outcomes. `--sandbox` is answered like `xray run` answers it --
 *   the resource profile itself is unavailable, so the command fails rather than
 *   reporting the option invalid. Diagnostics go to binary streams, as run,
 *   build and check already do, so their text is identical on every host.
 */
#include "xcli.h"
#include "shared/xr_string_core.h"
#include "xcli_fs.h"
#include "../../api/xsource_compile.h"
#include "../../api/xsource_paths.h"
#include "../../xir/xxir.h"
#include <stdio.h>
#include <string.h>
#if XR_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

typedef struct XrCliTestTally {
    uint32_t tests, skipped, hooks;
} XrCliTestTally;

typedef struct XrCliTestDiscoverOpts {
    const char *filter; /* NULL keeps every discovered test */
    FILE *report;
    bool verbose;
} XrCliTestDiscoverOpts;

static const char *test_role_name(uint32_t role) {
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

/* Imported modules never run their tests, so only the entry module's roles count. */
static bool test_discover(const XrXirModule *module, const XrCliTestDiscoverOpts *opts,
    XrCliTestTally *tally) {
    if (!module || !module->declarations || !module->declarations->functions || !module->functions)
        return false;
    uint32_t entry_identity = module->declarations->entry_function;
    if (entry_identity >= module->function_count) return false;
    uint32_t entry_module = module->declarations->functions[entry_identity].module;
    size_t filter_length = opts->filter ? strlen(opts->filter) : 0;
    *tally = (XrCliTestTally) {0};
    for (uint32_t i = 0; i < module->function_count; ++i) {
        const XrXirFunctionIdentity *identity = &module->declarations->functions[i];
        if (identity->module != entry_module || !identity->test_role) continue;
        bool is_test = identity->test_role == XR_XIR_TEST_ROLE_TEST
            || identity->test_role == XR_XIR_TEST_ROLE_SKIP;
        const XrXirFunction *function = &module->functions[i];
        if (is_test && opts->filter && !xr_string_core_contains(function->name,
                function->name_length, opts->filter, filter_length)) continue;
        if (!is_test) ++tally->hooks;
        else if (identity->test_role == XR_XIR_TEST_ROLE_SKIP) ++tally->skipped;
        else ++tally->tests;
        if (!opts->verbose) continue;
        fprintf(opts->report, "  %-11s ", test_role_name(identity->test_role));
        if (function->name_length) fwrite(function->name, 1, function->name_length, opts->report);
        if (identity->test_timeout_milliseconds)
            fprintf(opts->report, " timeout=%u", identity->test_timeout_milliseconds);
        fprintf(opts->report, "\n");
    }
    return true;
}

/* Options that promise an execution outcome cannot be honored before it exists. */
static bool test_refuse_execution_options(const XrCliInvocation *inv) {
    if (xr_cli_opt_string(&inv->options, "report", NULL)) {
        xr_cli_error("test", "--report needs backend test-role startup; there are no outcomes yet");
        return false;
    }
    return true;
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
    bool quiet = xr_cli_opt_present(&inv->options, "quiet");
    bool fail_fast = xr_cli_opt_present(&inv->options, "fail-fast");
    /* -j only governs running tests; discovery is a single check per file. */
    if (xr_cli_opt_int(&inv->options, "jobs", 1) < 1) {
        xr_cli_error("test", "parallel jobs must be at least one");
        return XR_CLI_EXIT_USAGE;
    }
    if (xr_cli_opt_present(&inv->options, "sandbox")) {
        xr_cli_error("test", "sandbox execution is temporarily unavailable");
        return XR_CLI_EXIT_FAIL;
    }
    if (!test_refuse_execution_options(inv)) return XR_CLI_EXIT_USAGE;
    XrCliTestDiscoverOpts discover = {
        .filter = xr_cli_opt_string(&inv->options, "filter", NULL),
        /* Nothing runs yet, so stdout stays empty and the report goes to stderr. */
        .report = stderr,
        .verbose = xr_cli_opt_present(&inv->options, "verbose") && !quiet,
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
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    if (xr_compile_resources_new(&limits, &resources) != XR_COMPILE_RESOURCE_OK) {
        xr_cli_error("test", "cannot initialize compiler resources");
        xr_cli_filelist_free(&files);
        return XR_CLI_EXIT_INTERNAL;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    int failures = 0;
    uint32_t total_tests = 0, total_skipped = 0, total_hooks = 0;
    for (int f = 0; f < files.count; ++f) {
        XrSourcePaths paths = {0};
        XrSourcePathsDiagnostic path_diagnostic = {0};
        XrSourceCompileDiagnostic diagnostic = {0};
        XrXirSourceResult source = {0};
        XrSourceCompileStatus status = xr_source_paths(resources, files.paths[f], &paths,
            &path_diagnostic);
        if (status == XR_SOURCE_COMPILE_OK) {
            XrSourceCompileRequest request = {&context, paths.entry, paths.stdlib, NULL,
                xr_source_compile_default_manifest_limits(), NULL, xr_xir_target_host()};
            status = xr_source_compile_check(&request, &source, &diagnostic);
        }
        if (status != XR_SOURCE_COMPILE_OK) {
            ++failures;
            if (!quiet) {
                if (diagnostic.status != XR_SOURCE_COMPILE_OK) {
                    char *report = NULL;
                    (void)xr_source_compile_diagnostic_render(&diagnostic, resources, &report);
                    xr_cli_error("test", "%s: %s", files.paths[f],
                        report ? report : xr_source_compile_status_name(diagnostic.status));
                    xr_compile_resources_free(report);
                } else {
                    char text[4096];
                    xr_source_paths_diagnostic_format(&path_diagnostic, text, sizeof(text));
                    xr_cli_error("test", "%s: %s", files.paths[f], text);
                }
            }
        } else {
            XrCliTestTally tally = {0};
            if (test_discover(xr_xir_compile_artifact_module(source.checked), &discover, &tally)) {
                total_tests += tally.tests;
                total_skipped += tally.skipped;
                total_hooks += tally.hooks;
                if (!quiet && discover.verbose)
                    fprintf(discover.report, "xray test: %s: %u test(s), %u skipped, %u hook(s)\n",
                        files.paths[f], tally.tests, tally.skipped, tally.hooks);
            } else {
                ++failures;
                if (!quiet) xr_cli_error("test", "%s: no entry module to discover tests from",
                    files.paths[f]);
            }
        }
        xr_source_compile_diagnostic_free(&diagnostic);
        xr_xir_compile_source_result_free(&source);
        xr_source_paths_free(&paths);
        xr_source_paths_diagnostic_free(&path_diagnostic);
        if (failures && fail_fast) {
            if (!quiet) fprintf(discover.report, "xray test: stopping after the first failure\n");
            break;
        }
    }
    xr_cli_filelist_free(&files);
    xr_compile_resources_release(resources);
    if (!quiet) {
        if (failures) fprintf(discover.report, "xray test: %d checked file(s) failed\n", failures);
        /* No test has run, so the results stream stays empty: the discovery
         * report and the reason it cannot become an outcome both go to stderr. */
        fprintf(discover.report, "xray test: running tests needs backend test-role startup; "
            "discovery only\n");
        fprintf(discover.report, "xray test: discovered %u test(s), %u skipped, %u hook(s)\n",
            total_tests, total_skipped, total_hooks);
    }
    return XR_CLI_EXIT_FAIL;
}
