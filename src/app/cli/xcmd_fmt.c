/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_fmt.c - 'xray fmt' command for code formatting
 *
 * KEY CONCEPT:
 *   AST-based formatter for Xray source code.
 *   Parses code to AST and regenerates with consistent style.
 *   Handles space-sensitive generic syntax correctly.
 */

#include "xcli.h"
#include "xcli_utils.h"
#include "xray.h"
#include "xray_isolate.h"
#include "xfmt.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <getopt.h>
#include "../../base/xmalloc.h"

// Format configuration
typedef struct {
    int indent_size;        // Indent spaces (default 4)
    int use_tabs;           // Use tabs instead of spaces
    int max_line_length;    // Max line length hint (default 100)
    int trailing_newline;   // Ensure trailing newline at EOF
} FmtConfig;

static FmtConfig default_config = {
    .indent_size = 4,
    .use_tabs = 0,
    .max_line_length = 100,
    .trailing_newline = 1
};

// Format source code using AST
static char* format_source(XrayIsolate *X, const char *source, const char *path, FmtConfig *config) {
    // Parse source to AST with trivia collection (preserves comments)
    AstNode *ast = xr_parse_with_trivia(X, source, path);
    if (!ast) {
        // Parse failed - return NULL to indicate error
        return NULL;
    }
    
    // Convert to XrFmtConfig
    XrFmtConfig xfmt_config = {
        .indent_size = config->indent_size,
        .use_tabs = config->use_tabs,
        .max_line_length = config->max_line_length,
        .trailing_newline = config->trailing_newline,
        .blank_lines_around_functions = 1,
        .blank_lines_around_classes = 1,
        .space_around_operators = 1,
        .space_after_comma = 1,
        .space_in_parentheses = 0,
        .brace_same_line = 1
    };
    
    // Format AST to string
    return xfmt_format_ast(ast, &xfmt_config, X);
}

// Format single file
// Returns: 0 = no change, 1 = formatted, -1 = error
static int format_file(XrayIsolate *X, const char *path, FmtConfig *config, int check_only, int verbose) {
    char *source = cli_read_file(path);
    if (!source) {
        fprintf(stderr, "Error: cannot read file '%s'\n", path);
        return -1;
    }
    
    char *formatted = format_source(X, source, path, config);
    if (!formatted) {
        xr_free(source);
        fprintf(stderr, "Error: formatting failed '%s' (syntax error?)\n", path);
        return -1;
    }
    
    int changed = strcmp(source, formatted) != 0;
    
    if (changed) {
        if (check_only) {
            printf("Needs formatting: %s\n", path);
        } else {
            if (cli_write_file(path, formatted) != 0) {
                fprintf(stderr, "Error: cannot write file '%s'\n", path);
                xr_free(source);
                xr_free(formatted);
                return -1;
            }
            if (verbose) {
                printf("Formatted: %s\n", path);
            }
        }
    } else {
        if (verbose) {
            printf("Unchanged: %s\n", path);
        }
    }
    
    xr_free(source);
    xr_free(formatted);
    return changed ? 1 : 0;
}

// Recursively format directory
static int format_directory(XrayIsolate *X, const char *path, FmtConfig *config, 
                           int check_only, int verbose, int *total, int *changed) {
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Error: cannot open directory '%s'\n", path);
        return -1;
    }
    
    int errors = 0;
    struct dirent *entry;
    char filepath[1024];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(filepath, &st) != 0) continue;
        
        if (S_ISDIR(st.st_mode)) {
            // Skip hidden directories and build directories
            if (entry->d_name[0] == '.' || 
                strcmp(entry->d_name, "node_modules") == 0 ||
                strcmp(entry->d_name, "build") == 0 ||
                strcmp(entry->d_name, "build-asan") == 0 ||
                strcmp(entry->d_name, "build-release") == 0) {
                continue;
            }
            format_directory(X, filepath, config, check_only, verbose, total, changed);
        } else if (S_ISREG(st.st_mode) && cli_is_xr_file(entry->d_name)) {
            (*total)++;
            int result = format_file(X, filepath, config, check_only, verbose);
            if (result > 0) (*changed)++;
            if (result < 0) errors++;
        }
    }
    
    closedir(dir);
    return errors;
}

static struct option fmt_long_options[] = {
    {"check",   no_argument,       0, 'c'},
    {"verbose", no_argument,       0, 'v'},
    {"tabs",    no_argument,       0, 't'},
    {"indent",  required_argument, 0, 'i'},
    {"help",    no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

void print_fmt_help(void) {
    printf("Usage: xray fmt [options] <file or directory...>\n");
    printf("\n");
    printf("Format Xray source code using AST-based formatting.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --check      Check only, do not modify files\n");
    printf("  -i, --indent <n> Indent spaces (default 4)\n");
    printf("  -t, --tabs       Use tab indent\n");
    printf("  -v, --verbose    Show all processed files\n");
    printf("  -h, --help       Show help\n");
    printf("\n");
    printf("Space-sensitive generics:\n");
    printf("  foo<int>()     -> Generic call (no space before <)\n");
    printf("  a < b          -> Comparison (space before <)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  xray fmt app.xr            # Format single file\n");
    printf("  xray fmt src/              # Format directory\n");
    printf("  xray fmt --check src/      # Check if formatting needed\n");
    printf("  xray fmt --indent 2 .      # Use 2-space indent\n");
    printf("\n");
}

int cmd_fmt(int argc, char **argv) {
    FmtConfig config = default_config;
    int check_only = 0;
    int verbose = 0;
    
    // Reset getopt state
    optind = 1;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "cvti:h", fmt_long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                check_only = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 't':
                config.use_tabs = 1;
                break;
            case 'i':
                if (!cli_parse_int(optarg, &config.indent_size) || config.indent_size < 1 || config.indent_size > 16) {
                    fprintf(stderr, "Error: invalid indent size '%s' (expected 1-16)\n", optarg);
                    return 1;
                }
                break;
            case 'h':
                print_fmt_help();
                return 0;
            default:
                print_fmt_help();
                return 1;
        }
    }
    
    // Create isolate for parsing
    XrayIsolate *X = cli_create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }
    
    int total = 0, changed = 0;
    int errors = 0;
    
    // No file arguments, format current directory
    if (optind >= argc) {
        errors = format_directory(X, ".", &config, check_only, verbose, &total, &changed);
    } else {
        // Format specified files or directories
        for (int i = optind; i < argc; i++) {
            const char *path = argv[i];
            struct stat st;
            
            if (stat(path, &st) != 0) {
                fprintf(stderr, "Error: path does not exist '%s'\n", path);
                errors++;
                continue;
            }
            
            if (S_ISDIR(st.st_mode)) {
                errors += format_directory(X, path, &config, check_only, verbose, &total, &changed);
            } else if (S_ISREG(st.st_mode)) {
                total++;
                int result = format_file(X, path, &config, check_only, verbose);
                if (result > 0) changed++;
                if (result < 0) errors++;
            }
        }
    }
    
    // Cleanup isolate
    xray_isolate_delete(X);
    
    // Output statistics
    if (total > 0) {
        printf("\n");
        if (check_only) {
            if (changed > 0) {
                printf("%d files need formatting (of %d)\n", changed, total);
            } else {
                printf("OK: all %d files properly formatted\n", total);
            }
        } else {
            printf("Formatted %d files (of %d)\n", changed, total);
        }
    }
    
    return (check_only && changed > 0) || errors > 0 ? 1 : 0;
}
