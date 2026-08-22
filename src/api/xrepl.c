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
#include "../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../frontend/xdiag_fmt.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xast_api.h"
#include "../frontend/lexer/xlex.h"
#include "../ir/xi.h"
#include "../module/xmodule_identity.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/xexec_state.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/value/xvalue_format.h"
#include "../runtime/value/xtype_names.h"
#include "../toolchain/xcompiler_session.h"
#include "../base/xmalloc.h"
#include "../base/xdynarray.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include "../frontend/parser/xparse.h"
#include "../frontend/parser/xparse_internal.h"

/* ========== REPL Symbol Table ========== */

#define REPL_SYMBOLS_INITIAL_CAPACITY 32
#define REPL_IT_NAME "it"
#define REPL_RESULT_PREFIX "__xray_repl_result$"

static XrCompilerSession *repl_compiler_session_for_isolate(XrVMRuntime *isolate) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    if (session)
        return session;

    XrCompilerSessionConfig cfg = {
        .vm_host = isolate,
        .source_file = "<repl>",
        .repl_mode = true,
    };
    session = xr_compiler_session_new(&cfg);
    if (!session)
        return NULL;
    xr_compiler_session_attach_isolate(isolate, session);
    return session;
}

XrReplSymbolTable *xr_repl_symbols_new(void) {
    XrReplSymbolTable *table = (XrReplSymbolTable *) xr_calloc(1, sizeof(XrReplSymbolTable));
    if (!table)
        return NULL;

    table->symbols =
        (XrReplSymbol *) xr_malloc(sizeof(XrReplSymbol) * REPL_SYMBOLS_INITIAL_CAPACITY);
    if (!table->symbols) {
        xr_free(table);
        return NULL;
    }

    table->capacity = REPL_SYMBOLS_INITIAL_CAPACITY;
    return table;
}

void xr_repl_symbols_free(XrReplSymbolTable *table) {
    if (!table)
        return;
    if (table->symbols) {
        xr_free(table->symbols);
    }
    xr_free(table->result_names);
    xr_free(table);
}

XrReplSymbolTable *xr_repl_symbols_of(XrVMRuntime *isolate) {
    if (!isolate)
        return NULL;
    return xr_compiler_session_repl_symbols(xr_compiler_session_current_for_isolate(isolate));
}

const char *xr_repl_symbol_cname(const XrReplSymbol *sym) {
    if (!sym || !sym->name)
        return NULL;
    return sym->name->data;
}

bool xr_repl_peek_int(XrVMRuntime *isolate, const char *name, int64_t *out) {
    if (!isolate || !name || !out)
        return false;

    /* REPL values live in the globals dict */
    if (!isolate->vm.globals)
        return false;
    XrReplSymbolTable *table = xr_repl_symbols_of(isolate);
    XrString *key = NULL;
    if (strcmp(name, REPL_IT_NAME) == 0 && table)
        key = table->latest_result_name;
    uint32_t len = (uint32_t) strlen(name);
    if (!key) {
        uint32_t hash = xr_hash_bytes(name, len);
        key = xr_string_intern(isolate, name, len, hash);
    }
    if (!key)
        return false;
    XrValue v = xr_global_dict_get(isolate->vm.globals, key);
    if (!XR_IS_INT(v))
        return false;
    *out = v.i;
    return true;
}

