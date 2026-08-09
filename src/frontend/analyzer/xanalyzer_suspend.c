/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_suspend.c - Suspend effect inference
 *
 * Two independent dimensions, because `Coro.yield()` and `yield expr` stop a
 * body in ways that differ in every property a caller can act on:
 *
 *   Scheduler suspension (XA_SEM_EFFECT_SCHED_SUSPEND) -- await / select /
 *   scope-join / Coro.yield() / the blocking concurrency-handle methods /
 *   time.sleep / an audited native contract.  Control reaches the scheduler, so
 *   the coroutine may resume on another OS thread and observes cancellation.
 *   This is caller-visible and composes transitively across call edges; a
 *   dynamic call target or open virtual dispatch leaves it unproven.
 *
 *   Generator suspension (XA_SEM_EFFECT_GEN_SUSPEND) -- the body lexically
 *   contains `yield expr`.  Its frame must survive a symmetric transfer to the
 *   iterator driving it, but the scheduler is never involved.  It is a purely
 *   local, always-decidable fact: it never propagates across a call edge and is
 *   never incomplete, because driving a generator resumes the generator's frame
 *   and returns normally, leaving the caller's own frame untouched.
 *
 * Strong requirements are checked later by the versioned external contract
 * verifier: `no_reschedule` forbids the first dimension, `no_suspend` forbids
 * both.
 */

#include "xanalyzer_suspend.h"
#include "../parser/xtype_ref.h"
#include "xa_native_effect.h"
#include "xa_selection.h"
#include "xanalyzer.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_symbol.h"
#include "xanalyzer_visitor_internal.h"
#include "../parser/xast_nodes.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xerror_codes.h"
#include <stdio.h>
#include <string.h>

/* Scheduler-suspend lattice; a stronger conclusion dominates a weaker one. */
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
    /* The call sits inside a `defer` body, so the callee's suspend result
     * decides whether that cleanup can run to completion. */
    bool in_cleanup_body;
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
    XaSuspendState spawn_direct;
    XaSuspendCause spawn_direct_cause;
    XaUnknownReason spawn_direct_incomplete_reason;
    XaSuspendState spawn_result;
    XaSuspendCause spawn_result_cause;
    XaUnknownReason spawn_result_incomplete_reason;
    /* Generator suspension is lexical and local: it needs no lattice, no edge
     * propagation, and has no incomplete state. */
    bool has_generator_yield;
    /* First `defer` lexically in this body, for the generator-defer rule. */
    AstNode *defer_site;
    /* Depth of `defer` bodies the scan is currently inside, and the first
     * suspension point found in one. A defer body runs as a nested VM call on
     * a frame that is already unwinding, so it cannot park and resume. */
    int cleanup_body_depth;
    XaSuspendCause cleanup_suspend_cause;
    bool has_cleanup_suspend;
    XaSuspendCause cleanup_spawn_cause;
    bool has_cleanup_spawn;
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
    const char *cleanup_loop_labels[64];
    int cleanup_loop_count;
    int cleanup_loop_bases[64];
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
    row->spawn_direct = XA_SUSPEND_NONE;
    row->spawn_direct_incomplete_reason = XA_UNKNOWN_DYNAMIC_CALL_TARGET;
    row->spawn_result_incomplete_reason = XA_UNKNOWN_DYNAMIC_CALL_TARGET;
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
    if (!row || !site)
        return;
    if (row->cleanup_body_depth > 0 && !row->has_cleanup_suspend) {
        sus_set_cause(&row->cleanup_suspend_cause, site, kind, detail);
        row->has_cleanup_suspend = true;
    }
    if (state <= row->direct)
        return;
    row->direct = state;
    sus_set_cause(&row->direct_cause, site, kind, detail);
}

