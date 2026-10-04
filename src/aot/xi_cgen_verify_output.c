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
#define XI_CGEN_VERIFY_DETAIL_CAPACITY 160

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

/* Each mask bit is owned by this verification. The first lexical scan
 * records exactly the bytes it neutralizes, so line storage never repeats
 * lexical classification or retains a second translation-unit copy. */
static void verify_mask_byte(VerifyContext *ctx, char *mask, size_t at) {
    unsigned char flags = verify_read(ctx, mask, at / 8);
    verify_write(ctx, mask, at / 8, (char)(flags | (1u << (at % 8))));
}

static bool verify_apply_mask(VerifyContext *ctx, char *code, const char *mask,
    size_t offset, size_t length) {
    for (size_t pos = 0; ctx->status == XI_CGEN_VERIFY_PASSED &&
         pos < length && verify_work(ctx, 1);) {
        size_t global = offset + pos;
        unsigned shift = (unsigned)(global % 8);
        size_t span = 8 - shift;
        if (span > length - pos) span = length - pos;
        unsigned flags = verify_read(ctx, mask, global / 8) >> shift;
        if (flags) {
            for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED &&
                 i < span && verify_work(ctx, 1); ++i)
                if (flags & (1u << i)) verify_write(ctx, code, pos + i, ' ');
        }
        pos += span;
    }
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static void verify_copy(VerifyContext *ctx, void *target, const void *source, size_t bytes) {
    if (verify_work(ctx, bytes)) memcpy(target, source, bytes);
}

static void verify_fill(VerifyContext *ctx, void *target, int value, size_t bytes) {
    if (verify_work(ctx, bytes)) memset(target, value, bytes);
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

typedef struct VerifyKeyword {
    const char *text; /* owned: static string literals, never compiler-stage memory. */
    size_t length;
} VerifyKeyword;
static bool inline_space(unsigned char c, bool carriage_return) {
    return c == ' ' || c == '\t' || (carriage_return && c == '\r');
}
static bool starts_with_kw(VerifyContext *ctx, const char *t, size_t n, const char *kw, size_t k) {
    if (n < k) return false;
    if (verify_compare(ctx, t, kw, k) != 0 || ctx->status != XI_CGEN_VERIFY_PASSED) return false;
    if (n == k) return true;
    unsigned char boundary = verify_read(ctx, t, k);
    return ctx->status == XI_CGEN_VERIFY_PASSED && !ident_char(boundary);
}

/* t/n is the trimmed code-only content of a file-scope line. */
static bool file_scope_statement_shape(VerifyContext *ctx, const char *t, size_t n) {
    if (n == 0)
        return false;
    /* vN = ... / phiN ... : a bare temporary at file scope. */
    unsigned char first = verify_read(ctx, t, 0);
    if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
    if (first == 'v' || (n > 3 && first == 'p' && verify_read(ctx, t, 1) == 'h' && verify_read(ctx, t, 2) == 'i')) {
        size_t p = first == 'v' ? 1 : 3;
        if (p < n && isdigit((unsigned char) verify_read(ctx, t, p))) {
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (p < n && isdigit((unsigned char) verify_read(ctx, t, p))) && verify_work(ctx, 1))
                p++;
            /* Followed by an assignment or use, not a declarator like `vec x`. */
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (p < n && inline_space(verify_read(ctx, t, p), false)) && verify_work(ctx, 1))
                p++;
            if (p < n) {
                unsigned char current = verify_read(ctx, t, p);
                if (ctx->status == XI_CGEN_VERIFY_PASSED && (current == '=' || current == '.' || current == '-' ||
                    current == '[' || current == '+' || current == ';' || current == ')')) return true;
            }
        }
    }
    /* Control-flow / jump statements are only ever valid inside a body. */
    static const VerifyKeyword kws[] = {
        {"if", sizeof("if") - 1},
        {"return", sizeof("return") - 1},
        {"else", sizeof("else") - 1},
        {"for", sizeof("for") - 1},
        {"while", sizeof("while") - 1},
        {"switch", sizeof("switch") - 1},
        {"goto", sizeof("goto") - 1},
        {"continue", sizeof("continue") - 1},
        {"break", sizeof("break") - 1},
        {"do", sizeof("do") - 1}
    };
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < sizeof(kws) / sizeof(kws[0])) && verify_work(ctx, 1); i++) {
        if (starts_with_kw(ctx, t, n, kws[i].text, kws[i].length))
            return true;
    }
    return false;
}

