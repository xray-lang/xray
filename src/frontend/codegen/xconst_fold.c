/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xconst_fold.c - Compile-time constant folding implementation
 */

#include "xconst_fold.h"
#include "../../base/xchecks.h"
#include "../../runtime/object/xstring.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../../base/xmalloc.h"

// Create failure result
static XrConstEvalResult fail_result(void) {
    XrConstEvalResult r = {.success = false, .value = xr_null()};
    return r;
}

// Create success result
static XrConstEvalResult success_result(XrValue val) {
    XrConstEvalResult r = {.success = true, .value = val};
    return r;
}

// Helper function: create boolean value
static XrValue make_bool(bool v) {
    return xr_bool(v);
}

// Check if node is binary operation
static bool is_binary_node(AstNodeType type) {
    return type >= AST_BINARY_ADD && type <= AST_BINARY_OR;
}

/* ========== Shared Folding Helpers ========== */

// Fold unary operation on a resolved value
static XrConstEvalResult fold_unary_value(AstNodeType op, XrValue operand) {
    switch (op) {
        case AST_UNARY_NEG:
            if (XR_IS_INT(operand))
                return success_result(xr_int(-XR_TO_INT(operand)));
            if (XR_IS_FLOAT(operand))
                return success_result(xr_float(-XR_TO_FLOAT(operand)));
            return fail_result();
        case AST_UNARY_NOT: {
            bool is_false = XR_IS_NULL(operand) ||
                            (XR_IS_INT(operand) && XR_TO_INT(operand) == 0) ||
                            (XR_IS_BOOL(operand) && !XR_TO_BOOL(operand));
            return success_result(make_bool(is_false));
        }
        case AST_UNARY_BNOT:
            if (XR_IS_INT(operand))
                return success_result(xr_int(~XR_TO_INT(operand)));
            return fail_result();
        default:
            return fail_result();
    }
}

// Fold binary numeric operation on two resolved values
static XrConstEvalResult fold_binary_values(AstNodeType op, XrValue lv, XrValue rv) {
    // Integer operation (with overflow checks)
    if (XR_IS_INT(lv) && XR_IS_INT(rv)) {
        int64_t l = XR_TO_INT(lv);
        int64_t r = XR_TO_INT(rv);
        int64_t res;

        switch (op) {
            case AST_BINARY_ADD:
                if (__builtin_add_overflow(l, r, &res))
                    return fail_result();
                return success_result(xr_int(res));
            case AST_BINARY_SUB:
                if (__builtin_sub_overflow(l, r, &res))
                    return fail_result();
                return success_result(xr_int(res));
            case AST_BINARY_MUL:
                if (__builtin_mul_overflow(l, r, &res))
                    return fail_result();
                return success_result(xr_int(res));
            case AST_BINARY_DIV:
                if (r == 0 || (l == INT64_MIN && r == -1))
                    return fail_result();
                return success_result(xr_int(l / r));
            case AST_BINARY_MOD:
                if (r == 0 || (l == INT64_MIN && r == -1))
                    return fail_result();
                return success_result(xr_int(l % r));
            case AST_BINARY_BAND:
                return success_result(xr_int(l & r));
            case AST_BINARY_BOR:
                return success_result(xr_int(l | r));
            case AST_BINARY_BXOR:
                return success_result(xr_int(l ^ r));
            case AST_BINARY_LSHIFT:
                if (r < 0 || r >= 64)
                    return fail_result();
                return success_result(xr_int(l << r));
            case AST_BINARY_RSHIFT:
                if (r < 0 || r >= 64)
                    return fail_result();
                return success_result(xr_int(l >> r));
            case AST_BINARY_LT:
                return success_result(make_bool(l < r));
            case AST_BINARY_LE:
                return success_result(make_bool(l <= r));
            case AST_BINARY_GT:
                return success_result(make_bool(l > r));
            case AST_BINARY_GE:
                return success_result(make_bool(l >= r));
            case AST_BINARY_EQ:
            case AST_BINARY_EQ_STRICT:
                return success_result(make_bool(l == r));
            case AST_BINARY_NE:
            case AST_BINARY_NE_STRICT:
                return success_result(make_bool(l != r));
            case AST_BINARY_AND:
                return success_result(make_bool(l && r));
            case AST_BINARY_OR:
                return success_result(make_bool(l || r));
            default:
                return fail_result();
        }
    }

    // Float operation (including int-float mixed)
    if ((XR_IS_INT(lv) || XR_IS_FLOAT(lv)) && (XR_IS_INT(rv) || XR_IS_FLOAT(rv))) {
        double l = XR_IS_INT(lv) ? (double) XR_TO_INT(lv) : XR_TO_FLOAT(lv);
        double r = XR_IS_INT(rv) ? (double) XR_TO_INT(rv) : XR_TO_FLOAT(rv);

        switch (op) {
            case AST_BINARY_ADD:
                return success_result(xr_float(l + r));
            case AST_BINARY_SUB:
                return success_result(xr_float(l - r));
            case AST_BINARY_MUL:
                return success_result(xr_float(l * r));
            case AST_BINARY_DIV:
                if (r == 0.0)
                    return fail_result();
                return success_result(xr_float(l / r));
            case AST_BINARY_LT:
                return success_result(make_bool(l < r));
            case AST_BINARY_LE:
                return success_result(make_bool(l <= r));
            case AST_BINARY_GT:
                return success_result(make_bool(l > r));
            case AST_BINARY_GE:
                return success_result(make_bool(l >= r));
            case AST_BINARY_EQ:
                return success_result(make_bool(l == r));
            case AST_BINARY_NE:
                return success_result(make_bool(l != r));
            default:
                return fail_result();
        }
    }

    return fail_result();
}

