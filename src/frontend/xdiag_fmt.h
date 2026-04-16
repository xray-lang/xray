/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdiag_fmt.h - diagnostic formatting for compiler errors/warnings
 *
 * KEY CONCEPT:
 *   Provides source-line display with caret indicators, producing output like:
 *
 *   error: expected expression
 *    --> src/main.xr:5:9
 *     |
 *   5 | let y = +
 *     |         ^ expected expression
 */

#ifndef XDIAG_FMT_H
#define XDIAG_FMT_H

#include <stdio.h>
#include <string.h>

// ANSI color codes
#define XR_CLR_RESET   "\033[0m"
#define XR_CLR_BOLD    "\033[1m"
#define XR_CLR_RED     "\033[1;31m"
#define XR_CLR_YELLOW  "\033[1;33m"
#define XR_CLR_BLUE    "\033[1;34m"
#define XR_CLR_CYAN    "\033[1;36m"

typedef enum {
    XR_DIAG_ERROR,
    XR_DIAG_WARNING,
    XR_DIAG_NOTE
} XrDiagLevel;

/*
 * Find the start of the line containing 'pos' in source text.
 * Returns pointer to the first character of that line.
 */
static inline const char *xr_diag_find_line_start(const char *source, const char *pos) {
    const char *p = pos;
    while (p > source && *(p - 1) != '\n') {
        p--;
    }
    return p;
}

/*
 * Find the end of the line containing 'pos' (points to '\n' or '\0').
 */
static inline const char *xr_diag_find_line_end(const char *pos) {
    const char *p = pos;
    while (*p != '\0' && *p != '\n') {
        p++;
    }
    return p;
}

/*
 * Count digits in a number (for alignment).
 */
static inline int xr_diag_num_digits(int n) {
    if (n < 10) return 1;
    if (n < 100) return 2;
    if (n < 1000) return 3;
    if (n < 10000) return 4;
    return 5;
}

/*
 * Print a diagnostic with source context.
 *
 * Parameters:
 *   level       - XR_DIAG_ERROR, XR_DIAG_WARNING, or XR_DIAG_NOTE
 *   code        - error code (0 = no code), e.g. 351 prints as [E0351]
 *   message     - the diagnostic message
 *   file        - source file path (for display)
 *   line        - 1-indexed line number
 *   column      - 1-indexed column number (0 = unknown)
 *   token_len   - length of the token to underline (0 = use single caret)
 *   source      - full source text (NULL = skip source display)
 *   token_start - pointer into source at the error token (NULL = skip source display)
 */
static inline void xr_diag_print(
    XrDiagLevel level,
    int code,
    const char *message,
    const char *file,
    int line,
    int column,
    int token_len,
    const char *source,
    const char *token_start
) {
    if (!file) file = "<script>";
    if (column <= 0) column = 1;
    if (token_len <= 0) token_len = 1;

    // Level label
    const char *level_color;
    const char *level_name;
    const char *code_prefix;
    switch (level) {
        case XR_DIAG_ERROR:
            level_color = XR_CLR_RED;
            level_name = "error";
            code_prefix = "E";
            break;
        case XR_DIAG_WARNING:
            level_color = XR_CLR_YELLOW;
            level_name = "warning";
            code_prefix = "W";
            break;
        case XR_DIAG_NOTE:
        default:
            level_color = XR_CLR_CYAN;
            level_name = "note";
            code_prefix = "N";
            break;
    }

    // Line 1: level + code + message
    if (code > 0) {
        fprintf(stderr, "%s%s[%s%04d]%s: %s\n",
                level_color, level_name, code_prefix, code, XR_CLR_RESET, message);
    } else {
        fprintf(stderr, "%s%s%s: %s\n",
                level_color, level_name, XR_CLR_RESET, message);
    }

    // Line 2: file location
    int gutter = xr_diag_num_digits(line);
    fprintf(stderr, " %*s%s-->%s %s:%d:%d\n",
            gutter, "", XR_CLR_BLUE, XR_CLR_RESET, file, line, column);

    // Source context (skip if no source available)
    if (source && token_start && token_start >= source) {
        const char *line_start = xr_diag_find_line_start(source, token_start);
        const char *line_end = xr_diag_find_line_end(line_start);
        int line_len = (int)(line_end - line_start);

        // Blank gutter line
        fprintf(stderr, " %*s %s|%s\n", gutter, "", XR_CLR_BLUE, XR_CLR_RESET);

        // Source line
        fprintf(stderr, " %s%*d%s %s|%s %.*s\n",
                XR_CLR_BLUE, gutter, line, XR_CLR_RESET,
                XR_CLR_BLUE, XR_CLR_RESET,
                line_len, line_start);

        // Caret/underline line
        int col_offset = (int)(token_start - line_start);
        if (col_offset < 0) col_offset = 0;
        if (col_offset > line_len) col_offset = line_len;

        // Clamp underline to not exceed line
        int underline_len = token_len;
        if (col_offset + underline_len > line_len) {
            underline_len = line_len - col_offset;
        }
        if (underline_len < 1) underline_len = 1;

        fprintf(stderr, " %*s %s|%s ", gutter, "", XR_CLR_BLUE, XR_CLR_RESET);
        // Spaces to reach the column
        for (int i = 0; i < col_offset; i++) {
            // Preserve tab alignment
            fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
        }
        // Underline carets
        fprintf(stderr, "%s", level_color);
        for (int i = 0; i < underline_len; i++) {
            fputc('^', stderr);
        }
        fprintf(stderr, "%s\n", XR_CLR_RESET);
    }

    fprintf(stderr, "\n");
}

/*
 * Print error summary line ().
 *
 *   error: aborting due to 3 previous errors
 */
static inline void xr_diag_print_summary(
    const char *file,
    int error_count,
    int warning_count,
    int max_errors_reached
) {
    if (!file) file = "<script>";

    if (max_errors_reached && error_count > 0) {
        fprintf(stderr, "%serror%s: could not compile `%s`: too many errors emitted, stopping now\n",
                XR_CLR_RED, XR_CLR_RESET, file);
    }

    if (error_count > 0) {
        fprintf(stderr, "%serror%s: aborting due to %d previous error%s",
                XR_CLR_RED, XR_CLR_RESET, error_count, error_count > 1 ? "s" : "");
        if (warning_count > 0) {
            fprintf(stderr, "; %d warning%s emitted",
                    warning_count, warning_count > 1 ? "s" : "");
        }
        fprintf(stderr, "\n");
    } else if (warning_count > 0) {
        fprintf(stderr, "%swarning%s: %d warning%s emitted\n",
                XR_CLR_YELLOW, XR_CLR_RESET, warning_count, warning_count > 1 ? "s" : "");
    }
}

#endif // XDIAG_FMT_H