/* ---- W2 helpers: identifier hygiene on a single code-only line. */

static bool identifier_hygiene_at(VerifyContext *ctx, const char *s, size_t n,
    size_t i, unsigned char current, char *detail) {
    /* Each current byte is shared with structural and temporary checks. */
    if (i + 2 < n && current == '.' && verify_read(ctx, s, i + 1) == '.' && verify_read(ctx, s, i + 2) == '/') {
        verify_format(ctx, detail, XI_CGEN_VERIFY_DETAIL_CAPACITY, "path fragment '../' in emitted code");
        return true;
    }
    if (current == '}') {
        long j = (long) i - 1;
        while (ctx->status == XI_CGEN_VERIFY_PASSED && (j >= 0 && inline_space(verify_read(ctx, s, j), false)) && verify_work(ctx, 1))
            j--;
        if (j >= 0 && ident_char((unsigned char) verify_read(ctx, s, j))) {
            size_t k = i + 1;
            while (ctx->status == XI_CGEN_VERIFY_PASSED && (k < n && inline_space(verify_read(ctx, s, k), false)) && verify_work(ctx, 1))
                k++;
            bool is_else = starts_with_kw(ctx, s + k, n - k, "else", sizeof("else") - 1);
            if (is_else || starts_with_kw(ctx, s + k, n - k, "return", sizeof("return") - 1)) {
                verify_format(ctx, detail, XI_CGEN_VERIFY_DETAIL_CAPACITY,
                    "identifier abuts '} %s' (source fragment in symbol)", is_else ? "else" : "return");
                return true;
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
    long *uses;
    size_t use_capacity, use_count, definition_count;
} W4State;

typedef struct VerifyScratch {
    W4State w4;
    XiCgenVerifyResult results[4];
    char detail[XI_CGEN_VERIFY_DETAIL_CAPACITY];
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
    static const VerifyKeyword kw[] = {
        {"return", sizeof("return") - 1},
        {"if", sizeof("if") - 1},
        {"else", sizeof("else") - 1},
        {"while", sizeof("while") - 1},
        {"for", sizeof("for") - 1},
        {"switch", sizeof("switch") - 1},
        {"case", sizeof("case") - 1},
        {"do", sizeof("do") - 1},
        {"goto", sizeof("goto") - 1},
        {"sizeof", sizeof("sizeof") - 1},
        {"break", sizeof("break") - 1},
        {"continue", sizeof("continue") - 1},
        {"default", sizeof("default") - 1}
    };
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < sizeof(kw) / sizeof(kw[0])) && verify_work(ctx, 1); i++)
        if (len == kw[i].length && verify_compare(ctx, s, kw[i].text, len) == 0 &&
            ctx->status == XI_CGEN_VERIFY_PASSED)
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
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (k < n && inline_space(verify_read(ctx, s, k), false)) && verify_work(ctx, 1))
        k++;
    if (k < n && verify_read(ctx, s, k) == '=' && (k + 1 >= n || verify_read(ctx, s, k + 1) != '='))
        return true;
    long b = (long) tok - 1;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (b >= 0 && inline_space(verify_read(ctx, s, b), false)) && verify_work(ctx, 1))
        b--;
    if (b < 0)
        return false;
    unsigned char previous = verify_read(ctx, s, b);
    if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
    if (previous == '*') return true;
    if (ident_char(previous)) {
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
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (b >= 0 && inline_space(verify_read(ctx, s, b), false)) && verify_work(ctx, 1))
        b--;
    if (b < 0) return false;
    unsigned char previous = verify_read(ctx, s, b);
    return ctx->status == XI_CGEN_VERIFY_PASSED && (previous == '.' ||
        (previous == '>' && b > 0 && verify_read(ctx, s, b - 1) == '-'));
}