static void sus_mark_spawn(XaSuspendRow *row, XaSuspendState state, AstNode *site, const char *kind,
                           const char *detail) {
    if (!row || !site)
        return;
    if (row->cleanup_body_depth > 0 && state == XA_SUSPEND_MAY && !row->has_cleanup_spawn) {
        sus_set_cause(&row->cleanup_spawn_cause, site, kind, detail);
        row->has_cleanup_spawn = true;
    }
    if (state <= row->spawn_direct)
        return;
    row->spawn_direct = state;
    sus_set_cause(&row->spawn_direct_cause, site, kind, detail);
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
        (XaSuspendEdge) {.callee = callee,
                         .site = site,
                         .is_callback = is_callback,
                         .in_cleanup_body = row->cleanup_body_depth > 0};
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

/* `Coro.yield()` is a scheduler suspension point, but the receiver is a
 * VM-intrinsic module rather than a resolved function symbol, so the ordinary
 * call-target path below sees nothing and would conclude "non-suspending". */
static bool sus_call_is_coro_yield(XaSuspendPass *pass, const MemberAccessNode *member) {
    if (!member || !member->name || strcmp(member->name, "yield") != 0)
        return false;
    return xa_symbol_is_builtin_module(pass->analyzer,
                                       sus_variable_symbol(pass->analyzer, member->object), "Coro");
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
        sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, site,
                       is_callback ? "dynamic callback" : "dynamic call", NULL);
        return;
    }
    /* A function-valued parameter/local has no statically closed target. */
    if (callee->kind == XA_SYM_PARAMETER || callee->kind == XA_SYM_VARIABLE) {
        sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, site,
                        is_callback ? "dynamic callback" : "dynamic call", callee->name);
        sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, site,
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
    sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, site, "dynamic callback", NULL);
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

