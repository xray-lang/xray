/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_suspend.c - Suspend effect inference and @no_suspend validation
 *
 * The suspend-point set mirrors the task-212 body suspend effect
 * (XG_BODY_MAY_SUSPEND in src/analysis/xglobal_summary.c): await / yield /
 * select / scope-join, the blocking concurrency-handle methods, and time.sleep.
 * @no_suspend (and the implicit @interrupt / @c_export boundaries) assert that
 * a function body reaches none of these transitively.  The pass is fail-closed:
 * a dynamic call target or open virtual dispatch is treated as unproven.
 */

#include "xanalyzer_suspend.h"
#include "../parser/xa_assertion_attr.h"
#include "../parser/xtype_ref.h"
#include "xa_selection.h"
#include "xanalyzer.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xerror_codes.h"
#include <stdio.h>
#include <string.h>

/* Suspend lattice; a stronger conclusion dominates a weaker one. */
typedef enum XaSuspendState {
    XA_SUSPEND_NONE = 0,       /* proven not to suspend */
    XA_SUSPEND_INCOMPLETE = 1, /* fail-closed: evidence incomplete (dynamic target) */
    XA_SUSPEND_MAY = 2,        /* proven to reach a suspend point */
} XaSuspendState;

typedef struct XaSuspendCause {
    AstNode *site;
    const char *kind;   /* "await" / "call" / "dynamic call" / ... */
    const char *detail; /* callee name / feature */
    uint32_t line;
    uint32_t column;
} XaSuspendCause;

typedef struct XaSuspendEdge {
    XaSymbol *callee;
    AstNode *site;
    bool is_callback;
} XaSuspendEdge;

typedef struct XaSuspendRow {
    AstNode *node;
    XaSymbol *symbol;
    XaSymbol synthetic_symbol;
    bool uses_synthetic_symbol;
    XaSuspendState direct;
    XaSuspendCause direct_cause;
    XaSuspendState result;
    XaSuspendCause result_cause;
    XaSuspendEdge *edges;
    int edge_count;
    int edge_capacity;
} XaSuspendRow;

typedef struct XaSuspendPass {
    XaAnalyzer *analyzer;
    XaSuspendRow *rows;
    int row_count;
    int row_capacity;
} XaSuspendPass;

typedef struct XaSuspendScan {
    XaSuspendPass *pass;
    XaSuspendRow *row;
    int nested_function_depth;
} XaSuspendScan;

static AstNode *sus_identity_expr(AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static XaSymbol *sus_symbol_by_id(XaAnalyzer *analyzer, uint32_t id) {
    return analyzer && id ? xa_scope_lookup_by_id(analyzer->global_scope, id) : NULL;
}

static XaSymbol *sus_variable_symbol(XaAnalyzer *analyzer, AstNode *expr) {
    expr = sus_identity_expr(expr);
    if (!analyzer || !expr || expr->type != AST_VARIABLE)
        return NULL;
    XaSymbol *symbol = sus_symbol_by_id(analyzer, expr->as.variable.symbol_id);
    if (symbol)
        return symbol;
    return expr->as.variable.name ? xa_analyzer_lookup_deep(analyzer, expr->as.variable.name)
                                  : NULL;
}

static XaSymbol *sus_resolve_function_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
    if (scope && scope->function_symbol)
        return scope->function_symbol;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)
        return sus_symbol_by_id(analyzer, node->as.function_decl.symbol_id);
    return NULL;
}

static AstNode *sus_function_body(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.body;
    if (node->type == AST_FUNCTION_EXPR)
        return node->as.function_expr.body;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.body;
    return NULL;
}

static const char *sus_function_name(AstNode *node, XaSymbol *symbol) {
    if (symbol && symbol->name)
        return symbol->name;
    if (!node)
        return "?";
    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.name)
        return node->as.function_decl.name;
    if (node->type == AST_FUNCTION_EXPR)
        return node->as.function_expr.name ? node->as.function_expr.name : "<anonymous>";
    if (node->type == AST_METHOD_DECL && node->as.method_decl.name)
        return node->as.method_decl.name;
    return "?";
}

