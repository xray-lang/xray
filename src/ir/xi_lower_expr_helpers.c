#include "xi_lower_expr_helpers.h"
#include "xi_lower_internal.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_info.h"
#include <string.h>

XR_FUNC struct XrStructLayout *xi_lower_struct_layout_of(struct XrType *t) {
    if (!t)
        return NULL;
    if (t->kind != XR_KIND_INSTANCE && t->kind != XR_KIND_CLASS)
        return NULL;
    if (!t->is_value_type)
        return NULL;
    if (!t->instance.class_ref)
        return NULL;
    return t->instance.class_ref->struct_layout;
}

XR_FUNC int xi_lower_struct_field_index(const struct XrStructLayout *layout, const char *name) {
    if (!layout || !layout->field_names || !name)
        return -1;
    for (int i = 0; i < layout->field_count; i++) {
        if (layout->field_names[i] && strcmp(layout->field_names[i], name) == 0)
            return i;
    }
    return -1;
}

XR_FUNC struct XrType *xi_lower_infer_binary_type(XiLower *l, AstNodeType ast_type,
                                                  struct XrType *left, struct XrType *right) {
    if (ast_type >= AST_BINARY_EQ && ast_type <= AST_BINARY_GE)
        return l->type_bool;
    if (ast_type == AST_BINARY_EQ_STRICT || ast_type == AST_BINARY_NE_STRICT)
        return l->type_bool;
    if (ast_type == AST_BINARY_AND || ast_type == AST_BINARY_OR)
        return l->type_bool;
    if (ast_type >= AST_BINARY_BAND && ast_type <= AST_BINARY_RSHIFT)
        return l->type_int;
    if (left && left->kind == XR_KIND_FLOAT)
        return l->type_float;
    if (right && right->kind == XR_KIND_FLOAT)
        return l->type_float;
    if (left && left->kind == XR_KIND_INT && right && right->kind == XR_KIND_INT)
        return l->type_int;
    if (left && left->kind == XR_KIND_STRING && right && right->kind == XR_KIND_STRING)
        return l->type_string;
    return left ? left : l->type_any;
}

XR_FUNC struct XrType *xi_lower_infer_unary_type(XiLower *l, AstNodeType ast_type,
                                                 struct XrType *operand) {
    switch (ast_type) {
        case AST_UNARY_NEG:
            return operand ? operand : l->type_int;
        case AST_UNARY_NOT:
            return l->type_bool;
        case AST_UNARY_BNOT:
            return l->type_int;
        default:
            return operand ? operand : l->type_any;
    }
}

XR_FUNC uint16_t xi_lower_binary_ast_to_xi_op(AstNodeType ast_type) {
    switch (ast_type) {
        case AST_BINARY_ADD:
            return XI_ADD;
        case AST_BINARY_SUB:
            return XI_SUB;
        case AST_BINARY_MUL:
            return XI_MUL;
        case AST_BINARY_DIV:
            return XI_DIV;
        case AST_BINARY_MOD:
            return XI_MOD;
        case AST_BINARY_BAND:
            return XI_BAND;
        case AST_BINARY_BOR:
            return XI_BOR;
        case AST_BINARY_BXOR:
            return XI_BXOR;
        case AST_BINARY_LSHIFT:
            return XI_SHL;
        case AST_BINARY_RSHIFT:
            return XI_SHR;
        case AST_BINARY_EQ:
            return XI_EQ;
        case AST_BINARY_NE:
            return XI_NE;
        case AST_BINARY_EQ_STRICT:
            return XI_EQ_STRICT;
        case AST_BINARY_NE_STRICT:
            return XI_NE_STRICT;
        case AST_BINARY_LT:
            return XI_LT;
        case AST_BINARY_LE:
            return XI_LE;
        case AST_BINARY_GT:
            return XI_GT;
        case AST_BINARY_GE:
            return XI_GE;
        default:
            return XI_ADD;
    }
}
