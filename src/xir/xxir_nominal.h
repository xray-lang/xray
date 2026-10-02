/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal.h - Owned nominal declaration metadata
 *
 * KEY CONCEPT:
 *   Declaration scope and identity do not depend on runtime class objects.
 */
#ifndef XXIR_NOMINAL_H
#define XXIR_NOMINAL_H
#include "xxir.h"
#define XR_XIR_FIELD_PRIVATE 1u
#define XR_XIR_FIELD_PROTECTED 2u
#define XR_XIR_FIELD_MUTABLE 4u
typedef struct XrXirNominalField {
    XrXirLiteral name;
    XrXirType type;
    uint32_t flags;
} XrXirNominalField;
/* CLASS FINAL is a declaration fact, not inferred from carrier kind. */
#define XR_XIR_NOMINAL_FINAL 1u

typedef enum XrXirNominalKind { XR_XIR_NOMINAL_STRUCT, XR_XIR_NOMINAL_ENUM, XR_XIR_NOMINAL_CLASS } XrXirNominalKind;
typedef struct XrXirNominalVariant {
    XrXirLiteral name;
    uint32_t field_begin, field_count;
} XrXirNominalVariant;

typedef struct XrXirNominalDeclaration {
    XrXirLiteral module, name;
    uint32_t exported;
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
    const XrXirNominalField *fields;
    uint32_t field_count;
    uint32_t kind;
    const XrXirNominalVariant *variants;
    uint32_t variant_count;
    uint32_t flags;
} XrXirNominalDeclaration;
typedef struct XrXirNominalFieldIdentity {
    XrXirLiteral name;
    uint32_t flags;
} XrXirNominalFieldIdentity;
typedef struct XrXirNominalIdentity {
    XrXirLiteral module, name;
    uint32_t exported, arity;
    const XrXirNominalFieldIdentity *fields;
    uint32_t field_count;
    uint32_t kind;
    const XrXirNominalVariant *variants;
    uint32_t variant_count;
    uint32_t flags;
} XrXirNominalIdentity;
typedef struct XrXirNominalTable {
    const XrXirNominalDeclaration *declarations;
    uint32_t count;
    const XrXirNominalIdentity *identities;
} XrXirNominalTable;
/* Metadata helpers grant no executable type or source admission.
 * Outputs publish only after complete successful validation; work is never refunded. */
XR_FUNC XrXirStatus xr_xir_compile_nominal_structure_verify(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types);
XR_FUNC XrXirStatus xr_xir_compile_nominal_clone(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types, XrXirNominalTable **output);
/* Projection requires a verified declaration table and owns only identities. */
XR_FUNC XrXirStatus xr_xir_compile_nominal_project(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, XrXirNominalTable **output);
XR_FUNC void xr_xir_compile_nominal_free(XrXirNominalTable *table);
typedef enum XrXirNominalAccess {
    XR_XIR_NOMINAL_CONSTRUCT, XR_XIR_NOMINAL_READ, XR_XIR_NOMINAL_WRITE, XR_XIR_NOMINAL_TYPE
} XrXirNominalAccess;
/* Requires a verified module; declaration ownership grants no generic facts. */
XR_FUNC XrXirStatus xr_xir_compile_nominal_access(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, uint32_t declaration, uint32_t field, XrXirNominalAccess access);
XR_FUNC XrXirStatus xr_xir_compile_type_access(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type);
#endif // XXIR_NOMINAL_H