/* The effective @no_suspend requirement and the label used in diagnostics.
 * Explicit @no_suspend and the implicit @interrupt / @c_export boundaries all
 * route through the same fail-closed check (task 217 §3.2). */
static const char *sus_required_label(AstNode *node) {
    if (xa_decl_has_attribute(node, ATTR_NO_SUSPEND))
        return "@no_suspend";
    if (xa_decl_has_attribute(node, ATTR_INTERRUPT))
        return "@interrupt";
    if (xa_decl_has_attribute(node, ATTR_C_EXPORT))
        return "@c_export";
    return NULL;
}

static bool sus_function_requires_no_suspend(AstNode *node) {
    return sus_required_label(node) != NULL;
}

/* Explicit @no_suspend is a user assertion and is fail-closed: an unprovable
 * (dynamic-target) body is rejected.  The implicit @interrupt / @c_export
 * boundaries only reject a *proven* suspend point, so pre-existing C-ABI
 * exports that call function values keep compiling. */
static bool sus_requirement_is_explicit(AstNode *node) {
    return xa_decl_has_attribute(node, ATTR_NO_SUSPEND);
}

static XaSuspendRow *sus_row_for_symbol(XaSuspendPass *pass, XaSymbol *symbol) {
    if (!pass || !symbol)
        return NULL;
    for (int i = 0; i < pass->row_count; i++) {
        if (pass->rows[i].symbol == symbol ||
            (symbol->id && pass->rows[i].symbol && pass->rows[i].symbol->id == symbol->id) ||
            (symbol->links.function_decl_node &&
             pass->rows[i].node == symbol->links.function_decl_node))
            return &pass->rows[i];
    }
    return NULL;
}

static XaSuspendRow *sus_row_for_node(XaSuspendPass *pass, AstNode *node) {
    if (!pass || !node)
        return NULL;
    for (int i = 0; i < pass->row_count; i++) {
        if (pass->rows[i].node == node)
            return &pass->rows[i];
    }
    return NULL;
}

static bool sus_append_row(XaSuspendPass *pass, AstNode *node, XaSymbol *symbol) {
    if (!pass || !node)
        return false;
    if ((symbol && sus_row_for_symbol(pass, symbol)) || sus_row_for_node(pass, node))
        return true;
    if (!symbol && node->type != AST_FUNCTION_EXPR)
        return false;
    if (pass->row_count >= pass->row_capacity) {
        int next_capacity = pass->row_capacity ? pass->row_capacity * 2 : 32;
        XaSuspendRow *next =
            (XaSuspendRow *) xr_realloc(pass->rows, (size_t) next_capacity * sizeof(XaSuspendRow));
        if (!next)
            return false;
        memset(&next[pass->row_capacity], 0,
               (size_t) (next_capacity - pass->row_capacity) * sizeof(XaSuspendRow));
        pass->rows = next;
        pass->row_capacity = next_capacity;
        for (int i = 0; i < pass->row_count; i++) {
            if (pass->rows[i].uses_synthetic_symbol)
                pass->rows[i].symbol = &pass->rows[i].synthetic_symbol;
        }
    }
    XaSuspendRow *row = &pass->rows[pass->row_count++];
    memset(row, 0, sizeof(*row));
    row->node = node;
    if (symbol) {
        row->symbol = symbol;
    } else {
        row->synthetic_symbol.name = "<anonymous>";
        row->synthetic_symbol.kind = XA_SYM_FUNCTION;
        row->synthetic_symbol.links.function_decl_node = node;
        row->symbol = &row->synthetic_symbol;
        row->uses_synthetic_symbol = true;
    }
    row->direct = XA_SUSPEND_NONE;
    return true;
}

static void sus_collect_node_pre(AstNode *node, void *userdata) {
    XaSuspendPass *pass = (XaSuspendPass *) userdata;
    if (!pass || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL)
        sus_append_row(pass, node, sus_resolve_function_symbol(pass->analyzer, node));
}

