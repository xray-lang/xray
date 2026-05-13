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
#include "../frontend/analyzer/xanalyzer.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xast_api.h"
#include "../frontend/lexer/xlex.h"
#include "../ir/xi.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/xexec_state.h"
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../base/xdynarray.h"
#include <stdio.h>
#include <string.h>
#include "../frontend/parser/xparse.h"

/* ========== REPL Symbol Table ========== */

#define REPL_SYMBOLS_INITIAL_CAPACITY 32

XrReplSymbolTable *xr_repl_symbols_new(void) {
    XrReplSymbolTable *table = (XrReplSymbolTable *) xr_malloc(sizeof(XrReplSymbolTable));
    if (!table)
        return NULL;

    table->symbols =
        (XrReplSymbol *) xr_malloc(sizeof(XrReplSymbol) * REPL_SYMBOLS_INITIAL_CAPACITY);
    if (!table->symbols) {
        xr_free(table);
        return NULL;
    }

    table->count = 0;
    table->capacity = REPL_SYMBOLS_INITIAL_CAPACITY;
    return table;
}

void xr_repl_symbols_free(XrReplSymbolTable *table) {
    if (!table)
        return;
    if (table->symbols) {
        xr_free(table->symbols);
    }
    xr_free(table);
}

void xr_repl_symbols_clear(XrReplSymbolTable *table) {
    if (!table)
        return;
    table->count = 0;
}

static void repl_symbols_ensure_capacity(XrReplSymbolTable *table, int needed) {
    XR_DCHECK(table != NULL, "repl_symbols_ensure_capacity: NULL table");
    XR_DCHECK(needed > 0, "repl_symbols_ensure_capacity: non-positive needed");
    if (needed <= table->capacity)
        return;

    int new_capacity = table->capacity * 2;
    if (new_capacity < needed)
        new_capacity = needed;

    XrReplSymbol *new_syms =
        (XrReplSymbol *) xr_realloc(table->symbols, sizeof(XrReplSymbol) * new_capacity);
    if (!new_syms)
        return;

    table->symbols = new_syms;
    table->capacity = new_capacity;
}

