/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_allocation.c - Allocation effect inference and @no_alloc validation
 */

#include "xanalyzer_allocation.h"
#include "xa_alloc_effect.h"
#include "../parser/xa_assertion_attr.h"
#include "xa_selection.h"
#include "xanalyzer.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_symbol.h"
#include "xbuiltin_receiver_registry.h"
#include "../parser/xast_nodes.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../runtime/xerror_codes.h"
#include <stdio.h>
#include <string.h>

typedef struct XaAllocEdge {
    XaSymbol *callee;
    XaAllocEffectId imported_effect_id;
    AstNode *site;
    XaAllocReasonSet unknown_reason;
    bool is_callback;
} XaAllocEdge;

typedef struct XaAllocFunctionRow {
    AstNode *node;
    XaSymbol *symbol;
    XaSymbol synthetic_symbol;
    bool uses_synthetic_symbol;
    XaAllocationSummary direct;
    XaAllocationSummary result;
    XaAllocEdge *edges;
    int edge_count;
    int edge_capacity;
} XaAllocFunctionRow;

typedef struct XaAllocPass {
    XaAnalyzer *analyzer;
    XaAllocFunctionRow *rows;
    int row_count;
    int row_capacity;
} XaAllocPass;

typedef struct XaAllocScan {
    XaAllocPass *pass;
    XaAllocFunctionRow *row;
    int nested_function_depth;
} XaAllocScan;

static AstNode *alloc_identity_expr(AstNode *expr) {
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

static XaSymbol *alloc_symbol_by_id(XaAnalyzer *analyzer, uint32_t id) {
    return analyzer && id ? xa_scope_lookup_by_id(analyzer->global_scope, id) : NULL;
}

static XaSymbol *alloc_variable_symbol(XaAnalyzer *analyzer, AstNode *expr) {
    expr = alloc_identity_expr(expr);
    if (!analyzer || !expr || expr->type != AST_VARIABLE)
        return NULL;
    XaSymbol *symbol = alloc_symbol_by_id(analyzer, expr->as.variable.symbol_id);
    if (symbol)
        return symbol;
    return expr->as.variable.name ? xa_analyzer_lookup_deep(analyzer, expr->as.variable.name)
                                  : NULL;
}

static XaSymbol *alloc_resolve_function_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
    if (scope && scope->function_symbol)
        return scope->function_symbol;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)
        return alloc_symbol_by_id(analyzer, node->as.function_decl.symbol_id);
    return NULL;
}