static bool w4_record_token(VerifyContext *ctx, const char *s, size_t n,
    size_t token, size_t end, W4State *st) {
    if (temp_is_field(ctx, s, token)) return ctx->status == XI_CGEN_VERIFY_PASSED;
    bool definition = temp_is_def(ctx, s, n, token, end);
    if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
    if (definition && st->definition_count == sizeof(st->line_defs) / sizeof(st->line_defs[0])) return true;
    long id = vN_id(ctx, s, token, end);
    if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
    if (definition) {
        if (!verify_work(ctx, 1)) return false;
        st->line_defs[st->definition_count++] = id;
    } else {
        if (st->use_count == st->use_capacity) {
            if (st->use_capacity > SIZE_MAX / 2) { ctx->status = XI_CGEN_VERIFY_BUDGET; return false; }
            size_t capacity = st->use_capacity ? st->use_capacity * 2 : 16;
            if (capacity > SIZE_MAX / sizeof(*st->uses)) { ctx->status = XI_CGEN_VERIFY_BUDGET; return false; }
            void *memory = st->uses;
            if (!verify_resize(ctx, &memory, capacity * sizeof(*st->uses))) return false;
            st->uses = memory; st->use_capacity = capacity;
        }
        if (!verify_work(ctx, 1)) return false;
        st->uses[st->use_count++] = id;
    }
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

/* Every use retains its source order until all same-line definitions are known.
 * The reusable vector belongs to this verification, never to borrowed input. */
static bool w4_finish_line(VerifyContext *ctx, W4State *st, int lineno, XiCgenVerifyResult *out) {
    for (size_t u = 0; ctx->status == XI_CGEN_VERIFY_PASSED && u < st->use_count && verify_work(ctx, 1); ++u) {
        if (!verify_work(ctx, 1)) return false;
        long id = st->uses[u];
        bool same_line_def = false;
        for (size_t d = 0; ctx->status == XI_CGEN_VERIFY_PASSED && d < st->definition_count && verify_work(ctx, 1); ++d)
            if (verify_work(ctx, 1) && st->line_defs[d] == id) same_line_def = true;
        if (!same_line_def && !w4_is_defined(ctx, st, id)) {
            set_result(ctx, out, XI_CGEN_VERIFY_W4_FORWARD_REF, lineno,
                "temporary v%ld used before it is defined", id);
            return true;
        }
    }
    for (size_t d = 0; ctx->status == XI_CGEN_VERIFY_PASSED && d < st->definition_count && verify_work(ctx, 1); ++d)
        if (verify_work(ctx, 1)) w4_mark_defined(ctx, st, st->line_defs[d]);
    return false;
}

/* If a line is `#define vN ...`, return the temp id it introduces, else -1. */
static long pp_define_temp(VerifyContext *ctx, const char *s, size_t n) {
    size_t i = 0;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && inline_space(verify_read(ctx, s, i), false)) && verify_work(ctx, 1))
        i++;
    if (i >= n || verify_read(ctx, s, i) != '#')
        return -1;
    i++;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && inline_space(verify_read(ctx, s, i), false)) && verify_work(ctx, 1))
        i++;
    const char *def = "define";
    size_t dl = sizeof("define") - 1;
    if (i + dl > n || verify_compare(ctx, s + i, def, dl) != 0 || ctx->status != XI_CGEN_VERIFY_PASSED)
        return -1;
    i += dl;
    if (i < n && ident_char((unsigned char) verify_read(ctx, s, i)))
        return -1; /* not the `define` keyword */
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && inline_space(verify_read(ctx, s, i), false)) && verify_work(ctx, 1))
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
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && inline_space(verify_read(ctx, s, i), false)) && verify_work(ctx, 1))
        i++;
    if (i >= n || verify_read(ctx, s, i) != '#')
        return 0;
    i++;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (i < n && inline_space(verify_read(ctx, s, i), false)) && verify_work(ctx, 1))
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

typedef enum VerifyLexMode { VERIFY_LEX_NORMAL, VERIFY_LEX_LINE_COMMENT, VERIFY_LEX_BLOCK_COMMENT, VERIFY_LEX_STRING, VERIFY_LEX_CHAR } VerifyLexMode;
typedef struct VerifyLexState {
    VerifyLexMode state;
    int line, open_line;
    size_t offset, line_start, max_line;
} VerifyLexState;
static void verify_lex_newline(VerifyLexState *lex, size_t at) {
    size_t bytes = at + 1 - lex->line_start;
    if (bytes > lex->max_line) lex->max_line = bytes;
    lex->line_start = at + 1;
}

