/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeclaration_load.h - Owned declarations from one explicit package root
 *
 * KEY CONCEPT:
 *   Read only the root manifest; do not search parents or infer authority.
 */
#ifndef XDECLARATION_LOAD_H
#define XDECLARATION_LOAD_H
#include "xdeclaration_manifest.h"

typedef struct XrDeclarationInputLimits {
    XrTomlParseLimits parsing;
    XrDeclarationLimits records;
} XrDeclarationInputLimits;

/* Establishes physical file containment, not semantic package identity. The
 * source owner must bind selectors to its explicit package authority and
 * prove every obligation before publication. All temporary and published
 * owners use one ledger. ABSENT and every other failure preserve output. */
XR_FUNC XrDeclarationStatus xr_compile_declaration_manifest_load(
    XrCompileResources *resources, const char *physical_root,
    const XrDeclarationInputLimits *limits, XrDeclarationManifest **output);
#endif // XDECLARATION_LOAD_H
