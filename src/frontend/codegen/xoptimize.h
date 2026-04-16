/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoptimize.h - Constant folding optimization
 *
 * KEY CONCEPT:
 *   Evaluates constant expressions at compile time to reduce
 *   runtime computation and bytecode size.
 */

#ifndef XOPTIMIZE_H
#define XOPTIMIZE_H

#include "../parser/xast.h"
#include "../../runtime/value/xvalue.h"
#include "../lexer/xlex.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

/* ========== Constant Folding Optimization ========== */

/*
** Try to fold binary operation constants
** 
** Parameters:
**   op     - Operator type (TK_PLUS, TK_MINUS, etc.)
**   left   - Left operand (must be number)
**   right  - Right operand (must be number)
**   result - Output result
** 
** Returns:
**   true  - Fold successful, result contains computed value
**   false - Cannot fold (non-numeric operands, division by zero, NaN, etc.)
** 
** Examples:
**   Fold "3 + 5" -> 8
**   Fold "10 * 2" -> 20
**   Cannot fold "1 / 0" -> false (division by zero)
*/
XR_FUNC bool xr_opt_fold_binary(
    TokenType op,
    XrValue left,
    XrValue right,
    XrValue *result
);

/*
** Try to fold unary operation constants
** 
** Parameters:
**   op     - Operator type (TK_MINUS, TK_NOT)
**   value  - Operand
**   result - Output result
** 
** Returns:
**   true  - Fold successful
**   false - Cannot fold
** 
** Examples:
**   Fold "-5" -> -5
**   Fold "!true" -> false
*/
XR_FUNC bool xr_opt_fold_unary(
    TokenType op,
    XrValue value,
    XrValue *result
);

/*
** Try to fold comparison operation constants
** 
** Parameters:
**   op     - Operator type (TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE)
**   left   - Left operand (must be number)
**   right  - Right operand (must be number)
**   result - Output result
** 
** Returns:
**   true  - Fold successful
**   false - Cannot fold
*/
XR_FUNC bool xr_opt_fold_comparison(
    TokenType op,
    XrValue left,
    XrValue right,
    XrValue *result
);

/* ========== Type-Aware Optimization Hints ========== */

// Forward declaration for XrType
struct XrType;

/*
 * Optimization hint from type analysis (FUTURE WORK)
 *
 * NOTE: xray uses a 16-byte tagged union, so int/float/bool are stored
 * inline WITHOUT heap allocation. "Unboxing" here means skipping type
 * checks, not memory layout changes.
 *
 * Current status:
 * - XR_OPT_UNBOX_*: Skip runtime type checks (minor benefit)
 * - XR_OPT_INLINE_*: Not implemented (needs IC support in VM)
 * - XR_OPT_DEVIRT_CALL: Not implemented (needs class hierarchy)
 * - XR_OPT_ELIM_NULL_CHECK: Can be useful with flow analysis
 */
typedef enum XrOptHint {
    XR_OPT_NONE = 0,
    XR_OPT_KNOWN_INT,       // Type is definitely int, skip type check
    XR_OPT_KNOWN_FLOAT,     // Type is definitely float
    XR_OPT_KNOWN_BOOL,      // Type is definitely bool
    XR_OPT_KNOWN_STRING,    // Type is definitely string
    XR_OPT_KNOWN_NULL,      // Type is definitely null
    XR_OPT_INLINE_ARRAY,    // (Future) Array access inline cache
    XR_OPT_INLINE_MAP,      // (Future) Map access inline cache
    XR_OPT_INLINE_FIELD,    // (Future) Field access inline cache
    XR_OPT_DEVIRT_CALL,     // (Future) Devirtualize method call
    XR_OPT_ELIM_NULL_CHECK, // Skip null check (non-nullable type)
} XrOptHint;

// Get optimization hint from type
XR_FUNC XrOptHint xr_opt_get_hint(struct XrType *type);

// Check if type allows unboxed arithmetic
XR_FUNC bool xr_opt_can_unbox_arith(struct XrType *left, struct XrType *right);

// Check if method call can be devirtualized (class is known)
XR_FUNC bool xr_opt_can_devirt(struct XrType *receiver_type, const char *method);

#endif // XOPTIMIZE_H
