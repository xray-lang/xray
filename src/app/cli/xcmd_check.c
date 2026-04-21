/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_check.c - 'xray check' command for syntax checking
 *
 * KEY CONCEPT:
 *   Checks source files for syntax errors without executing code.
 *   Designed for IDE integration and CI/CD pipelines.
 */

#include "xcli.h"
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <getopt.h>
#include "../../base/xmalloc.h"

// Check single file, returns: 0 = no error, 1 = has error
static int check_file(XrayIsolate *X, XaAnalyzer *analyzer,
                      const char *path, int verbose) {
    char *source = cli_read_file(path);
    if (!source) {
        fprintf(stderr, "Error: cannot read file '%s'\n", path);
        return 1;
    }

    // Parse source code - NULL result means syntax error
    AstNode *ast = xr_parse_with_source(X, source, path);

    int has_error = (ast == NULL);
    if (has_error) {
        // Error message already printed by parser
    } else if (analyzer) {
        // --strict mode: run analyzer type checking
        xa_analyzer_analyze(analyzer, path, (XrAstNode *)ast);
        int diag_count = 0;
        XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
        for (XaDiagnostic *d = diags; d; d = d->next) {
            if (d->severity == XR_DIAG_SEV_ERROR) {
                has_error = 1;
            }
            const char *sev = "error";
            if (d->severity == XR_DIAG_SEV_WARNING) sev = "warning";
            else if (d->severity == XR_DIAG_SEV_INFO) sev = "info";
            else if (d->severity == XR_DIAG_SEV_HINT) sev = "hint";
            fprintf(stderr, "%s:%d:%d: %s: %s\n",
                    path, d->location.line, d->location.column,
                    sev, d->message);
        }
        xa_analyzer_clear_diagnostics(analyzer);
        if (!has_error && verbose) {
            printf("ok %s\n", path);
        }
    } else if (verbose) {
        printf("ok %s\n", path);
    }

    // Release resources
    if (ast) {
        xr_program_destroy(ast);
    }
    xr_free(source);

    return has_error;
}


// Recursively check directory, returns: error file count
static int check_directory(XrayIsolate *X, XaAnalyzer *analyzer,
                           const char *path, int verbose, int *total, int *passed) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Error: cannot open directory '%s'\n", path);
        return 1;
    }

    int errors = 0;
    struct dirent *entry;
    char filepath[1024];

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            // Recursively check subdirectory
            errors += check_directory(X, analyzer, filepath, verbose, total, passed);
        } else if (S_ISREG(st.st_mode) && cli_is_xr_file(entry->d_name)) {
            // Check .xr file
            (*total)++;
            if (check_file(X, analyzer, filepath, verbose) == 0) {
                (*passed)++;
            } else {
                errors++;
            }
        }
    }

    closedir(dir);
    return errors;
}

static struct option check_long_options[] = {
    {"verbose", no_argument, 0, 'v'},
    {"quiet",   no_argument, 0, 'q'},
    {"strict",  no_argument, 0, 's'},
    {"help",    no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

void print_check_help(void) {
    printf("Usage: xray check [options] <file or directory...>\n");
    printf("\n");
    printf("Check source files for syntax errors without executing code.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -v, --verbose    Show all checked files\n");
    printf("  -q, --quiet      Show errors only\n");
    printf("  -s, --strict     Enable type checking via Analyzer\n");
    printf("  -h, --help       Show help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  xray check app.xr              # Check single file\n");
    printf("  xray check src/                # Check directory\n");
    printf("  xray check -v src/ tests/      # Verbose mode\n");
    printf("\n");
}

int cmd_check(int argc, char **argv) {
    int verbose = 0;
    int quiet = 0;
    int strict = 0;

    // Reset getopt state
    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "vqsh", check_long_options, NULL)) != -1) {
        switch (opt) {
            case 'v':
                verbose = 1;
                break;
            case 'q':
                quiet = 1;
                break;
            case 's':
                strict = 1;
                break;
            case 'h':
                print_check_help();
                return 0;
            default:
                print_check_help();
                return 1;
        }
    }

    // Create shared isolate for all checks
    XrayIsolate *X = cli_create_isolate();
    if (!X) {
        fprintf(stderr, "Error: cannot create parsing environment\n");
        return 1;
    }

    // Create analyzer for --strict mode
    XaAnalyzer *analyzer = NULL;
    if (strict) {
        analyzer = xa_analyzer_new();
        xa_analyzer_set_strict_mode(analyzer, true);
    }

    int result = 0;

    // No file arguments - check current directory
    if (optind >= argc) {
        struct stat st;
        if (stat(".", &st) == 0 && S_ISDIR(st.st_mode)) {
            int total = 0, passed = 0;
            int errors = check_directory(X, analyzer, ".", verbose, &total, &passed);

            if (!quiet && total > 0) {
                printf("\n");
                if (errors == 0) {
                    printf("OK: %d files checked, no errors\n", total);
                } else {
                    printf("FAIL: %d files checked, %d errors\n", total, errors);
                }
            }
            result = errors > 0 ? 1 : 0;
        } else {
            fprintf(stderr, "Usage: xray check <file or directory...>\n");
            result = 1;
        }
    } else {
        // Check specified files or directories
        int total_files = 0;
        int passed_files = 0;
        int total_errors = 0;

        for (int i = optind; i < argc; i++) {
            const char *path = argv[i];
            struct stat st;

            if (stat(path, &st) != 0) {
                fprintf(stderr, "Error: path does not exist '%s'\n", path);
                total_errors++;
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                total_errors += check_directory(X, analyzer, path, verbose, &total_files, &passed_files);
            } else if (S_ISREG(st.st_mode)) {
                total_files++;
                if (check_file(X, analyzer, path, verbose) == 0) {
                    passed_files++;
                } else {
                    total_errors++;
                }
            }
        }

        // Output statistics
        if (!quiet && total_files > 1) {
            printf("\n");
            if (total_errors == 0) {
                printf("OK: %d files checked, no errors\n", total_files);
            } else {
                printf("FAIL: %d files checked, %d errors\n", total_files, total_errors);
            }
        }

        result = total_errors > 0 ? 1 : 0;
    }

    if (analyzer) xa_analyzer_free(analyzer);
    xray_isolate_delete(X);
    return result;
}
