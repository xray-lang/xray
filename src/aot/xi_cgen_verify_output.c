/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_verify_output.c - CGen output well-formedness verifier (task 218).
 *
 * Passes over a generated C translation unit neutralize strings / char
 * literals / comments, then enforce four structural
 * invariants (W1-W4). See xi_cgen_verify_output.h for the contract.
 */

#include "xi_cgen_verify_output.h"
#include <limits.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#endif

/* Guard against pathological temp ids from corrupt input. */
#define XI_CGEN_VERIFY_MAX_TEMP 8388608 /* 8M distinct vN per function */

typedef struct VerifyContext {
    XrCompileResources *resources;
    XiCgenVerifyStatus status;
} VerifyContext;

static bool verify_resource(VerifyContext *ctx, XrCompileResourceStatus status) {
    if (ctx->status == XI_CGEN_VERIFY_PASSED) {
        switch (status) {
            case XR_COMPILE_RESOURCE_OK: break;
            case XR_COMPILE_RESOURCE_BUDGET: ctx->status = XI_CGEN_VERIFY_BUDGET; break;
            case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: ctx->status = XI_CGEN_VERIFY_OUT_OF_MEMORY; break;
            case XR_COMPILE_RESOURCE_BAD_ARGUMENT: ctx->status = XI_CGEN_VERIFY_BAD_ARGUMENT; break;
        }
    }
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static bool verify_work(VerifyContext *ctx, uint64_t units) {
    return ctx->status == XI_CGEN_VERIFY_PASSED &&
        verify_resource(ctx, xr_compile_resources_work(ctx->resources, units));
}

static unsigned char verify_read(VerifyContext *ctx, const char *source, size_t at) {
    return verify_work(ctx, 1) ? (unsigned char) source[at] : 0;
}

static void verify_write(VerifyContext *ctx, char *target, size_t at, char value) {
    if (verify_work(ctx, 1)) target[at] = value;
}

static void verify_copy(VerifyContext *ctx, void *target, const void *source, size_t bytes) {
    if (verify_work(ctx, bytes)) memcpy(target, source, bytes);
}

static void verify_fill(VerifyContext *ctx, void *target, int value, size_t bytes) {
    if (verify_work(ctx, bytes)) memset(target, value, bytes);
}

static size_t verify_length(VerifyContext *ctx, const char *text) {
    size_t length = 0;
    while (verify_read(ctx, text, length)) ++length;
    return length;
}

static int verify_compare(VerifyContext *ctx, const char *a, const char *b, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        if (!verify_work(ctx, 1)) return 0;
        unsigned char av = (unsigned char) a[i], bv = (unsigned char) b[i];
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

static void *verify_allocate(VerifyContext *ctx, size_t bytes, bool clear) {
    void *memory = NULL;
    if (ctx->status == XI_CGEN_VERIFY_PASSED)
        verify_resource(ctx, clear ? xr_compile_resources_calloc(ctx->resources, 1, bytes, &memory) :
            xr_compile_resources_alloc(ctx->resources, bytes, &memory));
    return memory;
}

static bool verify_resize(VerifyContext *ctx, void **memory, size_t bytes) {
    return ctx->status == XI_CGEN_VERIFY_PASSED &&
        verify_resource(ctx, xr_compile_resources_resize(ctx->resources, memory, bytes));
}

static void verify_put(VerifyContext *ctx, char *target, size_t capacity, size_t *position, char value) {
    if (*position + 1 < capacity) verify_write(ctx, target, *position, value);
    ++*position;
}

/* Diagnostics have only literals, strings and signed decimal arguments. This
 * writer charges the actual scan and output, including truncated input, rather
 * than asking libc to perform an unaccounted measuring pass. */
static void verify_format_args(VerifyContext *ctx, char *target, size_t capacity,
                               const char *format, va_list args) {
    size_t position = 0, at = 0;
    while (ctx->status == XI_CGEN_VERIFY_PASSED) {
        char c = (char) verify_read(ctx, format, at++);
        if (!c) break;
        if (c != '%') { verify_put(ctx, target, capacity, &position, c); continue; }
        c = (char) verify_read(ctx, format, at++);
        if (c == 's') {
            const char *text = va_arg(args, const char *);
            for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED; ++i) {
                char value = (char) verify_read(ctx, text, i);
                if (!value) break;
                verify_put(ctx, target, capacity, &position, value);
            }
        } else {
            bool wide = c == 'l';
            if (wide) c = (char) verify_read(ctx, format, at++);
            if (c != 'd') { if (ctx->status == XI_CGEN_VERIFY_PASSED) ctx->status = XI_CGEN_VERIFY_BAD_ARGUMENT; break; }
            long value = wide ? va_arg(args, long) : va_arg(args, int);
            unsigned long magnitude = value < 0 ? 0UL - (unsigned long) value : (unsigned long) value;
            unsigned long divisor = 1;
            while (magnitude / divisor >= 10 && verify_work(ctx, 1)) divisor *= 10;
            if (value < 0) verify_put(ctx, target, capacity, &position, '-');
            do {
                if (!verify_work(ctx, 1)) break;
                verify_put(ctx, target, capacity, &position, (char) ('0' + magnitude / divisor));
                magnitude %= divisor; divisor /= 10;
            } while (divisor);
        }
    }
    if (capacity) verify_write(ctx, target, position < capacity ? position : capacity - 1, 0);
}

static void verify_format(VerifyContext *ctx, char *target, size_t capacity, const char *format, ...) {
    va_list args; va_start(args, format);
    verify_format_args(ctx, target, capacity, format, args);
    va_end(args);
}

static void set_result(VerifyContext *ctx, XiCgenVerifyResult *out, XiCgenVerifyCategory category,
                       int line, const char *format, ...) {
    verify_fill(ctx, out, 0, sizeof(*out));
    if (!verify_work(ctx, sizeof(out->category) + sizeof(out->line))) return;
    out->category = category; out->line = line;
    va_list args; va_start(args, format);
    verify_format_args(ctx, out->message, sizeof(out->message), format, args);
    va_end(args);
}

static XiCgenVerifyStatus verify_publish(VerifyContext *ctx, XiCgenVerifyStatus status,
    const XiCgenVerifyResult *result, XiCgenVerifyResult *output) {
    if (output && ctx->status == XI_CGEN_VERIFY_PASSED) {
        if (status == XI_CGEN_VERIFY_PASSED) verify_fill(ctx, output, 0, sizeof(*output));
        else verify_copy(ctx, output, result, sizeof(*output));
    }
    return ctx->status == XI_CGEN_VERIFY_PASSED ? status : ctx->status;
}

static int64_t xi_cgen_process_id(void) {
#ifdef XR_OS_WINDOWS
    return (int64_t) _getpid();
#else
    return (int64_t) getpid();
#endif
}

static bool ident_char(int c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ident_start(int c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}


XR_FUNC const char *xi_cgen_verify_category_name(XiCgenVerifyCategory category) {
    switch (category) {
        case XI_CGEN_VERIFY_OK:
            return "OK";
        case XI_CGEN_VERIFY_W1_BALANCE:
            return "W1_BALANCE";
        case XI_CGEN_VERIFY_W2_IDENTIFIER:
            return "W2_IDENTIFIER";
        case XI_CGEN_VERIFY_W3_SCOPE:
            return "W3_SCOPE";
        case XI_CGEN_VERIFY_W4_FORWARD_REF:
            return "W4_FORWARD_REF";
        case XI_CGEN_VERIFY_C90_RESTRICTED:
            return "C90_RESTRICTED";
    }
    return "UNKNOWN";
}

/* ---- W3 helpers: statement-shaped lines that must never sit at file scope. */

static bool starts_with_kw(VerifyContext *ctx, const char *t, size_t n, const char *kw) {
    size_t k = verify_length(ctx, kw);
    if (n < k)
        return false;
    if (verify_compare(ctx, t, kw, k) != 0)
        return false;
    /* keyword boundary: next char is not an identifier char */
    return (n == k) || !ident_char((unsigned char) verify_read(ctx, t, k));
}

/* t/n is the trimmed code-only content of a file-scope line. */
static bool file_scope_statement_shape(VerifyContext *ctx, const char *t, size_t n) {
    if (n == 0)
        return false;
    /* vN = ... / phiN ... : a bare temporary at file scope. */
    if ((verify_read(ctx, t, 0) == 'v' || (n > 3 && verify_read(ctx, t, 0) == 'p' && verify_read(ctx, t, 1) == 'h' && verify_read(ctx, t, 2) == 'i'))) {
        size_t p = (verify_read(ctx, t, 0) == 'v') ? 1 : 3;
        if (p < n && isdigit((unsigned char) verify_read(ctx, t, p))) {
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (p < n && isdigit((unsigned char) verify_read(ctx, t, p))) && verify_work(ctx, 1))
                p++;
            /* Followed by an assignment or use, not a declarator like `vec x`. */
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (p < n && (verify_read(ctx, t, p) == ' ' || verify_read(ctx, t, p) == '\t')) && verify_work(ctx, 1))
                p++;
            if (p < n && (verify_read(ctx, t, p) == '=' || verify_read(ctx, t, p) == '.' || verify_read(ctx, t, p) == '-' || verify_read(ctx, t, p) == '[' || verify_read(ctx, t, p) == '+' ||
                          verify_read(ctx, t, p) == ';' || verify_read(ctx, t, p) == ')'))
                return true;
        }
    }
    /* Control-flow / jump statements are only ever valid inside a body. */
    static const char *kws[] = {"if",     "return", "else",     "for",   "while",
                                "switch", "goto",   "continue", "break", "do"};
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < sizeof(kws) / sizeof(kws[0])) && verify_work(ctx, 1); i++) {
        if (starts_with_kw(ctx, t, n, kws[i]))
            return true;
    }
    return false;
}

