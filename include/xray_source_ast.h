/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_ast.h - Owned syntax at the ordinary public compiler boundary
 */
#ifndef XRAY_SOURCE_AST_H
#define XRAY_SOURCE_AST_H
#include "xray_source.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrSourceAst XrSourceAst;
/* Parses one independent request. The AST copies input and locator, owns its
 * parser arena, and survives request destruction. Parsing grants no execution
 * or import authority. Failure facts remain available without allocation. */
XRAY_API XrSourceStatus xray_source_parse(const XrSourceText *, const XrSourceCompileOptions *,
    XrSourceAst **, XrSourceFailure *, XrSourceDiagnostic **);
/* Borrows the immutable owning syntax through an independent build request.
 * Imports, checking, specialization, lowering and target preparation complete
 * before a ready Artifact is published. The entry is never reparsed or written
 * to disk. Artifact, AST and request storage have independent lifetimes. */
XRAY_API XrSourceStatus xray_source_build_ast(const XrSourceAst *, const XrSourceCompileOptions *,
    XrSourceArtifact **, XrSourceFailure *, XrSourceDiagnostic **);
XRAY_API void xray_source_ast_free(XrSourceAst *);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_AST_H
