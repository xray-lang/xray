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
 *   5 | var y = +
 *     |         ^ expected expression
 */

#ifndef XDIAG_FMT_H
#define XDIAG_FMT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include "../base/xchecks.h"
#include "../os/os_fd.h"

/*
 * ROLE PIN:
 *   This header is the SOLE diagnostic-formatting helper allowed to be
 *   shared across the frontend (lexer / parser / analyzer / codegen).
 *   It owns ONLY string assembly: ANSI colour wrapping, gutter padding,
 *   caret underline, source-line slicing. It MUST NOT take on any
 *   higher-level semantics -- no AST awareness, no parser-state lookup,
 *   no error counting that callers do not pass in explicitly. If a
 *   future caller is tempted to thread richer context through this
 *   API, the right move is to add a thin wrapper at the call site
 *   instead of growing this header.
 *
 *   The XR_DCHECK guards below pin the invariants the call sites
 *   already rely on. Cases with an explicit graceful fallback in the
 *   body (e.g. `file == NULL -> "<script>"`) intentionally do NOT
 *   get a DCHECK -- the c-coding rule forbids the contradictory
 *   pattern `DCHECK(ptr != NULL); if (!ptr) ...`.
 */

// ANSI color codes
#define XR_CLR_RESET "\033[0m"
#define XR_CLR_BOLD "\033[1m"
#define XR_CLR_RED "\033[1;31m"
#define XR_CLR_YELLOW "\033[1;33m"
#define XR_CLR_BLUE "\033[1;34m"
#define XR_CLR_CYAN "\033[1;36m"

/*
 * Whether diagnostics should be colourised. Diagnostics are written to stderr,
 * so the decision gates on stderr — not stdout, which the CLI colour helper
 * uses. Two signals, in order:
 *
 *   1. NO_COLOR (https://no-color.org): any non-empty value forces colour off.
 *      This is the cross-platform, tool-agnostic opt-out — it works the same on
 *      a Windows Git-Bash shell as on a Unix pipe, and it is what CI and test
 *      harnesses set.
 *   2. Otherwise, auto-detect: colour only when stderr is a real terminal.
 *      Every redirected or piped run (tests, `$(...)`, `2>&1 | ...`, files)
 *      therefore gets clean, un-escaped text with no per-caller stripping.
 *
 * Computed once per diagnostic call and cached in a local; the isatty syscall
 * is not on any hot path.
 */
static inline bool xr_diag_use_color(void) {
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0')
        return false;
    return xr_isatty(xr_stderr_fd());
}

typedef enum {
    XR_DIAG_ERROR,
    XR_DIAG_WARNING,
    XR_DIAG_NOTE
} XrDiagLevel;

/* Policies are borrowed for one call. The reader does no hidden scanning;
 * work is charged before each read. Writers charge their actual memory copies
 * or requested I/O bytes before attempting them and report IO separately. */
typedef enum XrDiagStatus {
    XR_DIAG_OK, XR_DIAG_BAD_ARGUMENT, XR_DIAG_RESOURCE, XR_DIAG_IO
} XrDiagStatus;
typedef struct XrDiagPolicy {
    void *context;
    bool (*read)(void *, const char *, char *);
    XrDiagStatus (*write)(void *, char);
    bool (*work)(void *, uint64_t);
    XrDiagStatus status;
    bool color;
} XrDiagPolicy;

typedef struct XrDiagBuffer {
    char *data;
    size_t capacity, used;
    void *work_context;
    bool (*charge)(void *, uint64_t);
} XrDiagBuffer;
typedef struct XrDiagOutput {
    FILE *stream;
    void *work_context;
    bool (*charge)(void *, uint64_t);
} XrDiagOutput;

