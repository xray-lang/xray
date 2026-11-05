/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_literal_internal.h - Owned literal definitions from genuine instance results
 *
 * KEY CONCEPT:
 *   Current verified instance owners supply values and substitutions. The same
 *   ordinary recipe writer needs no AST and preserves the generic definition.
 */
#ifndef XXIR_LITERAL_INTERNAL_H
#define XXIR_LITERAL_INTERNAL_H
#include "xxir_ctfe_instance_internal.h"
#include "xxir_construction.h"

typedef struct XirLiteralRequest {
    const XrXirCompileContext *context;
    const XirCtfeInstanceBatch *instances;
    uint32_t count;
    XrCompileRequestControl *control; /* Borrowed only during this synchronous request. */
} XirLiteralRequest;
typedef struct XirLiteralBinding {
    uint32_t definition,instance,provider;
    const XrXirType *arguments;
    uint32_t argument_count;
} XirLiteralBinding;
typedef struct XirLiteralDefinitions {
    XrXirArtifact *definition;
    XirLiteralBinding *bindings;
    uint32_t count;
} XirLiteralDefinitions;

/* The batch and context retain the same original finite compiler ledger.
 * The batch is immutable throughout complete current verification and receiving.
 * No caller Value, foreign type ordinal, AST or validation hint is accepted.
 * All original generic definitions and declarations survive independent checking.
 * Bindings own complete ordered arguments in the returned definition's type pool.
 * Failure preserves an entirely zero output and never refunds consumed fees. */
XR_FUNC XrXirStatus xr_xir_compile_instance_literals(const XirLiteralRequest *request,
    XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic);
/* A genuine compiler-only pending root supplies the exact site. Its binding
 * enters construction before complete declaration checking, so closed opaque
 * substitutions retain their proven generic origin without field authority. */
XR_FUNC XrXirStatus xr_xir_compile_instance_literals_at(const XirLiteralRequest *request,
    const XrXirComptimeSite *site,XirLiteralDefinitions *output,XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_instance_literals_free(XirLiteralDefinitions *result);
#endif // XXIR_LITERAL_INTERNAL_H