/* ---- W2 helpers: identifier hygiene on a single code-only line. */

static bool line_has_identifier_hygiene_violation(VerifyContext *ctx, const char *s, size_t n, char *detail,
                                                  size_t detail_sz) {
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < n) && verify_work(ctx, 1); i++) {
        /* Parent-directory path fragment leaked into a symbol position. */
        if (i + 2 < n && verify_read(ctx, s, i) == '.' && verify_read(ctx, s, i + 1) == '.' && verify_read(ctx, s, i + 2) == '/') {
            verify_format(ctx, detail, detail_sz, "path fragment '../' in emitted code");
            return true;
        }
        /* An emitted identifier that runs straight into a close brace and a
         * control keyword: the historical `xr_ffi_ } else {` corruption. */
        if (verify_read(ctx, s, i) == '}') {
            /* identifier immediately before '}' (skip inline spaces)? */
            long j = (long) i - 1;
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (j >= 0 && (verify_read(ctx, s, j) == ' ' || verify_read(ctx, s, j) == '\t')) && verify_work(ctx, 1))
                j--;
            if (j >= 0 && ident_char((unsigned char) verify_read(ctx, s, j))) {
                size_t k = i + 1;
                while (ctx->status == XI_CGEN_VERIFY_PASSED && (k < n && (verify_read(ctx, s, k) == ' ' || verify_read(ctx, s, k) == '\t')) && verify_work(ctx, 1))
                    k++;
                if (starts_with_kw(ctx, s + k, n - k, "else") ||
                    starts_with_kw(ctx, s + k, n - k, "return")) {
                    verify_format(ctx, detail, detail_sz,
                             "identifier abuts '} %s' (source fragment in symbol)",
                             starts_with_kw(ctx, s + k, n - k, "else") ? "else" : "return");
                    return true;
                }
            }
        }
    }
    return false;
}