static bool sus_call_is_mem_with_slice_mut(XaSuspendPass *pass, const CallExprNode *call) {
    if (!call || call->arg_count != 4 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    return member->name && strcmp(member->name, "withSliceMut") == 0 &&
           xa_symbol_is_module(pass->analyzer, sus_variable_symbol(pass->analyzer, member->object),
                               "mem");
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

        if (sus_call_is_coro_yield(scan->pass, member)) {
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "call", "Coro.yield()");
        } else if (sus_is_suspending_handle_method(receiver, member->name)) {
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
            if (open_dispatch)
                sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, node, "open dispatch",
                               member->name);
            else
                sus_add_edge(scan->row, target, node, false);
        } else if (target &&
                   (target->kind == XA_SYM_PARAMETER || target->kind == XA_SYM_VARIABLE)) {
            sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", member->name);
            sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", member->name);
        }
        /* Otherwise: builtin member / resolved native — non-suspending. */

        int callback_index = sus_hof_callback_index(receiver, member->name);
        if (callback_index >= 0 && callback_index < call->arg_count)
            sus_scan_callback(scan, call->arguments[callback_index], node);
        if (sus_call_is_mem_with_slice_mut(scan->pass, call))
            sus_scan_callback(scan, call->arguments[3], node);
        return;
    }
    sus_mark_direct(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", NULL);
    sus_mark_spawn(scan->row, XA_SUSPEND_INCOMPLETE, node, "dynamic call", NULL);
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
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "await", NULL);
            break;
        case AST_GO_EXPR:
            sus_mark_spawn(scan->row, XA_SUSPEND_MAY, node, "task spawn", "go");
            break;
        case AST_YIELD_STMT:
            /* Generator value production: the driver resumes this frame, the
             * scheduler is not involved.  Deliberately does not mark the
             * scheduler dimension. */
            scan->row->has_generator_yield = true;
            break;
        case AST_DEFER_STMT: {
            /* Recorded unconditionally; only a body that also yields is
             * rejected, and the walk may reach the defer before the yield. */
            if (!scan->row->defer_site)
                scan->row->defer_site = node;
            int depth = scan->row->cleanup_body_depth;
            if (depth <
                (int) (sizeof(scan->cleanup_loop_bases) / sizeof(scan->cleanup_loop_bases[0])))
                scan->cleanup_loop_bases[depth] = scan->cleanup_loop_count;
            scan->row->cleanup_body_depth++;
            break;
        }
        case AST_WHILE_STMT:
        case AST_FOR_STMT:
        case AST_FOR_IN_STMT:
            if (scan->row->cleanup_body_depth > 0 &&
                scan->cleanup_loop_count < (int) (sizeof(scan->cleanup_loop_labels) /
                                                  sizeof(scan->cleanup_loop_labels[0]))) {
                const char *label = node->type == AST_WHILE_STMT ? node->as.while_stmt.label
                                    : node->type == AST_FOR_STMT ? node->as.for_stmt.label
                                                                 : node->as.for_in_stmt.label;
                scan->cleanup_loop_labels[scan->cleanup_loop_count++] = label;
            }
            break;
        case AST_RETURN_STMT: {
            if (scan->row->cleanup_body_depth <= 0)
                break;
            XrLocation location = {.file = scan->row->symbol ? scan->row->symbol->links.file_path
                                                             : NULL,
                                   .line = (uint32_t) node->line,
                                   .column = (uint32_t) node->column};
            xa_analyzer_add_diagnostic(
                scan->pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_DEFER_CONTROL,
                "a defer body cannot return from its owning function", &location);
            break;
        }
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT: {
            if (scan->row->cleanup_body_depth <= 0)
                break;
            int depth = scan->row->cleanup_body_depth - 1;
            int base = depth < (int) (sizeof(scan->cleanup_loop_bases) /
                                      sizeof(scan->cleanup_loop_bases[0]))
                           ? scan->cleanup_loop_bases[depth]
                           : scan->cleanup_loop_count;
            const char *label = node->type == AST_BREAK_STMT ? node->as.break_stmt.label
                                                             : node->as.continue_stmt.label;
            bool local_target = !label ? scan->cleanup_loop_count > base : false;
            for (int i = scan->cleanup_loop_count - 1; label && i >= base; i--) {
                if (scan->cleanup_loop_labels[i] &&
                    strcmp(scan->cleanup_loop_labels[i], label) == 0) {
                    local_target = true;
                    break;
                }
            }
            if (local_target)
                break;
            XrLocation location = {.file = scan->row->symbol ? scan->row->symbol->links.file_path
                                                             : NULL,
                                   .line = (uint32_t) node->line,
                                   .column = (uint32_t) node->column};
            xa_analyzer_add_diagnostic(
                scan->pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_DEFER_CONTROL,
                node->type == AST_BREAK_STMT
                    ? "a defer body cannot break to a loop outside the cleanup"
                    : "a defer body cannot continue a loop outside the cleanup",
                &location);
            break;
        }
        case AST_SELECT_STMT:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "select", NULL);
            break;
        case AST_SCOPE_BLOCK:
            sus_mark_direct(scan->row, XA_SUSPEND_MAY, node, "scope join", NULL);
            break;
        case AST_CALL_EXPR:
            if (xa_expr_is_sys_thread_spawn_call(node))
                sus_mark_spawn(scan->row, XA_SUSPEND_MAY, node, "task spawn", "sys.Thread.spawn");
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
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        if (scan->nested_function_depth > 0)
            scan->nested_function_depth--;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;
    if ((node->type == AST_WHILE_STMT || node->type == AST_FOR_STMT ||
         node->type == AST_FOR_IN_STMT) &&
        scan->row->cleanup_body_depth > 0 && scan->cleanup_loop_count > 0)
        scan->cleanup_loop_count--;
    if (node->type == AST_DEFER_STMT && scan->row && scan->row->cleanup_body_depth > 0)
        scan->row->cleanup_body_depth--;
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

