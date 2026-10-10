/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_metadata_domain_internal.h - Authentic declaration metadata for common proofs
 *
 * KEY CONCEPT:
 *   Declaration obligations do not require executable instruction storage.
 *   A metadata domain supplies no body admission or execution permission.
 */
#ifndef XXIR_METADATA_DOMAIN_INTERNAL_H
#define XXIR_METADATA_DOMAIN_INTERNAL_H
#include "xxir_constraint_proof.h"

/* All descriptor/declaration/signature priors belong to the same admitted
 * source. An owned material preserves these records and real ordinals; it
 * must independently preserve and recheck the complete executable body. */
typedef struct XirMetadataDomain {
    const XrXirTypes *types;
    const XrXirDeclarations *declarations;
    const XrXirGeneric *generics;
    const XrXirDefaultTable *defaults;
    uint32_t function_count;
} XirMetadataDomain;
typedef struct XirMetadataProofContext {
    const XirMetadataDomain *domain;
    XrXirDeclarationContext owner;
} XirMetadataProofContext;
typedef struct XirMetadataArguments {
    const XirMetadataDomain *declaration_domain;
    XrXirDeclarationContext declaration;
    const XrXirType *arguments;
    uint32_t argument_count;
} XirMetadataArguments;

/* A missing raw function vector supplies no authentic function ordinal.
 * Closed/nominal/interface proof contexts retain their existing behavior.
 * This adapter neither verifies a module nor invents executable storage. */
static inline XirMetadataDomain xir_metadata_domain_from_module(const XrXirModule *module) {
    if (!module) return (XirMetadataDomain){0};
    return (XirMetadataDomain){module->types,module->declarations,module->generics,module->defaults,
        module->functions ? module->function_count : 0};
}
static inline XirMetadataProofContext xir_metadata_context_from_raw(
    const XrXirProofContext *raw,XirMetadataDomain *domain) {
    *domain=xir_metadata_domain_from_module(raw ? raw->module : NULL);
    return (XirMetadataProofContext){raw && raw->module ? domain : NULL,
        raw ? raw->owner : (XrXirDeclarationContext){0}};
}

/* These operations preserve their original semantic cores. The caller must
 * already have admitted the complete body, generic-call/Unit binders and
 * descriptor structure. Metadata alone never discharges those priors. */
XR_FUNC XrXirStatus xir_metadata_call_type_matches(const XrXirCompileContext *context,
    const XirMetadataDomain *domain,uint32_t caller,const XrXirInstruction *call,
    XrXirType expected,XrXirType actual);
XR_FUNC XrXirStatus xir_metadata_default_lookup(const XrXirCompileContext *context,
    const XirMetadataDomain *domain,uint32_t owner,uint32_t ordinal,
    const XrXirDefaultBinding **output);
#endif // XXIR_METADATA_DOMAIN_INTERNAL_H