/* ---- W4 helpers: vN used before defined within a function. */

typedef struct {
    unsigned char *seen; /* bit per temp id: defined so far in this function */
    size_t cap;
    long line_defs[64];
} W4State;

typedef struct VerifyScratch {
    W4State w4;
    XiCgenVerifyResult results[4];
    char detail[160];
} VerifyScratch;

static bool w4_mark_defined(VerifyContext *ctx, W4State *st, long n) {
    if (n < 0 || n >= XI_CGEN_VERIFY_MAX_TEMP)
        return true; /* out of range: ignore, do not crash */
    if ((size_t) n >= st->cap) {
        size_t newcap = st->cap ? st->cap * 2 : 1024;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (newcap <= (size_t) n) && verify_work(ctx, 1))
            newcap *= 2;
        void *memory = st->seen;
        if (!verify_resize(ctx, &memory, newcap)) return false;
        unsigned char *p = memory;
        st->seen = p;
        verify_fill(ctx, p + st->cap, 0, newcap - st->cap);
        st->cap = newcap;
    }
    verify_write(ctx, (char *)st->seen, (size_t)n, 1);
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static bool w4_is_defined(VerifyContext *ctx, const W4State *st, long n) {
    if (n < 0 || (size_t) n >= st->cap)
        return false;
    return verify_read(ctx, (const char *)st->seen, (size_t)n) != 0;
}

static void w4_reset(VerifyContext *ctx, W4State *st) {
    if (st->seen && st->cap)
        verify_fill(ctx, st->seen, 0, st->cap);
}

static bool is_control_kw(VerifyContext *ctx, const char *s, size_t len) {
    static const char *kw[] = {"return", "if",   "else",   "while", "for",      "switch", "case",
                               "do",     "goto", "sizeof", "break", "continue", "default"};
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < sizeof(kw) / sizeof(kw[0])) && verify_work(ctx, 1); i++)
        if (len == verify_length(ctx, kw[i]) && verify_compare(ctx, s, kw[i], len) == 0)
            return true;
    return false;
}

/* Parse the numeric id of a vN token [tok..end) (tok[0]=='v', rest digits). */
static long vN_id(VerifyContext *ctx, const char *s, size_t tok, size_t end) {
    long id = 0;
    for (size_t p = tok + 1; ctx->status == XI_CGEN_VERIFY_PASSED && (p < end) && verify_work(ctx, 1); p++) {
        id = id * 10 + (verify_read(ctx, s, p) - '0');
        if (id > XI_CGEN_VERIFY_MAX_TEMP)
            return XI_CGEN_VERIFY_MAX_TEMP;
    }
    return id;
}

/* Is the ident token [tok..tend) a pure temporary name (v followed by digits)? */
static bool token_is_temp(VerifyContext *ctx, const char *s, size_t tok, size_t tend) {
    if (tend <= tok + 1 || verify_read(ctx, s, tok) != 'v')
        return false;
    for (size_t d = tok + 1; ctx->status == XI_CGEN_VERIFY_PASSED && (d < tend) && verify_work(ctx, 1); d++)
        if (!isdigit((unsigned char) verify_read(ctx, s, d)))
            return false;
    return true;
}