static void sus_combine_spawn_row(XaSuspendPass *pass, XaSuspendRow *row, XaSuspendState *out_state,
                                  XaSuspendCause *out_cause,
                                  XaUnknownReason *out_incomplete_reason) {
    XaSuspendState state = row->spawn_direct;
    XaSuspendCause cause = row->spawn_direct_cause;
    XaUnknownReason incomplete_reason = row->spawn_direct_incomplete_reason;
    for (int i = 0; i < row->edge_count; i++) {
        XaSuspendEdge *edge = &row->edges[i];
        XaSuspendRow *callee_row = sus_row_for_symbol(pass, edge->callee);
        XaSuspendState edge_state = callee_row ? callee_row->spawn_result : XA_SUSPEND_NONE;
        if (edge_state > state) {
            state = edge_state;
            if (edge_state == XA_SUSPEND_INCOMPLETE && callee_row)
                incomplete_reason = callee_row->spawn_result_incomplete_reason;
            sus_set_cause(&cause, edge->site, edge->is_callback ? "callback" : "call",
                          edge->callee && edge->callee->name ? edge->callee->name : "?");
        }
    }
    *out_state = state;
    *out_cause = cause;
    *out_incomplete_reason = incomplete_reason;
}

/* A generator frame is resumed by whoever drives it, never by the scheduler.
 * The lowering has no way to reconcile the two: a body that reaches a scheduler
 * suspension point between yields does not park and resume, it silently drops
 * the rest of its value stream (`await`/`scope` swallow every remaining yield;
 * `Coro.yield()` injects a spurious null element).  Both backends agree on the
 * wrong answer, so a differential test cannot catch it either.  Reject it in the
 * front end instead, and fail closed on unproven evidence exactly as the
 * `sys.Thread.spawn` and parallel-callback boundaries do. */
/* Render one cause as a single feature name: "`await`", "call to `time.sleep`",
 * "handle method `recv`", "dynamic call". */
static void sus_format_cause(const XaSuspendCause *cause, char *buf, size_t size) {
    const char *kind = cause->kind ? cause->kind : "suspension point";
    if (!cause->detail) {
        snprintf(buf, size, "`%s`", kind);
        return;
    }
    bool indirect = strcmp(kind, "call") == 0 || strcmp(kind, "callback") == 0 ||
                    strcmp(kind, "dynamic call") == 0 || strcmp(kind, "dynamic callback") == 0;
    snprintf(buf, size, "%s %s`%s`", kind, indirect ? "to " : "", cause->detail);
}

/* `defer` is the language's only deterministic cleanup mechanism (LANGUAGE_SPEC
 * 16.8), and a generator abandoned before exhaustion is never resumed, so its
 * deferred actions never run -- `for (x in gen()) { break }` silently skips
 * them, in both backends.  A cleanup that may never run is worse than no
 * cleanup, so the combination is rejected rather than documented.  The rule
 * covers the whole body deliberately: whether `defer` binds to the nearest
 * block or to the function is exactly the question the spec answers two ways,
 * and only the whole-body rule is correct under both readings. */
static void sus_reject_generator_defer(XaSuspendPass *pass, const XaSuspendRow *row) {
    if (!row->has_generator_yield || !row->defer_site)
        return;
    char message[320];
    snprintf(message, sizeof(message),
             "generator '%s' cannot register a deferred action; a generator abandoned before it is "
             "exhausted is never resumed, so the `defer` would not run\n"
             "hint: own the resource in the caller and pass it in, or drain the generator into a "
             "collection inside a function that owns the cleanup",
             sus_function_name(row->node, row->symbol));
    XrLocation location = {.file = row->symbol ? row->symbol->links.file_path : NULL,
                           .line = (uint32_t) row->defer_site->line,
                           .column = (uint32_t) row->defer_site->column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERATOR_DEFER,
                               message, &location);
}

