/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_verify_output.c - CGen output well-formedness verifier (task 218).
 *
 * A single linear pass over a generated C translation unit that neutralizes
 * strings / char literals / comments, then enforces four structural
 * invariants (W1-W4). See xi_cgen_verify_output.h for the contract.
 */

#include "xi_cgen_verify_output.h"

#include "../os/os_proc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guard against pathological temp ids from corrupt input. */
#define XI_CGEN_VERIFY_MAX_TEMP 8388608 /* 8M distinct vN per function */

static bool ident_char(int c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ident_start(int c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void set_result(XiCgenVerifyResult *out, XiCgenVerifyCategory cat, int line, const char *fmt,
                       ...) {
    if (!out)
        return;
    out->category = cat;
    out->line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->message, sizeof(out->message), fmt, ap);
    va_end(ap);
}

const char *xi_cgen_verify_category_name(XiCgenVerifyCategory category) {
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

static bool starts_with_kw(const char *t, size_t n, const char *kw) {
    size_t k = strlen(kw);
    if (n < k)
        return false;
    if (memcmp(t, kw, k) != 0)
        return false;
    /* keyword boundary: next char is not an identifier char */
    return (n == k) || !ident_char((unsigned char) t[k]);
}

/* t/n is the trimmed code-only content of a file-scope line. */
static bool file_scope_statement_shape(const char *t, size_t n) {
    if (n == 0)
        return false;
    /* vN = ... / phiN ... : a bare temporary at file scope. */
    if ((t[0] == 'v' || (n > 3 && t[0] == 'p' && t[1] == 'h' && t[2] == 'i'))) {
        size_t p = (t[0] == 'v') ? 1 : 3;
        if (p < n && isdigit((unsigned char) t[p])) {
            while (p < n && isdigit((unsigned char) t[p]))
                p++;
            /* Followed by an assignment or use, not a declarator like `vec x`. */
            while (p < n && (t[p] == ' ' || t[p] == '\t'))
                p++;
            if (p < n && (t[p] == '=' || t[p] == '.' || t[p] == '-' || t[p] == '[' || t[p] == '+' ||
                          t[p] == ';' || t[p] == ')'))
                return true;
        }
    }
    /* Control-flow / jump statements are only ever valid inside a body. */
    static const char *kws[] = {"if",     "return", "else",     "for",   "while",
                                "switch", "goto",   "continue", "break", "do"};
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if (starts_with_kw(t, n, kws[i]))
            return true;
    }
    return false;
}

/* ---- W2 helpers: identifier hygiene on a single code-only line. */

static bool line_has_identifier_hygiene_violation(const char *s, size_t n, char *detail,
                                                  size_t detail_sz) {
    for (size_t i = 0; i < n; i++) {
        /* Parent-directory path fragment leaked into a symbol position. */
        if (i + 2 < n && s[i] == '.' && s[i + 1] == '.' && s[i + 2] == '/') {
            snprintf(detail, detail_sz, "path fragment '../' in emitted code");
            return true;
        }
        /* An emitted identifier that runs straight into a close brace and a
         * control keyword: the historical `xr_ffi_ } else {` corruption. */
        if (s[i] == '}') {
            /* identifier immediately before '}' (skip inline spaces)? */
            long j = (long) i - 1;
            while (j >= 0 && (s[j] == ' ' || s[j] == '\t'))
                j--;
            if (j >= 0 && ident_char((unsigned char) s[j])) {
                size_t k = i + 1;
                while (k < n && (s[k] == ' ' || s[k] == '\t'))
                    k++;
                if (starts_with_kw(s + k, n - k, "else") ||
                    starts_with_kw(s + k, n - k, "return")) {
                    snprintf(detail, detail_sz,
                             "identifier abuts '} %s' (source fragment in symbol)",
                             starts_with_kw(s + k, n - k, "else") ? "else" : "return");
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
} W4State;

static bool w4_mark_defined(W4State *st, long n) {
    if (n < 0 || n >= XI_CGEN_VERIFY_MAX_TEMP)
        return true; /* out of range: ignore, do not crash */
    if ((size_t) n >= st->cap) {
        size_t newcap = st->cap ? st->cap * 2 : 1024;
        while (newcap <= (size_t) n)
            newcap *= 2;
        unsigned char *p = (unsigned char *) realloc(st->seen, newcap);
        if (!p)
            return false;
        memset(p + st->cap, 0, newcap - st->cap);
        st->seen = p;
        st->cap = newcap;
    }
    st->seen[n] = 1;
    return true;
}

static bool w4_is_defined(const W4State *st, long n) {
    if (n < 0 || (size_t) n >= st->cap)
        return false;
    return st->seen[n] != 0;
}

static void w4_reset(W4State *st) {
    if (st->seen && st->cap)
        memset(st->seen, 0, st->cap);
}

static bool is_control_kw(const char *s, size_t len) {
    static const char *kw[] = {"return", "if",   "else",   "while", "for",      "switch", "case",
                               "do",     "goto", "sizeof", "break", "continue", "default"};
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
        if (len == strlen(kw[i]) && memcmp(s, kw[i], len) == 0)
            return true;
    return false;
}

/* Parse the numeric id of a vN token [tok..end) (tok[0]=='v', rest digits). */
static long vN_id(const char *s, size_t tok, size_t end) {
    long id = 0;
    for (size_t p = tok + 1; p < end; p++) {
        id = id * 10 + (s[p] - '0');
        if (id > XI_CGEN_VERIFY_MAX_TEMP)
            return XI_CGEN_VERIFY_MAX_TEMP;
    }
    return id;
}

/* Is the ident token [tok..tend) a pure temporary name (v followed by digits)? */
static bool token_is_temp(const char *s, size_t tok, size_t tend) {
    if (s[tok] != 'v' || tend <= tok + 1)
        return false;
    for (size_t d = tok + 1; d < tend; d++)
        if (!isdigit((unsigned char) s[d]))
            return false;
    return true;
}

/* Is a temp token at [tok..tend) a definition point rather than a use?
 * Definitions: an SSA store `vN =` (single '='), a pointer/aggregate
 * declarator `Type *vN` / `Type vN` (a non-control identifier or '*' directly
 * precedes it). Coroutine frame fields `Type vN;` therefore count as defs. */
static bool temp_is_def(const char *s, size_t n, size_t tok, size_t tend) {
    size_t k = tend;
    while (k < n && (s[k] == ' ' || s[k] == '\t'))
        k++;
    if (k < n && s[k] == '=' && (k + 1 >= n || s[k + 1] != '='))
        return true;
    long b = (long) tok - 1;
    while (b >= 0 && (s[b] == ' ' || s[b] == '\t'))
        b--;
    if (b < 0)
        return false;
    if (s[b] == '*')
        return true;
    if (ident_char((unsigned char) s[b])) {
        long te = b + 1;
        while (b >= 0 && ident_char((unsigned char) s[b]))
            b--;
        const char *pt = s + (b + 1);
        size_t plen = (size_t) (te - (b + 1));
        if (!is_control_kw(pt, plen))
            return true; /* a type identifier precedes vN -> declaration */
    }
    return false;
}

/* Is a temp token at [tok..tend) a struct-field / member access (foo.vN)? */
static bool temp_is_field(const char *s, size_t tok) {
    long b = (long) tok - 1;
    while (b >= 0 && (s[b] == ' ' || s[b] == '\t'))
        b--;
    return b >= 0 && (s[b] == '.' || (s[b] == '>' && b > 0 && s[b - 1] == '-'));
}

/* Scan one function-body line for W4. Returns true and fills *out on a
 * use-before-def; otherwise records this line's definitions in st. */
static bool w4_scan_line(const char *s, size_t n, int lineno, W4State *st,
                         XiCgenVerifyResult *out) {
    long line_defs[64];
    int ndefs = 0;

    for (size_t i = 0; i < n;) {
        if (!ident_start((unsigned char) s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && ident_char((unsigned char) s[j]))
            j++;
        if (token_is_temp(s, i, j) && !temp_is_field(s, i) && temp_is_def(s, n, i, j)) {
            if (ndefs < (int) (sizeof(line_defs) / sizeof(line_defs[0])))
                line_defs[ndefs++] = vN_id(s, i, j);
        }
        i = j;
    }

    for (size_t i = 0; i < n;) {
        if (!ident_start((unsigned char) s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && ident_char((unsigned char) s[j]))
            j++;
        if (token_is_temp(s, i, j) && !temp_is_field(s, i) && !temp_is_def(s, n, i, j)) {
            long id = vN_id(s, i, j);
            bool same_line_def = false;
            for (int d = 0; d < ndefs; d++)
                if (line_defs[d] == id)
                    same_line_def = true;
            if (!same_line_def && !w4_is_defined(st, id)) {
                set_result(out, XI_CGEN_VERIFY_W4_FORWARD_REF, lineno,
                           "temporary v%ld used before it is defined", id);
                return true;
            }
        }
        i = j;
    }

    for (int d = 0; d < ndefs; d++)
        w4_mark_defined(st, line_defs[d]);
    return false;
}

/* If a line is `#define vN ...`, return the temp id it introduces, else -1. */
static long pp_define_temp(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= n || s[i] != '#')
        return -1;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    const char *def = "define";
    size_t dl = strlen(def);
    if (i + dl > n || memcmp(s + i, def, dl) != 0)
        return -1;
    i += dl;
    if (i < n && ident_char((unsigned char) s[i]))
        return -1; /* not the `define` keyword */
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    size_t tok = i;
    while (i < n && ident_char((unsigned char) s[i]))
        i++;
    if (token_is_temp(s, tok, i))
        return vN_id(s, tok, i);
    return -1;
}

/* Classify a preprocessor directive line: +1 opens a conditional (#if*),
 * -1 closes (#endif), 0 otherwise. */
static int pp_conditional_delta(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= n || s[i] != '#')
        return 0;
    i++;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    size_t tok = i;
    while (i < n && ident_char((unsigned char) s[i]))
        i++;
    size_t len = i - tok;
    const char *d = s + tok;
    if ((len == 2 && memcmp(d, "if", 2) == 0) || (len == 5 && memcmp(d, "ifdef", 5) == 0) ||
        (len == 6 && memcmp(d, "ifndef", 6) == 0))
        return 1;
    if (len == 5 && memcmp(d, "endif", 5) == 0)
        return -1;
    return 0;
}

/* ---- Main verifier. */

bool xi_cgen_verify_output(const char *c_src, size_t len, XiCgenVerifyResult *out) {
    if (out) {
        out->category = XI_CGEN_VERIFY_OK;
        out->line = 0;
        out->message[0] = '\0';
    }
    if (!c_src || len == 0)
        return true;

    char *code = (char *) malloc(len);
    if (!code)
        return true; /* cannot verify under OOM; do not manufacture a crash */
    memcpy(code, c_src, len);

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
    for (size_t i = 0; i < len; i++) {
        char c = code[i];
        char next = (i + 1 < len) ? code[i + 1] : '\0';
        switch (state) {
            case NORMAL:
                if (c == '/' && next == '/') {
                    code[i] = ' ';
                    code[i + 1] = ' ';
                    i++;
                    state = LINE_COMMENT;
                    open_line = line;
                } else if (c == '/' && next == '*') {
                    code[i] = ' ';
                    code[i + 1] = ' ';
                    i++;
                    state = BLOCK_COMMENT;
                    open_line = line;
                } else if (c == '"') {
                    code[i] = ' ';
                    state = STRING;
                    open_line = line;
                } else if (c == '\'') {
                    code[i] = ' ';
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
                    code[i] = ' ';
                }
                break;
            case BLOCK_COMMENT:
                if (c == '*' && next == '/') {
                    code[i] = ' ';
                    code[i + 1] = ' ';
                    i++;
                    state = NORMAL;
                } else if (c == '\n') {
                    line++;
                } else {
                    code[i] = ' ';
                }
                break;
            case STRING:
            case CHAR: {
                char quote = (state == STRING) ? '"' : '\'';
                if (c == '\\') {
                    code[i] = ' ';
                    if (i + 1 < len) {
                        if (code[i + 1] == '\n')
                            line++;
                        else
                            code[i + 1] = ' ';
                        i++;
                    }
                } else if (c == quote) {
                    code[i] = ' ';
                    state = NORMAL;
                } else if (c == '\n') {
                    /* raw newline inside a literal: keep counting, stay lenient */
                    line++;
                } else {
                    code[i] = ' ';
                }
                break;
            }
        }
    }

    if (state == STRING || state == CHAR) {
        set_result(out, XI_CGEN_VERIFY_W1_BALANCE, open_line, "unterminated %s literal",
                   state == STRING ? "string" : "character");
        free(code);
        return false;
    }
    if (state == BLOCK_COMMENT) {
        set_result(out, XI_CGEN_VERIFY_W1_BALANCE, open_line, "unterminated block comment");
        free(code);
        return false;
    }

    /* Pass 2: line-oriented structural checks over the neutralized code.
     *
     * Generated C is only well-formed *after* preprocessing: it carries
     * `#if defined(XRAY_AOT_DEBUG_LOCALS)` islands, `#define vN (f->vN)`
     * coroutine frame aliases, etc. So counting is done only for
     * unconditional code (preprocessor conditional depth 0), preprocessor
     * directives never contribute braces, and `#define vN` marks a temp
     * available for W4. */
    W4State w4 = {NULL, 0};
    int brace = 0, paren = 0;
    int pp_cond = 0; /* nesting depth of #if / #ifdef / #ifndef */
    XiCgenVerifyResult w2r, w3r, w4r;
    bool have_w1 = false, have_w2 = false, have_w3 = false, have_w4 = false;
    XiCgenVerifyResult w1r;

    size_t pos = 0;
    int lineno = 1;
    while (pos < len) {
        size_t ls = pos;
        while (pos < len && code[pos] != '\n')
            pos++;
        size_t le = pos; /* exclusive, excludes '\n' */
        if (pos < len)
            pos++; /* consume '\n' */

        const char *lp = code + ls;
        size_t ln = le - ls;

        /* trim leading whitespace for shape checks */
        size_t t0 = 0;
        while (t0 < ln && (lp[t0] == ' ' || lp[t0] == '\t' || lp[t0] == '\r'))
            t0++;
        size_t tn = ln;
        while (tn > t0 && (lp[tn - 1] == ' ' || lp[tn - 1] == '\t' || lp[tn - 1] == '\r'))
            tn--;
        const char *tp = lp + t0;
        size_t tlen = tn - t0;
        bool pp_line = (tlen > 0 && tp[0] == '#');

        if (pp_line) {
            /* Preprocessor directives never carry structural braces. Track
             * conditional nesting and honor `#define vN` frame aliases. */
            long def_temp = pp_define_temp(lp, ln);
            if (def_temp >= 0)
                w4_mark_defined(&w4, def_temp);
            int delta = pp_conditional_delta(lp, ln);
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
            w4_reset(&w4);

        /* W2 identifier hygiene */
        if (!have_w2) {
            char detail[160];
            if (line_has_identifier_hygiene_violation(lp, ln, detail, sizeof(detail))) {
                set_result(&w2r, XI_CGEN_VERIFY_W2_IDENTIFIER, lineno, "%s", detail);
                have_w2 = true;
            }
        }
        /* W3 file-scope statement shape */
        if (!have_w3 && start_brace == 0 && file_scope_statement_shape(tp, tlen)) {
            set_result(&w3r, XI_CGEN_VERIFY_W3_SCOPE, lineno,
                       "statement-shaped line at file scope (brace depth 0)");
            have_w3 = true;
        }
        /* W4 forward reference (only meaningful inside a function body) */
        if (!have_w4 && start_brace > 0) {
            if (w4_scan_line(lp, ln, lineno, &w4, &w4r))
                have_w4 = true;
        }

        /* brace/paren balance (W1); braces in unconditional code are real. */
        for (size_t j = 0; j < ln; j++) {
            char c = lp[j];
            if (c == '{') {
                brace++;
            } else if (c == '}') {
                brace--;
                if (brace < 0 && !have_w1) {
                    set_result(&w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno,
                               "unbalanced '}' (closes with no matching '{')");
                    have_w1 = true;
                }
            } else if (c == '(') {
                paren++;
            } else if (c == ')') {
                paren--;
                if (paren < 0 && !have_w1) {
                    set_result(&w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno,
                               "unbalanced ')' (closes with no matching '(')");
                    have_w1 = true;
                }
            }
        }
        lineno++;
    }

    if (!have_w1 && brace != 0) {
        set_result(&w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno > 1 ? lineno - 1 : 1,
                   "unbalanced braces at end of unit (depth %d)", brace);
        have_w1 = true;
    }
    if (!have_w1 && paren != 0) {
        set_result(&w1r, XI_CGEN_VERIFY_W1_BALANCE, lineno > 1 ? lineno - 1 : 1,
                   "unbalanced parentheses at end of unit (depth %d)", paren);
        have_w1 = true;
    }

    free(w4.seen);
    free(code);

    /* Priority: W1 > W2 > W3 > W4. */
    if (have_w1) {
        if (out)
            *out = w1r;
        return false;
    }
    if (have_w2) {
        if (out)
            *out = w2r;
        return false;
    }
    if (have_w3) {
        if (out)
            *out = w3r;
        return false;
    }
    if (have_w4) {
        if (out)
            *out = w4r;
        return false;
    }
    return true;
}

/* ---- Restricted C90 dialect policy. */

typedef struct C90ForbiddenToken {
    const char *token;  /* owned: static string literal */
    const char *detail; /* owned: static string literal */
} C90ForbiddenToken;

bool xi_cgen_verify_c90_output(const char *c_src, size_t len, XiCgenVerifyResult *out) {
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
    enum { C90_SCAN_CODE = 0, C90_SCAN_BLOCK_COMMENT, C90_SCAN_STRING, C90_SCAN_CHAR } state =
        C90_SCAN_CODE;
    int line = 1;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!c_src || len == 0)
        return true;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) c_src[i];
        unsigned char next = i + 1 < len ? (unsigned char) c_src[i + 1] : 0;
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
                if (c_src[i + 1] == '\n')
                    line++;
                i++;
                continue;
            }
            if ((state == C90_SCAN_STRING && c == '"') ||
                (state == C90_SCAN_CHAR && c == '\''))
                state = C90_SCAN_CODE;
            continue;
        }
        if (c == '/' && next == '*') {
            state = C90_SCAN_BLOCK_COMMENT;
            i++;
            continue;
        }
        if (c == '/' && next == '/') {
            set_result(out, XI_CGEN_VERIFY_C90_RESTRICTED, line, "C++ line-comment residue");
            return false;
        }
        if (c == '"') {
            state = C90_SCAN_STRING;
            continue;
        }
        if (c == '\'') {
            state = C90_SCAN_CHAR;
            continue;
        }
        if (ident_start(c) && i + 6 <= len && memcmp(c_src + i, "inline", 6) == 0 &&
            (i == 0 || !ident_char((unsigned char) c_src[i - 1])) &&
            (i + 6 == len || !ident_char((unsigned char) c_src[i + 6]))) {
            set_result(out, XI_CGEN_VERIFY_C90_RESTRICTED, line, "C99 inline residue");
            return false;
        }
        for (size_t t = 0; t < sizeof(forbidden) / sizeof(forbidden[0]); t++) {
            size_t token_len = strlen(forbidden[t].token);
            if (i + token_len <= len && memcmp(c_src + i, forbidden[t].token, token_len) == 0) {
                set_result(out, XI_CGEN_VERIFY_C90_RESTRICTED, line, "%s",
                           forbidden[t].detail);
                return false;
            }
        }
    }
    return true;
}

/* ---- Fail-closed ICE wrapper used at the C-write boundary. */

void xi_cgen_verify_output_or_ice(const char *c_src, size_t len, const char *tu_name) {
    XiCgenVerifyResult r;
    if (xi_cgen_verify_output(c_src, len, &r))
        return;

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
             (long long) xr_proc_self_pid());

    FILE *f = fopen(path, "w");
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
