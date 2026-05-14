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
 *   REPL uses the name-keyed globals dict (OP_GETGLOBAL/OP_SETGLOBAL)
 *   instead of the slot-indexed shared array.
 */

#include "xrepl.h"
#include "../base/xchecks.h"
#include "../base/xhash.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xglobal_dict.h"
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
#include "../runtime/value/xvalue_format.h"
#include "../runtime/value/xtype_names.h"
#include "../base/xmalloc.h"
#include "../base/xdynarray.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
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

XrReplSymbolTable *xr_repl_symbols_of(XrayIsolate *isolate) {
    if (!isolate)
        return NULL;
    return isolate->repl_symbols;
}

const char *xr_repl_symbol_cname(const XrReplSymbol *sym) {
    if (!sym || !sym->name || !sym->name->data)
        return NULL;
    return sym->name->data;
}

bool xr_repl_peek_int(XrayIsolate *isolate, const char *name, int64_t *out) {
    if (!isolate || !name || !out)
        return false;

    /* REPL values live in the globals dict */
    if (!isolate->vm.globals)
        return false;
    uint32_t len = (uint32_t) strlen(name);
    uint32_t hash = xr_hash_bytes(name, len);
    XrString *key = xr_string_intern(isolate, name, len, hash);
    if (!key)
        return false;
    XrValue v = xr_global_dict_get(isolate->vm.globals, key);
    if (!XR_IS_INT(v))
        return false;
    *out = v.i;
    return true;
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

/* Collect new declarations from the compiled Xi IR function.
 * proto->xi_func->slot_owned_names has a non-NULL entry for every
 * shared slot declared by this compilation unit; REPL-seeded prior
 * slots are NULL.  Names from the arena are interned so they outlive
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
        bool is_const = xf->slot_owned_consts && xf->slot_owned_consts[slot] != 0;
        repl_symbols_add_or_update(table, interned, (int) slot, is_const);
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

/* Name of the implicit REPL "last result" binding.  GHCi convention;
 * `_` is taken by the match-wildcard token in the xray lexer so cannot
 * be used as an identifier.  Setting this on every auto-echoed
 * expression lets the user chain off the previous result: `1 + 2`
 * then `it * 10` yields 30. */
#define REPL_IT_NAME "it"

/* True iff the persistent REPL symbol table already has a binding for
 * `it` — checked once per echo to decide between AST_VAR_DECL (first
 * time) and AST_ASSIGNMENT (subsequent times).  Avoids re-declaration
 * errors from the analyzer / lowerer. */
static bool repl_has_it_binding(XrayIsolate *isolate) {
    if (!isolate || !isolate->repl_symbols)
        return false;
    XrReplSymbolTable *t = isolate->repl_symbols;
    for (int i = 0; i < t->count; i++) {
        XrString *name = t->symbols[i].name;
        if (name && name->data && strcmp(name->data, REPL_IT_NAME) == 0)
            return true;
    }
    return false;
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

    /* Re-install the program arena so the new node allocations share
     * the AST lifetime.  Restored before returning regardless of
     * partial-failure path. */
    struct XrArena *saved = xr_isolate_get_current_arena(isolate);
    xr_isolate_set_current_arena(isolate, program->as.program.arena);

    /* Build `it = <expr>` or `let it = <expr>` depending on whether
     * the binding already exists.  Using assignment on subsequent
     * echoes avoids analyzer-side "name already declared" errors that
     * would fire for repeated `let it = ...`. */
    AstNode *bind = NULL;
    if (repl_has_it_binding(isolate)) {
        bind = xr_ast_assignment(isolate, REPL_IT_NAME, expr, expr->line);
    } else {
        bind = xr_ast_var_decl(isolate, REPL_IT_NAME, expr, /*is_const=*/false, expr->line);
    }

    /* Print uses a fresh variable reference (not the original expr),
     * so side effects in `expr` happen exactly once via `bind`. */
    AstNode *it_ref = xr_ast_variable(isolate, REPL_IT_NAME, expr->line);
    AstNode *print_args[1] = {it_ref};
    AstNode *synth = (it_ref) ? xr_ast_print_stmt(isolate, print_args, 1, expr->line) : NULL;

    xr_isolate_set_current_arena(isolate, saved);

    if (!bind || !synth) {
        /* Fallback to legacy behaviour (print the expression directly)
         * so a node-allocation failure does not regress to silent
         * dropping of the trailing expression. */
        xr_isolate_set_current_arena(isolate, program->as.program.arena);
        AstNode *args[1] = {expr};
        AstNode *fb = xr_ast_print_stmt(isolate, args, 1, expr->line);
        xr_isolate_set_current_arena(isolate, saved);
        if (fb) {
            fb->as.print_stmt.skip_null = true;
            stmts[count - 1] = fb;
        }
        return;
    }

    /* Auto-echo must stay quiet when the expression evaluates to null
     * — matching Python / Node.js REPL behaviour.  skip_null lets the
     * VM dispatch the binding (so side effects still fire) but print
     * nothing for null values.  `it` is always updated regardless. */
    synth->as.print_stmt.skip_null = true;

    /* Replace the trailing expression statement with the binding and
     * append the print as a new statement.  xr_ast_program_add grows
     * the program's statements array as needed. */
    stmts[count - 1] = bind;
    xr_ast_program_add(isolate, program, synth);
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

    /* Lazy-create the persistent REPL analyzer.  Its global_scope and
     * type pool survive across inputs, so variables, functions, and
     * classes declared in earlier inputs keep their full XaSymbol +
     * inferred type and need no re-seeding from the REPL symbol table. */
    if (!isolate->repl_analyzer) {
        isolate->repl_analyzer = xa_analyzer_new(isolate);
        if (!isolate->repl_analyzer) {
            xr_program_destroy(ast);
            return NULL;
        }
    }

    /* Per-input state reset on the persistent analyzer.  Diagnostics and
     * per-AST side tables must not leak across inputs because prior AST
     * nodes were freed with their owning program arena; their pointers
     * are stale keys in the analyzer's node_table / selection_table. */
    xa_analyzer_clear_diagnostics(isolate->repl_analyzer);

    /* Create compiler context that borrows the persistent analyzer. */
    XrCompilerContext *ctx = xr_compiler_context_new_with_analyzer(isolate, isolate->repl_analyzer);
    if (!ctx) {
        xr_program_destroy(ast);
        return NULL;
    }
    ctx->source_file = "<repl>";
    ctx->repl_mode = true;

    /* Seed compiler-side shared_vars from the REPL symbol table so
     * the analyzer can resolve names from prior inputs. */
    xr_repl_symbols_seed_context(isolate->repl_symbols, ctx);

    XrProto *proto = xr_compile(ctx, ast);

    if (proto && !ctx->had_error) {
        /* Collect new declarations from the Xi IR output so the REPL
         * symbol table stays current for .vars display and peek. */
        repl_symbols_collect_from_xi(isolate->repl_symbols, isolate, proto);
    }

    xr_compiler_context_free(ctx);

    /* Keep the persistent REPL analyzer's pool installed as the current
     * type pool so subsequent parses / analyses in the same REPL session
     * continue allocating into it.  A script-mode compile would restore
     * isolate->analyzer_pool here, but REPL never uses that pool. */
    isolate->current_type_pool = isolate->repl_analyzer->type_pool;

    xr_program_destroy(ast);

    return proto;
}

/* ========== Interactive Inspection ========== */

/* Soft width target for inline value rendering in `.vars`.  Values
 * whose single-line repr exceeds this are dropped to a new indented
 * line so the `name : type` header stays scannable.  72 picks a width
 * that fits comfortably in an 80-column terminal alongside the row
 * prefix `  let|const  <name> : <type> = `. */
#define REPL_VARS_INLINE_WIDTH 72

/* Hard cap on how much of a single value's repr we are willing to
 * dump per `.vars` row before truncating with "..".  Containers
 * naturally limited by xr_value_to_strbuf's depth handling, but a
 * 4KB Array.toString() would still flood the REPL — keep things
 * bounded.  Truncation happens at byte boundaries; not UTF-8 safe
 * but values that long are pathological anyway. */
#define REPL_VARS_HARD_CAP 4096

void xr_repl_print_vars(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_repl_print_vars: NULL isolate");
    if (!isolate || !isolate->repl_symbols || isolate->repl_symbols->count == 0) {
        printf("  (no bindings)\n");
        return;
    }

    XrReplSymbolTable *table = isolate->repl_symbols;

    for (int i = 0; i < table->count; i++) {
        XrReplSymbol *sym = &table->symbols[i];
        const char *name = sym->name && sym->name->data ? sym->name->data : "<anon>";

        XrValue val = xr_shared_array_get(&isolate->vm.shared, sym->shared_index);
        const char *type_name = xr_typeid_name(xr_value_typeid(val));

        XrString *str = xr_value_to_string(isolate, val);
        const char *raw = (str && str->data) ? str->data : "<?>";
        int raw_len = (int) strlen(raw);

        /* Apply hard cap on rendered length to bound pathological
         * cases (very large strings / containers). */
        bool truncated = false;
        int show_len = raw_len;
        if (show_len > REPL_VARS_HARD_CAP) {
            show_len = REPL_VARS_HARD_CAP;
            truncated = true;
        }

        const char *kw = sym->is_const ? "const" : "let  ";

        if (show_len <= REPL_VARS_INLINE_WIDTH && !truncated) {
            /* Short value: keep inline. */
            printf("  %s %s : %s = %.*s\n", kw, name, type_name, show_len, raw);
        } else {
            /* Long value: drop onto a new indented line so the
             * header (name : type) stays scannable.  Indent matches
             * the row prefix (two spaces + "let|const " width). */
            printf("  %s %s : %s =\n", kw, name, type_name);
            printf("        %.*s%s\n", show_len, raw, truncated ? " .." : "");
        }
    }
}

void xr_repl_print_type(XrayIsolate *isolate, const char *expr) {
    XR_DCHECK(isolate != NULL, "xr_repl_print_type: NULL isolate");

    if (!expr)
        expr = "";
    while (*expr && isspace((unsigned char) *expr))
        expr++;

    if (*expr == '\0') {
        printf("Usage: .type <expression>\n");
        return;
    }

    /* Wrap the user expression in `print(typename(...))` and route
     * through the normal incremental pipeline.  The added trailing
     * newline lets users include comments on the same line. */
    size_t expr_len = strlen(expr);
    size_t src_size = expr_len + 32;
    char *src = (char *) xr_malloc(src_size);
    if (!src)
        return;
    snprintf(src, src_size, "print(typename(%s))\n", expr);

    XrProto *proto = xr_repl_compile(isolate, src);
    xr_free(src);

    if (!proto)
        return; /* compile error already reported */

    xr_execute(isolate, proto);
    /* Proto leaked here intentionally: callers in CLI track protos and
     * free them on .reset / shutdown.  Freeing per-invocation would
     * also free any sub-protos still reachable via closures.  Pass-
     * through ownership keeps the model consistent with .load / .time. */
}
