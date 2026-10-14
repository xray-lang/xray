/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_description.h - Borrowed facts from the canonical compiler type pool
 *
 * KEY CONCEPT:
 *   Descriptions share the original pool and grant no operation authority.
 */
#ifndef XXIR_TYPE_DESCRIPTION_H
#define XXIR_TYPE_DESCRIPTION_H
#include "xxir_nominal.h"

/* Kind zero denotes a builtin scalar; other kinds are XrXirTypeKind values.
 * The original pool must be structurally admitted and remain unchanged while
 * these synchronous views are in use. References die with that pool. This is
 * not a pool certificate, an owning constant, or a visibility/access grant. */
typedef struct XrXirTypeDescription {
    XrXirType type;
    uint32_t kind;
    const XrXirTypeNode *node;
    XrXirLiteral module, name;
    uint32_t declaration, nominal_kind, field_count;
    const XrXirNominalType *instance;
    const XrXirNominalDeclaration *definition;
    const XrXirNominalIdentity *identity;
    const XrXirNominalVariant *variants;
    uint32_t variant_count;
} XrXirTypeDescription;

typedef struct XrXirFieldDescription {
    XrXirLiteral name;
    XrXirType type, template_type;
    uint32_t flags;
    bool resolved;
} XrXirFieldDescription;

/* Each query charges one original-ledger work unit without allocation or refund.
 * All failures preserve output. Field order is declaration order for nominals
 * and positional order for tuples; flags describe facts, never permissions.
 * A declaration may have only a template field type before substitution. In
 * that case resolved is false and type is Unit; consumers must use the normal
 * substitution checker rather than treating the template as a resolved type. */
XR_FUNC XrXirStatus xr_xir_compile_type_description(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, XrXirTypeDescription *output);
XR_FUNC XrXirStatus xr_xir_compile_description_field(const XrXirCompileContext *context,
    const XrXirTypeDescription *description, uint32_t field, XrXirFieldDescription *output);
#endif // XXIR_TYPE_DESCRIPTION_H
