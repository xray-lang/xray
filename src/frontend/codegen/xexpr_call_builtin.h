/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_call_builtin.h - Builtin function call dispatch
 *
 * KEY CONCEPT:
 *   The codegen layer recognises a fixed set of builtin global names
 *   (`assert`, `int`, `Array`, `typeof`, ...) and emits specialised
 *   opcodes for them instead of OP_CALL. This file owns the lookup
 *   table, the per-builtin compilers, and the dispatcher.
 *
 *   The builtin section lives here so the xexpr_call.c dispatcher
 *   stays focused on the regular / method / tail-call paths. The split
 *   is by cohesion, not size.
 *
 * BOUNDARY:
 *   Builtins recognised here are *call-site* builtins (the call
 *   expression's callee is one of a fixed set of names). Method-call
 *   builtins like `arr.map(...)` go through xexpr_call.c's method
 *   dispatch, not through this table.
 */

#ifndef XEXPR_CALL_BUILTIN_H
#define XEXPR_CALL_BUILTIN_H

#include <stdbool.h>
#include "../../base/xdefs.h"

struct XrCompilerContext;
struct XrCompiler;
struct CallExprNode;

/*
 * Try to compile the call as a builtin function.
 *
 * Returns:
 *   >= 0  the call was dispatched as a builtin; this is the result
 *         register number (and, when `is_tail` is true, the
 *         implicit RETURN1 has been emitted, so the caller can
 *         simply propagate the value).
 *   < 0   not a builtin. The caller MUST fall through to the
 *         regular method-call / OP_CALL dispatch path. The exact
 *         negative value is unspecified and not interesting.
 */
XR_FUNC int xr_compile_call_builtin(struct XrCompilerContext *ctx,
                                    struct XrCompiler *compiler,
                                    struct CallExprNode *node,
                                    bool is_tail);

#endif // XEXPR_CALL_BUILTIN_H
