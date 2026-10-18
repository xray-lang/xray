/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_ast_owner.h - Real public parser ownership for Source history
 */
#ifndef XSOURCE_AST_OWNER_H
#define XSOURCE_AST_OWNER_H
#include "../../include/xray_source_ast.h"
#include "../xir/xxir_source_ast_owner.h"
/* Descriptor construction borrows the AST. A Source generation acquires its
 * actual pin during prepare; the caller may then release its public handle.
 * Calls are serialized, input syntax stays immutable and no quota is copied. */
XR_FUNC XrSourceStatus xr_source_ast_owned_input(const XrSourceAst *ast,XrXirSourceAstOwner *output);
#endif // XSOURCE_AST_OWNER_H