static void sus_collect_functions(XaSuspendPass *pass, AstNode *node) {
    if (!pass || !node)
        return;
    xa_ast_walk(node, sus_collect_node_pre, NULL, pass);
    for (int i = 0; i < pass->row_count; i++) {
        if (pass->rows[i].uses_synthetic_symbol)
            pass->rows[i].symbol = &pass->rows[i].synthetic_symbol;
    }
}

static void sus_set_cause(XaSuspendCause *cause, AstNode *site, const char *kind,
                          const char *detail) {
    cause->site = site;
    cause->kind = kind;
    cause->detail = detail;
    cause->line = site ? (uint32_t) site->line : 0;
    cause->column = site ? (uint32_t) site->column : 0;
}

/* Record a direct conclusion for the scanned body.  MAY dominates INCOMPLETE. */
static void sus_mark_direct(XaSuspendRow *row, XaSuspendState state, AstNode *site,
                            const char *kind, const char *detail) {
    if (!row || !site || state <= row->direct)
        return;
    row->direct = state;
    sus_set_cause(&row->direct_cause, site, kind, detail);
}

static bool sus_add_edge(XaSuspendRow *row, XaSymbol *callee, AstNode *site, bool is_callback) {
    if (!row || !callee || !site)
        return false;
    for (int i = 0; i < row->edge_count; i++) {
        if (row->edges[i].callee == callee && row->edges[i].site == site &&
            row->edges[i].is_callback == is_callback)
            return true;
    }
    if (row->edge_count >= row->edge_capacity) {
        int next_capacity = row->edge_capacity ? row->edge_capacity * 2 : 8;
        XaSuspendEdge *next = (XaSuspendEdge *) xr_realloc(row->edges, (size_t) next_capacity *
                                                                           sizeof(XaSuspendEdge));
        if (!next)
            return false;
        row->edges = next;
        row->edge_capacity = next_capacity;
    }
    row->edges[row->edge_count++] =
        (XaSuspendEdge) {.callee = callee, .site = site, .is_callback = is_callback};
    return true;
}

/* Blocking concurrency-handle methods that park the current coroutine.  Kept in
 * sync with body_builtin_method_call_may_suspend / body_stdlib_call_may_suspend
 * in src/analysis/xglobal_producer.c. */
static bool sus_is_suspending_handle_method(const XrType *receiver, const char *method) {
    if (!receiver || !method)
        return false;
    if (receiver->kind == XR_KIND_CHANNEL)
        return strcmp(method, "send") == 0 || strcmp(method, "sendTimeout") == 0 ||
               strcmp(method, "recv") == 0 || strcmp(method, "recvOr") == 0 ||
               strcmp(method, "recvTimeout") == 0;
    const char *cls = receiver->kind == XR_KIND_INSTANCE ? receiver->instance.class_name : NULL;
    if (!cls)
        return false;
    if (strcmp(cls, "Task") == 0)
        return strcmp(method, "awaitResult") == 0 || strcmp(method, "awaitTimeout") == 0;
    if (strcmp(cls, "WorkQueue") == 0)
        return strcmp(method, "pop") == 0;
    if (strcmp(cls, "ResultGroup") == 0)
        return strcmp(method, "recv") == 0;
    if (strcmp(cls, "CountdownLatch") == 0)
        return strcmp(method, "wait") == 0;
    if (strcmp(cls, "Semaphore") == 0)
        return strcmp(method, "acquire") == 0;
    if (strcmp(cls, "EventCount") == 0)
        return strcmp(method, "wait") == 0;
    return false;
}

static bool sus_symbol_is_time_sleep(XaSymbol *symbol, const char *member_name) {
    if (!symbol || !member_name || strcmp(member_name, "sleep") != 0)
        return false;
    const char *module = symbol->links.module_name;
    return module && strcmp(module, "time") == 0;
}

static bool sus_symbol_has_body(XaSymbol *symbol) {
    return symbol && symbol->links.function_decl_node &&
           sus_function_body(symbol->links.function_decl_node) != NULL;
}