static inline bool xr_diag_direct_read(void *context, const char *address, char *output) {
    (void) context; *output = *address; return true;
}
static inline bool xr_diag_system_work(void *context, uint64_t amount) {
    (void) context; (void) amount; return true;
}
static inline bool xr_diag_buffer_work(void *context, uint64_t amount) {
    XrDiagBuffer *buffer = context;
    return buffer->charge(buffer->work_context, amount);
}
static inline XrDiagStatus xr_diag_buffer_write(void *context, char byte) {
    XrDiagBuffer *buffer = context;
    if (!buffer || !buffer->data || !buffer->capacity || !buffer->charge) return XR_DIAG_BAD_ARGUMENT;
    if (buffer->used < buffer->capacity - 1) {
        if (!buffer->charge(buffer->work_context, 1)) return XR_DIAG_RESOURCE;
        buffer->data[buffer->used++] = byte;
    }
    return XR_DIAG_OK;
}
static inline bool xr_diag_output_work(void *context, uint64_t amount) {
    XrDiagOutput *output = context;
    return output->charge(output->work_context, amount);
}
static inline XrDiagStatus xr_diag_output_write(void *context, char byte) {
    XrDiagOutput *output = context;
    if (!output->charge(output->work_context, 1)) return XR_DIAG_RESOURCE;
    return fputc((unsigned char) byte, output->stream) == EOF ? XR_DIAG_IO : XR_DIAG_OK;
}
static inline bool xr_diag_charge(XrDiagPolicy *policy, uint64_t amount) {
    if (!policy || policy->status != XR_DIAG_OK) return false;
    if (!policy->work || !policy->read || !policy->write) {
        policy->status = XR_DIAG_BAD_ARGUMENT; return false;
    }
    if (!policy->work(policy->context, amount)) {
        policy->status = XR_DIAG_RESOURCE; return false;
    }
    return true;
}
static inline bool xr_diag_read(XrDiagPolicy *policy, const char *address, char *output) {
    if (!xr_diag_charge(policy, 1)) return false;
    if (!policy->read(policy->context, address, output)) {
        policy->status = XR_DIAG_IO; return false;
    }
    return true;
}
static inline bool xr_diag_emit(XrDiagPolicy *policy, char byte) {
    if (!policy || policy->status != XR_DIAG_OK) return false;
    policy->status = policy->write(policy->context, byte);
    return policy->status == XR_DIAG_OK;
}
static inline bool xr_diag_padding(XrDiagPolicy *policy, char byte, size_t count) {
    while (count--) if (!xr_diag_emit(policy, byte)) return false;
    return true;
}

/* The frontend uses only strings, characters and decimal integers. This one
 * bounded formatter covers those formats, width/precision and %llu/%zu; an
 * unsupported format is BAD_ARGUMENT rather than an unmetered CRT fallback. */