static bool verify_neutralize(VerifyContext *ctx, const char *source, char *mask,
    size_t len, VerifyLexState *lex) {
    /* Record masks while validating lexical state and measuring line storage.
     * Newlines remain unmasked, including escaped physical newlines. */
    VerifyLexMode state = lex->state;
    int line = lex->line, open_line = lex->open_line;
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && (i < len) && verify_work(ctx, 1); i++) {
        char c = verify_read(ctx, source, i);
        if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
        if (c == '\n') verify_lex_newline(lex, lex->offset + i);
        char next = '\0';
        bool lookahead = (state == VERIFY_LEX_NORMAL && c == '/') || (state == VERIFY_LEX_BLOCK_COMMENT && c == '*') ||
            ((state == VERIFY_LEX_STRING || state == VERIFY_LEX_CHAR) && c == '\\');
        if (lookahead && i + 1 < len) next = verify_read(ctx, source, i + 1);
        if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
        switch (state) {
            case VERIFY_LEX_NORMAL:
                if (c == '/' && next == '/') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    if (mask) verify_mask_byte(ctx, mask, i + 1);
                    i++;
                    state = VERIFY_LEX_LINE_COMMENT;
                    open_line = line;
                } else if (c == '/' && next == '*') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    if (mask) verify_mask_byte(ctx, mask, i + 1);
                    i++;
                    state = VERIFY_LEX_BLOCK_COMMENT;
                    open_line = line;
                } else if (c == '"') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    state = VERIFY_LEX_STRING;
                    open_line = line;
                } else if (c == '\'') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    state = VERIFY_LEX_CHAR;
                    open_line = line;
                } else if (c == '\n') {
                    line++;
                }
                break;
            case VERIFY_LEX_LINE_COMMENT:
                if (c == '\n') {
                    state = VERIFY_LEX_NORMAL;
                    line++;
                } else {
                    if (mask) verify_mask_byte(ctx, mask, i);
                }
                break;
            case VERIFY_LEX_BLOCK_COMMENT:
                if (c == '*' && next == '/') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    if (mask) verify_mask_byte(ctx, mask, i + 1);
                    i++;
                    state = VERIFY_LEX_NORMAL;
                } else if (c == '\n') {
                    line++;
                } else {
                    if (mask) verify_mask_byte(ctx, mask, i);
                }
                break;
            case VERIFY_LEX_STRING:
            case VERIFY_LEX_CHAR: {
                char quote = (state == VERIFY_LEX_STRING) ? '"' : '\'';
                if (c == '\\') {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    if (i + 1 < len) {
                        if (next == '\n') {
                            verify_lex_newline(lex, lex->offset + i + 1);
                            line++;
                        } else if (mask)
                            verify_mask_byte(ctx, mask, i + 1);
                        i++;
                    }
                } else if (c == quote) {
                    if (mask) verify_mask_byte(ctx, mask, i);
                    state = VERIFY_LEX_NORMAL;
                } else if (c == '\n') {
                    /* raw newline inside a literal: keep counting, stay lenient */
                    line++;
                } else {
                    if (mask) verify_mask_byte(ctx, mask, i);
                }
                break;
            }
        }
    }

    lex->state = state; lex->line = line; lex->open_line = open_line;
    lex->offset += len;
    if (lex->offset - lex->line_start > lex->max_line)
        lex->max_line = lex->offset - lex->line_start;
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static bool verify_lex_finish(VerifyContext *ctx, const VerifyLexState *lex,
    XiCgenVerifyResult *w1r) {
    if (lex->state == VERIFY_LEX_STRING || lex->state == VERIFY_LEX_CHAR) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lex->open_line, "unterminated %s literal",
                   lex->state == VERIFY_LEX_STRING ? "string" : "character");
        return false;
    }
    if (lex->state == VERIFY_LEX_BLOCK_COMMENT) {
        set_result(ctx, w1r, XI_CGEN_VERIFY_W1_BALANCE, lex->open_line, "unterminated block comment");
        return false;
    }
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

/* Unconditional code owns one persistent scope state across line storage.
 * Preprocessor islands and frame aliases retain their existing checks. */
typedef struct VerifyStructureState {
    int brace, paren, pp_cond, lineno;
    bool have_w1, have_w2, have_w3, have_w4;
} VerifyStructureState;

/* One charged byte traversal shares hygiene, balance and token boundaries.
 * Conditional islands and directives are filtered before this worker runs. */