static bool sus_decl_params(AstNode *node, XrParamNode ***out_params, int *out_count) {
    if (!node)
        return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        *out_params = node->as.function_decl.params;
        *out_count = node->as.function_decl.param_count;
        return true;
    }
    if (node->type == AST_METHOD_DECL) {
        *out_params = node->as.method_decl.params;
        *out_count = node->as.method_decl.param_count;
        return true;
    }
    return false;
}

/* A parameter whose annotation is `@no_suspend (...) -> R`. */
static bool sus_param_type_is_no_suspend(const XrParamNode *param) {
    return param && param->type && param->type->kind == XR_TREF_FUNCTION && param->type->no_suspend;
}

/* True when |param| is a @no_suspend-constrained callback of |fn_node|; calling
 * it is therefore proven non-suspending by its type constraint. */
static bool sus_symbol_is_constrained_param(AstNode *fn_node, const XaSymbol *param) {
    XrParamNode **params = NULL;
    int count = 0;
    if (!param || !sus_decl_params(fn_node, &params, &count))
        return false;
    for (int i = 0; i < count; i++) {
        if (!params[i])
            continue;
        bool match = (params[i]->symbol_id && params[i]->symbol_id == param->id) ||
                     (params[i]->name && param->name && strcmp(params[i]->name, param->name) == 0);
        if (match)
            return sus_param_type_is_no_suspend(params[i]);
    }
    return false;
}

static void sus_scan_symbol_call(XaSuspendScan *scan, XaSymbol *callee, AstNode *site,
                                 bool is_callback) {
    if (!scan || !scan->row || !site)
        return;
    if (!callee) {
        sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, site,
                        is_callback ? "dynamic callback" : "dynamic call", NULL);
        return;
    }
    /* A function-valued parameter/local has no statically closed target,
     * unless the parameter carries a `@no_suspend` callback constraint, which
     * proves the call cannot suspend (task 217 §3.2). */
    if (callee->kind == XA_SYM_PARAMETER || callee->kind == XA_SYM_VARIABLE) {
        if (callee->kind == XA_SYM_PARAMETER && scan->row &&
            sus_symbol_is_constrained_param(scan->row->node, callee))
            return;
        sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, site,
                        is_callback ? "dynamic callback" : "dynamic call", callee->name);
        return;
    }
    if (sus_row_for_symbol(scan->pass, callee) || sus_symbol_has_body(callee)) {
        sus_add_edge(scan->row, callee, site, is_callback);
        return;
    }
    /* Resolved bodyless native/builtin: non-suspending unless it is in the
     * blocking set, which is handled by the member-call path above. */
}

static void sus_scan_callback(XaSuspendScan *scan, AstNode *callback, AstNode *site) {
    callback = sus_identity_expr(callback);
    if (!callback || callback->type == AST_LITERAL_NULL)
        return;
    if (callback->type == AST_FUNCTION_EXPR) {
        XaSuspendRow *cb_row = sus_row_for_node(scan->pass, callback);
        XaSymbol *symbol =
            cb_row ? cb_row->symbol : sus_resolve_function_symbol(scan->pass->analyzer, callback);
        if (symbol)
            sus_add_edge(scan->row, symbol, site, true);
        return;
    }
    if (callback->type == AST_VARIABLE) {
        sus_scan_symbol_call(scan, sus_variable_symbol(scan->pass->analyzer, callback), site, true);
        return;
    }
    if (callback->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection = xa_selection_table_get(
            (XaSelectionTable *) scan->pass->analyzer->selection_table, callback);
        sus_scan_symbol_call(scan, selection ? selection->target_symbol : NULL, site, true);
        return;
    }
    sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, site, "dynamic callback", NULL);
}

