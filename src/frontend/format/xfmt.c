/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfmt.c - AST-based code formatter (entry, config, buffer, public API)
 *
 * KEY CONCEPT:
 *   The formatter is split into thematic translation units; this file
 *   keeps only the per-context buffer plumbing and the three public
 *   entry points (xfmt_init / xfmt_free / xfmt_format_ast plus the
 *   xfmt_node and xfmt_type dispatchers). All AST-shape formatting
 *   lives in xfmt_expr.c / xfmt_stmt.c / xfmt_decl.c / xfmt_type.c /
 *   xfmt_trivia.c / xfmt_literal.c. Cross-TU helpers are declared in
 *   xfmt_internal.h.
 */

#include "xfmt.h"
#include "xfmt_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Default configuration
// ============================================================================

XrFmtConfig xfmt_default_config = {.indent_size = 4,
                                   .use_tabs = 0,
                                   .max_line_length = 100,
                                   .trailing_newline = 1,
                                   .blank_lines_around_functions = 1,
                                   .blank_lines_around_classes = 1,
                                   .space_around_operators = 1,
                                   .space_after_comma = 1,
                                   .space_in_parentheses = 0,
                                   .brace_same_line = 1,
                                   .align_match_arms = 0,
                                   .align_enum_values = 0,
                                   .align_struct_fields = 0,
                                   .align_trailing_comments = 0,
                                   .wrap_long_lines = 0,
                                   .multiline_trailing_comma = 1};

// ============================================================================
// Buffer helpers (cross-TU; declared in xfmt_internal.h)
// ============================================================================

void xfmt_ensure_capacity(XrFmtContext *ctx, size_t additional) {
    if (ctx->length + additional >= ctx->capacity) {
        ctx->capacity = (ctx->capacity + additional) * 2;
        ctx->output = (char *) xr_realloc(ctx->output, ctx->capacity);
    }
}

void xfmt_write_char(XrFmtContext *ctx, char c) {
    xfmt_ensure_capacity(ctx, 1);
    ctx->output[ctx->length++] = c;
    ctx->output[ctx->length] = '\0';

    if (c == '\n') {
        ctx->line_start = 1;
        ctx->column = 0;
    } else {
        ctx->column++;
    }
}

void xfmt_write_str(XrFmtContext *ctx, const char *str) {
    if (!str)
        return;
    size_t len = strlen(str);
    if (len == 0)
        return;
    xfmt_ensure_capacity(ctx, len);
    memcpy(ctx->output + ctx->length, str, len);
    ctx->length += len;
    ctx->output[ctx->length] = '\0';
    // Update line/column tracking
    const char *last_nl = NULL;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n')
            last_nl = str + i;
    }
    if (last_nl) {
        ctx->line_start = 1;
        ctx->column = (int) (str + len - 1 - last_nl);
    } else {
        ctx->column += (int) len;
    }
}

void xfmt_write_fmt(XrFmtContext *ctx, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    xfmt_write_str(ctx, buf);
}

void xfmt_write_indent(XrFmtContext *ctx) {
    if (!ctx->line_start)
        return;

    if (ctx->config->use_tabs) {
        for (int i = 0; i < ctx->indent_level; i++) {
            xfmt_write_char(ctx, '\t');
        }
    } else {
        int spaces = ctx->indent_level * ctx->config->indent_size;
        for (int i = 0; i < spaces; i++) {
            xfmt_write_char(ctx, ' ');
        }
    }
    ctx->line_start = 0;
}

void xfmt_write_newline(XrFmtContext *ctx) {
    xfmt_write_char(ctx, '\n');
}

void xfmt_write_space(XrFmtContext *ctx) {
    xfmt_write_char(ctx, ' ');
}

// ============================================================================
// Snapshot / rollback for long-line wrapping
// ============================================================================

void xfmt_snapshot(XrFmtContext *ctx, XfmtSnapshot *snap) {
    snap->length = ctx->length;
    snap->column = ctx->column;
    snap->line_start = ctx->line_start;
    snap->indent_level = ctx->indent_level;
    snap->in_template_expr = ctx->in_template_expr;
}