bool xr_repl_has_last_result(XrVMRuntime *isolate) {
    XrReplSymbolTable *table = xr_repl_symbols_of(isolate);
    return table && table->latest_result_name;
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
static void repl_symbols_add_or_update(XrReplSymbolTable *table, XrString *name, bool is_const) {
    /* Update existing entry on redefinition */
    for (int i = 0; i < table->count; i++) {
        if (table->symbols[i].name != NULL &&
            strcmp(table->symbols[i].name->data, name->data) == 0) {
            table->symbols[i].is_const = is_const;
            return;
        }
    }

    /* New symbol */
    repl_symbols_ensure_capacity(table, table->count + 1);
    table->symbols[table->count].name = name;
    table->symbols[table->count].is_const = is_const;
    table->count++;
}

void xr_repl_symbols_seed_context(XrReplSymbolTable *table, XrCompilerContext *ctx) {
    XR_DCHECK(table != NULL, "xr_repl_symbols_seed_context: NULL table");
    XR_DCHECK(ctx != NULL, "xr_repl_symbols_seed_context: NULL ctx");
    if (!table || !ctx || (table->count == 0 && table->result_count == 0))
        return;

    int total = table->count + table->result_count;

    /* Ensure ctx->shared_vars has enough capacity */
    while (ctx->shared_var_capacity < total) {
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

    /* Seed names and const flags — slot index is irrelevant for REPL
     * since the lowerer emits OP_GETGLOBAL/OP_SETGLOBAL by name.
     * Use sequential indices so prescan_shared_vars recognizes them
     * as top-level variables (shared_map[vid] >= 0). */
    for (int i = 0; i < table->count; i++) {
        ctx->shared_vars[i].name = table->symbols[i].name;
        ctx->shared_vars[i].index = i;
        ctx->shared_vars[i].scope_depth = 0;
        ctx->shared_vars[i].function_depth = 1;
        ctx->shared_vars[i].is_const = table->symbols[i].is_const;
        ctx->shared_vars[i].state = SHARED_STATE_OWNED;
        ctx->shared_vars[i].moved_line = 0;
        ctx->shared_vars[i].moved_column = 0;
        ctx->shared_vars[i].compile_type = NULL;
    }

    for (int i = 0; i < table->result_count; i++) {
        int slot = table->count + i;
        ctx->shared_vars[slot].name = table->result_names[i];
        ctx->shared_vars[slot].index = slot;
        ctx->shared_vars[slot].scope_depth = 0;
        ctx->shared_vars[slot].function_depth = 1;
        ctx->shared_vars[slot].is_const = true;
        ctx->shared_vars[slot].state = SHARED_STATE_OWNED;
        ctx->shared_vars[slot].moved_line = 0;
        ctx->shared_vars[slot].moved_column = 0;
        ctx->shared_vars[slot].compile_type = NULL;
    }

    ctx->shared_var_count = total;
}

/* Collect new declarations from the compiled Xi IR function.
 * slot_owned_names has a non-NULL entry for every top-level name
 * declared by this compilation unit; REPL-seeded prior slots are
 * NULL.  Names from the arena are interned so they outlive the
 * XiFunc. */
static void repl_symbols_collect_from_xi(XrReplSymbolTable *table, XrVMRuntime *isolate,
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
        repl_symbols_add_or_update(table, interned, is_const);
    }
}

static bool repl_results_append(XrReplSymbolTable *table, XrString *name) {
    if (!table || !name)
        return false;
    if (table->result_count == table->result_capacity) {
        int next_capacity = table->result_capacity ? table->result_capacity * 2 : 8;
        XrString **next = (XrString **) xr_realloc(
            table->result_names, (size_t) next_capacity * sizeof(*table->result_names));
        if (!next)
            return false;
        table->result_names = next;
        table->result_capacity = next_capacity;
    }
    table->result_names[table->result_count++] = name;
    table->latest_result_name = name;
    return true;
}

/* ========== REPL Auto-echo ==========
 *
 * REPL convention: the value of a trailing bare expression is printed.
 * This mirrors Python / Node.js / SBCL interactive behaviour, which is
 * what users expect when typing `1 + 2` or `x` at the prompt.
 *
 * After the first analysis pass, a meaningful tail expression is elaborated
 * into an immutable, versioned result binding plus a print. Unit expressions
 * stay untouched so their side effects run without replacing `it`. */
static bool is_imperative_call_name(const char *name) {
    if (!name)
        return false;
    return strcmp(name, "print") == 0 || strcmp(name, "dump") == 0;
}

/* Name of the implicit REPL "last result" binding.  GHCi convention;
 * `_` is taken by the match-wildcard token in the xray lexer so cannot
 * be used as an identifier.  Setting this on every auto-echoed
 * expression lets the user chain off the previous result: `1 + 2`
 * then `it * 10` yields 30. */
typedef struct ReplEchoPlan {
    XrCompilerSession *session;
    XrReplSymbolTable *table;
    AstNode *expr;
    char result_name[96];
    bool has_result;
} ReplEchoPlan;

static void repl_plan_last_expr(ReplEchoPlan *plan, AstNode *program) {
    if (!plan)
        return;
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

    plan->expr = expr;
}

typedef struct ReplAliasRewrite {
    XrCompilerSession *session;
    uint32_t symbol_id;
    const char *canonical_name;
} ReplAliasRewrite;

static void repl_rewrite_it_ref(AstNode *node, void *user_data) {
    ReplAliasRewrite *rewrite = (ReplAliasRewrite *) user_data;
    if (!node || !rewrite || node->type != AST_VARIABLE || !node->as.variable.name ||
        node->as.variable.symbol_id != rewrite->symbol_id ||
        strcmp(node->as.variable.name, REPL_IT_NAME) != 0)
        return;
    node->as.variable.name = ast_strdup(rewrite->session, rewrite->canonical_name);
}

static bool repl_analyzer_has_error(XaAnalyzer *analyzer) {
    int count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &count); diag;
         diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return true;
    }
    return false;
}

