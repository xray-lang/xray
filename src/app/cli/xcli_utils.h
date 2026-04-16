/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_utils.h - Common utility functions for CLI commands
 *
 * KEY CONCEPT:
 *   Provides file I/O and string utilities shared across CLI subcommands.
 */

#ifndef XCLI_UTILS_H
#define XCLI_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include "../../base/xdefs.h"

// Path buffer size used across CLI subcommands
#define CLI_PATH_MAX 1024

// Unified ANSI color codes for CLI output
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_DIM     "\033[2m"
#define CLR_RED     "\033[1;31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_GRAY    "\033[90m"

// Read entire file content (caller must free), returns NULL on failure
XR_FUNC char* cli_read_file(const char *path);

// Read all content from stdin (caller must free), returns NULL on failure
XR_FUNC char* cli_read_stdin(void);

// Write content to file, returns 0 on success, -1 on failure
XR_FUNC int cli_write_file(const char *path, const char *content);

XR_FUNC bool cli_file_exists(const char *path);
XR_FUNC bool cli_is_xr_file(const char *filename);
XR_FUNC bool cli_is_directory(const char *path);

// Compute string distance for command suggestions
XR_FUNC int cli_string_distance(const char *s1, const char *s2);

// Safe integer parsing (returns false on error)
XR_FUNC bool cli_parse_int(const char *str, int *out);

// Safe port number parsing (0-65535, returns false on error)
XR_FUNC bool cli_parse_port(const char *str, int *out);

// Monotonic time in milliseconds (for performance measurement)
XR_FUNC double cli_get_time_ms(void);

// Create a default isolate with full setup (parser+compiler+stdlib).
// Convenience wrapper to eliminate repeated boilerplate across subcommands.
typedef struct XrayIsolate XrayIsolate;
XR_FUNC XrayIsolate* cli_create_isolate(void);

#endif // XCLI_UTILS_H
