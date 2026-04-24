/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_rename.h - Semantic rename support (extracted from xlsp_analysis.c)
 */

#ifndef XLSP_RENAME_H
#define XLSP_RENAME_H

#include "xlsp_json.h"
#include "xlsp_types.h"

typedef struct XrLspDocument XrLspDocument;
typedef struct XrLspServer XrLspServer;

// Prepare rename (check if rename is valid at position)
// Returns Range JSON or NULL if rename not valid
XR_FUNC XrJsonValue *xlsp_analyze_prepare_rename(XrLspDocument *doc, XrLspPosition pos);

// Rename symbol at position (single-document, scope-aware)
// Returns WorkspaceEdit JSON or NULL if rename not possible
XR_FUNC XrJsonValue *xlsp_analyze_rename(XrLspServer *server, XrLspDocument *doc,
                                          XrLspPosition pos, const char *new_name);

#endif // XLSP_RENAME_H
