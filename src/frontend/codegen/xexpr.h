/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr.h - Expression compiler
 *
 * KEY CONCEPT:
 *   Compiles all expression types with modular organization.
 *   Each expression type has its own implementation file.
 */

#ifndef XEXPR_H
#define XEXPR_H

#include "../parser/xast.h"
#include "../../runtime/value/xchunk.h"
#include "xregalloc.h"
#include "xexpr_desc.h"
#include "../../base/xdefs.h"

typedef struct XrCompilerContext XrCompilerContext;
typedef struct XrCompiler XrCompiler;

XR_FUNC XrExprDesc xr_compile_expr(XrCompilerContext *ctx, XrCompiler *c, AstNode *node);

XR_FUNC XrExprDesc compile_literal(XrCompilerContext *ctx, XrCompiler *c, LiteralNode *node);
XR_FUNC XrExprDesc compile_binary(XrCompilerContext *ctx, XrCompiler *c, BinaryNode *node,
                                  AstNodeType type);
XR_FUNC XrExprDesc compile_comparison(XrCompilerContext *ctx, XrCompiler *c, BinaryNode *node,
                                      AstNodeType type);
XR_FUNC XrExprDesc compile_is_expr(XrCompilerContext *ctx, XrCompiler *c, IsExprNode *node);
XR_FUNC int compile_and(XrCompilerContext *ctx, XrCompiler *c, BinaryNode *node);
XR_FUNC int compile_or(XrCompilerContext *ctx, XrCompiler *c, BinaryNode *node);

XR_FUNC XrExprDesc compile_unary(XrCompilerContext *ctx, XrCompiler *c, UnaryNode *node,
                                 AstNodeType type);

XR_FUNC XrExprDesc compile_variable(XrCompilerContext *ctx, XrCompiler *c, VariableNode *node);

XR_FUNC XrExprDesc compile_call(XrCompilerContext *ctx, XrCompiler *c, CallExprNode *node);

XR_FUNC XrExprDesc compile_array_literal(XrCompilerContext *ctx, XrCompiler *c,
                                         ArrayLiteralNode *node);
XR_FUNC XrExprDesc compile_object_literal(XrCompilerContext *ctx, XrCompiler *c,
                                          ObjectLiteralNode *node);
XR_FUNC XrExprDesc compile_map_literal(XrCompilerContext *ctx, XrCompiler *c, MapLiteralNode *node);
XR_FUNC XrExprDesc compile_set_literal(XrCompilerContext *ctx, XrCompiler *c, SetLiteralNode *node);
XR_FUNC XrExprDesc compile_index_get(XrCompilerContext *ctx, XrCompiler *c, IndexGetNode *node);
XR_FUNC XrExprDesc compile_slice_expr(XrCompilerContext *ctx, XrCompiler *c, SliceExprNode *node);

XR_FUNC XrExprDesc compile_new_expr(XrCompilerContext *ctx, XrCompiler *c, NewExprNode *node);
XR_FUNC XrExprDesc compile_struct_literal(XrCompilerContext *ctx, XrCompiler *c,
                                          StructLiteralNode *node);
XR_FUNC XrExprDesc compile_member_access(XrCompilerContext *ctx, XrCompiler *c,
                                         MemberAccessNode *node);

XR_FUNC XrExprDesc compile_enum_access(XrCompilerContext *ctx, XrCompiler *c, EnumAccessNode *node);
XR_FUNC XrExprDesc compile_enum_convert(XrCompilerContext *ctx, XrCompiler *c,
                                        EnumConvertNode *node);
XR_FUNC XrExprDesc compile_enum_index(XrCompilerContext *ctx, XrCompiler *c, EnumIndexNode *node);

XR_FUNC int compile_match_expr(XrCompilerContext *ctx, XrCompiler *c, MatchExprNode *node);

#endif  // XEXPR_H