/* First-pass types are available when this hook runs. Resolve the public `it`
 * alias to its canonical versioned binding, then elaborate a meaningful tail
 * expression into one immutable result binding plus one print. Unit expressions
 * remain ordinary expression statements: their side effects run, but they do
 * not replace the last meaningful result. */
static void repl_elaborate_last_expr(XrCompilerContext *ctx, AstNode *program, void *user_data) {
    ReplEchoPlan *plan = (ReplEchoPlan *) user_data;
    if (!ctx || !ctx->analyzer || !program || !plan)
        return;

    XrCompilerSessionScope synth_scope;
    bool has_synth_scope = xr_compiler_session_push_arena(plan->session, program->as.program.arena,
                                                          "<repl>", &synth_scope);

    if (plan->table->latest_result_name) {
        XaSymbol *last =
            xa_scope_lookup(ctx->analyzer->current_scope, plan->table->latest_result_name->data);
        if (last) {
            ReplAliasRewrite rewrite = {
                .session = plan->session,
                .symbol_id = last->id,
                .canonical_name = last->name,
            };
            xa_ast_visit_refs(program, repl_rewrite_it_ref, &rewrite);
        }
    }

    if (!plan->expr || repl_analyzer_has_error(ctx->analyzer))
        goto done;

    XrType *result_type = xa_analyzer_get_node_type(ctx->analyzer, plan->expr);
    if (!result_type || XR_TYPE_IS_ERROR(result_type) || XR_TYPE_IS_UNIT(result_type))
        goto done;

    uint64_t result_id = plan->table->next_result_id++;
    snprintf(plan->result_name, sizeof(plan->result_name), REPL_RESULT_PREFIX "%" PRIu64,
             result_id);

    AstNode *bind = xr_ast_var_decl(plan->session, plan->result_name, plan->expr,
                                    /*is_const=*/true, /*line=*/0);
    AstNode *result_ref = xr_ast_variable(plan->session, plan->result_name, plan->expr->line);
    AstNode *print_args[1] = {result_ref};
    AstNode *print =
        result_ref ? xr_ast_print_stmt(plan->session, print_args, 1, plan->expr->line) : NULL;
    if (!bind || !print)
        goto done;

    print->as.print_stmt.skip_null = true;
    AstNode **stmts = program->as.program.statements;
    int count = program->as.program.count;
    stmts[count - 1] = bind;
    xr_ast_program_add(plan->session, program, print);
    plan->has_result = true;

done:
    if (has_synth_scope)
        xr_compiler_session_pop_arena(&synth_scope);
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

/* ========== REPL Evaluation ========== */

static bool repl_pattern_binds_it(const XrDestructurePattern *pattern) {
    if (!pattern)
        return false;
    if (pattern->type == PATTERN_IDENTIFIER)
        return pattern->as.identifier.name &&
               strcmp(pattern->as.identifier.name, REPL_IT_NAME) == 0;
    if (pattern->type == PATTERN_ARRAY || pattern->type == PATTERN_TUPLE) {
        for (int i = 0; i < pattern->as.array.element_count; i++) {
            if (repl_pattern_binds_it(pattern->as.array.elements[i]))
                return true;
        }
    } else if (pattern->type == PATTERN_OBJECT) {
        for (int i = 0; i < pattern->as.object.field_count; i++) {
            if (repl_pattern_binds_it(pattern->as.object.patterns[i]))
                return true;
        }
    }
    return false;
}

static bool repl_stmt_declares_it(const AstNode *stmt) {
    if (!stmt)
        return false;
    const char *name = NULL;
    switch (stmt->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            name = stmt->as.var_decl.name;
            break;
        case AST_FUNCTION_DECL:
            name = stmt->as.function_decl.name;
            break;
        case AST_CLASS_DECL:
            name = stmt->as.class_decl.name;
            break;
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            name = stmt->as.struct_decl.name;
            break;
        case AST_INTERFACE_DECL:
            name = stmt->as.interface_decl.name;
            break;
        case AST_ENUM_DECL:
            name = stmt->as.enum_decl.name;
            break;
        case AST_TYPE_ALIAS:
            name = stmt->as.type_alias.name;
            break;
        case AST_DESTRUCTURE_DECL:
            return repl_pattern_binds_it(stmt->as.destructure_decl.pattern);
        case AST_IMPORT_STMT: {
            const ImportStmtNode *import = &stmt->as.import_stmt;
            if (import->member_count == 0) {
                name = import->alias ? import->alias : import->module_name;
                break;
            }
            for (int i = 0; i < import->member_count; i++) {
                const ImportMember *member = &import->members[i];
                const char *local_name = member->alias ? member->alias : member->name;
                if (local_name && strcmp(local_name, REPL_IT_NAME) == 0)
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
    return name && strcmp(name, REPL_IT_NAME) == 0;
}

static AstNode *repl_find_reserved_it_decl(AstNode *program) {
    if (!program || program->type != AST_PROGRAM)
        return NULL;
    for (int i = 0; i < program->as.program.count; i++) {
        AstNode *stmt = program->as.program.statements[i];
        if (repl_stmt_declares_it(stmt))
            return stmt;
    }
    return NULL;
}

static void repl_abandon_declaration(
    XrCompilerSessionReplDeclarationScope *scope,
    XrCompilerSessionReplDeclarationState state) {
    XR_CHECK(xr_compiler_session_repl_declaration_abandon(scope, state),
             "REPL declaration generation abandonment failed");
}

XrReplEvalResult xr_repl_eval(XrCompilerSession *session, XrVMRuntime *vm_host,
                              const char *source,
                              const XrModuleIdentityAuthority *authority) {
    XrReplEvalResult result = {.proto = NULL, .status = XR_REPL_EVAL_COMPILE_ERROR};
    XR_DCHECK(session != NULL, "xr_repl_eval: NULL compiler session");
    XR_DCHECK(vm_host != NULL, "xr_repl_eval: NULL VM host");
    XR_DCHECK(source != NULL, "xr_repl_eval: NULL source");
    if (!session || !vm_host || !source || !authority ||
        authority->kind != XR_MODULE_IDENTITY_MEMORY ||
        !xr_module_identity_authority_valid(authority))
        return result;
    XR_DCHECK(xr_compiler_session_vm_host(session) == vm_host,
              "xr_repl_eval: compiler session VM host mismatch");
    if (xr_compiler_session_vm_host(session) != vm_host)
        return result;

    XrCompilerSessionReplDeclarationScope declaration_scope = {0};
    if (!xr_compiler_session_repl_declaration_begin(session, &declaration_scope))
        return result;

    XrReplSymbolTable *repl_symbols = xr_compiler_session_ensure_repl_symbols(session);
    if (!repl_symbols) {
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }

    // Parse. REPL units observe the value of a trailing bare expression (see
    // repl_plan_* below), so they must not be rejected as effectless (E0208).
    AstNode *ast = xr_parse_repl_unit(session, source);
    if (!ast) {
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }
    if (ast->as.program.count > UINT32_MAX) {
        xr_program_destroy(ast);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }

    AstNode *reserved_decl = repl_find_reserved_it_decl(ast);
    if (reserved_decl) {
        xr_diag_print(XR_DIAG_ERROR, 0,
                      "'it' is reserved for the REPL's last successful non-unit result", "<repl>",
                      reserved_decl->line, reserved_decl->column, 0, NULL, NULL);
        xr_diag_print_summary("<repl>", 1, 0, 0);
        xr_program_destroy(ast);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }

    ReplEchoPlan echo_plan = {.session = session, .table = repl_symbols};
    repl_plan_last_expr(&echo_plan, ast);

    /* Lazy-create the persistent REPL analyzer.  Its global_scope and
     * type pool survive across inputs, so variables, functions, and
     * classes declared in earlier inputs keep their full XaSymbol +
     * inferred type and need no re-seeding from the REPL symbol table. */
    XaAnalyzer *repl_analyzer = xr_compiler_session_ensure_repl_analyzer(session);
    if (!repl_analyzer) {
        xr_program_destroy(ast);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }
    if (!xr_compiler_session_retain_repl_program(session, ast)) {
        xr_program_destroy(ast);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }

    /* Per-input state reset on the persistent analyzer.  Diagnostics and
     * per-AST side tables must not leak across inputs because prior AST
     * nodes were freed with their owning program arena; their pointers
     * are stale keys in the analyzer's node_table / selection_table. */
    xa_analyzer_clear_diagnostics(repl_analyzer);

    /* Create compiler context that borrows the persistent analyzer. */
    XrCompilerContext *ctx = xr_compiler_context_new_with_analyzer(session, repl_analyzer);
    if (!ctx) {
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }
    ctx->source_file = "<repl>";
    ctx->repl_mode = true;
    ctx->post_analyze_hook = repl_elaborate_last_expr;
    ctx->post_analyze_user_data = &echo_plan;

    /* Seed compiler-side shared_vars from the REPL symbol table so
     * the analyzer can resolve names from prior inputs. */
    xr_repl_symbols_seed_context(repl_symbols, ctx);

    char *module_identity = NULL;
    XrCompileUnitIdentity compile_identity = {
        .kind = XR_COMPILE_UNIT_MEMORY,
    };
    if (!xr_module_identity_from_logical(authority, NULL, &module_identity)) {
        xr_compiler_context_free(ctx);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }
    compile_identity.module_identity = module_identity;
    if (!xr_compiler_session_set_compile_unit_identity(session, &compile_identity)) {
        xr_compiler_context_free(ctx);
        xr_free(module_identity);
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }
    XrProto *proto = xr_compile(ctx, ast);
    xr_compiler_session_set_compile_unit_identity(session, NULL);
    xr_free(module_identity);

    if (proto && !ctx->had_error && !xr_entry_plan_derive(proto)) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }

    xr_compiler_context_free(ctx);

    /* Keep the persistent REPL analyzer's pool installed as the current
     * type pool so subsequent parses / analyses in the same REPL session
     * continue allocating into it.  A script-mode compile would restore
     * session-owned analyzer_pool here, but REPL never uses that pool. */
    xr_type_set_current_pool(repl_analyzer->type_pool, &repl_analyzer->type_pool->next_type_id);

    if (!proto) {
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_COMPILE);
        return result;
    }

    result.proto = proto;
    if (xr_execute(vm_host, proto) != 0) {
        result.status = XR_REPL_EVAL_RUNTIME_ERROR;
        repl_abandon_declaration(
            &declaration_scope,
            XR_COMPILER_SESSION_REPL_DECLARATION_ABANDONED_RUNTIME);
        return result;
    }

    /* Publish only after successful execution. A failed submission cannot
     * change name resolution for the next prompt. */
    repl_symbols_collect_from_xi(repl_symbols, vm_host, proto);
    if (echo_plan.has_result) {
        size_t name_len = strlen(echo_plan.result_name);
        XrString *interned = xr_string_intern(vm_host, echo_plan.result_name, name_len, /*hash=*/0);
        XR_CHECK(interned != NULL, "REPL result name interning failed");
        XR_CHECK(xr_global_dict_has(vm_host->vm.globals, interned),
                 "successful REPL result was not stored in globals");
        XR_CHECK(repl_results_append(repl_symbols, interned),
                 "REPL result metadata allocation failed");

        XaSymbol *result_symbol =
            xa_scope_lookup(repl_analyzer->current_scope, echo_plan.result_name);
        XR_CHECK(result_symbol != NULL, "REPL result symbol missing after successful analysis");
        xa_scope_set_alias(repl_analyzer->current_scope, REPL_IT_NAME, result_symbol);
    }

    XR_CHECK(xr_compiler_session_repl_declaration_publish(
                 &declaration_scope, (uint32_t) ast->as.program.count),
             "REPL declaration generation publication failed");
    result.status = XR_REPL_EVAL_OK;
    return result;
}

/* ========== Interactive Inspection ========== */

/* Soft width target for inline value rendering in `.vars`.  Values
 * whose single-line repr exceeds this are dropped to a new indented
 * line so the `name : type` header stays scannable.  72 picks a width
 * that fits comfortably in an 80-column terminal alongside the row
 * prefix `  var|const  <name> : <type> = `. */
#define REPL_VARS_INLINE_WIDTH 72

/* Hard cap on how much of a single value's repr we are willing to
 * dump per `.vars` row before truncating with "..".  Containers
 * naturally limited by xr_value_to_strbuf's depth handling, but a
 * 4KB Array.toString() would still flood the REPL — keep things
 * bounded.  Truncation happens at byte boundaries; not UTF-8 safe
 * but values that long are pathological anyway. */
#define REPL_VARS_HARD_CAP 4096

/* Look up const-ness in the REPL symbol table for a given name.
 * Returns true if declared as const, false otherwise (including
 * when the symbol table has no entry for this name). */
static bool repl_symbol_is_const(XrReplSymbolTable *table, XrString *name) {
    if (!table || !name)
        return false;
    for (int i = 0; i < table->count; i++) {
        if (table->symbols[i].name == name)
            return table->symbols[i].is_const;
    }
    return false;
}

/* Visitor callback for xr_global_dict_iter.  Prints one `.vars` row. */
typedef struct {
    XrVMRuntime *isolate;
    XrReplSymbolTable *table;
    int printed_count;
} ReplVarsCtx;

static bool repl_result_is_internal(XrReplSymbolTable *table, XrString *name) {
    if (!table || !name)
        return false;
    for (int i = 0; i < table->result_count; i++) {
        if (table->result_names[i] == name)
            return true;
    }
    return false;
}

static void print_vars_value(ReplVarsCtx *ctx, const char *cname, XrValue value, bool is_const) {
    const char *type_name = xr_typeid_name(xr_value_typeid(value));

    XrString *str = xr_value_to_string(ctx->isolate, value);
    const char *raw = str ? str->data : "<?>";
    int raw_len = (int) strlen(raw);

    bool truncated = false;
    int show_len = raw_len;
    if (show_len > REPL_VARS_HARD_CAP) {
        show_len = REPL_VARS_HARD_CAP;
        truncated = true;
    }

    const char *kw = is_const ? "const" : "var  ";

    if (show_len <= REPL_VARS_INLINE_WIDTH && !truncated) {
        printf("  %s %s : %s = %.*s\n", kw, cname, type_name, show_len, raw);
    } else {
        printf("  %s %s : %s =\n", kw, cname, type_name);
        printf("        %.*s%s\n", show_len, raw, truncated ? " .." : "");
    }
    ctx->printed_count++;
}

static void print_vars_visitor(XrString *name, XrValue *value, void *ud) {
    ReplVarsCtx *ctx = (ReplVarsCtx *) ud;
    if (!ctx || !value || repl_result_is_internal(ctx->table, name))
        return;
    const char *cname = name ? name->data : "<anon>";
    print_vars_value(ctx, cname, *value, repl_symbol_is_const(ctx->table, name));
}

void xr_repl_print_vars(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_repl_print_vars: NULL isolate");
    if (!isolate || !isolate->vm.globals) {
        printf("  (no bindings)\n");
        return;
    }

    ReplVarsCtx ctx = {.isolate = isolate, .table = xr_repl_symbols_of(isolate)};
    xr_global_dict_iter(isolate->vm.globals, print_vars_visitor, &ctx);
    if (ctx.table && ctx.table->latest_result_name) {
        XrValue value = xr_global_dict_get(isolate->vm.globals, ctx.table->latest_result_name);
        print_vars_value(&ctx, REPL_IT_NAME, value, /*is_const=*/true);
    }
    if (ctx.printed_count == 0)
        printf("  (no bindings)\n");
}

void xr_repl_print_type(XrVMRuntime *isolate, const char *expr,
                        const XrModuleIdentityAuthority *authority) {
    XR_DCHECK(isolate != NULL, "xr_repl_print_type: NULL isolate");

    if (!expr)
        expr = "";
    while (*expr && isspace((unsigned char) *expr))
        expr++;

    if (*expr == '\0') {
        printf("Usage: .type <expression>\n");
        return;
    }

    /* Wrap the user expression in `print(typeName(...))` and route
     * through the normal incremental pipeline.  The added trailing
     * newline lets users include comments on the same line. */
    size_t expr_len = strlen(expr);
    size_t src_size = expr_len + 32;
    char *src = (char *) xr_malloc(src_size);
    if (!src)
        return;
    snprintf(src, src_size, "print(typeName(%s))\n", expr);

    XrCompilerSession *session = repl_compiler_session_for_isolate(isolate);
    XrReplEvalResult result = xr_repl_eval(session, isolate, src, authority);
    xr_free(src);

    if (!result.proto)
        return; /* compile error already reported */

    /* Proto leaked here intentionally: callers in CLI track protos and
     * free them on .reset / shutdown.  Freeing per-invocation would
     * also free any sub-protos still reachable via closures.  Pass-
     * through ownership keeps the model consistent with .load / .time. */
}
