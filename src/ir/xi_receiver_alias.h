/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_receiver_alias.h - sealed "this call returns its own receiver" fact
 *
 * Some native members hand back the receiver itself (`return self`) so callers
 * can chain: Array.reverse/sort/fill/reserve/resize, StringBuilder.append/clear.
 * The result and the receiver are then ONE object under two SSA names, at +0 —
 * the callee added no reference.
 *
 * The fact is declared once in compiler-owned typed native-member contracts
 * and sealed by lowering as XiValue.result_alias_operand. This module only
 * reads that target-neutral fact; it never queries method names or analyzer
 * tables. ARC insertion, its independent verifier, SemanticPlan, VM, and AOT
 * therefore consume the same immutable alias evidence.
 */

#ifndef XI_RECEIVER_ALIAS_H
#define XI_RECEIVER_ALIAS_H

#include "xi.h"
#include "../base/xdefs.h"

#include <stdbool.h>

/* True when `v` is a call whose RC result is its own receiver (operand 0)
 * rather than a fresh reference. False for every other value, including calls
 * whose result merely has the receiver's TYPE (Array.concat, Array.filter and
 * friends all return a new array). */
XR_FUNC bool xi_call_result_aliases_receiver(const XiValue *v);

#endif /* XI_RECEIVER_ALIAS_H */
