/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_query.h - Owned facts from the source semantic owner
 *
 * KEY CONCEPT:
 *   Queries observe immutable checked decisions and cannot execute or infer.
 */
#ifndef XXIR_SOURCE_QUERY_H
#define XXIR_SOURCE_QUERY_H
#include "xxir.h"
#include "../base/xstable_id.h"
typedef struct XrXirSourceDiagnostic {
    XrXirStatus status;
    uint32_t module;
    int line, column;
    char message[192];
} XrXirSourceDiagnostic;
typedef struct XrXirSourceRange {
    uint32_t module;
    int line, column, end_line, end_column;
} XrXirSourceRange;
typedef struct XrXirSourceType {
    XrXirType type;
    uint32_t generic_owner;
    bool known;
} XrXirSourceType;
typedef enum XrXirSourceDeclarationKind {
    XR_XIR_SOURCE_FUNCTION, XR_XIR_SOURCE_BINDING, XR_XIR_SOURCE_PARAMETER,
    XR_XIR_SOURCE_IMPORT, XR_XIR_SOURCE_MODULE, XR_XIR_SOURCE_TYPE,
    XR_XIR_SOURCE_TYPE_PARAMETER, XR_XIR_SOURCE_MEMBER, XR_XIR_SOURCE_INTRINSIC
} XrXirSourceDeclarationKind;
typedef struct XrXirSourceDeclaration {
    uint32_t id, parent, target;
    XrXirSourceDeclarationKind kind;
    const char *name;
    XrXirSourceRange range;
    XrXirSourceType type;
    const XrXirSourceType *parameters;
    uint32_t parameter_count;
    bool mutable, exported;
    uint32_t native_identity;
    const char *signature;
    uint32_t generic_parent, generic_parent_count, generic_parameter_count;
    const uint32_t *generic_constraints;
} XrXirSourceDeclaration;
typedef enum XrXirSourceAccess {
    XR_XIR_SOURCE_READ, XR_XIR_SOURCE_WRITE, XR_XIR_SOURCE_READ_WRITE,
    XR_XIR_SOURCE_CALL, XR_XIR_SOURCE_FUNCTION_VALUE, XR_XIR_SOURCE_TYPE_USE
} XrXirSourceAccess;
typedef struct XrXirSourceReference {
    XrXirSourceRange range;
    uint32_t declaration, target;
    XrXirSourceAccess access;
} XrXirSourceReference;
typedef struct XrXirSourceExpression {
    uint32_t node;
    XrXirSourceRange range;
    XrXirSourceType type;
} XrXirSourceExpression;
typedef struct XrXirSourceQueryModule {
    const char *identity, *path;
    XrFingerprint fingerprint;
} XrXirSourceQueryModule;
typedef struct XrXirSourceView {
    bool complete;
    XrXirSourceDiagnostic diagnostic;
    const XrXirSourceQueryModule *modules;
    uint32_t module_count;
    const XrXirSourceDeclaration *declarations;
    uint32_t declaration_count;
    const XrXirSourceReference *references;
    uint32_t reference_count;
    const XrXirSourceExpression *expressions;
    uint32_t expression_count;
    const XrXirTypes *types;
} XrXirSourceView;
typedef struct XrXirSourceSnapshot XrXirSourceSnapshot;
typedef struct XrXirSourceResult {
    XrXirArtifact *checked;
    XrXirSourceSnapshot *snapshot;
} XrXirSourceResult;
XR_FUNC const XrXirSourceView *xr_xir_source_snapshot_view(const XrXirSourceSnapshot *snapshot);
XR_FUNC void xr_xir_source_snapshot_free(XrXirSourceSnapshot *snapshot);
XR_FUNC void xr_xir_source_result_free(XrXirSourceResult *result);
#endif // XXIR_SOURCE_QUERY_H