static bool verify_structure_bytes(VerifyContext *ctx, const char *lp, size_t ln,
    VerifyScratch *scratch, VerifyStructureState *flow) {
    W4State *w4 = &scratch->w4;
    bool check_w4 = !flow->have_w4 && flow->brace > 0;
    size_t token = 0; bool identifier = false, temporary = false;
    w4->definition_count = 0; w4->use_count = 0;
    for (size_t i = 0; ctx->status == XI_CGEN_VERIFY_PASSED && i < ln && verify_work(ctx, 1); ++i) {
        unsigned char current = verify_read(ctx, lp, i);
        if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
        if (!flow->have_w2 && identifier_hygiene_at(ctx, lp, ln, i, current, scratch->detail)) {
            set_result(ctx, &scratch->results[1], XI_CGEN_VERIFY_W2_IDENTIFIER, flow->lineno, "%s", scratch->detail);
            flow->have_w2 = true;
        }
        if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
        if (current == '{') ++flow->brace;
        else if (current == '}') {
            --flow->brace;
            if (flow->brace < 0 && !flow->have_w1) {
                set_result(ctx, &scratch->results[0], XI_CGEN_VERIFY_W1_BALANCE, flow->lineno,
                    "unbalanced '}' (closes with no matching '{')"); flow->have_w1 = true;
            }
        } else if (current == '(') ++flow->paren;
        else if (current == ')') {
            --flow->paren;
            if (flow->paren < 0 && !flow->have_w1) {
                set_result(ctx, &scratch->results[0], XI_CGEN_VERIFY_W1_BALANCE, flow->lineno,
                    "unbalanced ')' (closes with no matching '(')"); flow->have_w1 = true;
            }
        }
        if (!check_w4 || ctx->status != XI_CGEN_VERIFY_PASSED) continue;
        bool part = ident_char(current);
        if (part) {
            if (!identifier) { token = i; identifier = true; temporary = current == 'v'; }
            else if (current < '0' || current > '9') temporary = false;
        }
        if (identifier && (!part || i + 1 == ln)) {
            size_t end = part ? i + 1 : i;
            if (temporary && end > token + 1 && !w4_record_token(ctx, lp, ln, token, end, w4)) return false;
            identifier = false;
        }
    }
    if (check_w4 && ctx->status == XI_CGEN_VERIFY_PASSED &&
        w4_finish_line(ctx, w4, flow->lineno, &scratch->results[3])) flow->have_w4 = true;
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static bool verify_structure_line(VerifyContext *ctx, const char *lp, size_t ln,
    VerifyScratch *scratch, VerifyStructureState *flow) {
    W4State *w4 = &scratch->w4;
    size_t t0 = 0;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (t0 < ln && inline_space(verify_read(ctx, lp, t0), true)) && verify_work(ctx, 1))
        t0++;
    size_t tn = ln;
    while (ctx->status == XI_CGEN_VERIFY_PASSED && (tn > t0 && inline_space(verify_read(ctx, lp, tn - 1), true)) && verify_work(ctx, 1))
        tn--;
    const char *tp = lp + t0;
    size_t tlen = tn - t0;
    bool pp_line = (tlen > 0 && verify_read(ctx, tp, 0) == '#');
    if (pp_line) {
        long def_temp = pp_define_temp(ctx, lp, ln);
        if (def_temp >= 0) w4_mark_defined(ctx, w4, def_temp);
        if (ctx->status != XI_CGEN_VERIFY_PASSED) return false;
        int delta = pp_conditional_delta(ctx, lp, ln);
        if (delta > 0) ++flow->pp_cond;
        else if (delta < 0 && flow->pp_cond > 0) --flow->pp_cond;
    } else if (flow->pp_cond == 0) {
        /* File-scope reset and statement shape use the line's entry depth. */
        if (flow->brace == 0) w4_reset(ctx, w4);
        if (!flow->have_w3 && flow->brace == 0 && file_scope_statement_shape(ctx, tp, tlen)) {
            set_result(ctx, &scratch->results[2], XI_CGEN_VERIFY_W3_SCOPE, flow->lineno,
                "statement-shaped line at file scope (brace depth 0)"); flow->have_w3 = true;
        }
        if (!verify_structure_bytes(ctx, lp, ln, scratch, flow)) return false;
    }
    ++flow->lineno;
    return ctx->status == XI_CGEN_VERIFY_PASSED;
}