/* Is a temp token at [tok..tend) a definition point rather than a use?
 * Definitions: an SSA store `vN =` (single '='), a pointer/aggregate
 * declarator `Type *vN` / `Type vN` (a non-control identifier or '*' directly
 * precedes it). Coroutine frame fields `Type vN;` therefore count as defs. */
static bool temp_is_def(VerifyContext *ctx, const char *s, size_t n, size_t tok, size_t tend) {
    size_t k = tend;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (k < n && (verify_read(ctx, s, k) == ' ' || verify_read(ctx, s, k) == '\t')) && verify_work(ctx, 1))
        k++;
    if (k < n && verify_read(ctx, s, k) == '=' && (k + 1 >= n || verify_read(ctx, s, k + 1) != '='))
        return true;
    long b = (long) tok - 1;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (b >= 0 && (verify_read(ctx, s, b) == ' ' || verify_read(ctx, s, b) == '\t')) && verify_work(ctx, 1))
        b--;
    if (b < 0)
        return false;
    if (verify_read(ctx, s, b) == '*')
        return true;
    if (ident_char((unsigned char) verify_read(ctx, s, b))) {
        long te = b + 1;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (b >= 0 && ident_char((unsigned char) verify_read(ctx, s, b))) && verify_work(ctx, 1))
            b--;
        const char *pt = s + (b + 1);
        size_t plen = (size_t) (te - (b + 1));
        if (!is_control_kw(ctx, pt, plen))
            return true; /* a type identifier precedes vN -> declaration */
    }
    return false;
}

/* Is a temp token at [tok..tend) a struct-field / member access (foo.vN)? */
static bool temp_is_field(VerifyContext *ctx, const char *s, size_t tok) {
    long b = (long) tok - 1;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (b >= 0 && (verify_read(ctx, s, b) == ' ' || verify_read(ctx, s, b) == '\t')) && verify_work(ctx, 1))
        b--;
    return b >= 0 && (verify_read(ctx, s, b) == '.' || (verify_read(ctx, s, b) == '>' && b > 0 && verify_read(ctx, s, b - 1) == '-'));
}

/* Scan one function-body line for W4. Returns true and fills *out on a
 * use-before-def; otherwise records this line's definitions in st. */
static bool w4_scan_line(VerifyContext *ctx, const char *s, size_t n, int lineno, W4State *st,
                         XiCgenVerifyResult *out) {
    long *line_defs = st->line_defs;
    int ndefs = 0;

    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < n) && verify_work(ctx, 1);) {
        if (!ident_start((unsigned char) verify_read(ctx, s, i))) {
            i++;
            continue;
        }
        size_t j = i;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (j < n && ident_char((unsigned char) verify_read(ctx, s, j))) && verify_work(ctx, 1))
            j++;
        if (token_is_temp(ctx, s, i, j) && !temp_is_field(ctx, s, i) && temp_is_def(ctx, s, n, i, j)) {
            if (ndefs < (int) (sizeof(st->line_defs) / sizeof(st->line_defs[0]))) {
                long id = vN_id(ctx, s, i, j);
                if (verify_work(ctx, 1)) line_defs[ndefs++] = id;
            }
        }
        i = j;
    }

    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < n) && verify_work(ctx, 1);) {
        if (!ident_start((unsigned char) verify_read(ctx, s, i))) {
            i++;
            continue;
        }
        size_t j = i;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (j < n && ident_char((unsigned char) verify_read(ctx, s, j))) && verify_work(ctx, 1))
            j++;
        if (token_is_temp(ctx, s, i, j) && !temp_is_field(ctx, s, i) && !temp_is_def(ctx, s, n, i, j)) {
            long id = vN_id(ctx, s, i, j);
            bool same_line_def = false;
            for (int d = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (d < ndefs) && verify_work(ctx, 1); d++)
                if (verify_work(ctx, 1) && line_defs[d] == id)
                    same_line_def = true;
            if (!same_line_def && !w4_is_defined(ctx, st, id)) {
                set_result(ctx, out, XI_CGEN_VERIFY_W4_FORWARD_REF, lineno,
                           "temporary v%ld used before it is defined", id);
                return true;
            }
        }
        i = j;
    }

    for (int d = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (d < ndefs) && verify_work(ctx, 1); d++)
        if (verify_work(ctx, 1)) w4_mark_defined(ctx, st, line_defs[d]);
    return false;
}