void xfmt_rollback(XrFmtContext *ctx, const XfmtSnapshot *snap) {
    ctx->length = snap->length;
    ctx->output[snap->length] = '\0';
    ctx->column = snap->column;
    ctx->line_start = snap->line_start;
    ctx->indent_level = snap->indent_level;
    ctx->in_template_expr = snap->in_template_expr;
}

bool xfmt_fits_on_line(XrFmtContext *ctx, const XfmtSnapshot *snap) {
    for (size_t k = snap->length; k < ctx->length; k++) {
        if (ctx->output[k] == '\n')
            return false;
    }
    int limit = ctx->config ? ctx->config->max_line_length : 100;
    return ctx->column <= limit;
}

// ============================================================================
// Initialize / Free
// ============================================================================

void xfmt_init(XrFmtContext *ctx, XrFmtConfig *config, XrayIsolate *X) {
    ctx->capacity = 4096;
    ctx->output = (char *) xr_malloc(ctx->capacity);
    XR_DCHECK(ctx->output != NULL, "xfmt: output buffer allocation failed");
    ctx->output[0] = '\0';
    ctx->length = 0;
    ctx->indent_level = 0;
    ctx->line_start = 1;
    ctx->column = 0;
    ctx->in_template_expr = 0;
    ctx->config = config ? config : &xfmt_default_config;
    ctx->X = X;
}

void xfmt_free(XrFmtContext *ctx) {
    if (ctx->output) {
        xr_free(ctx->output);
        ctx->output = NULL;
    }
}

// ============================================================================
// Trailing comment column alignment (post-process pass)
// ============================================================================

// Detect a trailing line comment on [line_start, line_end). Returns 1 and
// fills out_code_col / out_comment_off when the line has executable code
// followed by `//`. Strings (`"..."`, `'...'`), block comments and `//`
// inside strings are correctly skipped. Standalone `//` lines (no code
// preceding) are NOT considered trailing.
static int xfmt_detect_trailing_comment(const char *buf, size_t line_start, size_t line_end,
                                        int *out_code_col, size_t *out_comment_off) {
    bool in_dq = false, in_sq = false;
    size_t pos = line_start;
    while (pos < line_end) {
        char c = buf[pos];
        if (in_dq) {
            if (c == '\\' && pos + 1 < line_end) {
                pos += 2;
                continue;
            }
            if (c == '"')
                in_dq = false;
            pos++;
        } else if (in_sq) {
            if (c == '\\' && pos + 1 < line_end) {
                pos += 2;
                continue;
            }
            if (c == '\'')
                in_sq = false;
            pos++;
        } else if (c == '"') {
            in_dq = true;
            pos++;
        } else if (c == '\'') {
            in_sq = true;
            pos++;
        } else if (c == '/' && pos + 1 < line_end && buf[pos + 1] == '/') {
            // Need at least one non-space before `//` to qualify as trailing.
            size_t code_end = pos;
            while (code_end > line_start && buf[code_end - 1] == ' ')
                code_end--;
            if (code_end == line_start)
                return 0;  // standalone comment, not trailing
            *out_code_col = (int) (code_end - line_start);
            *out_comment_off = pos;
            return 1;
        } else if (c == '/' && pos + 1 < line_end && buf[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < line_end && !(buf[pos] == '*' && buf[pos + 1] == '/'))
                pos++;
            if (pos + 1 < line_end)
                pos += 2;
        } else {
            pos++;
        }
    }
    return 0;
}

typedef struct {
    size_t start;        // line start offset (inclusive)
    size_t end;          // line end offset (exclusive of '\n')
    int has_trailing;    // 1 if the line has a trailing `//`
    int code_col;        // bytes of code before the trailing-space gap
    size_t comment_off;  // offset of the `//`
} XfmtLine;