static AstNode *alloc_function_body(AstNode *node) {
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

static const char *alloc_function_name(AstNode *node, XaSymbol *symbol) {
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

static bool alloc_function_has_contract(AstNode *node) {
    return xa_decl_has_attribute(node, ATTR_NO_ALLOC);
}

static XaAllocFunctionRow *alloc_row_for_symbol(XaAllocPass *pass, XaSymbol *symbol) {
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

static XaAllocFunctionRow *alloc_row_for_node(XaAllocPass *pass, AstNode *node) {
    if (!pass || !node)
        return NULL;
    for (int i = 0; i < pass->row_count; i++) {
        if (pass->rows[i].node == node)
            return &pass->rows[i];
    }
    return NULL;
}

static bool alloc_append_row(XaAllocPass *pass, AstNode *node, XaSymbol *symbol) {
    if (!pass || !node)
        return false;
    if ((symbol && alloc_row_for_symbol(pass, symbol)) || alloc_row_for_node(pass, node))
        return true;
    if (!symbol && node->type != AST_FUNCTION_EXPR)
        return false;
    if (pass->row_count >= pass->row_capacity) {
        int next_capacity = pass->row_capacity ? pass->row_capacity * 2 : 32;
        XaAllocFunctionRow *next = (XaAllocFunctionRow *) xr_realloc(
            pass->rows, (size_t) next_capacity * sizeof(XaAllocFunctionRow));
        if (!next)
            return false;
        memset(&next[pass->row_capacity], 0,
               (size_t) (next_capacity - pass->row_capacity) * sizeof(XaAllocFunctionRow));
        pass->rows = next;
        pass->row_capacity = next_capacity;
        for (int i = 0; i < pass->row_count; i++) {
            if (pass->rows[i].uses_synthetic_symbol)
                pass->rows[i].symbol = &pass->rows[i].synthetic_symbol;
        }
    }
    XaAllocFunctionRow *row = &pass->rows[pass->row_count++];
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
    row->direct.state = XA_ALLOC_PROVEN_NONE;
    row->result.state = XA_ALLOC_PROVEN_NONE;
    return true;
}

static void alloc_collect_node_pre(AstNode *node, void *userdata) {
    XaAllocPass *pass = (XaAllocPass *) userdata;
    if (!pass || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL)
        alloc_append_row(pass, node, alloc_resolve_function_symbol(pass->analyzer, node));
}

static void alloc_collect_functions(XaAllocPass *pass, AstNode *node) {
    if (pass && node) {
        xa_ast_walk(node, alloc_collect_node_pre, NULL, pass);
        /* Row growth may move embedded synthetic closure symbols.  Repair the
         * non-owning pointers once collection (and therefore growth) ends. */
        for (int i = 0; i < pass->row_count; i++) {
            if (pass->rows[i].uses_synthetic_symbol)
                pass->rows[i].symbol = &pass->rows[i].synthetic_symbol;
        }
    }
}

static bool alloc_add_edge(XaAllocFunctionRow *row, XaSymbol *callee,
                           XaAllocEffectId imported_effect_id, AstNode *site,
                           XaAllocReasonSet unknown_reason, bool is_callback) {
    if (!row || !site)
        return false;
    for (int i = 0; i < row->edge_count; i++) {
        XaAllocEdge *edge = &row->edges[i];
        if (edge->callee == callee && edge->imported_effect_id == imported_effect_id &&
            edge->site == site && edge->unknown_reason == unknown_reason &&
            edge->is_callback == is_callback)
            return true;
    }
    if (row->edge_count >= row->edge_capacity) {
        int next_capacity = row->edge_capacity ? row->edge_capacity * 2 : 8;
        XaAllocEdge *next =
            (XaAllocEdge *) xr_realloc(row->edges, (size_t) next_capacity * sizeof(XaAllocEdge));
        if (!next)
            return false;
        row->edges = next;
        row->edge_capacity = next_capacity;
    }
    row->edges[row->edge_count++] = (XaAllocEdge) {.callee = callee,
                                                   .imported_effect_id = imported_effect_id,
                                                   .site = site,
                                                   .unknown_reason = unknown_reason,
                                                   .is_callback = is_callback};
    return true;
}

static void alloc_mark_direct(XaAllocFunctionRow *row, AstNode *site, XaAllocReasonSet reason,
                              const char *kind, const char *detail) {
    if (!row || !site || row->direct.state == XA_ALLOC_MAY)
        return;
    row->direct.state = XA_ALLOC_MAY;
    row->direct.reason_bits = reason;
    row->direct.first_site_node_id = site->node_id;
    row->direct.line = (uint32_t) site->line;
    row->direct.column = (uint32_t) site->column;
    row->direct.cause_kind = kind;
    row->direct.cause_detail = detail;
}

static bool alloc_pod_span_element(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    return type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT || type->kind == XR_KIND_BOOL ||
           type->kind == XR_KIND_RUNE;
}

static bool alloc_receiver_matches(const XrType *receiver, XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver && receiver->kind == XR_KIND_INT && !receiver->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver && receiver->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver && receiver->kind == XR_KIND_SPAN &&
                   alloc_pod_span_element(receiver->container.element_type);
    }
    return false;
}

static const XaBuiltinReceiverMethodSpec *alloc_receiver_method(const XrType *receiver,
                                                                const char *name) {
    if (!receiver || !name)
        return NULL;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (strcmp(spec->source_name, name) == 0 &&
            alloc_receiver_matches(receiver, spec->receiver))
            return spec;
    }
    return NULL;
}

static XaAllocationContractKind alloc_builtin_method_contract(const XrType *receiver,
                                                              const char *method, bool is_static) {
    if (!receiver || !method)
        return XA_ALLOCATION_CONTRACT_MISSING;
    XaAllocationContractKind contract =
        xa_builtin_get_type_member_allocation_contract((XrType *) receiver, method, is_static);
    if (contract != XA_ALLOCATION_CONTRACT_MISSING)
        return contract;
    if (!is_static && receiver->kind == XR_KIND_ENUM)
        return xa_builtin_get_named_type_member_allocation_contract("<enum>", method, false);
    if (!is_static && receiver->kind == XR_KIND_NULL)
        return xa_builtin_get_named_type_member_allocation_contract("<null>", method, false);
    if (receiver->kind == XR_KIND_INSTANCE && receiver->instance.class_name) {
        contract = xa_builtin_get_named_type_member_allocation_contract(
            receiver->instance.class_name, method, is_static);
        if (contract != XA_ALLOCATION_CONTRACT_MISSING)
            return contract;
        if (!is_static)
            return xa_builtin_get_handle_method_allocation_contract(receiver->instance.class_name,
                                                                    method);
    }
    return XA_ALLOCATION_CONTRACT_MISSING;
}

static XaAllocationContractKind alloc_module_contract(XaSymbol *symbol) {
    if (!symbol)
        return XA_ALLOCATION_CONTRACT_MISSING;
    const char *module = symbol->links.module_name;
    const char *name =
        symbol->links.import_member_name ? symbol->links.import_member_name : symbol->name;
    return module && name ? xa_builtin_get_module_func_allocation_contract(module, name)
                          : XA_ALLOCATION_CONTRACT_MISSING;
}

typedef struct XaAllocIntrinsicContract {
    const char *name;
    XaAllocationContractKind allocation;
} XaAllocIntrinsicContract;

static XaAllocationContractKind alloc_intrinsic_contract(const char *name) {
    static const XaAllocIntrinsicContract contracts[] = {
        {"len", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"capacity", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"min", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"max", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"abs", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"assert", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"isNull", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"typeId", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"uint8", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"uint16", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"uint32", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"uint64", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"int8", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"int16", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"int32", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"int64", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"int", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"float", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"bool", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"byte", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"cancelled", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"unreachable", XA_ALLOCATION_CONTRACT_NO_HEAP},
        {"Array", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Map", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Set", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Json", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"StringBuilder", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"Atomic", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"copy", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"copy_shared", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"copy_owned", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"to_shared", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"str_concat", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"regex_compile", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"chr", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"string", XA_ALLOCATION_CONTRACT_MAY_HEAP},
        {"typeName", XA_ALLOCATION_CONTRACT_MAY_HEAP},
    };
    if (!name)
        return XA_ALLOCATION_CONTRACT_MISSING;
    for (size_t i = 0; i < sizeof(contracts) / sizeof(contracts[0]); i++) {
        if (strcmp(name, contracts[i].name) == 0)
            return contracts[i].allocation;
    }
    return XA_ALLOCATION_CONTRACT_MISSING;
}

static bool alloc_fixed_value_copy_is_no_heap(const XrType *type, int depth) {
    if (!type || type->is_nullable || depth > 8)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_POINTER:
            return true;
        case XR_KIND_FIXED_ARRAY:
            return type->fixed_array.length >= 0 &&
                   alloc_fixed_value_copy_is_no_heap(type->fixed_array.element_type, depth + 1);
        default:
            return false;
    }
}

static bool alloc_copy_call_is_fixed_value(XaAllocScan *scan, const CallExprNode *call,
                                           const char *name) {
    if (!scan || !call || !name || strcmp(name, "copy") != 0 || call->arg_count != 1)
        return false;
    const XrType *arg_type = xa_analyzer_get_node_type(scan->pass->analyzer, call->arguments[0]);
    return alloc_fixed_value_copy_is_no_heap(arg_type, 0);
}

static int alloc_hof_callback_index(const XrType *receiver, const char *method) {
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

static void alloc_add_symbol_edge(XaAllocScan *scan, XaSymbol *callee, AstNode *site,
                                  bool is_callback) {
    if (!scan || !scan->row || !site)
        return;
    if (!callee) {
        alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, site, XA_ALLOC_REASON_DYNAMIC_CALL,
                       is_callback);
        return;
    }
    XaAllocFunctionRow *local_row = alloc_row_for_symbol(scan->pass, callee);
    if (!local_row && callee->links.alloc_effect_complete) {
        XaAllocationSummary imported;
        memset(&imported, 0, sizeof(imported));
        imported.state = callee->links.alloc_state;
        imported.reason_bits = callee->links.alloc_reason_bits;
        imported.cause_kind =
            imported.state == XA_ALLOC_UNKNOWN ? "imported unknown" : "imported allocation";
        imported.cause_detail = callee->name;
        imported.callee_name = callee->name;
        XaAllocEffectId imported_id =
            xa_allocation_db_intern(scan->pass->analyzer->allocation_db, &imported);
        alloc_add_edge(scan->row, callee, imported_id, site, XA_ALLOC_REASON_NONE, is_callback);
        return;
    }
    if (!local_row && callee->links.alloc_effect_id != XA_ALLOC_EFFECT_NONE) {
        alloc_add_edge(scan->row, callee, callee->links.alloc_effect_id, site, XA_ALLOC_REASON_NONE,
                       is_callback);
        return;
    }
    if (callee->links.function_decl_node && alloc_function_body(callee->links.function_decl_node)) {
        alloc_add_edge(scan->row, callee, XA_ALLOC_EFFECT_NONE, site, XA_ALLOC_REASON_NONE,
                       is_callback);
        return;
    }
    if (callee->links.alloc_effect_id != XA_ALLOC_EFFECT_NONE) {
        alloc_add_edge(scan->row, callee, callee->links.alloc_effect_id, site, XA_ALLOC_REASON_NONE,
                       is_callback);
        return;
    }
    XaAllocationContractKind contract = alloc_module_contract(callee);
    if (contract == XA_ALLOCATION_CONTRACT_MAY_HEAP) {
        const char *detail = callee->links.module_name ? callee->links.module_name : callee->name;
        alloc_mark_direct(scan->row, site, XA_ALLOC_REASON_RUNTIME, "stdlib", detail);
    } else if (contract == XA_ALLOCATION_CONTRACT_MISSING) {
        alloc_add_edge(scan->row, callee, XA_ALLOC_EFFECT_NONE, site,
                       XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING, is_callback);
    }
}

static void alloc_scan_callback(XaAllocScan *scan, AstNode *callback, AstNode *site) {
    callback = alloc_identity_expr(callback);
    if (!callback)
        return;
    if (callback->type == AST_LITERAL_NULL)
        return;
    if (callback->type == AST_FUNCTION_EXPR) {
        XaAllocFunctionRow *callback_row = alloc_row_for_node(scan->pass, callback);
        XaSymbol *symbol = callback_row
                               ? callback_row->symbol
                               : alloc_resolve_function_symbol(scan->pass->analyzer, callback);
        alloc_add_symbol_edge(scan, symbol, site, true);
        return;
    }
    if (callback->type == AST_VARIABLE) {
        alloc_add_symbol_edge(scan, alloc_variable_symbol(scan->pass->analyzer, callback), site,
                              true);
        return;
    }
    if (callback->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection = xa_selection_table_get(
            (XaSelectionTable *) scan->pass->analyzer->selection_table, callback);
        alloc_add_symbol_edge(scan, selection ? selection->target_symbol : NULL, site, true);
        return;
    }
    alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, site, XA_ALLOC_REASON_DYNAMIC_CALL, true);
}

