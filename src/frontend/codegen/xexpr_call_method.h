/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_call_method.h - Method-call dispatch for codegen (C-02 split)
 *
 * KEY CONCEPT:
 *   Pre-Phase-3 the entire `obj.method(...)` lowering pipeline lived
 *   inline inside `compile_call_internal` in xexpr_call.c, sharing
 *   the file with the regular CALL / CALLSELF / TAILCALL paths.
 *   That made the function 1300+ lines and forced anyone touching
 *   any single inline optimisation (StringBuilder, Map, Channel,
 *   Coro, INVOKE_DIRECT, ...) to read past everything else.
 *
 *   Phase 3 (C-02) lifts the method branch into its own TU. The
 *   call dispatcher in xexpr_call.c now reads:
 *
 *       int builtin_result = xr_compile_call_builtin(...);
 *       if (builtin_result >= 0) return builtin_result;
 *       if (callee is MEMBER_ACCESS)
 *           return xr_compile_call_method(...);
 *       // ... regular function call path ...
 *
 *   This file owns every method-call codegen path:
 *     - Method self-recursion -> OP_LOOP_BACK
 *     - Static method calls (Coro.* / class registry hits)
 *     - StringBuilder / toString inlines (OP_STRBUF_*)
 *     - String.substring inline (OP_SUBSTRING)
 *     - Map.get / set / increment inlines (OP_MAP_*)
 *     - Array.push inline (OP_TARRAY_PUSH / OP_INVOKE_BUILTIN)
 *     - Channel send / recv / try / timeout / close / isClosed
 *     - Coroutine.info via OP_CORO_CTRL
 *     - Builtin-type INVOKE_BUILTIN (Map / Array / String / Set / Json)
 *     - INVOKE_DIRECT for compile-time-known class instances
 *     - Chain calls with primitive receivers
 *     - Generic OP_INVOKE / OP_INVOKE_TAIL fallback
 *
 * BOUNDARY:
 *   The split is by cohesion, not size: every code path that depends
 *   on `node->callee->type == AST_MEMBER_ACCESS` lives here, and
 *   nothing else does.
 */

#ifndef XEXPR_CALL_METHOD_H
#define XEXPR_CALL_METHOD_H

#include <stdbool.h>
#include "../../base/xdefs.h"

struct XrCompilerContext;
struct XrCompiler;
struct CallExprNode;

/*
 * Compile a method call (callee is AST_MEMBER_ACCESS).
 *
 * Pre-condition: `node->callee->type == AST_MEMBER_ACCESS`. The
 * caller (compile_call_internal in xexpr_call.c) guarantees this
 * before invoking; the function asserts it via XR_DCHECK in
 * release-asserts builds.
 *
 * Returns the result register holding the call's value. A return
 * value of -1 means the call has no usable result (void / tail
 * call) -- the caller treats it the same way the original inline
 * path did.
 */
XR_FUNC int xr_compile_call_method(struct XrCompilerContext *ctx,
                                   struct XrCompiler *compiler,
                                   struct CallExprNode *node,
                                   bool is_tail);

#endif // XEXPR_CALL_METHOD_H