// Fold string binary operations (concatenation and repeat)
static XrConstEvalResult fold_string_binary(AstNodeType op, XrValue lv, XrValue rv,
                                            XrayIsolate *X) {
    // String concatenation: string + string
    if (XR_IS_STRING(lv) && XR_IS_STRING(rv) && op == AST_BINARY_ADD) {
        XrString *ls = XR_TO_STRING(lv);
        XrString *rs = XR_TO_STRING(rv);
        size_t new_len = ls->length + rs->length;
        char *buf = (char *) xr_malloc(new_len + 1);
        memcpy(buf, ls->data, ls->length);
        memcpy(buf + ls->length, rs->data, rs->length);
        buf[new_len] = '\0';
        XrString *result = xr_compile_time_intern(X, buf, new_len);
        xr_free(buf);
        return success_result(xr_string_value(result));
    }

    // String repeat: string * int
    if (XR_IS_STRING(lv) && XR_IS_INT(rv) && op == AST_BINARY_MUL) {
        XrString *s = XR_TO_STRING(lv);
        int64_t count = XR_TO_INT(rv);
        if (count <= 0) {
            XrString *empty = xr_compile_time_intern(X, "", 0);
            return success_result(xr_string_value(empty));
        }
        if (count > 10000)
            return fail_result();
        size_t new_len = s->length * count;
        char *buf = (char *) xr_malloc(new_len + 1);
        for (int64_t i = 0; i < count; i++) {
            memcpy(buf + i * s->length, s->data, s->length);
        }
        buf[new_len] = '\0';
        XrString *result = xr_compile_time_intern(X, buf, new_len);
        xr_free(buf);
        return success_result(xr_string_value(result));
    }

    return fail_result();
}

// Evaluate literal node to XrValue
static XrConstEvalResult eval_literal(XrayIsolate *X, AstNode *expr) {
    switch (expr->type) {
        case AST_LITERAL_INT:
            return success_result(xr_int(expr->as.literal.raw_value.int_val));
        case AST_LITERAL_FLOAT:
            return success_result(xr_float(expr->as.literal.raw_value.float_val));
        case AST_LITERAL_STRING: {
            const char *str = expr->as.literal.raw_value.string_val;
            XrString *s = xr_compile_time_intern(X, str, strlen(str));
            return success_result(xr_string_value(s));
        }
        case AST_LITERAL_TRUE:
            return success_result(make_bool(true));
        case AST_LITERAL_FALSE:
            return success_result(make_bool(false));
        case AST_LITERAL_NULL:
            return success_result(xr_null());
        default:
            return fail_result();
    }
}

/* ========== Public API ========== */