static void alloc_scan_call(XaAllocScan *scan, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    AstNode *callee = alloc_identity_expr(call->callee);
    if (!callee)
        return;
    if (callee->type == AST_FUNCTION_EXPR) {
        XaAllocFunctionRow *callee_row = alloc_row_for_node(scan->pass, callee);
        alloc_add_symbol_edge(scan,
                              callee_row
                                  ? callee_row->symbol
                                  : alloc_resolve_function_symbol(scan->pass->analyzer, callee),
                              node, false);
        return;
    }
    if (callee->type == AST_VARIABLE) {
        const char *name = callee->as.variable.name;
        if (alloc_copy_call_is_fixed_value(scan, call, name))
            return;
        XaAllocationContractKind intrinsic_contract = alloc_intrinsic_contract(name);
        if (intrinsic_contract == XA_ALLOCATION_CONTRACT_NO_HEAP)
            return;
        if (intrinsic_contract == XA_ALLOCATION_CONTRACT_MAY_HEAP) {
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_HEAP_CONSTRUCT, "constructor", name);
            return;
        }
        XaSymbol *symbol = alloc_variable_symbol(scan->pass->analyzer, callee);
        /* A function-valued parameter has no statically closed target in this
         * body.  It is a dynamic call, not a bodyless native declaration, so
         * preserve that distinction in diagnostics and exported evidence. */
        if (symbol && symbol->kind == XA_SYM_PARAMETER) {
            alloc_add_edge(scan->row, symbol, XA_ALLOC_EFFECT_NONE, node,
                           XA_ALLOC_REASON_DYNAMIC_CALL, false);
            return;
        }
        alloc_add_symbol_edge(scan, symbol, node, false);
        return;
    }
    if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        const XaSelection *selection = xa_selection_table_get(
            (XaSelectionTable *) scan->pass->analyzer->selection_table, callee);
        /* Enum variants are value constructors.  Their compact typed lowering
         * is an inline aggregate even when the variant carries payloads. */
        if (selection && selection->kind == XA_SEL_ENUM_MEMBER && selection->target_symbol &&
            selection->target_symbol->kind == XA_SYM_ENUM)
            return;
        XrType *receiver = xa_analyzer_get_node_type(scan->pass->analyzer, member->object);
        AstNode *receiver_expr = alloc_identity_expr(member->object);
        const char *receiver_name = receiver_expr && receiver_expr->type == AST_VARIABLE
                                        ? receiver_expr->as.variable.name
                                        : NULL;
        const XaBuiltinReceiverMethodSpec *receiver_spec =
            alloc_receiver_method(receiver, member->name);
        if (receiver_spec) {
            if (receiver_spec->allocation == XA_BUILTIN_ALLOCATION_MAY_HEAP) {
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "method",
                                  member->name);
            }
        } else {
            bool is_static_builtin = receiver_name && xa_builtin_get_by_name(receiver_name) != NULL;
            XaAllocationContractKind builtin_contract =
                is_static_builtin ? xa_builtin_get_named_type_member_allocation_contract(
                                        receiver_name, member->name, true)
                                  : alloc_builtin_method_contract(receiver, member->name, false);
            bool is_builtin_member =
                is_static_builtin ||
                (receiver && xa_builtin_get_member_signature(receiver, member->name)) ||
                (receiver && receiver->kind == XR_KIND_INSTANCE && receiver->instance.class_name &&
                 xa_builtin_find_handle_by_name(receiver->instance.class_name));
            if (builtin_contract == XA_ALLOCATION_CONTRACT_MAY_HEAP) {
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_RUNTIME, "method", member->name);
            } else if (builtin_contract == XA_ALLOCATION_CONTRACT_NO_HEAP) {
                /* Explicit registry proof. */
            } else if (is_builtin_member) {
                alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, node,
                               XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING, false);
            } else {
                XaSymbol *target = selection ? selection->target_symbol : NULL;
                if (selection && selection->kind == XA_SEL_MODULE_EXPORT && target &&
                    (target->links.function_decl_node ||
                     target->links.alloc_effect_id != XA_ALLOC_EFFECT_NONE)) {
                    alloc_add_symbol_edge(scan, target, node, false);
                } else if (selection && selection->kind == XA_SEL_MODULE_EXPORT) {
                    XaSymbol synthetic;
                    memset(&synthetic, 0, sizeof(synthetic));
                    synthetic.name = member->name;
                    synthetic.links.module_name = target && target->links.module_name
                                                      ? target->links.module_name
                                                      : (receiver_name ? receiver_name : NULL);
                    synthetic.links.import_member_name = member->name;
                    synthetic.links.return_type =
                        xa_analyzer_get_node_type(scan->pass->analyzer, node);
                    XaAllocationContractKind contract = alloc_module_contract(&synthetic);
                    if (contract == XA_ALLOCATION_CONTRACT_MAY_HEAP)
                        alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_RUNTIME, "stdlib",
                                          member->name);
                    else if (contract == XA_ALLOCATION_CONTRACT_MISSING)
                        alloc_add_edge(scan->row, target, XA_ALLOC_EFFECT_NONE, node,
                                       XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING, false);
                } else if (target && target->links.function_decl_node) {
                    bool open_dispatch = receiver && receiver->kind == XR_KIND_INSTANCE &&
                                         target->parent && target->parent->links.class_info &&
                                         !target->parent->links.class_info->explicit_final &&
                                         target->parent->links.class_info->struct_layout == NULL;
                    if (open_dispatch) {
                        alloc_add_edge(scan->row, target, XA_ALLOC_EFFECT_NONE, node,
                                       XA_ALLOC_REASON_OPEN_DISPATCH, false);
                    } else {
                        alloc_add_symbol_edge(scan, target, node, false);
                    }
                } else if (target) {
                    alloc_add_symbol_edge(scan, target, node, false);
                } else {
                    AstNode *module_object = alloc_identity_expr(member->object);
                    XaSymbol *module_symbol =
                        alloc_variable_symbol(scan->pass->analyzer, module_object);
                    if (module_symbol && module_symbol->links.module_name) {
                        XaSymbol synthetic;
                        memset(&synthetic, 0, sizeof(synthetic));
                        synthetic.name = member->name;
                        synthetic.links.module_name = module_symbol->links.module_name;
                        synthetic.links.import_member_name = member->name;
                        synthetic.links.return_type =
                            xa_analyzer_get_node_type(scan->pass->analyzer, node);
                        XaAllocationContractKind contract = alloc_module_contract(&synthetic);
                        if (contract == XA_ALLOCATION_CONTRACT_MAY_HEAP)
                            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_RUNTIME, "stdlib",
                                              member->name);
                        else if (contract == XA_ALLOCATION_CONTRACT_MISSING)
                            alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, node,
                                           XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING, false);
                    } else {
                        alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, node,
                                       XA_ALLOC_REASON_DYNAMIC_CALL, false);
                    }
                }
            }
        }

        int callback_index = alloc_hof_callback_index(receiver, member->name);
        if (callback_index >= 0 && callback_index < call->arg_count)
            alloc_scan_callback(scan, call->arguments[callback_index], node);
        return;
    }
    alloc_add_edge(scan->row, NULL, XA_ALLOC_EFFECT_NONE, node, XA_ALLOC_REASON_DYNAMIC_CALL,
                   false);
}

