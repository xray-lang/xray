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
XR_FUNC bool xr_opt_fold_binary(TokenType op, XrValue left, XrValue right, XrValue *result);

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
XR_FUNC bool xr_opt_fold_unary(TokenType op, XrValue value, XrValue *result);

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
XR_FUNC bool xr_opt_fold_comparison(TokenType op, XrValue left, XrValue right, XrValue *result);

/* ========== Type-Aware Optimization Hints ========== */

// The XrType-classification helpers (XrOptHint enum + xr_opt_get_hint
// / xr_opt_can_unbox_arith / xr_opt_can_devirt) live in
// runtime/value/xtype_opt_hint.h so the analyzer's JIT-metadata pass
// can use them without pulling a downward
// analyzer -> codegen include. xoptimize.{h,c} now only contains
// the codegen-only constant-folding helpers (xr_opt_fold_*).

#endif  // XOPTIMIZE_H
