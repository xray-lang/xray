/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrepl.c - REPL incremental execution support
 *
 * KEY CONCEPT:
 *   Persistent symbol table for cross-compilation-unit name resolution.
 *   Seeds each new compiler context so previously defined names are visible.
 *   Uses shared_offset=0 (absolute indices) to avoid unsigned underflow.
 */

#include "xrepl.h"
#include "../base/xchecks.h"
#include "../runtime/xisolate_internal.h"
#include "../frontend/codegen/xcompiler.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xast_api.h"
#include "../frontend/lexer/xlex.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/xexec_state.h"
#include "../base/xmalloc.h"
#include "../base/xdynarray.h"
#include <stdio.h>
#include <string.h>
#include "../frontend/parser/xparse.h"

/* ========== REPL Symbol Table ========== */

#define REPL_SYMBOLS_INITIAL_CAPACITY 32

XrReplSymbolTable* xr_repl_symbols_new(void) {
    XrReplSymbolTable *table = (XrReplSymbolTable *)xr_malloc(sizeof(XrReplSymbolTable));
    if (!table) return NULL;

    table->symbols = (XrReplSymbol *)xr_malloc(sizeof(XrReplSymbol) * REPL_SYMBOLS_INITIAL_CAPACITY);
    if (!table->symbols) {
        xr_free(table);
        return NULL;
    }

    table->count = 0;
    table->capacity = REPL_SYMBOLS_INITIAL_CAPACITY;
    return table;
}

void xr_repl_symbols_free(XrReplSymbolTable *table) {
    if (!table) return;
    if (table->symbols) {
        xr_free(table->symbols);
    }
    xr_free(table);
}

void xr_repl_symbols_clear(XrReplSymbolTable *table) {
    if (!table) return;
    table->count = 0;
}

static void repl_symbols_ensure_capacity(XrReplSymbolTable *table, int needed) {
    XR_DCHECK(table != NULL, "repl_symbols_ensure_capacity: NULL table");
    XR_DCHECK(needed > 0, "repl_symbols_ensure_capacity: non-positive needed");
    if (needed <= table->capacity) return;

    int new_capacity = table->capacity * 2;
    if (new_capacity < needed) new_capacity = needed;

    XrReplSymbol *new_syms = (XrReplSymbol *)xr_realloc(table->symbols,
        sizeof(XrReplSymbol) * new_capacity);
    if (!new_syms) return;

    table->symbols = new_syms;
    table->capacity = new_capacity;
}

// Add or update a symbol in the REPL table
static void repl_symbols_add_or_update(XrReplSymbolTable *table,
                                        XrString *name, int shared_index, bool is_const) {
    // Check if already exists (update case: redefinition)
    for (int i = 0; i < table->count; i++) {
        if (table->symbols[i].name != NULL &&
            strcmp(table->symbols[i].name->data, name->data) == 0) {
            table->symbols[i].shared_index = shared_index;
            table->symbols[i].is_const = is_const;
            return;
        }
    }

    // New symbol
    repl_symbols_ensure_capacity(table, table->count + 1);
    table->symbols[table->count].name = name;
    table->symbols[table->count].shared_index = shared_index;
    table->symbols[table->count].is_const = is_const;
    table->count++;
}

void xr_repl_symbols_seed_context(XrReplSymbolTable *table, XrCompilerContext *ctx) {
    XR_DCHECK(table != NULL, "xr_repl_symbols_seed_context: NULL table");
    XR_DCHECK(ctx != NULL, "xr_repl_symbols_seed_context: NULL ctx");
    if (!table || !ctx || table->count == 0) return;

    // Ensure ctx->shared_vars has enough capacity
    while (ctx->shared_var_capacity < table->count) {
        int new_capacity = ctx->shared_var_capacity < 8
            ? 8 : ctx->shared_var_capacity * 2;
        XrSharedVar *new_vars = (XrSharedVar *)xr_realloc(ctx->shared_vars,
            sizeof(XrSharedVar) * new_capacity);
        if (!new_vars) return;
        for (int i = ctx->shared_var_capacity; i < new_capacity; i++) {
            new_vars[i].name = NULL;
            new_vars[i].index = -1;
            new_vars[i].is_const = false;
        }
        ctx->shared_vars = new_vars;
        ctx->shared_var_capacity = new_capacity;
    }

    // Copy symbols into shared_vars
    for (int i = 0; i < table->count; i++) {
        ctx->shared_vars[i].name = table->symbols[i].name;
        ctx->shared_vars[i].index = table->symbols[i].shared_index;
        ctx->shared_vars[i].scope_depth = 0;
        ctx->shared_vars[i].function_depth = 1;  // top-level script depth
        ctx->shared_vars[i].is_const = table->symbols[i].is_const;
        ctx->shared_vars[i].state = SHARED_STATE_OWNED;
        ctx->shared_vars[i].moved_line = 0;
        ctx->shared_vars[i].moved_column = 0;
        ctx->shared_vars[i].compile_type = NULL;
    }

    ctx->shared_var_count = table->count;
}