static void alloc_scan_node_pre(AstNode *node, void *userdata) {
    XaAllocScan *scan = (XaAllocScan *) userdata;
    if (!scan || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        scan->nested_function_depth++;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;

    XrType *node_type = xa_analyzer_get_node_type(scan->pass->analyzer, node);
    switch (node->type) {
        case AST_LITERAL_BIGINT:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_HEAP_CONSTRUCT, "literal", "BigInt");
            break;
        case AST_LITERAL_REGEX:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_HEAP_CONSTRUCT, "literal", "Regex");
            break;
        case AST_TEMPLATE_STRING:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_STRING, "expression",
                              "template string");
            break;
        case AST_ARRAY_LITERAL:
            if (!node_type || node_type->kind != XR_KIND_FIXED_ARRAY)
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "literal", "Array");
            break;
        case AST_MAP_LITERAL:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "literal", "Map");
            break;
        case AST_SET_LITERAL:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "literal", "Set");
            break;
        case AST_OBJECT_LITERAL:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "literal", "Json");
            break;
        case AST_NEW_EXPR:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_HEAP_CONSTRUCT, "constructor",
                              node->as.new_expr.class_name);
            break;
        case AST_RANGE:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_HEAP_CONSTRUCT, "op", "RANGE");
            break;
        case AST_CHANNEL_NEW:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_RUNTIME, "constructor", "Channel");
            break;
        case AST_GO_EXPR:
            alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_RUNTIME, "operation", "go");
            break;
        case AST_BINARY_ADD:
            if (node_type && node_type->kind == XR_KIND_STRING)
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_STRING, "operation",
                                  "string.concat");
            break;
        case AST_AS_EXPR: {
            XrType *source_type =
                xa_analyzer_get_node_type(scan->pass->analyzer, node->as.as_expr.expr);
            if (node_type && node_type->kind == XR_KIND_STRING &&
                (!source_type || source_type->kind != XR_KIND_STRING))
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_STRING, "conversion", "string");
            break;
        }
        case AST_SLICE_EXPR:
            if (node_type && (node_type->kind == XR_KIND_STRING || node_type->kind == XR_KIND_VIEW))
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "method", "slice");
            break;
        case AST_MEMBER_ACCESS:
            if (node_type && node_type->kind == XR_KIND_STRING) {
                XrType *receiver_type =
                    xa_analyzer_get_node_type(scan->pass->analyzer, node->as.member_access.object);
                if (receiver_type && receiver_type->kind == XR_KIND_ENUM &&
                    strcmp(node->as.member_access.name, "name") == 0)
                    alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_STRING, "property",
                                      "enum.name");
            }
            break;
        case AST_FOR_IN_STMT: {
            XrType *collection_type =
                xa_analyzer_get_node_type(scan->pass->analyzer, node->as.for_in_stmt.collection);
            if (collection_type && collection_type->kind == XR_KIND_STRING)
                alloc_mark_direct(scan->row, node, XA_ALLOC_REASON_CONTAINER, "iterator",
                                  "string.runes");
            break;
        }
        case AST_CALL_EXPR:
            alloc_scan_call(scan, node);
            break;
        default:
            break;
    }
}

