/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xaddressability.h"
#include "xanalyzer_visitor_internal.h"
#include <stdio.h>
#include <string.h>

static XaSymbol *address_symbol(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr || expr->type != AST_VARIABLE)
        return NULL;
    uint32_t symbol_id = expr->as.variable.symbol_id;
    if (symbol_id != 0) {
        XaSymbol *symbol = xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
        if (symbol)
            return symbol;
    }
    return expr->as.variable.name ? xa_lookup_visible_symbol(ctx, expr->as.variable.name) : NULL;
}

static AstNode *pointer_value_expr(AstNode *expr) {
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
            case AST_MOVE_EXPR:
                expr = expr->as.move_expr.expr;
                break;
            case AST_EXPR_STMT:
                expr = expr->as.expr_stmt;
                break;
            case AST_UNSAFE_EXPR: {
                AstNode *body = expr->as.unsafe_expr.operand;
                if (!body || body->type != AST_BLOCK) {
                    expr = body;
                    break;
                }
                BlockNode *block = &body->as.block;
                if (block->count <= 0 || !block->statements[block->count - 1] ||
                    block->statements[block->count - 1]->type != AST_EXPR_STMT)
                    return NULL;
                expr = block->statements[block->count - 1]->as.expr_stmt;
                break;
            }
            case AST_BLOCK: {
                BlockNode *block = &expr->as.block;
                if (block->count <= 0 || !block->statements[block->count - 1] ||
                    block->statements[block->count - 1]->type != AST_EXPR_STMT)
                    return NULL;
                expr = block->statements[block->count - 1]->as.expr_stmt;
                break;
            }
            default:
                return expr;
        }
    }
    return NULL;
}

static XrType *pointer_node_type(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr)
        return NULL;
    return xa_analyzer_get_node_type(ctx->analyzer, expr);
}

static void pointer_set_mutability(XaPointerProvenance *out, XrType *type) {
    if (!out)
        return;
    out->address.mutability = type && XR_TYPE_IS_POINTER(type) && type->ptr_is_mut
                                  ? XR_STORAGE_MUTABLE
                                  : XR_STORAGE_READONLY;
}

static void pointer_set_foreign(XaPointerProvenance *out, XrType *type) {
    memset(out, 0, sizeof(*out));
    out->address.owner = XR_STORAGE_FOREIGN;
    out->address.address_identity = XR_ADDRESS_FOREIGN;
    out->address.origin = XR_POINTER_ORIGIN_FOREIGN;
    out->address.escape = XR_POINTER_ESCAPE_STABLE;
    pointer_set_mutability(out, type);
}

static void pointer_set_null(XaPointerProvenance *out, XrType *type) {
    memset(out, 0, sizeof(*out));
    out->address.origin = XR_POINTER_ORIGIN_NULL;
    out->address.escape = XR_POINTER_ESCAPE_STABLE;
    pointer_set_mutability(out, type);
}

static void pointer_set_static_literal(XaPointerProvenance *out, AstNode *expr, XrType *type) {
    memset(out, 0, sizeof(*out));
    out->address.storage_id = expr ? expr->node_id + 1 : 0;
    out->address.lifetime_id = out->address.storage_id;
    out->address.owner = XR_STORAGE_MODULE;
    out->address.address_identity = XR_ADDRESS_MODULE_STABLE;
    out->address.origin = XR_POINTER_ORIGIN_STATIC;
    out->address.escape = XR_POINTER_ESCAPE_STABLE;
    pointer_set_mutability(out, type);
}

static void pointer_set_owner(XaInferContext *ctx, XaPointerProvenance *out, XaSymbol *owner,
                              XrType *pointer_type) {
    memset(out, 0, sizeof(*out));
    out->owner_symbol = owner;
    out->address.storage_id = owner ? owner->id : 0;
    out->address.lifetime_id = owner ? owner->id : 0;
    XrType *owner_type = owner ? xa_analyzer_get_type(ctx->analyzer, owner) : NULL;
    bool module_fixed = owner && owner->scope && owner->scope->kind == XA_SCOPE_GLOBAL &&
                        owner_type && owner_type->kind == XR_KIND_FIXED_ARRAY;
    if (module_fixed) {
        out->address.owner = XR_STORAGE_MODULE;
        out->address.address_identity = XR_ADDRESS_MODULE_STABLE;
        out->address.origin = XR_POINTER_ORIGIN_MODULE;
        out->address.escape = XR_POINTER_ESCAPE_STABLE;
    } else {
        out->address.owner = owner && owner->is_shared  ? XR_STORAGE_SHARED_SYSTEM
                             : owner && owner->is_owned ? XR_STORAGE_OWNED_SYSTEM
                                                        : XR_STORAGE_EXEC_LOCAL;
        out->address.address_identity = XR_ADDRESS_LEXICAL;
        out->address.origin = owner_type && owner_type->kind == XR_KIND_FIXED_ARRAY
                                  ? XR_POINTER_ORIGIN_STACK_BORROW
                                  : XR_POINTER_ORIGIN_OWNER_BORROW;
        out->address.escape = XR_POINTER_ESCAPE_CALL_BOUND;
    }
    pointer_set_mutability(out, pointer_type);
}

