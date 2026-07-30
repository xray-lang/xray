/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_suspend.c - Suspend effect inference
 *
 * The suspend-point set mirrors the task-212 body suspend effect
 * (XG_BODY_MAY_SUSPEND in src/analysis/xglobal_summary.c): await / yield /
 * select / scope-join, the blocking concurrency-handle methods, and time.sleep.
 * A dynamic call target or open virtual dispatch is treated as unproven. Strong
 * requirements are checked later by the versioned external contract verifier.
 */

#include "xanalyzer_suspend.h"
#include "../parser/xtype_ref.h"
#include "xa_native_effect.h"
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
    /* Why an INCOMPLETE conclusion is incomplete.  Defaults to a dynamic target;
     * a bodyless extern without an audited contract reports the native-contract
     * reason instead so the witness names the real gap. */
    XaUnknownReason direct_incomplete_reason;
    XaUnknownReason result_incomplete_reason;
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
    row->direct_incomplete_reason = XA_UNKNOWN_DYNAMIC_CALL_TARGET;
    row->result_incomplete_reason = XA_UNKNOWN_DYNAMIC_CALL_TARGET;
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

static void sus_scan_symbol_call(XaSuspendScan *scan, XaSymbol *callee, AstNode *site,
                                 bool is_callback) {
    if (!scan || !scan->row || !site)
        return;
    if (!callee) {
        sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, site,
                        is_callback ? "dynamic callback" : "dynamic call", NULL);
        return;
    }
    /* A function-valued parameter/local has no statically closed target. */
    if (callee->kind == XA_SYM_PARAMETER || callee->kind == XA_SYM_VARIABLE) {
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

static bool sus_call_is_mem_with_slice_mut(const CallExprNode *call) {
    if (!call || call->arg_count != 4 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "withSliceMut") == 0 && member->object &&
           member->object->type == AST_VARIABLE && member->object->as.variable.name &&
           strcmp(member->object->as.variable.name, "mem") == 0;
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
        if (sus_call_is_mem_with_slice_mut(call))
            sus_scan_callback(scan, call->arguments[3], node);
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

/* A bodyless extern "C" declaration has nothing to walk, so an empty scan must
 * never be read as a no-suspend proof.  The audited manifest contract is the
 * only admissible evidence (LANGUAGE_SPEC 5.2.11). */
static void sus_seed_bodyless_extern(XaSuspendPass *pass, XaSuspendRow *row) {
    XaNativeEffectAxioms axioms = xa_native_effect_axioms(pass->analyzer, row->symbol);
    const char *name = row->symbol && row->symbol->name ? row->symbol->name : "?";
    if (!axioms.has_contract) {
        row->direct_incomplete_reason = XA_UNKNOWN_NATIVE_CONTRACT_MISSING;
        sus_mark_direct(row, XA_SUSPEND_INCOMPLETE, row->node, "native contract missing", name);
    } else if (axioms.suspends) {
        sus_mark_direct(row, XA_SUSPEND_MAY, row->node, "native contract", name);
    }
    /* suspend = "never": the contract is the proof. */
}

static void sus_scan_function(XaSuspendPass *pass, XaSuspendRow *row) {
    if (!pass || !row || !row->node)
        return;
    AstNode *body = sus_function_body(row->node);
    if (!body) {
        if (xa_native_effect_is_bodyless_extern(row->symbol))
            sus_seed_bodyless_extern(pass, row);
        return;
    }
    XaSuspendScan scan = {.pass = pass, .row = row};
    xa_ast_walk(body, sus_scan_node_pre, sus_scan_node_post, &scan);
}

/* Combine the direct conclusion with the transitive edges once. */
static void sus_combine_row(XaSuspendPass *pass, XaSuspendRow *row, XaSuspendState *out_state,
                            XaSuspendCause *out_cause, XaUnknownReason *out_incomplete_reason) {
    XaSuspendState state = row->direct;
    XaSuspendCause cause = row->direct_cause;
    XaUnknownReason incomplete_reason = row->direct_incomplete_reason;
    for (int i = 0; i < row->edge_count; i++) {
        XaSuspendEdge *edge = &row->edges[i];
        XaSuspendRow *callee_row = sus_row_for_symbol(pass, edge->callee);
        XaSuspendState edge_state = callee_row ? callee_row->result : XA_SUSPEND_NONE;
        if (edge_state > state) {
            state = edge_state;
            if (edge_state == XA_SUSPEND_INCOMPLETE && callee_row)
                incomplete_reason = callee_row->result_incomplete_reason;
            sus_set_cause(&cause, edge->site, edge->is_callback ? "callback" : "call",
                          edge->callee && edge->callee->name ? edge->callee->name : "?");
        }
    }
    *out_state = state;
    *out_cause = cause;
    *out_incomplete_reason = incomplete_reason;
}

static void sus_publish_summaries(XaSuspendPass *pass) {
    if (!pass)
        return;
    for (;;) {
        bool changed = false;
        for (int i = 0; i < pass->row_count; i++) {
            XaSuspendState state;
            XaSuspendCause cause;
            XaUnknownReason incomplete_reason;
            sus_combine_row(pass, &pass->rows[i], &state, &cause, &incomplete_reason);
            if (state != pass->rows[i].result ||
                incomplete_reason != pass->rows[i].result_incomplete_reason) {
                pass->rows[i].result = state;
                pass->rows[i].result_cause = cause;
                pass->rows[i].result_incomplete_reason = incomplete_reason;
                changed = true;
            } else if (state != XA_SUSPEND_NONE) {
                pass->rows[i].result_cause = cause;
            }
        }
        if (!changed)
            break;
    }
    for (int i = 0; i < pass->row_count; i++) {
        XaSuspendRow *row = &pass->rows[i];
        if (!row->symbol)
            continue;
        XaEffectSummary summary;
        xa_effect_summary_init(&summary);
        const XaEffectSummary *current =
            xa_effect_db_get(pass->analyzer->effect_db, row->symbol->links.effect_id);
        bool ok =
            !current || xa_effect_summary_add_summary(pass->analyzer->effect_db, &summary, current);
        if (row->result == XA_SUSPEND_MAY)
            xa_effect_summary_add_semantic_effects(&summary, XA_SEM_EFFECT_SUSPEND);
        else if (row->result == XA_SUSPEND_INCOMPLETE)
            xa_effect_summary_mark_semantic_incomplete(&summary, XA_SEM_EFFECT_SUSPEND,
                                                       row->result_incomplete_reason);
        XaEffectId effect_id =
            ok ? xa_effect_db_intern(pass->analyzer->effect_db, &summary) : XA_EFFECT_NONE;
        xa_effect_summary_clear(&summary);
        if (effect_id != XA_EFFECT_NONE) {
            row->symbol->links.effect_id = effect_id;
            for (int slot = 0; slot < row->symbol->links.param_effect_count; slot++)
                row->symbol->links.param_effects[slot].callable_effects = effect_id;
            continue;
        }
        char message[256];
        snprintf(message, sizeof(message),
                 "analysis resource failure while publishing suspend effect for '%s'",
                 sus_function_name(row->node, row->symbol));
        XrLocation location = {.file = row->symbol->links.file_path,
                               .line = (uint32_t) row->node->line,
                               .column = (uint32_t) row->node->column};
        xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                                   &location);
    }
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

static void sus_check_call_constraints(XaSuspendPass *pass, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    if (sus_call_is_mem_with_slice_mut(call)) {
        const char *arg_name = "callback";
        XaSuspendState state = sus_argument_state(pass, call->arguments[3], &arg_name);
        if (state != XA_SUSPEND_NONE) {
            AstNode *arg = call->arguments[3];
            char message[512];
            if (state == XA_SUSPEND_MAY)
                snprintf(message, sizeof(message),
                         "mem.withSliceMut callback rejects argument '%s': it may suspend",
                         arg_name);
            else
                snprintf(message, sizeof(message),
                         "mem.withSliceMut callback rejects argument '%s': it cannot be proven "
                         "non-suspending",
                         arg_name);
            XrLocation location = {.file = pass->analyzer->current_file,
                                   .line = arg ? (uint32_t) arg->line : (uint32_t) node->line,
                                   .column = arg ? (uint32_t) arg->column : 0};
            xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                                       &location);
        }
        return;
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
    xa_ast_walk(ast, sus_constraint_scan_pre, NULL, &pass);
    for (int i = 0; i < pass.row_count; i++)
        xr_free(pass.rows[i].edges);
    xr_free(pass.rows);
}