static inline XrDiagStatus xr_diag_format_v(XrDiagPolicy *policy, const char *format, va_list arguments) {
    if (!policy) return XR_DIAG_BAD_ARGUMENT;
    if (policy->status != XR_DIAG_OK) return policy->status;
    if (!format) return policy->status = XR_DIAG_BAD_ARGUMENT;
    if (!xr_diag_charge(policy, 0)) return policy->status;
    const char *p = format;
    char spec;
    while (xr_diag_read(policy, p++, &spec) && spec) {
        if (spec != '%') {
            if (!xr_diag_emit(policy, spec)) break;
            continue;
        }
        if (!xr_diag_read(policy, p++, &spec)) break;
        if (spec == '%') { if (!xr_diag_emit(policy, '%')) break; continue; }
        bool zero = spec == '0';
        if (zero && !xr_diag_read(policy, p++, &spec)) break;
        int width = 0;
        if (spec == '*') {
            width = va_arg(arguments, int);
            if (!xr_diag_read(policy, p++, &spec)) break;
        } else {
            while (spec >= '0' && spec <= '9') {
                if (width > (INT_MAX - (spec - '0')) / 10) {
                    policy->status = XR_DIAG_BAD_ARGUMENT; return policy->status;
                }
                width = width * 10 + spec - '0';
                if (!xr_diag_read(policy, p++, &spec)) return policy->status;
            }
        }
        if (width < 0) { policy->status = XR_DIAG_BAD_ARGUMENT; break; }
        int precision = -1;
        if (spec == '.') {
            if (!xr_diag_read(policy, p++, &spec)) break;
            if (spec == '*') {
                precision = va_arg(arguments, int);
                if (!xr_diag_read(policy, p++, &spec)) break;
            } else {
                precision = 0;
                while (spec >= '0' && spec <= '9') {
                    if (precision > (INT_MAX - (spec - '0')) / 10) {
                        policy->status = XR_DIAG_BAD_ARGUMENT; return policy->status;
                    }
                    precision = precision * 10 + spec - '0';
                    if (!xr_diag_read(policy, p++, &spec)) return policy->status;
                }
            }
        }
        enum { INTEGER_INT, INTEGER_LONG, INTEGER_LLONG, INTEGER_SIZE } integer = INTEGER_INT;
        if (spec == 'l') {
            integer = INTEGER_LONG;
            if (!xr_diag_read(policy, p++, &spec)) break;
            if (spec == 'l') {
                integer = INTEGER_LLONG;
                if (!xr_diag_read(policy, p++, &spec)) break;
            }
        } else if (spec == 'z') {
            integer = INTEGER_SIZE;
            if (!xr_diag_read(policy, p++, &spec)) break;
        }
        if (spec == 's' && integer == INTEGER_INT) {
            const char *text = va_arg(arguments, const char *);
            if (!text) text = "(null)";
            size_t limit = precision < 0 ? SIZE_MAX : (size_t) precision;
            if (width) {
                size_t length = 0;
                for (; length < limit; ++length) {
                    char byte;
                    if (!xr_diag_read(policy, text + length, &byte)) return policy->status;
                    if (!byte) break;
                }
                if ((size_t) width > length && !xr_diag_padding(policy, ' ', (size_t) width - length)) break;
            }
            for (size_t i = 0; i < limit; ++i) {
                char byte;
                if (!xr_diag_read(policy, text + i, &byte)) return policy->status;
                if (!byte) break;
                if (!xr_diag_emit(policy, byte)) return policy->status;
            }
        } else if (spec == 'c' && integer == INTEGER_INT) {
            char byte = (char) va_arg(arguments, int);
            if (width > 1 && !xr_diag_padding(policy, ' ', (size_t) width - 1)) break;
            if (!xr_diag_emit(policy, byte)) break;
        } else if (spec == 'd' || spec == 'i' || spec == 'u') {
            uint64_t value; bool negative = false;
            if (spec == 'u') {
                value = integer == INTEGER_LLONG ? va_arg(arguments, unsigned long long) :
                        integer == INTEGER_LONG ? va_arg(arguments, unsigned long) :
                        integer == INTEGER_SIZE ? va_arg(arguments, size_t) : va_arg(arguments, unsigned int);
            } else {
                int64_t signed_value = integer == INTEGER_LLONG ? va_arg(arguments, long long) :
                    integer == INTEGER_LONG ? va_arg(arguments, long) :
                    integer == INTEGER_SIZE ? (int64_t) va_arg(arguments, ptrdiff_t) : va_arg(arguments, int);
                negative = signed_value < 0;
                value = negative ? (uint64_t) (-(signed_value + 1)) + 1 : (uint64_t) signed_value;
            }
            char digits[20]; size_t count = 0;
            do {
                if (!xr_diag_charge(policy, 1)) return policy->status;
                digits[count++] = (char) ('0' + value % 10); value /= 10;
            } while (value);
            size_t occupied = count + (negative ? 1u : 0u);
            size_t padding = (size_t) width > occupied ? (size_t) width - occupied : 0;
            if (!zero && !xr_diag_padding(policy, ' ', padding)) break;
            if (negative && !xr_diag_emit(policy, '-')) break;
            if (zero && !xr_diag_padding(policy, '0', padding)) break;
            while (count) {
                char byte;
                if (!xr_diag_read(policy, &digits[--count], &byte) || !xr_diag_emit(policy, byte)) return policy->status;
            }
        } else { policy->status = XR_DIAG_BAD_ARGUMENT; break; }
    }
    return policy->status;
}
static inline XrDiagStatus xr_diag_format(XrDiagPolicy *policy, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    XrDiagStatus status = xr_diag_format_v(policy, format, arguments);
    va_end(arguments);
    return status;
}
static inline XrDiagStatus xr_diag_buffer_finish(XrDiagPolicy *policy, XrDiagBuffer *buffer) {
    if (policy->status != XR_DIAG_OK) return policy->status;
    if (!buffer || !buffer->data || !buffer->capacity) return policy->status = XR_DIAG_BAD_ARGUMENT;
    if (!buffer->charge(buffer->work_context, 1)) return policy->status = XR_DIAG_RESOURCE;
    buffer->data[buffer->used] = '\0';
    return XR_DIAG_OK;
}
static inline XrDiagStatus xr_diag_detect_color(XrDiagPolicy *policy) {
    if (!xr_diag_charge(policy, 1)) return policy->status;
    const char *no_color = getenv("NO_COLOR");
    char byte = 0;
    if (no_color && !xr_diag_read(policy, no_color, &byte)) return policy->status;
    if (byte) { policy->color = false; return XR_DIAG_OK; }
    if (!xr_diag_charge(policy, 1)) return policy->status;
    policy->color = xr_isatty(xr_stderr_fd());
    return XR_DIAG_OK;
}

/*
 * Find the start of the line containing 'pos' in source text.
 * Returns pointer to the first character of that line.
 */
