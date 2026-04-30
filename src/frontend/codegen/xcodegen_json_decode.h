/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcodegen_json_decode.h - Compiler-generated Json.decode<T>() bytecode
 *
 * When the compiler sees Json.decode<User>(data), it generates inline
 * bytecode that validates and extracts fields from the Json value,
 * returning T? (null on validation failure).
 */

#ifndef XCODEGEN_JSON_DECODE_H
#define XCODEGEN_JSON_DECODE_H

#include "xcompiler.h"
#include "xcompiler_context.h"
#include "../../runtime/value/xtype.h"

/*
 * Compile Json.decode<T>(data) into inline validation + extraction bytecode.
 *
 * @param ctx       Compiler context
 * @param compiler  Current compiler
 * @param data_node AST node for the data argument
 * @param target    Resolved object type T (must be XR_KIND_OBJECT with fields)
 * @return          Register holding the result (T? — object or null)
 *
 * Returns -1 on compilation error.
 */
XR_FUNC int xr_compile_json_decode(XrCompilerContext *ctx, XrCompiler *compiler,
                                   struct AstNode *data_node, XrType *target);

#endif  // XCODEGEN_JSON_DECODE_H