/* If a line is `#define vN ...`, return the temp id it introduces, else -1. */
static long pp_define_temp(VerifyContext *ctx, const char *s, size_t n) {
    size_t i = 0;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && (verify_read(ctx, s, i) == ' ' || verify_read(ctx, s, i) == '\t')) && verify_work(ctx, 1))
        i++;
    if (i >= n || verify_read(ctx, s, i) != '#')
        return -1;
    i++;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && (verify_read(ctx, s, i) == ' ' || verify_read(ctx, s, i) == '\t')) && verify_work(ctx, 1))
        i++;
    const char *def = "define";
    size_t dl = verify_length(ctx, def);
    if (i + dl > n || verify_compare(ctx, s + i, def, dl) != 0)
        return -1;
    i += dl;
    if (i < n && ident_char((unsigned char) verify_read(ctx, s, i)))
        return -1; /* not the `define` keyword */
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && (verify_read(ctx, s, i) == ' ' || verify_read(ctx, s, i) == '\t')) && verify_work(ctx, 1))
        i++;
    size_t tok = i;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && ident_char((unsigned char) verify_read(ctx, s, i))) && verify_work(ctx, 1))
        i++;
    if (token_is_temp(ctx, s, tok, i))
        return vN_id(ctx, s, tok, i);
    return -1;
}

/* Classify a preprocessor directive line: +1 opens a conditional (#if*),
 * -1 closes (#endif), 0 otherwise. */
static int pp_conditional_delta(VerifyContext *ctx, const char *s, size_t n) {
    size_t i = 0;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && (verify_read(ctx, s, i) == ' ' || verify_read(ctx, s, i) == '\t')) && verify_work(ctx, 1))
        i++;
    if (i >= n || verify_read(ctx, s, i) != '#')
        return 0;
    i++;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && (verify_read(ctx, s, i) == ' ' || verify_read(ctx, s, i) == '\t')) && verify_work(ctx, 1))
        i++;
    size_t tok = i;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && ident_char((unsigned char) verify_read(ctx, s, i))) && verify_work(ctx, 1))
        i++;
    size_t len = i - tok;
    const char *d = s + tok;
    if ((len == 2 && verify_compare(ctx, d, "if", 2) == 0) || (len == 5 && verify_compare(ctx, d, "ifdef", 5) == 0) ||
        (len == 6 && verify_compare(ctx, d, "ifndef", 6) == 0))
        return 1;
    if (len == 5 && verify_compare(ctx, d, "endif", 5) == 0)
        return -1;
    return 0;
}

/* ---- Main verifier. */