static void sus_reject_suspending_generator(XaSuspendPass *pass, const XaSuspendRow *row) {
    if (!row->has_generator_yield || row->result == XA_SUSPEND_NONE)
        return;
    const XaSuspendCause *cause = &row->result_cause;
    char feature[160];
    sus_format_cause(cause, feature, sizeof(feature));
    char message[320];
    if (row->result == XA_SUSPEND_MAY)
        snprintf(message, sizeof(message),
                 "generator '%s' cannot reach the scheduler; %s is not allowed in a body that uses "
                 "`yield expr`\n"
                 "hint: a generator is resumed by whoever drives it, not by the scheduler; do the "
                 "suspending work outside the generator and yield its result",
                 sus_function_name(row->node, row->symbol), feature);
    else
        snprintf(message, sizeof(message),
                 "generator '%s' cannot be proven not to reach the scheduler; %s leaves the "
                 "evidence incomplete\n"
                 "hint: a generator cannot call through an unresolved function value, because the "
                 "target may suspend; call a named function instead",
                 sus_function_name(row->node, row->symbol), feature);
    AstNode *site = cause->site ? cause->site : row->node;
    XrLocation location = {.file = row->symbol ? row->symbol->links.file_path : NULL,
                           .line = (uint32_t) site->line,
                           .column = (uint32_t) site->column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_GENERATOR_SUSPEND,
                               message, &location);
}

/* A `defer` body is cleanup, and cleanup has to finish. It runs as a nested
 * call on a frame that is already leaving -- returning, unwinding a throw, or
 * being cancelled -- and that frame has no suspension point left to park at.
 * A body that reaches the scheduler therefore does not park and resume: it
 * aborts, and the rest of the cleanup silently does not happen. Both backends
 * agree on that wrong answer, so a differential test cannot catch it either.
 * Rejecting it here is what makes `defer` total: every registered action runs
 * to completion on every exit path, cancellation included. */
/* Only a proven suspension is rejected. A dynamic target - a `fn()` parameter
 * called from a defer body - is INCOMPLETE, not MAY, and the language offers no
 * way to declare a function value non-suspending, so rejecting the unknown
 * would ban every callback-driven cleanup instead of the broken ones. An
 * unknown target that does reach the scheduler is caught where it happens: the
 * leaving frame has no suspension point to park at and aborts. */
static void sus_reject_suspending_cleanup(XaSuspendPass *pass, const XaSuspendRow *row) {
    XaSuspendCause edge_cause;
    const XaSuspendCause *cause = NULL;
    if (row->has_cleanup_suspend) {
        cause = &row->cleanup_suspend_cause;
    } else {
        for (int i = 0; i < row->edge_count; i++) {
            const XaSuspendEdge *edge = &row->edges[i];
            if (!edge->in_cleanup_body)
                continue;
            XaSuspendRow *callee = sus_row_for_symbol(pass, edge->callee);
            if (!callee || callee->result != XA_SUSPEND_MAY)
                continue;
            sus_set_cause(&edge_cause, edge->site, edge->is_callback ? "callback" : "call",
                          edge->callee && edge->callee->name ? edge->callee->name : "?");
            cause = &edge_cause;
            break;
        }
    }
    if (!cause)
        return;
    char feature[160];
    sus_format_cause(cause, feature, sizeof(feature));
    char message[320];
    snprintf(message, sizeof(message),
             "a deferred action cannot reach the scheduler; %s runs while the frame is already "
             "leaving, so the rest of the cleanup would be dropped\n"
             "hint: do the suspending work before the `defer`, or hand the resource to a "
             "coroutine that owns closing it",
             feature);
    AstNode *site = cause->site ? cause->site : row->node;
    XrLocation location = {.file = row->symbol ? row->symbol->links.file_path : NULL,
                           .line = (uint32_t) site->line,
                           .column = (uint32_t) site->column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_DEFER_SUSPEND,
                               message, &location);
}