void xr_repl_symbols_collect(XrReplSymbolTable *table, XrCompilerContext *ctx,
                             int seeded_count) {
    if (!table || !ctx) return;

    // Collect new definitions (indices beyond seeded_count)
    for (int i = seeded_count; i < ctx->shared_var_count; i++) {
        if (ctx->shared_vars[i].name != NULL) {
            repl_symbols_add_or_update(table,
                ctx->shared_vars[i].name,
                ctx->shared_vars[i].index,
                ctx->shared_vars[i].is_const);
        }
    }
}

/* ========== REPL Input Completeness Check ========== */

XrInputStatus xr_repl_check_input(const char *source) {
    if (!source || !*source) return XR_INPUT_COMPLETE;

    Scanner scanner;
    xr_scanner_init(&scanner, source);

    int paren_depth = 0;    // ()
    int bracket_depth = 0;  // []
    int brace_depth = 0;    // {}

    for (;;) {
        Token token = xr_scanner_scan(&scanner);

        switch (token.type) {
            case TK_LPAREN:         paren_depth++; break;
            case TK_RPAREN:         paren_depth--; break;
            case TK_LBRACKET:       bracket_depth++; break;
            case TK_RBRACKET:       bracket_depth--; break;
            case TK_LBRACE:         brace_depth++; break;
            case TK_RBRACE:         brace_depth--; break;
            case TK_SET_START:      bracket_depth++; break;  // #[
            case TK_EMPTY_MAP_START: brace_depth++; break;   // #{

            case TK_EOF:
                if (paren_depth > 0 || bracket_depth > 0 || brace_depth > 0) {
                    return XR_INPUT_INCOMPLETE;
                }
                return XR_INPUT_COMPLETE;

            case TK_ERROR:
                // Unterminated string/comment/regex → incomplete input
                if (token.start && strstr(token.start, "nterminated") != NULL) {
                    return XR_INPUT_INCOMPLETE;
                }
                // Other lexer errors: let compiler report them
                return XR_INPUT_COMPLETE;

            default:
                break;
        }
    }
}

/* ========== REPL Compilation ========== */

XrProto* xr_repl_compile(XrayIsolate *isolate, const char *source) {
    XR_DCHECK(isolate != NULL, "xr_repl_compile: NULL isolate");
    XR_DCHECK(source != NULL, "xr_repl_compile: NULL source");
    if (!isolate || !source) return NULL;

    // Ensure REPL symbol table exists
    if (!isolate->repl_symbols) {
        isolate->repl_symbols = xr_repl_symbols_new();
        if (!isolate->repl_symbols) return NULL;
    }

    // Parse
    AstNode *ast = xr_parse(isolate, source);
    if (!ast) return NULL;

    // Create compiler context
    XrCompilerContext *ctx = xr_compiler_context_new(isolate);
    if (!ctx) {
        xr_program_destroy(ast);
        return NULL;
    }
    ctx->source_file = "<repl>";
    ctx->repl_mode = true;
    ctx->shared_offset = 0;  // REPL: absolute indices

    // Seed with prior definitions
    int seeded_count = isolate->repl_symbols->count;
    xr_repl_symbols_seed_context(isolate->repl_symbols, ctx);

    // REPL mode: disable analyzer — it cannot see cross-compilation-unit
    // shared variables seeded from prior inputs, producing false warnings.
    if (ctx->analyzer) {
        xa_analyzer_free(ctx->analyzer);
        ctx->analyzer = NULL;
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);

    if (proto && !ctx->had_error) {
        // Force shared_offset=0 on compiled proto
        proto->shared_offset = 0;

        // Update isolate shared array count
        if (ctx->shared_var_count > isolate->vm.shared.count) {
            isolate->vm.shared.count = ctx->shared_var_count;
            xr_shared_array_ensure(&isolate->vm.shared, ctx->shared_var_count - 1);
        }

        // Collect new definitions into REPL symbol table
        xr_repl_symbols_collect(isolate->repl_symbols, ctx, seeded_count);
    }

    xr_compiler_context_free(ctx);
    xr_program_destroy(ast);

    return proto;
}