static bool verify_neutralize(VerifyContext *ctx, char *code, size_t len, XiCgenVerifyResult *w1r) {
    /* Pass 1: neutralize strings / chars / comments in place (replace content
     * with spaces, preserve newlines and offsets) and detect unterminated
     * lexical constructs (W1). */
    enum {
        NORMAL,
        LINE_COMMENT,
        BLOCK_COMMENT,
        STRING,
        CHAR
    } state = NORMAL;
    int line = 1;
    int open_line = 1; /* line where the current string/char/comment started */
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < len) && verify_work(ctx, 1); i++) {
        char c = verify_read(ctx, code, i);
        char next = (i + 1 < len) ? verify_read(ctx, code, i + 1) : '\0';
        switch (state) {
            case NORMAL:
                if (c == '/' && next == '/') {
                    verify_write(ctx, code, i, ' ');
                    verify_write(ctx, code, i + 1, ' ');
                    i++;
                    state = LINE_COMMENT;
                    open_line = line;
                } else if (c == '/' && next == '*') {
                    verify_write(ctx, code, i, ' ');
                    verify_write(ctx, code, i + 1, ' ');
                    i++;
                    state = BLOCK_COMMENT;
                    open_line = line;
                } else if (c == '"') {
                    verify_write(ctx, code, i, ' ');
                    state = STRING;
                    open_line = line;
                } else if (c == '\'') {
                    verify_write(ctx, code, i, ' ');
                    state = CHAR;
                    open_line = line;
                } else if (c == '\n') {
                    line++;
                }
                break;
            case LINE_COMMENT:
                if (c == '\n') {
                    state = NORMAL;
                    line++;
                } else {
                    verify_write(ctx, code, i, ' ');
                }
                break;
            case BLOCK_COMMENT:
                if (c == '*' && next == '/') {
                    verify_write(ctx, code, i, ' ');
                    verify_write(ctx, code, i + 1, ' ');
                    i++;
                    state = NORMAL;
                } else if (c == '\n') {
                    line++;
                } else {
                    verify_write(ctx, code, i, ' ');
                }
                break;
            case STRING:
            case CHAR: {
                char quote = (state == STRING) ? '"' : '\'';
                if (c == '\\') {
                    verify_write(ctx, code, i, ' ');
                    if (i + 1 < len) {
                        if (verify_read(ctx, code, i + 1) == '\n')
                            line++;
                        else
                            verify_write(ctx, code, i + 1, ' ');
                        i++;
                    }
                } else if (c == quote) {
                    verify_write(ctx, code, i, ' ');
                    state = NORMAL;
                } else if (c == '\n') {
                    /* raw newline inside a literal: keep counting, stay lenient */
                    line++;
                } else {
                    verify_write(ctx, code, i, ' ');
                }
                break;
            }
        }
    }

    if (state == STRING || state == CHAR) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, open_line, "unterminated %s literal",
                   state == STRING ? "string" : "character");
        return false;
    }
    if (state == BLOCK_COMMENT) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, open_line, "unterminated block comment");
        return false;
    }

    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static const XiCgenVerifyResult *verify_structure(VerifyContext *ctx, const char *code,
    size_t len, VerifyScratch *scratch) {
    W4State *w4 = &scratch->w4;
    XiCgenVerifyResult *w1r = &scratch->results[0], *w2r = &scratch->results[1];
    XiCgenVerifyResult *w3r = &scratch->results[2], *w4r = &scratch->results[3];
    const XiCgenVerifyResult *selected = NULL;
    /* Pass 2: line-oriented structural checks over the neutralized code.
     *
     * Generated C is only well-formed *after* preprocessing: it carries
     * `#if defined(XRAY_AOT_DEBUG_LOCALS)` islands, `#define vN (f->vN)`
     * coroutine frame aliases, etc. So counting is done only for
     * unconditional code (preprocessor conditional depth 0), preprocessor
     * directives never contribute braces, and `#define vN` marks a temp
     * available for W4. */
    int brace = 0, paren = 0;
    int pp_cond = 0; /* nesting depth of #if / #ifdef / #ifndef */
    bool have_w1 = false, have_w2 = false, have_w3 = false, have_w4 = false;

    size_t pos = 0;
    int lineno = 1;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (pos < len) && verify_work(ctx, 1)) {
        size_t ls = pos;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (pos < len && verify_read(ctx, code, pos) != '\n') && verify_work(ctx, 1))
            pos++;
        size_t le = pos; /* exclusive, excludes '\n' */
        if (pos < len)
            pos++; /* consume '\n' */

        const char *lp = code + ls;
        size_t ln = le - ls;

        /* trim leading whitespace for shape checks */
        size_t t0 = 0;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (t0 < ln && (verify_read(ctx, lp, t0) == ' ' || verify_read(ctx, lp, t0) == '\t' || verify_read(ctx, lp, t0) == '\r')) && verify_work(ctx, 1))
            t0++;
        size_t tn = ln;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (tn > t0 && (verify_read(ctx, lp, tn - 1) == ' ' || verify_read(ctx, lp, tn - 1) == '\t' || verify_read(ctx, lp, tn - 1) == '\r')) && verify_work(ctx, 1))
            tn--;
        const char *tp = lp + t0;
        size_t tlen = tn - t0;
        bool pp_line = (tlen > 0 && verify_read(ctx, tp, 0) == '#');

        if (pp_line) {
            /* Preprocessor directives never carry structural braces. Track
             * conditional nesting and honor `#define vN` frame aliases. */
            long def_temp = pp_define_temp(ctx, lp, ln);
            if (def_temp >= 0)
                w4_mark_defined(ctx, w4, def_temp);
            if (ctx->status != XI_CGEN_VERIFY_PASSED) {
                return NULL;
            }
            int delta = pp_conditional_delta(ctx, lp, ln);
            if (delta > 0)
                pp_cond++;
            else if (delta < 0 && pp_cond > 0)
                pp_cond--;
            lineno++;
            continue;
        }

        /* Skip conditionally-compiled code: text-level balance is meaningless
         * there, and it is not the primary corruption surface. */
        if (pp_cond > 0) {
            lineno++;
            continue;
        }

        int start_brace = brace;

        /* W4 scope: function bodies live at brace depth > 0; reset temps at
         * file scope so each function is checked independently. */
        if (start_brace == 0)
            w4_reset(ctx, w4);

        /* W2 identifier hygiene */
        if (!have_w2) {
            char *detail = scratch->detail;
            if (line_has_identifier_hygiene_violation(ctx, lp, ln, detail, sizeof(scratch->detail))) {
                set_result(ctx, w2r, XI_CGEN_VERIFY_W2_IDENTIFIER, lineno, "%s", detail);
                have_w2 = true;
            }
        }
        /* W3 file-scope statement shape */
        if (!have_w3 && start_brace == 0 && file_scope_statement_shape(ctx, tp, tlen)) {
            set_result(ctx, w3r, XI_CGEN_VERIFY_W3_SCOPE, lineno,
                       "statement-shaped line at file scope (brace depth 0)");
            have_w3 = true;
        }
        /* W4 forward reference (only meaningful inside a function body) */
        if (!have_w4 && start_brace > 0) {
            if (w4_scan_line(ctx, lp, ln, lineno, w4, w4r))
                have_w4 = true;
        }
        if (ctx->status != XI_CGEN_VERIFY_PASSED) {
            return NULL;
        }

        /* brace/paren balance (W1); braces in unconditional code are real. */
        for (size_t j = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (j < ln) && verify_work(ctx, 1); j++) {
            char c = verify_read(ctx, lp, j);
            if (c == '{') {
                brace++;
            } else if (c == '}') {
                brace--;
                if (brace < 0 && !have_w1) {
                    set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno,
                               "unbalanced '}' (closes with no matching '{')");
                    have_w1 = true;
                }
            } else if (c == '(') {
                paren++;
            } else if (c == ')') {
                paren--;
                if (paren < 0 && !have_w1) {
                    set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno,
                               "unbalanced ')' (closes with no matching '(')");
                    have_w1 = true;
                }
            }
        }
        lineno++;
    }

    if (!have_w1 && brace != 0) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno > 1 ? lineno - 1 : 1,
                   "unbalanced braces at end of unit (depth %d)", brace);
        have_w1 = true;
    }
    if (!have_w1 && paren != 0) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno > 1 ? lineno - 1 : 1,
                   "unbalanced parentheses at end of unit (depth %d)", paren);
        have_w1 = true;
    }

    /* Priority: W1 > W2 > W3 > W4. Resource failure never publishes a
     * diagnostic assembled before the incomplete verification stopped. */
    if (have_w1) selected = w1r;
    else if (have_w2) selected = w2r;
    else if (have_w3) selected = w3r;
    else if (have_w4) selected = w4r;
    return selected;
}

XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_output(XrCompileResources *resources,
    const char *c_src, size_t len, XiCgenVerifyResult *out) {
    if (!resources || (!c_src && len)) return XI_CGEN_VERIFY_BAD_ARGUMENT;
    if (len > (size_t)INT_MAX - 1) return XI_CGEN_VERIFY_BUDGET;
    VerifyContext context = {resources, XI_CGEN_VERIFY_PASSED};
    VerifyContext *ctx = &context;
    if (!len) return verify_publish(ctx, XI_CGEN_VERIFY_PASSED, NULL, out);
    VerifyScratch *scratch = verify_allocate(ctx, sizeof(*scratch), true);
    if (!scratch) return ctx->status;
    const XiCgenVerifyResult *selected = NULL;
    char *code = verify_allocate(ctx, len, false);
    if (code) {
        verify_copy(ctx, code, c_src, len);
        if (!verify_neutralize(ctx, code, len, &scratch->results[0])) selected = &scratch->results[0];
        else selected = verify_structure(ctx, code, len, scratch);
    }
    XiCgenVerifyStatus status = verify_publish(ctx,
        selected ? XI_CGEN_VERIFY_MALFORMED : XI_CGEN_VERIFY_PASSED, selected, out);
    xr_compile_resources_free(scratch->w4.seen);
    xr_compile_resources_free(code);
    xr_compile_resources_free(scratch);
    return status;
}

/* ---- Restricted C90 dialect policy. */

typedef struct C90ForbiddenToken {
    const char *token;  /* owned: static string literal */
    const char *detail; /* owned: static string literal */
} C90ForbiddenToken;

XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_c90_output(XrCompileResources *resources, const char *c_src, size_t len, XiCgenVerifyResult *out) {
    static const C90ForbiddenToken forbidden[] = {
        {"_Atomic", "C11 _Atomic residue"},
        {"_Thread_local", "C11 _Thread_local residue"},
        {"_Alignof", "C11 _Alignof residue"},
        {"long long", "long long residue"},
        {"({", "GNU statement-expression residue"},
        {"){", "C99 compound-literal residue"},
        {"{ .", "C99 designated-initializer residue"},
        {"...", "variadic residue"},
        {"union {", "anonymous-union residue"},
        {"[];", "flexible-array residue"},
        {"for (int ", "C99 loop-declaration residue"},
        {"for (size_t ", "C99 loop-declaration residue"},
        {"xrt_shared_", "module shared-slot residue"},
        {"xrt_builtins", "dynamic builtin-table residue"},
        {"xrt_global_ctx", "hosted runtime-context residue"},
        {"xrt_map_", "dynamic Map runtime residue"},
        {"xrt_set_", "dynamic Set runtime residue"},
        {"xrt_thread", "thread runtime residue"},
        {"xrt_coro", "coroutine runtime residue"},
        {"xrt_task", "task runtime residue"},
        {"xrt_channel", "channel runtime residue"},
    };
    enum {
        C90_SCAN_CODE = 0,
        C90_SCAN_BLOCK_COMMENT,
        C90_SCAN_STRING,
        C90_SCAN_CHAR
    } state = C90_SCAN_CODE;
    int line = 1;

    if (!resources || (!c_src && len)) return XI_CGEN_VERIFY_BAD_ARGUMENT;
    if (len > (size_t)INT_MAX - 1) return XI_CGEN_VERIFY_BUDGET;
    VerifyContext context = {resources, XI_CGEN_VERIFY_PASSED}; VerifyContext *ctx = &context;
    if (!len) return verify_publish(ctx, XI_CGEN_VERIFY_PASSED, NULL, out);
    XiCgenVerifyResult *result = verify_allocate(ctx, sizeof(*result), false);
    if (!result) return ctx->status;
    XiCgenVerifyStatus status = XI_CGEN_VERIFY_PASSED;

    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < len) && verify_work(ctx, 1); i++) {
        unsigned char c = (unsigned char) verify_read(ctx, c_src, i);
        unsigned char next = i + 1 < len ? (unsigned char) verify_read(ctx, c_src, i + 1) : 0;
        if (c == '\n')
            line++;
        if (state == C90_SCAN_BLOCK_COMMENT) {
            if (c == '*' && next == '/') {
                state = C90_SCAN_CODE;
                i++;
            }
            continue;
        }
        if (state == C90_SCAN_STRING || state == C90_SCAN_CHAR) {
            if (c == '\\' && i + 1 < len) {
                if (verify_read(ctx, c_src, i + 1) == '\n')
                    line++;
                i++;
                continue;
            }
            if ((state == C90_SCAN_STRING && c == '"') || (state == C90_SCAN_CHAR && c == '\''))
                state = C90_SCAN_CODE;
            continue;
        }
        if (c == '/' && next == '*') {
            state = C90_SCAN_BLOCK_COMMENT;
            i++;
            continue;
        }
        if (c == '/' && next == '/') {
            set_result(ctx, result, XI_CGEN_VERIFY_C90_RESTRICTED, line, "C++ line-comment residue");
            status = XI_CGEN_VERIFY_MALFORMED; goto finish;
        }
        if (c == '"') {
            state = C90_SCAN_STRING;
            continue;
        }
        if (c == '\'') {
            state = C90_SCAN_CHAR;
            continue;
        }
        if (ident_start(c) && i + 6 <= len && verify_compare(ctx, c_src + i, "inline", 6) == 0 &&
            (i == 0 || !ident_char((unsigned char) verify_read(ctx, c_src, i - 1))) &&
            (i + 6 == len || !ident_char((unsigned char) verify_read(ctx, c_src, i + 6)))) {
            set_result(ctx, result, XI_CGEN_VERIFY_C90_RESTRICTED, line, "C99 inline residue");
            status = XI_CGEN_VERIFY_MALFORMED; goto finish;
        }
        for (size_t t = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (t < sizeof(forbidden) / sizeof(forbidden[0])) && verify_work(ctx, 1); t++) {
            size_t token_len = verify_length(ctx, forbidden[t].token);
            if (i + token_len <= len && verify_compare(ctx, c_src + i, forbidden[t].token, token_len) == 0) {
                set_result(ctx, result, XI_CGEN_VERIFY_C90_RESTRICTED, line, "%s", forbidden[t].detail);
                status = XI_CGEN_VERIFY_MALFORMED; goto finish;
            }
        }
    }
finish:
    status = verify_publish(ctx, status, result, out);
    xr_compile_resources_free(result);
    return status;
}

/* ---- Fail-closed ICE wrapper used at the C-write boundary. */

XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_output_or_ice(XrCompileResources *resources, const char *c_src, size_t len,
                                              const char *tu_name) {
    XiCgenVerifyResult r;
    XiCgenVerifyStatus status = xr_compile_cgen_verify_output(resources, c_src, len, &r);
    if (status != XI_CGEN_VERIFY_MALFORMED)
        return status;

    /* Emergency reporting follows a terminal structural failure. It cannot
     * consume a new admission budget or downgrade the mandatory abort. */

    const char *dir = getenv("XRAY_CGEN_ICE_DIR");
    if (!dir || !*dir)
        dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";

    /* sanitize the TU name for use in a filename */
    char safe[128];
    size_t si = 0;
    const char *tn = tu_name ? tu_name : "unit";
    for (; tn[si] != '\0' && si + 1 < sizeof(safe); si++) {
        char c = tn[si];
        safe[si] = ident_char((unsigned char) c) ? c : '_';
    }
    safe[si] = '\0';

    char path[1024];
    snprintf(path, sizeof(path), "%s/xray_cgen_ice_%s_%lld.c", dir, safe,
             (long long) xi_cgen_process_id());

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(c_src, 1, len, f);
        fclose(f);
    }

    fflush(stdout);
    fprintf(stderr,
            "\n[xi_cgen][ICE] internal compiler error: generated C is malformed.\n"
            "  translation unit : %s\n"
            "  category         : %s\n"
            "  line             : %d\n"
            "  detail           : %s\n"
            "  generated C dump : %s\n"
            "Task 218 defense line 3: malformed generated C is never handed to the\n"
            "C toolchain. This is a compiler bug; please report with the dump above.\n",
            tu_name ? tu_name : "?", xi_cgen_verify_category_name(r.category), r.line, r.message,
            f ? path : "(dump failed)");
    fflush(stderr);
    abort();
}
