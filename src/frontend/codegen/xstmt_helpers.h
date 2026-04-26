/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstmt_helpers.h - Shared helpers for statement-level type bridging
 *
 * KEY CONCEPT:
 *   These four helpers are used by both the "simple" and "typed"
 *   statement compilers (xstmt_simple.c, xstmt_typed.c, and
 *   xstmt_assignment.c). They sit at the boundary between the
 *   compile-time XrType system and the VM runtime tag system, doing
 *   the small bookkeeping needed to keep raw I64/F64 slots boxed at
 *   assignment edges, to deep-copy value-type structs into the
 *   compiler's struct area, and to format human-readable type names
 *   for diagnostics.
 *
 * WHY THIS HEADER EXISTS:
 *   xstmt_simple.c is split into xstmt_simple.c +
 *   xstmt_typed.c. The four helpers below were file-static in
 *   xstmt_simple.c but used by both halves of the split. Promoting
 *   them to a small shared header is preferable to duplicating
 *   them or to leaving them inline in just one TU.
 */

#ifndef XSTMT_HELPERS_H
#define XSTMT_HELPERS_H

#include "../../base/xdefs.h"

struct XrCompilerContext;
struct XrCompiler;
struct XrEmitter;
struct XrType;
struct XrExprDesc;
struct AstNode;

// Emit value-type copy: OP_STRUCT_COPY for structs with a known
// layout, OP_COPY as fallback. Allocates new struct_area space and
// encodes the slot offset in the C operand.
XR_FUNC void xstmt_emit_value_copy(struct XrCompilerContext *ctx,
                                   struct XrCompiler *compiler,
                                   int dest_reg, int src_reg,
                                   struct XrType *compile_type);

// Human-readable type name for diagnostics. Result is either a
// static string (primitive types) or a thread-unsafe static buffer
// (union types). Caller must not free.
XR_FUNC const char *xstmt_type_flag_name(struct XrType *type);

// BOX raw expression at assignment boundary. Locals are always
// tagged -- if the source XrExprDesc is raw I64/F64, emit BOX_I64 /
// BOX_F64 in place; otherwise no-op.
XR_FUNC void xstmt_emit_box_if_raw(struct XrEmitter *emitter, int reg,
                                   const struct XrExprDesc *expr);

// Emit OP_CHECKTYPE for Json/JsonValue -> concrete-type coercion.
// Uses bitmask encoding: single primitive = one bit, union = OR of
// member bits. No-op when the source type is not a JSON coercion
// candidate.
XR_FUNC void xstmt_emit_json_checktype(struct XrCompilerContext *ctx,
                                       struct XrCompiler *compiler,
                                       int reg,
                                       struct XrType *declared_type,
                                       struct AstNode *init_expr);

#endif // XSTMT_HELPERS_H
