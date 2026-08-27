/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_source_semantic_identity.h - Canonical source identity authority
 */
#ifndef XR_SOURCE_SEMANTIC_IDENTITY_H
#define XR_SOURCE_SEMANTIC_IDENTITY_H

#include "xr_program_semantic_closure.h"

#define XR_SOURCE_SEMANTIC_IDENTITY_VERSION UINT32_C(1)

XR_FUNC bool xr_source_semantic_module_authority(const char *canonical_module,
                                                 XrFingerprint source_fingerprint,
                                                 XrProgramSemanticModuleInput *out,
                                                 XrFingerprint *authority_fingerprint);

XR_FUNC bool xr_source_semantic_callsite_identity(XrFingerprint source_fingerprint,
                                                  XrStableId module_identity,
                                                  XrStableId caller_declaration,
                                                  XrProgramSemanticSourceLocator locator,
                                                  XrStableId *out);

XR_FUNC bool xr_source_semantic_scalar_i64_export_fingerprint(
    const XrProgramSemanticModuleInput *module, XrStableId exported_declaration,
    XrStableId exported_function, XrFingerprint signature, XrFingerprint effect,
    uint64_t capability_mask, XrFingerprint *out);

XR_FUNC bool xr_source_semantic_scalar_i64_import_binding(
    const XrProgramSemanticModuleInput *source, const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId exported_declaration,
    XrStableId exported_function, XrStableId return_type, XrFingerprint signature,
    XrFingerprint effect, uint64_t capability_mask, XrStableId *out);

/* Canonical whole-source module-graph identities. These deliberately bind
 * only source/module/dependency facts and never manufacture functions or
 * executable-plan authority. */
XR_FUNC bool xr_source_semantic_module_graph_policy(XrFingerprint *out);
XR_FUNC bool xr_source_semantic_module_graph_exports(
    const XrProgramSemanticModuleInput *module, XrFingerprint *out);
XR_FUNC bool xr_source_semantic_module_graph_import_binding(
    const XrProgramSemanticModuleInput *source,
    const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId *out);
XR_FUNC bool xr_source_semantic_module_graph_dependency_contract(
    const XrProgramSemanticModuleInput *source,
    const XrProgramSemanticModuleInput *dependency,
    XrProgramSemanticSourceLocator import_locator, XrStableId resolver_binding,
    XrFingerprint *out);

#endif  // XR_SOURCE_SEMANTIC_IDENTITY_H
