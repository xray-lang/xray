/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeclaration_manifest.h - Owned package declaration selectors
 *
 * KEY CONCEPT:
 *   Schema admission supplies obligations, never authority or proved effects.
 */
#ifndef XDECLARATION_MANIFEST_H
#define XDECLARATION_MANIFEST_H
#include "../base/xtoml.h"

typedef struct XrDeclarationRecord {
    char *module, *owner, *name;
    char **parameters;
    uint32_t parameter_count;
    bool no_suspend;
} XrDeclarationRecord;
typedef struct XrDeclarationManifest {
    XrDeclarationRecord *records;
    uint32_t count;
} XrDeclarationManifest;
typedef struct XrDeclarationBudget {
    size_t bytes;
    uint32_t records, parameters, work;
} XrDeclarationBudget;
typedef enum XrDeclarationStatus {
    XR_DECLARATION_OK,
    XR_DECLARATION_ABSENT,
    XR_DECLARATION_INVALID,
    XR_DECLARATION_LIMIT,
    XR_DECLARATION_OUT_OF_MEMORY,
    XR_DECLARATION_FORBIDDEN,
    XR_DECLARATION_IO
} XrDeclarationStatus;

/* The parsed DOM is borrowed only during admission. All successful records own
 * their text. The caller must separately establish file and package authority. */
XR_FUNC XrDeclarationStatus xr_declaration_manifest_read(XrTomlValue *document,
    XrDeclarationBudget budget, XrDeclarationManifest **output, size_t *work_used);
XR_FUNC void xr_declaration_manifest_free(XrDeclarationManifest *manifest);
#endif // XDECLARATION_MANIFEST_H