static void alloc_scan_node_post(AstNode *node, void *userdata) {
    XaAllocScan *scan = (XaAllocScan *) userdata;
    if (!scan || !node)
        return;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL) &&
        scan->nested_function_depth > 0)
        scan->nested_function_depth--;
}

static void alloc_scan_function(XaAllocPass *pass, XaAllocFunctionRow *row) {
    AstNode *body = alloc_function_body(row ? row->node : NULL);
    if (!pass || !row || !body)
        return;
    XaAllocScan scan = {.pass = pass, .row = row};
    xa_ast_walk(body, alloc_scan_node_pre, alloc_scan_node_post, &scan);
}

static XaAllocationSummary alloc_combine_row(XaAllocPass *pass, XaAllocFunctionRow *row) {
    XaAllocationSummary result = row->direct;
    for (int i = 0; i < row->edge_count; i++) {
        XaAllocEdge *edge = &row->edges[i];
        const XaAllocationSummary *callee_summary = NULL;
        XaAllocFunctionRow *callee_row = alloc_row_for_symbol(pass, edge->callee);
        if (callee_row)
            callee_summary = &callee_row->result;
        else if (edge->imported_effect_id != XA_ALLOC_EFFECT_NONE)
            callee_summary =
                xa_allocation_db_get(pass->analyzer->allocation_db, edge->imported_effect_id);

        XaAllocState edge_state = XA_ALLOC_PROVEN_NONE;
        XaAllocReasonSet edge_reason = XA_ALLOC_REASON_NONE;
        if (edge->unknown_reason != XA_ALLOC_REASON_NONE) {
            edge_state = XA_ALLOC_UNKNOWN;
            edge_reason = edge->unknown_reason;
        } else if (!callee_summary) {
            edge_state = XA_ALLOC_UNKNOWN;
            edge_reason = XA_ALLOC_REASON_UNRESOLVED_CALLEE;
        } else {
            edge_state = callee_summary->state;
            edge_reason = callee_summary->reason_bits;
        }

        if (edge_state == XA_ALLOC_MAY && result.state != XA_ALLOC_MAY) {
            result.state = XA_ALLOC_MAY;
            result.reason_bits =
                (edge->is_callback ? XA_ALLOC_REASON_CALLBACK : XA_ALLOC_REASON_CALLEE) |
                edge_reason;
            result.first_site_node_id = edge->site->node_id;
            result.first_callee_symbol_id = edge->callee ? edge->callee->id : 0;
            result.line = (uint32_t) edge->site->line;
            result.column = (uint32_t) edge->site->column;
            result.cause_kind = edge->is_callback ? "callback" : "call";
            result.cause_detail = edge->callee && edge->callee->name ? edge->callee->name : "?";
            result.callee_name = edge->callee && edge->callee->name ? edge->callee->name : "?";
            result.callee_effect_id = edge->imported_effect_id;
        } else if (edge_state == XA_ALLOC_UNKNOWN && result.state == XA_ALLOC_PROVEN_NONE) {
            result.state = XA_ALLOC_UNKNOWN;
            result.reason_bits = edge_reason;
            result.first_site_node_id = edge->site->node_id;
            result.first_callee_symbol_id = edge->callee ? edge->callee->id : 0;
            result.line = (uint32_t) edge->site->line;
            result.column = (uint32_t) edge->site->column;
            result.cause_kind = edge->is_callback ? "unknown callback" : "unknown call";
            result.cause_detail = edge->callee && edge->callee->name ? edge->callee->name : "?";
            result.callee_name = edge->callee && edge->callee->name ? edge->callee->name : "?";
            result.callee_effect_id = edge->imported_effect_id;
        }
    }
    result.stable_fingerprint = xa_allocation_summary_fingerprint(&result);
    return result;
}

