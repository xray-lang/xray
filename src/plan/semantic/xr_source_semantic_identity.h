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

#endif  // XR_SOURCE_SEMANTIC_IDENTITY_H