static XaActiveSpanBorrow *pointer_active_borrow(XaInferContext *ctx, XaSymbol *symbol) {
    if (!ctx || !symbol)
        return NULL;
    for (XaActiveSpanBorrow *borrow = ctx->active_span_borrows; borrow; borrow = borrow->next) {
        if (borrow->view_symbol == symbol && borrow->is_pointer_borrow)
            return borrow;
    }
    return NULL;
}

static bool pointer_projection_receiver(XaInferContext *ctx, AstNode *call_expr,
                                        AstNode **out_receiver) {
    if (out_receiver)
        *out_receiver = NULL;
    if (!ctx || !call_expr || call_expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &call_expr->as.call_expr;
    if (call->arg_count != 0 || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *member = &call->callee->as.member_access;
    if (!member->name || !member->object)
        return false;
    XrType *receiver_type = pointer_node_type(ctx, member->object);
    bool projection = false;
    if ((strcmp(member->name, "ptr") == 0 || strcmp(member->name, "mutPtr") == 0) &&
        receiver_type &&
        (XR_TYPE_IS_ARRAY(receiver_type) || XR_TYPE_IS_SPAN(receiver_type) ||
         receiver_type->kind == XR_KIND_FIXED_ARRAY)) {
        projection = true;
    } else if (strcmp(member->name, "borrowPtr") == 0 && receiver_type &&
               xr_type_is_named_class(receiver_type, "Buffer")) {
        projection = true;
    }
    if (projection && out_receiver)
        *out_receiver = member->object;
    return projection;
}

static bool pointer_merge(XaPointerProvenance *out, const XaPointerProvenance *left, bool has_left,
                          const XaPointerProvenance *right, bool has_right) {
    if (!has_left && !has_right)
        return false;
    if (!has_left) {
        *out = *right;
        return true;
    }
    if (!has_right) {
        *out = *left;
        return true;
    }
    *out = *left;
    bool same = memcmp(&left->address, &right->address, sizeof(left->address)) == 0 &&
                left->owner_symbol == right->owner_symbol;
    out->mixed = left->mixed || right->mixed || !same;
    if (left->address.escape != XR_POINTER_ESCAPE_STABLE ||
        right->address.escape != XR_POINTER_ESCAPE_STABLE) {
        out->address.escape = XR_POINTER_ESCAPE_CALL_BOUND;
        if (left->address.escape == XR_POINTER_ESCAPE_STABLE)
            out->address = right->address;
        out->address.escape = XR_POINTER_ESCAPE_CALL_BOUND;
    }
    if (left->owner_symbol != right->owner_symbol)
        out->owner_symbol = NULL;
    return true;
}

bool xa_pointer_provenance_for_expr(XaInferContext *ctx, AstNode *expr, XaPointerProvenance *out) {
    if (!ctx || !ctx->analyzer || !expr || !out)
        return false;
    AstNode *direct = pointer_value_expr(expr);
    if (!direct)
        return false;
    XrType *pointer_type = pointer_node_type(ctx, expr);
    if (!pointer_type)
        pointer_type = pointer_node_type(ctx, direct);

    if (direct->type == AST_LITERAL_NULL) {
        pointer_set_null(out, pointer_type);
        return true;
    }
    if (direct->type == AST_VARIABLE) {
        XaSymbol *symbol = address_symbol(ctx, direct);
        XaSymbolLinks *links = symbol ? xa_analyzer_get_links(ctx->analyzer, symbol) : NULL;
        if (links && links->pointer_provenance_known) {
            out->address = links->pointer_provenance;
            out->owner_symbol = links->pointer_owner_symbol;
            out->mixed = links->pointer_provenance_mixed;
            pointer_set_mutability(out, pointer_type);
            return true;
        }
        XaActiveSpanBorrow *borrow = pointer_active_borrow(ctx, symbol);
        if (borrow) {
            pointer_set_owner(ctx, out, borrow->owner_symbol, pointer_type);
            return true;
        }
        pointer_set_foreign(out, pointer_type);
        return true;
    }
    if (direct->type == AST_TERNARY) {
        XaPointerProvenance yes = {0};
        XaPointerProvenance no = {0};
        bool has_yes = xa_pointer_provenance_for_expr(ctx, direct->as.ternary.true_expr, &yes);
        bool has_no = xa_pointer_provenance_for_expr(ctx, direct->as.ternary.false_expr, &no);
        return pointer_merge(out, &yes, has_yes, &no, has_no);
    }
    if (direct->type == AST_MATCH_EXPR) {
        bool have = false;
        XaPointerProvenance merged = {0};
        for (int i = 0; i < direct->as.match_expr.arm_count; i++) {
            AstNode *arm = direct->as.match_expr.arms[i];
            AstNode *body = arm && arm->type == AST_MATCH_ARM ? arm->as.match_arm.body : NULL;
            XaPointerProvenance next = {0};
            bool has_next = xa_pointer_provenance_for_expr(ctx, body, &next);
            XaPointerProvenance combined = {0};
            if (pointer_merge(&combined, &merged, have, &next, has_next)) {
                merged = combined;
                have = true;
            }
        }
        if (have)
            *out = merged;
        return have;
    }
    if (direct->type == AST_CALL_EXPR) {
        AstNode *receiver = NULL;
        if (pointer_projection_receiver(ctx, direct, &receiver)) {
            if (receiver->type == AST_ARRAY_LITERAL &&
                receiver->as.array_literal.is_fixed_bytes_literal) {
                pointer_set_static_literal(out, direct, pointer_type);
                return true;
            }
            XaSymbol *owner = xa_root_variable_symbol_for_expr(ctx, receiver);
            if (!owner)
                return false;
            pointer_set_owner(ctx, out, owner, pointer_type);
            return true;
        }
        CallExprNode *call = &direct->as.call_expr;
        if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
            MemberAccessNode *member = &call->callee->as.member_access;
            if (member->name && strcmp(member->name, "view") == 0 && member->object &&
                member->object->type == AST_VARIABLE && member->object->as.variable.name &&
                call->arg_count == 1 && call->arguments && call->arguments[0]) {
                bool is_mem = strcmp(member->object->as.variable.name, "mem") == 0;
                XaSymbol *module_symbol = address_symbol(ctx, member->object);
                XaSymbolLinks *module_links =
                    module_symbol ? xa_analyzer_get_links(ctx->analyzer, module_symbol) : NULL;
                is_mem = is_mem || (module_links && module_links->module_name &&
                                    strcmp(module_links->module_name, "mem") == 0);
                if (is_mem)
                    return xa_pointer_provenance_for_expr(ctx, call->arguments[0], out);
            }
            if (member->name && strcmp(member->name, "offset") == 0 && member->object)
                return xa_pointer_provenance_for_expr(ctx, member->object, out);
            if (member->name && strcmp(member->name, "null") == 0 && member->object &&
                member->object->type == AST_NEW_EXPR) {
                pointer_set_null(out, pointer_type);
                return true;
            }
        }
        pointer_set_foreign(out, pointer_type);
        return true;
    }
    if (direct->type == AST_MEMBER_ACCESS && pointer_type && XR_TYPE_IS_POINTER(pointer_type) &&
        pointer_type->ptr_is_c_view && direct->as.member_access.object)
        return xa_pointer_provenance_for_expr(ctx, direct->as.member_access.object, out);

    if (pointer_type && XR_TYPE_IS_POINTER(pointer_type)) {
        pointer_set_foreign(out, pointer_type);
        return true;
    }
    return false;
}

void xa_record_pointer_provenance(XaInferContext *ctx, XaSymbol *symbol, AstNode *value,
                                  XrType *value_type) {
    if (!ctx || !ctx->analyzer || !symbol)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    if (!links)
        return;
    links->pointer_provenance_known = false;
    links->pointer_provenance_mixed = false;
    links->pointer_owner_symbol = NULL;
    memset(&links->pointer_provenance, 0, sizeof(links->pointer_provenance));
    if (!value || !value_type || !XR_TYPE_IS_POINTER(value_type))
        return;
    XaPointerProvenance provenance = {0};
    if (!xa_pointer_provenance_for_expr(ctx, value, &provenance))
        return;
    links->pointer_provenance = provenance.address;
    links->pointer_owner_symbol = provenance.owner_symbol;
    links->pointer_provenance_known = true;
    links->pointer_provenance_mixed = provenance.mixed;
}

void xa_check_pointer_borrow_escape(XaInferContext *ctx, AstNode *location_node, AstNode *value,
                                    XrType *value_type, const char *escape_context) {
    if (!ctx || !ctx->analyzer || !location_node || !value || !value_type ||
        !XR_TYPE_IS_POINTER(value_type))
        return;
    XaPointerProvenance provenance = {0};
    if (!xa_pointer_provenance_for_expr(ctx, value, &provenance) ||
        provenance.address.escape == XR_POINTER_ESCAPE_STABLE)
        return;
    XrLocation loc = {
        .file = ctx->file_path, .line = location_node->line, .column = location_node->column};
    char msg[320];
    snprintf(
        msg, sizeof(msg),
        "cannot %s; raw pointer is tied to %s '%s' and has only call-bound lifetime",
        escape_context ? escape_context : "let raw pointer borrow escape",
        provenance.address.origin == XR_POINTER_ORIGIN_STACK_BORROW ? "stack storage" : "owner",
        provenance.owner_symbol && provenance.owner_symbol->name ? provenance.owner_symbol->name
                                                                 : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static XrType *address_expr_type(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr)
        return NULL;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    return type ? type : xa_visit_infer_expr(ctx, expr);
}

static bool address_type_has_native_layout(XaInferContext *ctx, XrType *type) {
    return xa_type_supports_const_static_data_object(type) ||
           xr_type_has_static_layout(xa_analyzer_target_data_layout(ctx->analyzer), type, NULL,
                                     NULL);
}

static XaAddressability address_none(XaAddressRejectReason rejection) {
    XaAddressability result = {0};
    result.rejection = rejection;
    return result;
}

static XaAddressability classify_variable(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    XaSymbol *symbol = address_symbol(ctx, expr);
    if (!symbol || (symbol->kind != XA_SYM_VARIABLE && symbol->kind != XA_SYM_PARAMETER))
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);

    XaAddressability result = {0};
    result.base_symbol = symbol;
    result.pointee_type = xa_analyzer_get_type(ctx->analyzer, symbol);
    if (!result.pointee_type)
        result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(ctx, result.pointee_type);
    result.mutable_ok = !symbol->is_const && !symbol->is_readonly_binding;
    result.is_imported = symbol->is_imported;
    result.is_shared = symbol->is_shared;

    if (symbol->scope && symbol->scope->kind == XA_SCOPE_GLOBAL) {
        result.kind = XA_ADDRESS_MODULE_STATIC;
        result.lifetime = XA_ADDRESS_LIFETIME_MODULE;
        result.storage_owner = symbol->is_shared ? XR_STORAGE_SHARED_SYSTEM : XR_STORAGE_MODULE;
        result.address_identity =
            symbol->is_shared ? XR_ADDRESS_SHARED_STABLE : XR_ADDRESS_MODULE_STABLE;
    } else if (symbol->kind == XA_SYM_PARAMETER) {
        result.kind = XA_ADDRESS_PARAMETER;
        result.lifetime = XA_ADDRESS_LIFETIME_CALL;
        result.storage_owner = XR_STORAGE_EXEC_LOCAL;
        result.address_identity = XR_ADDRESS_LEXICAL;
    } else {
        result.kind = XA_ADDRESS_STACK_LOCAL;
        result.lifetime = XA_ADDRESS_LIFETIME_LEXICAL;
        result.storage_owner = symbol->is_owned ? XR_STORAGE_OWNED_SYSTEM : XR_STORAGE_EXEC_LOCAL;
        result.address_identity = XR_ADDRESS_LEXICAL;
    }

    if (!result.native_layout_ok)
        result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
    else if (wants_mutable && !result.mutable_ok)
        result.rejection = XA_ADDRESS_REJECT_READONLY;
    else if (result.kind == XA_ADDRESS_STACK_LOCAL || result.kind == XA_ADDRESS_PARAMETER)
        result.rejection = XA_ADDRESS_REJECT_ESCAPE_UNPROVEN;
    else
        result.rejection = XA_ADDRESS_OK;
    return result;
}

static XaAddressability classify_member(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    MemberAccessNode *member = &expr->as.member_access;
    if (!member->object || !member->name)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    XaAddressability base = xa_classify_addressability(ctx, member->object, wants_mutable);
    XaAddressability result = base;
    result.kind = XA_ADDRESS_FIELD;
    result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(ctx, result.pointee_type);
    if (!result.native_layout_ok) {
        result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
        return result;
    }
    XrType *base_type = address_expr_type(ctx, member->object);
    if (!base_type ||
        !xr_type_has_static_field_offset(xa_analyzer_target_data_layout(ctx->analyzer), base_type,
                                         member->name, &result.field_offset)) {
        result.rejection = XA_ADDRESS_REJECT_STORAGE_MAY_MOVE;
        return result;
    }
    if (wants_mutable && !result.mutable_ok) {
        result.rejection = XA_ADDRESS_REJECT_READONLY;
        return result;
    }
    if (base.rejection != XA_ADDRESS_OK)
        result.rejection = base.rejection;
    return result;
}

static XaAddressability classify_index(XaInferContext *ctx, AstNode *expr, bool wants_mutable) {
    IndexGetNode *index = &expr->as.index_get;
    if (!index->array)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    XaAddressability base = xa_classify_addressability(ctx, index->array, wants_mutable);
    XaAddressability result = base;
    XrType *owner_type = address_expr_type(ctx, index->array);
    result.pointee_type = address_expr_type(ctx, expr);
    result.native_layout_ok = address_type_has_native_layout(ctx, result.pointee_type);
    if (owner_type && owner_type->kind == XR_KIND_FIXED_ARRAY) {
        result.kind = XA_ADDRESS_FIXED_ARRAY_ELEMENT;
        if (!result.native_layout_ok)
            result.rejection = XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT;
        else if (wants_mutable && !result.mutable_ok)
            result.rejection = XA_ADDRESS_REJECT_READONLY;
        else if (base.rejection != XA_ADDRESS_OK)
            result.rejection = base.rejection;
        return result;
    }
    result.kind = XA_ADDRESS_OWNER_ELEMENT;
    result.lifetime = XA_ADDRESS_LIFETIME_OWNER;
    result.address_identity = XR_ADDRESS_NONE;
    result.rejection = XA_ADDRESS_REJECT_DYNAMIC_OWNER_ELEMENT;
    return result;
}

XaAddressability xa_classify_addressability(XaInferContext *ctx, AstNode *expr,
                                            bool wants_mutable) {
    if (!expr)
        return address_none(XA_ADDRESS_REJECT_NOT_LVALUE);
    switch (expr->type) {
        case AST_VARIABLE:
            return classify_variable(ctx, expr, wants_mutable);
        case AST_MEMBER_ACCESS:
            return classify_member(ctx, expr, wants_mutable);
        case AST_INDEX_GET:
            return classify_index(ctx, expr, wants_mutable);
        case AST_GROUPING:
            return xa_classify_addressability(ctx, expr->as.grouping, wants_mutable);
        case AST_CALL_EXPR:
        case AST_ARRAY_LITERAL:
        case AST_OBJECT_LITERAL:
        case AST_STRUCT_LITERAL:
            return address_none(XA_ADDRESS_REJECT_TEMPORARY);
        default:
            /* The classifier is called for expressions. Any other expression
             * shape produces a temporary value rather than an addressable
             * storage location. */
            return address_none(XA_ADDRESS_REJECT_TEMPORARY);
    }
}

const char *xa_address_kind_name(XaAddressKind kind) {
    static const char *names[] = {
        "none",  "module static",       "stack local",   "parameter",
        "field", "fixed-array element", "owner element",
    };
    return (unsigned) kind < sizeof(names) / sizeof(names[0]) ? names[kind]
                                                              : "invalid address kind";
}

const char *xa_address_reject_reason_name(XaAddressRejectReason reason) {
    static const char *names[] = {
        "addressable",      "not an lvalue",    "temporary expression",  "no native layout",
        "storage may move", "readonly storage", "dynamic owner element", "escape cannot be proven",
    };
    return (unsigned) reason < sizeof(names) / sizeof(names[0]) ? names[reason]
                                                                : "invalid addressability";
}