static int sus_hof_callback_index(const XrType *receiver, const char *method) {
    if (!receiver || !method)
        return -1;
    if (receiver->kind == XR_KIND_ARRAY &&
        (strcmp(method, "map") == 0 || strcmp(method, "filter") == 0 ||
         strcmp(method, "reduce") == 0 || strcmp(method, "forEach") == 0 ||
         strcmp(method, "find") == 0 || strcmp(method, "findIndex") == 0 ||
         strcmp(method, "every") == 0 || strcmp(method, "some") == 0 ||
         strcmp(method, "sort") == 0))
        return 0;
    if ((receiver->kind == XR_KIND_MAP || receiver->kind == XR_KIND_SET) &&
        strcmp(method, "forEach") == 0)
        return 0;
    return -1;
}

static void sus_scan_call(XaSuspendScan *scan, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    AstNode *callee = sus_identity_expr(call->callee);
    if (!callee)
        return;
    if (callee->type == AST_FUNCTION_EXPR) {
        XaSuspendRow *callee_row = sus_row_for_node(scan->pass, callee);
        XaSymbol *symbol = callee_row ? callee_row->symbol
                                      : sus_resolve_function_symbol(scan->pass->analyzer, callee);
        if (symbol)
            sus_add_edge(scan->row, symbol, node, false);
        return;
    }
    if (callee->type == AST_VARIABLE) {
        sus_scan_symbol_call(scan, sus_variable_symbol(scan->pass->analyzer, callee), node, false);
        return;
    }
    if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        XrType *receiver = xa_analyzer_get_node_type(scan->pass->analyzer, member->object);
        const XaSelection *selection = xa_selection_table_get(
            (XaSelectionTable *) scan->pass->analyzer->selection_table, callee);
        XaSymbol *target = selection ? selection->target_symbol : NULL;

        if (sus_is_suspending_handle_method(receiver, member->name)) {
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "handle method", member->name);
        } else if (target && sus_symbol_is_time_sleep(target, member->name)) {
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "call", "time.sleep");
        } else if (target &&
                   (sus_row_for_symbol(scan->pass, target) || sus_symbol_has_body(target))) {
            /* Open virtual dispatch cannot close the concrete target. */
            bool open_dispatch = receiver && receiver->kind == XR_KIND_INSTANCE && target->parent &&
                                 target->parent->links.class_info &&
                                 !target->parent->links.class_info->explicit_final &&
                                 target->parent->links.class_info->struct_layout == NULL;
            if (open_dispatch)
                sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, node, "open dispatch",
                                member->name);
            else
                sus_add_edge(scan->row, target, node, false);
        } else if (target &&
                   (target->kind == XA_SYM_PARAMETER || target->kind == XA_SYM_VARIABLE)) {
            sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", member->name);
        }
        /* Otherwise: builtin member / resolved native — non-suspending. */

        int callback_index = sus_hof_callback_index(receiver, member->name);
        if (callback_index >= 0 && callback_index < call->arg_count)
            sus_scan_callback(scan, call->arguments[callback_index], node);
        return;
    }
    sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", NULL);
}

static void sus_scan_node_pre(AstNode *node, void *userdata) {
    XaSuspendScan *scan = (XaSuspendScan *) userdata;
    if (!scan || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        scan->nested_function_depth++;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;
    switch (node->type) {
        case AST_AWAIT_EXPR:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "await", "await");
            break;
        case AST_YIELD_STMT:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "yield", "yield");
            break;
        case AST_SELECT_STMT:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "select", "select");
            break;
        case AST_SCOPE_BLOCK:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "scope", "scope join");
            break;
        case AST_CALL_EXPR:
            sus_scan_call(scan, node);
            break;
        default:
            break;
    }
}

static void sus_scan_node_post(AstNode *node, void *userdata) {
    XaSuspendScan *scan = (XaSuspendScan *) userdata;
    if (!scan || !node)
        return;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL) &&
        scan->nested_function_depth > 0)
        scan->nested_function_depth--;
}

static void sus_scan_function(XaSuspendPass *pass, XaSuspendRow *row) {
    AstNode *body = sus_function_body(row ? row->node : NULL);
    if (!pass || !row || !body)
        return;
    XaSuspendScan scan = {.pass = pass, .row = row};
    xa_ast_walk(body, sus_scan_node_pre, sus_scan_node_post, &scan);
}