static const XiCgenVerifyResult *verify_structure_finish(VerifyContext *ctx,
    VerifyScratch *scratch, const VerifyStructureState *flow) {
    XiCgenVerifyResult *w1r = &scratch->results[0], *w2r = &scratch->results[1];
    XiCgenVerifyResult *w3r = &scratch->results[2], *w4r = &scratch->results[3];
    int brace = flow->brace, paren = flow->paren, lineno = flow->lineno;
    bool have_w1 = flow->have_w1, have_w2 = flow->have_w2;
    bool have_w3 = flow->have_w3, have_w4 = flow->have_w4;
    const XiCgenVerifyResult *selected = NULL;
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
    char *code = NULL;
    char *mask = verify_allocate(ctx, (len + 7) / 8, true);
    VerifyLexState first = {VERIFY_LEX_NORMAL, 1, 1, 0, 0, 0};
    /* Lexical W1 precedes every structural diagnostic, including late EOF
     * failures. This real readonly traversal charges all reads and iterations. */
    if (!mask || !verify_neutralize(ctx, c_src, mask, len, &first) ||
        !verify_lex_finish(ctx, &first, &scratch->results[0])) selected = &scratch->results[0];
    else {
        code = verify_allocate(ctx, first.max_line, false);
        VerifyStructureState flow = {0}; flow.lineno = 1;
        size_t pos = 0;
        while (code && ctx->status == XI_CGEN_VERIFY_PASSED && pos < len && verify_work(ctx, 1)) {
            size_t ls = pos;
            while (pos < len) {
                size_t length = len - pos;
                if (length > 64) length = 64;
                size_t advanced = 0; bool found = false;
                if (!verify_resource(ctx, xr_compile_resources_scan_delimiter(
                        ctx->resources, c_src + pos, length, '\n', &advanced, &found))) break;
                pos += advanced;
                if (found) break;
            }
            if (ctx->status != XI_CGEN_VERIFY_PASSED) break;
            size_t ln = pos - ls;
            if (pos < len) pos++;
            size_t bytes = pos - ls;
            if (bytes > first.max_line) { ctx->status = XI_CGEN_VERIFY_BUDGET; break; }
            verify_copy(ctx, code, c_src + ls, bytes);
            if (!verify_apply_mask(ctx, code, mask, ls, ln)) break;
            if (!verify_structure_line(ctx, code, ln, scratch, &flow)) break;
        }
        if (ctx->status == XI_CGEN_VERIFY_PASSED)
            selected = verify_structure_finish(ctx, scratch, &flow);
    }
    XiCgenVerifyStatus status = verify_publish(ctx,
        selected ? XI_CGEN_VERIFY_MALFORMED : XI_CGEN_VERIFY_PASSED, selected, out);
    xr_compile_resources_free(scratch->w4.uses);
    xr_compile_resources_free(scratch->w4.seen);
    xr_compile_resources_free(code);
    xr_compile_resources_free(mask);
    xr_compile_resources_free(scratch);
    return status;
}

/* ---- Restricted C90 dialect policy. */

typedef struct C90ForbiddenToken {
    const char *token;  /* owned: static string literal */
    const char *detail; /* owned: static string literal */
    size_t length;
} C90ForbiddenToken;

XR_FUNC XiCgenVerifyStatus xr_compile_cgen_verify_c90_output(XrCompileResources *resources, const char *c_src, size_t len, XiCgenVerifyResult *out) {
    static const C90ForbiddenToken forbidden[] = {
        {"_Atomic", "C11 _Atomic residue", sizeof("_Atomic") - 1},
        {"_Thread_local", "C11 _Thread_local residue", sizeof("_Thread_local") - 1},
        {"_Alignof", "C11 _Alignof residue", sizeof("_Alignof") - 1},
        {"long long", "long long residue", sizeof("long long") - 1},
        {"({", "GNU statement-expression residue", sizeof("({") - 1},
        {"){", "C99 compound-literal residue", sizeof("){") - 1},
        {"{ .", "C99 designated-initializer residue", sizeof("{ .") - 1},
        {"...", "variadic residue", sizeof("...") - 1},
        {"union {", "anonymous-union residue", sizeof("union {") - 1},
        {"[];", "flexible-array residue", sizeof("[];") - 1},
        {"for (int ", "C99 loop-declaration residue", sizeof("for (int ") - 1},
        {"for (size_t ", "C99 loop-declaration residue", sizeof("for (size_t ") - 1},
        {"xrt_shared_", "module shared-slot residue", sizeof("xrt_shared_") - 1},
        {"xrt_builtins", "dynamic builtin-table residue", sizeof("xrt_builtins") - 1},
        {"xrt_global_ctx", "hosted runtime-context residue", sizeof("xrt_global_ctx") - 1},
        {"xrt_map_", "dynamic Map runtime residue", sizeof("xrt_map_") - 1},
        {"xrt_set_", "dynamic Set runtime residue", sizeof("xrt_set_") - 1},
        {"xrt_thread", "thread runtime residue", sizeof("xrt_thread") - 1},
        {"xrt_coro", "coroutine runtime residue", sizeof("xrt_coro") - 1},
        {"xrt_task", "task runtime residue", sizeof("xrt_task") - 1},
        {"xrt_channel", "channel runtime residue", sizeof("xrt_channel") - 1},
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
            size_t token_len = forbidden[t].length;
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
