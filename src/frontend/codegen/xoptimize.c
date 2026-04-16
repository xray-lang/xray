/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoptimize.c - Compiler optimization implementation
 *
 * KEY CONCEPT:
 *   Constant folding for binary, unary, logical, and comparison operations.
 */

#include "xoptimize.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xvalue.h"
#include <math.h>

/* ========== Constant Folding Implementation ========== */

/*
 * Fold binary operation.
 */
bool xr_opt_fold_binary(
    TokenType op,
    XrValue left,
    XrValue right,
    XrValue *result
) {
    // 1. Type check: only fold numeric operations
    if (!XR_IS_INT(left) && !XR_IS_FLOAT(left)) {
        return false;
    }
    if (!XR_IS_INT(right) && !XR_IS_FLOAT(right)) {
        return false;
    }
    
    // 2. Convert to floating point for calculation
    double l = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
    double r = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
    double res;
    
    // 3. Execute operation
    switch (op) {
        case TK_PLUS:
            res = l + r;
            break;
            
        case TK_MINUS:
            res = l - r;
            break;
            
        case TK_STAR:
            res = l * r;
            break;
            
        case TK_SLASH:
            // Avoid division by zero
            if (r == 0.0) {
                return false;
            }
            // int / int → int (type determines result)
            if (XR_IS_INT(left) && XR_IS_INT(right)) {
                *result = xr_int(XR_TO_INT(left) / XR_TO_INT(right));
                return true;
            }
            res = l / r;
            break;
            
            
        case TK_PERCENT:
            // Avoid division by zero
            if (r == 0.0) {
                return false;
            }
            res = fmod(l, r);
            break;
            
        default:
            // Unsupported operator (comparison operators handled elsewhere)
            return false;
    }
    
    /* 4. Safety check
     *    Avoid NaN and -0.0 issues
     *    NaN: uncertain result, handle at runtime
     *    -0.0: may cause sign issues
     */
    if (isnan(res)) {
        return false;  // Don't fold NaN
    }
    
    // Check -0.0
    if (res == 0.0 && signbit(res)) {
        return false;  // Don't fold -0.0
    }
    
    // 5. Folding succeeded - create value based on type
    // If both operands are integers and result is also integer, keep integer type
    if (XR_IS_INT(left) && XR_IS_INT(right) && res == (double)(long long)res) {
        *result = xr_int((xr_Integer)res);
    } else {
        *result = xr_float(res);
    }
    
    return true;
}

/*
 * Fold unary operation.
 */
bool xr_opt_fold_unary(
    TokenType op,
    XrValue value,
    XrValue *result
) {
    switch (op) {
        case TK_MINUS:
            // Numeric negation
            if (XR_IS_INT(value)) {
                xr_Integer num = XR_TO_INT(value);
                if (num == 0) {
                    return false;  // Avoid -0
                }
                *result = xr_int(-num);
                return true;
            }
            if (XR_IS_FLOAT(value)) {
                double num = XR_TO_FLOAT(value);
                if (num == 0.0) {
                    return false;  // Avoid -0.0
                }
                *result = xr_float(-num);
                return true;
            }
            return false;
            
        case TK_NOT:
            // Logical NOT
            if (XR_IS_BOOL(value)) {
                *result = xr_bool(!XR_TO_BOOL(value));
                return true;
            }
            // null can also be negated
            if (XR_IS_NULL(value)) {
                *result = xr_bool(1);  // !null = true
                return true;
            }
            return false;
            
        default:
            return false;
    }
}

/*
 * Fold comparison operation.
 */
bool xr_opt_fold_comparison(
    TokenType op,
    XrValue left,
    XrValue right,
    XrValue *result
) {
    // Only support numeric comparison
    if (!XR_IS_INT(left) && !XR_IS_FLOAT(left)) {
        return false;
    }
    if (!XR_IS_INT(right) && !XR_IS_FLOAT(right)) {
        return false;
    }
    
    // Convert to floating point
    double l = XR_IS_INT(left) ? (double)XR_TO_INT(left) : XR_TO_FLOAT(left);
    double r = XR_IS_INT(right) ? (double)XR_TO_INT(right) : XR_TO_FLOAT(right);
    
    // Execute comparison
    bool res_bool;
    switch (op) {
        case TK_EQ:
            res_bool = (l == r);
            break;
        case TK_NE:
            res_bool = (l != r);
            break;
        case TK_LT:
            res_bool = (l < r);
            break;
        case TK_LE:
            res_bool = (l <= r);
            break;
        case TK_GT:
            res_bool = (l > r);
            break;
        case TK_GE:
            res_bool = (l >= r);
            break;
        default:
            return false;
    }
    
    *result = xr_bool(res_bool);
    return true;
}


/* ========== Type-Aware Optimization Implementation ========== */

#include "../../runtime/value/xtype.h"

XrOptHint xr_opt_get_hint(struct XrType *type) {
    if (!type) return XR_OPT_NONE;
    
    // Known primitive types - can skip runtime type checks
    if (type->kind == XR_KIND_INT) return XR_OPT_KNOWN_INT;
    if (type->kind == XR_KIND_FLOAT) return XR_OPT_KNOWN_FLOAT;
    if (type->kind == XR_KIND_BOOL) return XR_OPT_KNOWN_BOOL;
    if (type->kind == XR_KIND_STRING) return XR_OPT_KNOWN_STRING;
    if (type->kind == XR_KIND_NULL) return XR_OPT_KNOWN_NULL;
    
    // Container types - future inline cache candidates
    if (type->kind == XR_KIND_ARRAY) return XR_OPT_INLINE_ARRAY;
    if (type->kind == XR_KIND_MAP) return XR_OPT_INLINE_MAP;
    if (type->kind == XR_KIND_INSTANCE) return XR_OPT_INLINE_FIELD;
    
    // Non-nullable types can skip null checks
    if (!type->is_nullable && !(type->kind == XR_KIND_NULL)) {
        return XR_OPT_ELIM_NULL_CHECK;
    }
    
    return XR_OPT_NONE;
}

bool xr_opt_can_unbox_arith(struct XrType *left, struct XrType *right) {
    if (!left || !right) return false;
    
    // Both must be numeric primitives
    bool left_numeric = (left->kind == XR_KIND_INT || left->kind == XR_KIND_FLOAT);
    bool right_numeric = (right->kind == XR_KIND_INT || right->kind == XR_KIND_FLOAT);
    
    if (left_numeric && right_numeric) {
        return true;
    }
    
    return false;
}

bool xr_opt_can_devirt(struct XrType *receiver_type, const char *method) {
    if (!receiver_type || !method) return false;
    
    // Can devirtualize if receiver is a known class instance
    if (receiver_type->kind == XR_KIND_INSTANCE) {
        return true;
    }
    
    return false;
}