/* Combine the direct conclusion with the transitive edges once. */
static void sus_combine_row(XaSuspendPass *pass, XaSuspendRow *row, XaSuspendState *out_state,
                            XaSuspendCause *out_cause) {
    XaSuspendState state = row->direct;
    XaSuspendCause cause = row->direct_cause;
    for (int i = 0; i < row->edge_count; i++) {
        XaSuspendEdge *edge = &row->edges[i];
        XaSuspendRow *callee_row = sus_row_for_symbol(pass, edge->callee);
        XaSuspendState edge_state = callee_row ? callee_row->result : XA_SUSPEND_NONE;
        if (edge_state > state) {
            state = edge_state;
            sus_set_cause(&cause, edge->site, edge->is_callback ? "callback" : "call",
                          edge->callee && edge->callee->name ? edge->callee->name : "?");
        }
    }
    *out_state = state;
    *out_cause = cause;
}

static void sus_publish_summaries(XaSuspendPass *pass) {
    if (!pass)
        return;
    for (;;) {
        bool changed = false;
        for (int i = 0; i < pass->row_count; i++) {
            XaSuspendState state;
            XaSuspendCause cause;
            sus_combine_row(pass, &pass->rows[i], &state, &cause);
            if (state != pass->rows[i].result) {
                pass->rows[i].result = state;
                pass->rows[i].result_cause = cause;
                changed = true;
            } else if (state != XA_SUSPEND_NONE) {
                pass->rows[i].result_cause = cause;
            }
        }
        if (!changed)
            break;
    }
}

static void sus_validate_contract(XaSuspendPass *pass, XaSuspendRow *row) {
    if (!pass || !row || !sus_function_requires_no_suspend(row->node) ||
        row->result == XA_SUSPEND_NONE)
        return;
    /* Implicit boundaries tolerate incomplete evidence; only explicit
     * @no_suspend is fail-closed on unprovable dynamic targets. */
    if (row->result == XA_SUSPEND_INCOMPLETE && !sus_requirement_is_explicit(row->node))
        return;
    const char *label = sus_required_label(row->node);
    const char *name = sus_function_name(row->node, row->symbol);
    const XaSuspendCause *cause = &row->result_cause;
    char message[768];
    if (row->result == XA_SUSPEND_MAY) {
        snprintf(message, sizeof(message),
                 "%s contract is not satisfied for '%s': may suspend via %s '%s' at line %u", label,
                 name, cause->kind ? cause->kind : "operation", cause->detail ? cause->detail : "?",
                 cause->line);
    } else {
        snprintf(message, sizeof(message),
                 "%s contract cannot be proven for '%s': %s '%s' has no statically known "
                 "suspend evidence at line %u",
                 label, name, cause->kind ? cause->kind : "call target",
                 cause->detail ? cause->detail : "?", cause->line);
    }
    XrLocation location = {.file = row->symbol ? row->symbol->links.file_path : NULL,
                           .line = cause->line ? cause->line : (uint32_t) row->node->line,
                           .column = cause->column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                               &location);
}