// Add or update a symbol in the REPL table
static void repl_symbols_add_or_update(XrReplSymbolTable *table, XrString *name, int shared_index,
                                       bool is_const) {
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
    if (!table || !ctx || table->count == 0)
        return;

    // Ensure ctx->shared_vars has enough capacity
    while (ctx->shared_var_capacity < table->count) {
        int new_capacity = ctx->shared_var_capacity < 8 ? 8 : ctx->shared_var_capacity * 2;
        XrSharedVar *new_vars =
            (XrSharedVar *) xr_realloc(ctx->shared_vars, sizeof(XrSharedVar) * new_capacity);
        if (!new_vars)
            return;
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

void xr_repl_symbols_collect(XrReplSymbolTable *table, XrCompilerContext *ctx, int seeded_count) {
    if (!table || !ctx)
        return;

    // Collect new definitions (indices beyond seeded_count)
    for (int i = seeded_count; i < ctx->shared_var_count; i++) {
        if (ctx->shared_vars[i].name != NULL) {
            repl_symbols_add_or_update(table, ctx->shared_vars[i].name, ctx->shared_vars[i].index,
                                       ctx->shared_vars[i].is_const);
        }
    }
}

/* Collect new declarations from the compiled Xi IR function.
 * proto->xi_func->slot_owned_names has a non-NULL entry for every
 * shared slot declared by this compilation unit; REPL-seeded prior
 * slots are NULL.  The slot index is absolute because REPL forces
 * shared_offset=0.  Names from the arena are interned so they outlive
 * the XiFunc. */
static void repl_symbols_collect_from_xi(XrReplSymbolTable *table, XrayIsolate *isolate,
                                         XrProto *proto) {
    if (!table || !proto || !proto->xi_func)
        return;
    XiFunc *xf = (XiFunc *) proto->xi_func;
    if (!xf->slot_owned_names || xf->nshared == 0)
        return;
    for (uint16_t slot = 0; slot < xf->nshared; slot++) {
        const char *name = xf->slot_owned_names[slot];
        if (!name)
            continue;
        size_t nlen = strlen(name);
        XrString *interned = xr_string_intern(isolate, name, nlen, 0);
        if (!interned)
            continue;
        repl_symbols_add_or_update(table, interned, (int) slot, false);
    }
}

/* ========== REPL Auto-echo ==========
 *
 * REPL convention: the value of a trailing bare expression is printed.
 * This mirrors Python / Node.js / SBCL interactive behaviour, which is
 * what users expect when typing `1 + 2` or `x` at the prompt.
 *
 * The rewrite runs on the parsed AST before analysis: if the final
 * top-level statement is an expression statement AND the expression is
 * not an obvious imperative call (print/println/dump) or an assignment
 * / pre-post inc-dec, it is converted in-place to a PRINT statement.
 *
 * Rewrite happens before analysis so the analyzer and lowerer handle
 * the synthesised print like any other print, including propagating
 * type info and line numbers from the original expression. */
static bool is_imperative_call_name(const char *name) {
    if (!name)
        return false;
    return strcmp(name, "print") == 0 || strcmp(name, "println") == 0 || strcmp(name, "dump") == 0;
}

static void repl_maybe_echo_last_expr(XrayIsolate *isolate, AstNode *program) {
    if (!program || program->type != AST_PROGRAM)
        return;
    AstNode **stmts = program->as.program.statements;
    int count = program->as.program.count;
    if (!stmts || count <= 0)
        return;
    AstNode *last = stmts[count - 1];
    if (!last || last->type != AST_EXPR_STMT)
        return;
    AstNode *expr = last->as.expr_stmt;
    if (!expr)
        return;

    /* Skip rewrites that would cause double output or illegal AST */
    switch (expr->type) {
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
            return;
        case AST_CALL_EXPR: {
            AstNode *callee = expr->as.call_expr.callee;
            if (callee && callee->type == AST_VARIABLE &&
                is_imperative_call_name(callee->as.variable.name)) {
                return;
            }
            break;
        }
        default:
            break;
    }

    /* Re-install the program arena so xr_ast_print_stmt's allocations
     * (node + exprs array) share the AST lifetime. */
    struct XrArena *saved = xr_isolate_get_current_arena(isolate);
    xr_isolate_set_current_arena(isolate, program->as.program.arena);

    AstNode *args[1] = {expr};
    AstNode *synth = xr_ast_print_stmt(isolate, args, 1, expr->line);

    xr_isolate_set_current_arena(isolate, saved);

    if (synth) {
        stmts[count - 1] = synth;
    }
}

/* ========== REPL Input Completeness Check ========== */

XrInputStatus xr_repl_check_input(const char *source) {
    if (!source || !*source)
        return XR_INPUT_COMPLETE;

    Scanner scanner;
    xr_scanner_init(&scanner, source);

    int paren_depth = 0;    // ()
    int bracket_depth = 0;  // []
    int brace_depth = 0;    // {}

    for (;;) {
        Token token = xr_scanner_scan(&scanner);

        switch (token.type) {
            case TK_LPAREN:
                paren_depth++;
                break;
            case TK_RPAREN:
                paren_depth--;
                break;
            case TK_LBRACKET:
                bracket_depth++;
                break;
            case TK_RBRACKET:
                bracket_depth--;
                break;
            case TK_LBRACE:
                brace_depth++;
                break;
            case TK_RBRACE:
                brace_depth--;
                break;
            case TK_SET_START:
                bracket_depth++;
                break;  // #[
            case TK_EMPTY_MAP_START:
                brace_depth++;
                break;  // #{

            case TK_EOF:
                if (paren_depth > 0 || bracket_depth > 0 || brace_depth > 0) {
                    return XR_INPUT_INCOMPLETE;
                }
                return XR_INPUT_COMPLETE;

            case TK_ERROR:
                // Unterminated string/comment/regex -> incomplete input.
                // L-03: diagnostic text is in error_message; start points into source.
                if (token.error_message && strstr(token.error_message, "nterminated") != NULL) {
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

XrProto *xr_repl_compile(XrayIsolate *isolate, const char *source) {
    XR_DCHECK(isolate != NULL, "xr_repl_compile: NULL isolate");
    XR_DCHECK(source != NULL, "xr_repl_compile: NULL source");
    if (!isolate || !source)
        return NULL;

    // Ensure REPL symbol table exists
    if (!isolate->repl_symbols) {
        isolate->repl_symbols = xr_repl_symbols_new();
        if (!isolate->repl_symbols)
            return NULL;
    }

    // Parse
    AstNode *ast = xr_parse(isolate, source);
    if (!ast)
        return NULL;

    /* REPL auto-echo: convert a trailing bare expression into a print
     * so the value shows up without the user typing print() explicitly. */
    repl_maybe_echo_last_expr(isolate, ast);

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

    /* Seed the analyzer's global scope with prior REPL symbols so that
     * variable references in this input resolve to real XaSymbols
     * (instead of producing symbol_id=0, which the Xi IR lowerer rejects
     * as unresolved).  Type information from prior inputs cannot survive
     * because each REPL compilation uses a fresh analyzer + type_pool,
     * so seeded symbols are declared with unknown type.  Analyzer
     * diagnostics are suppressed by xr_compile() in REPL mode since
     * unknown types would otherwise produce false-positive warnings. */
    if (ctx->analyzer) {
        XrType *any_type = xr_type_new_unknown(isolate);
        for (int i = 0; i < isolate->repl_symbols->count; i++) {
            XrReplSymbol *s = &isolate->repl_symbols->symbols[i];
            if (!s->name || !s->name->data)
                continue;
            xa_analyzer_define_var(ctx->analyzer, s->name->data, any_type);
        }
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);

    if (proto && !ctx->had_error) {
        // Force shared_offset=0 on compiled proto
        proto->shared_offset = 0;

        /* Collect new declarations from the Xi IR output.  This is the
         * authoritative source for name → shared slot mappings in the
         * Xi pipeline; ctx->shared_vars is not populated by the Xi
         * lowerer, so the legacy xr_repl_symbols_collect path would
         * miss every new declaration. */
        repl_symbols_collect_from_xi(isolate->repl_symbols, isolate, proto);

        /* Ensure the shared array can hold every slot declared so far
         * (prior inputs + current input).  The Xi emit path allocated
         * slots starting at isolate->vm.shared.count, but REPL forces
         * shared_offset=0, so the highest slot index is now the total
         * symbol count. */
        int highest_slot = 0;
        for (int i = 0; i < isolate->repl_symbols->count; i++) {
            int idx = isolate->repl_symbols->symbols[i].shared_index;
            if (idx + 1 > highest_slot)
                highest_slot = idx + 1;
        }
        if (highest_slot > isolate->vm.shared.count) {
            isolate->vm.shared.count = highest_slot;
            xr_shared_array_ensure(&isolate->vm.shared, highest_slot - 1);
        }
    }
    (void) seeded_count;

    xr_compiler_context_free(ctx);

    // Restore type pool (compiler context freed its analyzer's pool)
    if (isolate->analyzer_pool) {
        isolate->current_type_pool = isolate->analyzer_pool;
    }

    xr_program_destroy(ast);

    return proto;
}
