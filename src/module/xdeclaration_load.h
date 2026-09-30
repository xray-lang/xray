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

typedef struct XrDeclarationInputBudget {
    XrTomlParseBudget parsing;
    XrDeclarationBudget records;
} XrDeclarationInputBudget;

/* Establishes physical file containment, not semantic package identity. The
 * source owner must bind selectors to its explicit package authority and
 * prove every obligation before publication. parsing.work bounds the combined
 * parser and record work; work_used reports consumption even for ABSENT. No borrowed file/DOM data escapes. */
XR_FUNC XrDeclarationStatus xr_declaration_manifest_load(const char *physical_root,
    XrDeclarationInputBudget budget, XrDeclarationManifest **output, size_t *work_used);
#endif // XDECLARATION_LOAD_H