/* Suspend conclusion for a function-valued call argument. */
static XaSuspendState sus_argument_state(XaSuspendPass *pass, AstNode *expr,
                                         const char **out_name) {
    expr = sus_identity_expr(expr);
    if (out_name)
        *out_name = "callback";
    if (!expr)
        return XA_SUSPEND_INCOMPLETE;
    if (expr->type == AST_LITERAL_NULL)
        return XA_SUSPEND_NONE;
    if (expr->type == AST_FUNCTION_EXPR) {
        XaSuspendRow *row = sus_row_for_node(pass, expr);
        return row ? row->result : XA_SUSPEND_INCOMPLETE;
    }
    XaSymbol *symbol = NULL;
    if (expr->type == AST_VARIABLE) {
        symbol = sus_variable_symbol(pass->analyzer, expr);
    } else if (expr->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection =
            xa_selection_table_get((XaSelectionTable *) pass->analyzer->selection_table, expr);
        symbol = selection ? selection->target_symbol : NULL;
    }
    if (!symbol)
        return XA_SUSPEND_INCOMPLETE;
    if (out_name && symbol->name)
        *out_name = symbol->name;
    if (symbol->kind == XA_SYM_PARAMETER || symbol->kind == XA_SYM_VARIABLE)
        return XA_SUSPEND_INCOMPLETE;
    XaSuspendRow *row = sus_row_for_symbol(pass, symbol);
    if (row)
        return row->result;
    /* A resolved bodyless native function reference does not suspend. */
    return sus_symbol_has_body(symbol) ? XA_SUSPEND_INCOMPLETE : XA_SUSPEND_NONE;
}

static AstNode *sus_call_target_decl(XaSuspendPass *pass, AstNode *callee) {
    callee = sus_identity_expr(callee);
    if (!callee)
        return NULL;
    if (callee->type == AST_VARIABLE) {
        XaSymbol *symbol = sus_variable_symbol(pass->analyzer, callee);
        return symbol ? symbol->links.function_decl_node : NULL;
    }
    if (callee->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection =
            xa_selection_table_get((XaSelectionTable *) pass->analyzer->selection_table, callee);
        return selection && selection->target_symbol
                   ? selection->target_symbol->links.function_decl_node
                   : NULL;
    }
    return NULL;
}

/* Enforce `@no_suspend (...) -> R` callback parameters at a call site: the
 * function passed for such a parameter must be proven non-suspending. */
static void sus_check_call_constraints(XaSuspendPass *pass, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    AstNode *fn_decl = sus_call_target_decl(pass, call->callee);
    XrParamNode **params = NULL;
    int param_count = 0;
    if (!fn_decl || !sus_decl_params(fn_decl, &params, &param_count))
        return;
    int limit = call->arg_count < param_count ? call->arg_count : param_count;
    for (int i = 0; i < limit; i++) {
        if (!sus_param_type_is_no_suspend(params[i]))
            continue;
        const char *arg_name = "callback";
        XaSuspendState state = sus_argument_state(pass, call->arguments[i], &arg_name);
        if (state == XA_SUSPEND_NONE)
            continue;
        const char *pname = params[i]->name ? params[i]->name : "callback";
        AstNode *arg = call->arguments[i];
        char message[512];
        if (state == XA_SUSPEND_MAY)
            snprintf(message, sizeof(message),
                     "@no_suspend callback parameter '%s' rejects argument '%s': it may suspend",
                     pname, arg_name);
        else
            snprintf(message, sizeof(message),
                     "@no_suspend callback parameter '%s' rejects argument '%s': it cannot be "
                     "proven non-suspending",
                     pname, arg_name);
        XrLocation location = {.file = pass->analyzer->current_file,
                               .line = arg ? (uint32_t) arg->line : (uint32_t) node->line,
                               .column = arg ? (uint32_t) arg->column : 0};
        xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                                   &location);
    }
}

static void sus_constraint_scan_pre(AstNode *node, void *userdata) {
    XaSuspendPass *pass = (XaSuspendPass *) userdata;
    if (pass && node && node->type == AST_CALL_EXPR)
        sus_check_call_constraints(pass, node);
}

void xa_verify_no_suspend(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast)
        return;
    XaSuspendPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.analyzer = analyzer;
    sus_collect_functions(&pass, ast);
    for (int i = 0; i < pass.row_count; i++)
        sus_scan_function(&pass, &pass.rows[i]);
    sus_publish_summaries(&pass);
    for (int i = 0; i < pass.row_count; i++)
        sus_validate_contract(&pass, &pass.rows[i]);
    xa_ast_walk(ast, sus_constraint_scan_pre, NULL, &pass);
    for (int i = 0; i < pass.row_count; i++)
        xr_free(pass.rows[i].edges);
    xr_free(pass.rows);
}