static inline const char *xr_diag_find_line_start_policy(XrDiagPolicy *policy, const char *source, const char *pos) {
    // Walking below `source` would deref out-of-bounds memory. The
    // call sites already guarantee both invariants (token_start was
    // captured FROM source by the lexer); a violation here is a
    // corrupted token, not a recoverable user error.
    XR_DCHECK(source != NULL, "xr_diag_find_line_start: NULL source");
    XR_DCHECK(pos != NULL, "xr_diag_find_line_start: NULL pos");
    XR_DCHECK(pos >= source, "xr_diag_find_line_start: pos before source");
    const char *p = pos;
    while (p > source) {
        char byte;
        if (!xr_diag_read(policy, p - 1, &byte)) return NULL;
        if (byte == '\n') break;
        p--;
    }
    return p;
}

/*
 * Find the end of the line containing 'pos' (points to '\n' or '\0').
 */
static inline const char *xr_diag_find_line_end_policy(XrDiagPolicy *policy, const char *pos) {
    XR_DCHECK(pos != NULL, "xr_diag_find_line_end: NULL pos");
    const char *p = pos;
    for (;;) {
        char byte;
        if (!xr_diag_read(policy, p, &byte)) return NULL;
        if (!byte || byte == '\n') break;
        p++;
    }
    return p;
}

/*
 * Count digits in a number (for alignment).
 */