// Post-process the formatted output: for every maximal run of consecutive
// lines that each carry a trailing `// ...`, pad them so their `//` lines
// up at column max(code_col)+2. Lines without a trailing comment, blank
// lines, and lines outside any such run are left untouched. Caller frees
// the returned buffer; the original `input` is freed inside.
static char *xfmt_align_trailing_comments(char *input, size_t in_len, size_t *out_len) {
    if (!input || in_len == 0) {
        *out_len = in_len;
        return input;
    }

    // Phase 1: split into lines and detect trailing comments.
    int line_count = 1;
    for (size_t i = 0; i < in_len; i++)
        if (input[i] == '\n')
            line_count++;
    XfmtLine *lines = (XfmtLine *) xr_malloc(sizeof(XfmtLine) * (size_t) line_count);
    int li = 0;
    size_t s = 0;
    for (size_t i = 0; i <= in_len; i++) {
        if (i == in_len || input[i] == '\n') {
            lines[li].start = s;
            lines[li].end = i;
            lines[li].has_trailing = xfmt_detect_trailing_comment(input, s, i, &lines[li].code_col,
                                                                  &lines[li].comment_off);
            li++;
            s = i + 1;
        }
    }
    int n_lines = li;

    // Phase 2: emit a new buffer, padding consecutive trailing-comment runs.
    size_t cap = in_len + 64;
    char *out = (char *) xr_malloc(cap);
    size_t pos = 0;

    int i = 0;
    while (i < n_lines) {
        if (!lines[i].has_trailing) {
            // Copy verbatim (incl. trailing newline if any).
            size_t copy_end = lines[i].end;
            size_t need = copy_end - lines[i].start + 1;  // +1 for '\n'
            if (pos + need + 1 >= cap) {
                cap = (cap + need) * 2;
                out = (char *) xr_realloc(out, cap);
            }
            memcpy(out + pos, input + lines[i].start, copy_end - lines[i].start);
            pos += copy_end - lines[i].start;
            if (lines[i].end < in_len) {
                out[pos++] = '\n';
            }
            i++;
            continue;
        }

        // Build a run [i, j) of consecutive trailing-comment lines.
        int j = i + 1;
        int max_code_col = lines[i].code_col;
        while (j < n_lines && lines[j].has_trailing) {
            if (lines[j].code_col > max_code_col)
                max_code_col = lines[j].code_col;
            j++;
        }
        int target_col = max_code_col + 2;

        for (int k = i; k < j; k++) {
            // code part: [start, start + code_col)
            size_t code_len = (size_t) lines[k].code_col;
            // comment part: [comment_off, end)
            size_t comment_len = lines[k].end - lines[k].comment_off;
            int pad = target_col - lines[k].code_col;
            size_t need = code_len + (size_t) pad + comment_len + 1;  // +1 for '\n'
            if (pos + need + 1 >= cap) {
                cap = (cap + need) * 2;
                out = (char *) xr_realloc(out, cap);
            }
            memcpy(out + pos, input + lines[k].start, code_len);
            pos += code_len;
            for (int p = 0; p < pad; p++)
                out[pos++] = ' ';
            memcpy(out + pos, input + lines[k].comment_off, comment_len);
            pos += comment_len;
            if (lines[k].end < in_len) {
                out[pos++] = '\n';
            }
        }
        i = j;
    }

    out[pos] = '\0';
    xr_free(lines);
    xr_free(input);
    *out_len = pos;
    return out;
}

// ============================================================================
// Public API
// ============================================================================

void xfmt_node(XrFmtContext *ctx, AstNode *node) {
    if (!node)
        return;

    if (node->type == AST_PROGRAM) {
        xfmt_emit_program(ctx, node);
    } else {
        xfmt_emit_statement(ctx, node);
    }
}

void xfmt_type(XrFmtContext *ctx, XrTypeRef *tref) {
    xfmt_emit_type(ctx, tref);
}

char *xfmt_format_ast(AstNode *ast, XrFmtConfig *config, XrayIsolate *X) {
    XrFmtContext ctx;
    xfmt_init(&ctx, config, X);

    xfmt_node(&ctx, ast);

    if (config && config->trailing_newline && ctx.length > 0 &&
        ctx.output[ctx.length - 1] != '\n') {
        xfmt_write_char(&ctx, '\n');
    }

    char *result = ctx.output;
    size_t result_len = ctx.length;
    ctx.output = NULL;

    // Post-process: align consecutive trailing `//` comments to the same
    // column. Only runs when the user opts in; default leaves the buffer
    // untouched (xfmt always emits exactly two spaces before `//`).
    if (config && config->align_trailing_comments && result) {
        result = xfmt_align_trailing_comments(result, result_len, &result_len);
    }

    return result;
}