static void sus_reject_spawning_cleanup(XaSuspendPass *pass, const XaSuspendRow *row) {
    XaSuspendCause edge_cause;
    const XaSuspendCause *cause = NULL;
    if (row->has_cleanup_spawn) {
        cause = &row->cleanup_spawn_cause;
    } else {
        for (int i = 0; i < row->edge_count; i++) {
            const XaSuspendEdge *edge = &row->edges[i];
            if (!edge->in_cleanup_body)
                continue;
            XaSuspendRow *callee = sus_row_for_symbol(pass, edge->callee);
            if (!callee || callee->spawn_result != XA_SUSPEND_MAY)
                continue;
            sus_set_cause(&edge_cause, edge->site, edge->is_callback ? "callback" : "call",
                          edge->callee && edge->callee->name ? edge->callee->name : "?");
            cause = &edge_cause;
            break;
        }
    }
    if (!cause)
        return;
    char feature[160];
    sus_format_cause(cause, feature, sizeof(feature));
    char message[320];
    snprintf(message, sizeof(message),
             "a deferred action cannot create a task; %s would let work escape the cleanup "
             "boundary\n"
             "hint: create and join the task before registering cleanup, or transfer resource "
             "ownership to the task",
             feature);
    AstNode *site = cause->site ? cause->site : row->node;
    XrLocation location = {.file = row->symbol ? row->symbol->links.file_path : NULL,
                           .line = (uint32_t) site->line,
                           .column = (uint32_t) site->column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_DEFER_SUSPEND,
                               message, &location);
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
            XaSuspendState spawn_state;
            XaSuspendCause spawn_cause;
            XaUnknownReason spawn_incomplete_reason;
            sus_combine_spawn_row(pass, &pass->rows[i], &spawn_state, &spawn_cause,
                                  &spawn_incomplete_reason);
            if (state != pass->rows[i].result ||
                incomplete_reason != pass->rows[i].result_incomplete_reason ||
                spawn_state != pass->rows[i].spawn_result ||
                spawn_incomplete_reason != pass->rows[i].spawn_result_incomplete_reason) {
                pass->rows[i].result = state;
                pass->rows[i].result_cause = cause;
                pass->rows[i].result_incomplete_reason = incomplete_reason;
                pass->rows[i].spawn_result = spawn_state;
                pass->rows[i].spawn_result_cause = spawn_cause;
                pass->rows[i].spawn_result_incomplete_reason = spawn_incomplete_reason;
                changed = true;
            } else if (state != XA_SUSPEND_NONE) {
                pass->rows[i].result_cause = cause;
            }
            if (spawn_state != XA_SUSPEND_NONE)
                pass->rows[i].spawn_result_cause = spawn_cause;
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
            xa_effect_summary_add_semantic_effects(&summary, XA_SEM_EFFECT_SCHED_SUSPEND);
        else if (row->result == XA_SUSPEND_INCOMPLETE)
            xa_effect_summary_mark_semantic_incomplete(&summary, XA_SEM_EFFECT_SCHED_SUSPEND,
                                                       row->result_incomplete_reason);
        /* Lexical and local: published straight from the scan, never combined
         * over call edges and never incomplete. */
        if (row->has_generator_yield)
            xa_effect_summary_add_semantic_effects(&summary, XA_SEM_EFFECT_GEN_SUSPEND);
        if (row->spawn_result == XA_SUSPEND_MAY)
            xa_effect_summary_add_semantic_effects(&summary, XA_SEM_EFFECT_TASK_SPAWN);
        else if (row->spawn_result == XA_SUSPEND_INCOMPLETE)
            xa_effect_summary_mark_semantic_incomplete(&summary, XA_SEM_EFFECT_TASK_SPAWN,
                                                       row->spawn_result_incomplete_reason);
        sus_reject_suspending_generator(pass, row);
        sus_reject_generator_defer(pass, row);
        sus_reject_suspending_cleanup(pass, row);
        sus_reject_spawning_cleanup(pass, row);
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
    if (sus_call_is_mem_with_slice_mut(pass, call)) {
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
