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
typedef enum XrXirNominalKind { XR_XIR_NOMINAL_STRUCT, XR_XIR_NOMINAL_ENUM } XrXirNominalKind;
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
} XrXirNominalIdentity;
typedef struct XrXirNominalTable {
    const XrXirNominalDeclaration *declarations;
    uint32_t count;
    const XrXirNominalIdentity *identities;
} XrXirNominalTable;
/* Metadata helpers grant no executable type or source admission.
 * Budgets and outputs publish only after complete successful validation. */
XR_FUNC XrXirStatus xr_xir_nominal_structure_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_nominal_clone(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget, XrXirNominalTable **output);
/* Projection requires a verified declaration table and owns only identities. */
XR_FUNC XrXirStatus xr_xir_nominal_project(const XrXirNominalTable *table,
    XrXirBudget *budget, XrXirNominalTable **output);
XR_FUNC void xr_xir_nominal_free(XrXirNominalTable *table);
typedef enum XrXirNominalAccess {
    XR_XIR_NOMINAL_CONSTRUCT, XR_XIR_NOMINAL_READ, XR_XIR_NOMINAL_WRITE, XR_XIR_NOMINAL_TYPE
} XrXirNominalAccess;
/* Requires a verified module; declaration ownership grants no generic facts. */
XR_FUNC XrXirStatus xr_xir_nominal_access(const XrXirModule *module, uint32_t function,
    uint32_t declaration, uint32_t field, XrXirNominalAccess access, uint64_t *work);
XR_FUNC XrXirStatus xr_xir_type_access(const XrXirModule *module, uint32_t function,
    XrXirType type, XrXirBudget *remaining);
#endif // XXIR_NOMINAL_H