static inline int xr_diag_num_digits(int n) {
    // Negative line numbers are never legitimate; callers pass
    // `line` from the lexer (1-indexed) or from a default of 0 for
    // "unknown". A negative value would silently fall through to
    // the `return 5` branch and corrupt gutter alignment.
    XR_DCHECK(n >= 0, "xr_diag_num_digits: negative input");
    if (n < 10)
        return 1;
    if (n < 100)
        return 2;
    if (n < 1000)
        return 3;
    if (n < 10000)
        return 4;
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
static inline XrDiagStatus xr_diag_print_policy(XrDiagPolicy *policy, XrDiagLevel level, int code, const char *message, const char *file,
                                 int line, int column, int token_len, const char *source,
                                 const char *token_start) {
    // Caller-side invariants (no graceful fallback exists for these):
    //   - level must be a valid enum so the switch picks a colour;
    //   - message must be non-NULL or fprintf("%s") explodes;
    //   - line must be non-negative for digit-counting.
    XR_DCHECK(level == XR_DIAG_ERROR || level == XR_DIAG_WARNING || level == XR_DIAG_NOTE,
              "xr_diag_print: invalid level");
    XR_DCHECK(message != NULL, "xr_diag_print: NULL message");
    XR_DCHECK(line >= 0, "xr_diag_print: negative line");

    if (!file)
        file = "<script>";
    if (column <= 0)
        column = 1;
    if (token_len <= 0)
        token_len = 1;

    // Resolve colour once; every code below is empty when colour is off, so a
    // piped/redirected diagnostic is plain text with no escape sequences.
    if (!policy || policy->status != XR_DIAG_OK) return policy ? policy->status : XR_DIAG_BAD_ARGUMENT;
    bool use_color = policy->color;
    const char *c_reset = use_color ? XR_CLR_RESET : "";
    const char *c_blue = use_color ? XR_CLR_BLUE : "";

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
    if (!use_color)
        level_color = "";

    // Line 1: level + code + message
    if (code > 0) {
        if (xr_diag_format(policy, "%s%s[%s%04d]%s: %s\n", level_color, level_name, code_prefix, code, c_reset,
                message) != XR_DIAG_OK) return policy->status;
    } else {
        if (xr_diag_format(policy, "%s%s%s: %s\n", level_color, level_name, c_reset, message) != XR_DIAG_OK) return policy->status;
    }

    // Line 2: file location
    int gutter = xr_diag_num_digits(line);
    if (xr_diag_format(policy, " %*s%s-->%s %s:%d:%d\n", gutter, "", c_blue, c_reset, file, line, column) != XR_DIAG_OK) return policy->status;

    // Source context (skip if no source available)
    if (source && token_start && token_start >= source) {
        const char *line_start = xr_diag_find_line_start_policy(policy, source, token_start);
        if (!line_start) return policy->status;
        const char *line_end = xr_diag_find_line_end_policy(policy, line_start);
        if (!line_end) return policy->status;
        if (line_end - line_start > INT_MAX) return policy->status = XR_DIAG_BAD_ARGUMENT;
        int line_len = (int) (line_end - line_start);

        // Blank gutter line
        if (xr_diag_format(policy, " %*s %s|%s\n", gutter, "", c_blue, c_reset) != XR_DIAG_OK) return policy->status;

        // Source line
        if (xr_diag_format(policy, " %s%*d%s %s|%s %.*s\n", c_blue, gutter, line, c_reset, c_blue, c_reset,
                line_len, line_start) != XR_DIAG_OK) return policy->status;

        // Caret/underline line
        int col_offset = (int) (token_start - line_start);
        if (col_offset < 0)
            col_offset = 0;
        if (col_offset > line_len)
            col_offset = line_len;

        // Clamp underline to not exceed line
        int underline_len = token_len;
        if (underline_len > line_len - col_offset) {
            underline_len = line_len - col_offset;
        }
        if (underline_len < 1)
            underline_len = 1;

        if (xr_diag_format(policy, " %*s %s|%s ", gutter, "", c_blue, c_reset) != XR_DIAG_OK) return policy->status;
        // Spaces to reach the column
        for (int i = 0; i < col_offset; i++) {
            // Preserve tab alignment
            char byte;
            if (!xr_diag_read(policy, line_start + i, &byte) ||
                !xr_diag_emit(policy, byte == '\t' ? '\t' : ' ')) return policy->status;
        }
        // Underline carets
        if (xr_diag_format(policy, "%s", level_color) != XR_DIAG_OK) return policy->status;
        for (int i = 0; i < underline_len; i++) {
            if (!xr_diag_emit(policy, '^')) return policy->status;
        }
        if (xr_diag_format(policy, "%s\n", c_reset) != XR_DIAG_OK) return policy->status;
    }

    if (xr_diag_format(policy, "\n") != XR_DIAG_OK) return policy->status;
    return policy->status;
}

/*
 * Print error summary line ().
 *
 *   error: aborting due to 3 previous errors
 */
static inline XrDiagStatus xr_diag_print_summary_policy(XrDiagPolicy *policy, const char *file, int error_count, int warning_count,
                                         int max_errors_reached) {
    XR_DCHECK(error_count >= 0, "xr_diag_print_summary: negative error_count");
    XR_DCHECK(warning_count >= 0, "xr_diag_print_summary: negative warning_count");

    if (!file)
        file = "<script>";

    if (!policy || policy->status != XR_DIAG_OK) return policy ? policy->status : XR_DIAG_BAD_ARGUMENT;
    bool use_color = policy->color;
    const char *c_reset = use_color ? XR_CLR_RESET : "";
    const char *c_red = use_color ? XR_CLR_RED : "";
    const char *c_yellow = use_color ? XR_CLR_YELLOW : "";

    if (max_errors_reached && error_count > 0) {
        if (xr_diag_format(policy, "%serror%s: could not compile `%s`: too many errors emitted, stopping now\n", c_red,
                c_reset, file) != XR_DIAG_OK) return policy->status;
    }

    if (error_count > 0) {
        if (xr_diag_format(policy, "%serror%s: aborting due to %d previous error%s", c_red, c_reset,
                error_count, error_count > 1 ? "s" : "") != XR_DIAG_OK) return policy->status;
        if (warning_count > 0) {
            if (xr_diag_format(policy, "; %d warning%s emitted", warning_count, warning_count > 1 ? "s" : "") != XR_DIAG_OK) return policy->status;
        }
        if (xr_diag_format(policy, "\n") != XR_DIAG_OK) return policy->status;
    } else if (warning_count > 0) {
        if (xr_diag_format(policy, "%swarning%s: %d warning%s emitted\n", c_yellow, c_reset, warning_count,
                warning_count > 1 ? "s" : "") != XR_DIAG_OK) return policy->status;
    }
    return policy->status;
}

/* Explicit system display callers share the policy algorithm above. */
static inline const char *xr_diag_find_line_start(const char *source, const char *position) {
    XrDiagOutput output = {stderr, NULL, xr_diag_system_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, false};
    return xr_diag_find_line_start_policy(&policy, source, position);
}
static inline const char *xr_diag_find_line_end(const char *position) {
    XrDiagOutput output = {stderr, NULL, xr_diag_system_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, false};
    return xr_diag_find_line_end_policy(&policy, position);
}
static inline void xr_diag_print(XrDiagLevel level, int code, const char *message, const char *file,
                                 int line, int column, int token_len, const char *source, const char *token_start) {
    XrDiagOutput output = {stderr, NULL, xr_diag_system_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, xr_diag_use_color()};
    (void) xr_diag_print_policy(&policy, level, code, message, file, line, column, token_len, source, token_start);
}
static inline void xr_diag_print_summary(const char *file, int error_count, int warning_count, int max_errors_reached) {
    XrDiagOutput output = {stderr, NULL, xr_diag_system_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, xr_diag_use_color()};
    (void) xr_diag_print_summary_policy(&policy, file, error_count, warning_count, max_errors_reached);
}

#endif  // XDIAG_FMT_H