// Try to evaluate expression at compile time
XrConstEvalResult xr_const_eval(XrayIsolate *X, AstNode *expr) {
    XR_DCHECK(X != NULL, "xr_const_eval: NULL isolate");
    if (!expr)
        return fail_result();

    // Literals
    if (expr->type >= AST_LITERAL_INT && expr->type <= AST_LITERAL_NULL) {
        return eval_literal(X, expr);
    }

    // Grouping
    if (expr->type == AST_GROUPING) {
        return xr_const_eval(X, expr->as.grouping);
    }

    // Unary
    if (expr->type == AST_UNARY_NEG || expr->type == AST_UNARY_NOT ||
        expr->type == AST_UNARY_BNOT) {
        XrConstEvalResult operand = xr_const_eval(X, expr->as.unary.operand);
        if (!operand.success)
            return fail_result();
        return fold_unary_value(expr->type, operand.value);
    }

    // Binary
    if (is_binary_node(expr->type)) {
        BinaryNode *bin = &expr->as.binary;
        XrConstEvalResult left = xr_const_eval(X, bin->left);
        if (!left.success)
            return fail_result();
        XrConstEvalResult right = xr_const_eval(X, bin->right);
        if (!right.success)
            return fail_result();

        XrConstEvalResult r = fold_binary_values(expr->type, left.value, right.value);
        if (r.success)
            return r;
        return fold_string_binary(expr->type, left.value, right.value, X);
    }

    return fail_result();
}

// Try to evaluate expression at compile time (with constant variable lookup)
XrConstEvalResult xr_const_eval_with_ctx(XrCompilerContext *ctx, AstNode *expr) {
    if (!ctx || !expr)
        return fail_result();

    // Variable: lookup compile-time constant
    if (expr->type == AST_VARIABLE) {
        const char *var_name = expr->as.variable.name;
        XrString *name_str = xr_compile_time_intern(ctx->X, var_name, strlen(var_name));
        ConstEntry *entry = xr_compiler_ctx_find_const(ctx, name_str);
        if (entry) {
            switch (entry->type) {
                case CONST_INT:
                    return success_result(xr_int(entry->value.int_val));
                case CONST_FLOAT:
                    return success_result(xr_float(entry->value.float_val));
                case CONST_STRING:
                    return success_result(xr_string_value(entry->value.str_val));
                case CONST_BOOL:
                    return success_result(make_bool(entry->value.bool_val));
                case CONST_NULL:
                    return success_result(xr_null());
            }
        }
        return fail_result();
    }

    // Literals
    if (expr->type >= AST_LITERAL_INT && expr->type <= AST_LITERAL_NULL) {
        return eval_literal(ctx->X, expr);
    }

    // Grouping
    if (expr->type == AST_GROUPING) {
        return xr_const_eval_with_ctx(ctx, expr->as.grouping);
    }

    // Unary
    if (expr->type == AST_UNARY_NEG || expr->type == AST_UNARY_NOT ||
        expr->type == AST_UNARY_BNOT) {
        XrConstEvalResult operand = xr_const_eval_with_ctx(ctx, expr->as.unary.operand);
        if (!operand.success)
            return fail_result();
        return fold_unary_value(expr->type, operand.value);
    }

    // Binary
    if (is_binary_node(expr->type)) {
        BinaryNode *bin = &expr->as.binary;
        XrConstEvalResult left = xr_const_eval_with_ctx(ctx, bin->left);
        if (!left.success)
            return fail_result();
        XrConstEvalResult right = xr_const_eval_with_ctx(ctx, bin->right);
        if (!right.success)
            return fail_result();

        XrConstEvalResult r = fold_binary_values(expr->type, left.value, right.value);
        if (r.success)
            return r;
        return fold_string_binary(expr->type, left.value, right.value, ctx->X);
    }

    return fail_result();
}

// Check if expression is compile-time constant
bool xr_is_const_expr(AstNode *expr) {
    if (!expr)
        return false;

    switch (expr->type) {
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            return true;

        case AST_GROUPING:
            return xr_is_const_expr(expr->as.grouping);

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return xr_is_const_expr(expr->as.unary.operand);

        default:
            break;
    }

    // Binary operation
    if (is_binary_node(expr->type)) {
        BinaryNode *bin = &expr->as.binary;
        return xr_is_const_expr(bin->left) && xr_is_const_expr(bin->right);
    }

    return false;
}