static bool alloc_summary_same(const XaAllocationSummary *a, const XaAllocationSummary *b) {
    return a && b && a->state == b->state && a->reason_bits == b->reason_bits &&
           a->first_site_node_id == b->first_site_node_id &&
           a->first_callee_symbol_id == b->first_callee_symbol_id && a->line == b->line &&
           a->column == b->column && a->stable_fingerprint == b->stable_fingerprint;
}

static void alloc_publish_summaries(XaAllocPass *pass) {
    if (!pass)
        return;
    for (;;) {
        bool changed = false;
        for (int i = 0; i < pass->row_count; i++) {
            XaAllocationSummary next = alloc_combine_row(pass, &pass->rows[i]);
            if (!alloc_summary_same(&next, &pass->rows[i].result)) {
                pass->rows[i].result = next;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    for (int i = 0; i < pass->row_count; i++) {
        XaAllocFunctionRow *row = &pass->rows[i];
        row->symbol->links.alloc_effect_id =
            xa_allocation_db_intern(pass->analyzer->allocation_db, &row->result);
        row->symbol->links.alloc_state = row->result.state;
        row->symbol->links.alloc_reason_bits = row->result.reason_bits;
        row->symbol->links.alloc_fingerprint = row->result.stable_fingerprint;
        row->symbol->links.alloc_effect_complete = true;
        row->symbol->links.has_no_alloc_contract = alloc_function_has_contract(row->node);
    }
}

static const char *alloc_unknown_reason_text(XaAllocReasonSet reasons) {
    if (reasons & XA_ALLOC_REASON_OPEN_DISPATCH)
        return "open virtual dispatch target is not closed";
    if (reasons & XA_ALLOC_REASON_NATIVE_CONTRACT_MISSING)
        return "native or extern callee has no allocation contract";
    if (reasons & XA_ALLOC_REASON_DYNAMIC_CALL)
        return "indirect callback target is unknown";
    if (reasons & XA_ALLOC_REASON_UNRESOLVED_CALLEE)
        return "callee allocation effect is unresolved";
    if (reasons & XA_ALLOC_REASON_INVALID_PROGRAM)
        return "the function contains prior semantic errors";
    return "allocation effect is incomplete";
}

static void alloc_validate_contract(XaAllocPass *pass, XaAllocFunctionRow *row) {
    if (!pass || !row || !alloc_function_has_contract(row->node) ||
        row->result.state == XA_ALLOC_PROVEN_NONE)
        return;
    const char *name = alloc_function_name(row->node, row->symbol);
    char message[768];
    if (row->result.state == XA_ALLOC_MAY) {
        if ((row->result.reason_bits & (XA_ALLOC_REASON_CALLEE | XA_ALLOC_REASON_CALLBACK)) != 0) {
            const char *callee = row->result.callee_name ? row->result.callee_name : "?";
            const XaAllocationSummary *callee_summary = NULL;
            const char *callee_file = NULL;
            for (int i = 0; i < row->edge_count; i++) {
                XaAllocEdge *edge = &row->edges[i];
                if (!edge->callee || edge->callee->id != row->result.first_callee_symbol_id)
                    continue;
                XaAllocFunctionRow *callee_row = alloc_row_for_symbol(pass, edge->callee);
                if (callee_row)
                    callee_summary = &callee_row->result;
                else if (edge->imported_effect_id != XA_ALLOC_EFFECT_NONE)
                    callee_summary = xa_allocation_db_get(pass->analyzer->allocation_db,
                                                          edge->imported_effect_id);
                callee_file = edge->callee->links.file_path;
                break;
            }
            if (callee_summary && callee_summary->state == XA_ALLOC_MAY) {
                snprintf(message, sizeof(message),
                         "@no_alloc contract is not satisfied for '%s': allocates via call '%s' "
                         "at line %u\n  '%s' allocates via %s '%s' at %s:%u",
                         name, callee, row->result.line, callee,
                         callee_summary->cause_kind ? callee_summary->cause_kind : "operation",
                         callee_summary->cause_detail ? callee_summary->cause_detail : "?",
                         callee_file ? callee_file : "<unknown>", callee_summary->line);
            } else {
                snprintf(message, sizeof(message),
                         "@no_alloc contract is not satisfied for '%s': allocates via call '%s' "
                         "at line %u",
                         name, callee, row->result.line);
            }
        } else {
            snprintf(message, sizeof(message),
                     "@no_alloc contract is not satisfied for '%s': allocates via %s '%s' at "
                     "line %u",
                     name, row->result.cause_kind ? row->result.cause_kind : "operation",
                     row->result.cause_detail ? row->result.cause_detail : "?", row->result.line);
        }
    } else {
        snprintf(message, sizeof(message),
                 "@no_alloc contract cannot be proven for '%s': %s at line %u", name,
                 alloc_unknown_reason_text(row->result.reason_bits), row->result.line);
    }
    XrLocation location = {.file = row->symbol->links.file_path,
                           .line = row->result.line ? row->result.line : (uint32_t) row->node->line,
                           .column = row->result.column};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                               &location);
}

void xa_infer_allocation_effects(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !analyzer->allocation_db || !ast)
        return;
    XaAllocPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.analyzer = analyzer;
    alloc_collect_functions(&pass, ast);
    for (int i = 0; i < pass.row_count; i++)
        alloc_scan_function(&pass, &pass.rows[i]);
    alloc_publish_summaries(&pass);
    for (int i = 0; i < pass.row_count; i++)
        alloc_validate_contract(&pass, &pass.rows[i]);
    for (int i = 0; i < pass.row_count; i++)
        xr_free(pass.rows[i].edges);
    xr_free(pass.rows);
}
