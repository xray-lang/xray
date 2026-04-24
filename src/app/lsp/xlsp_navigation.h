/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_navigation.h - Semantic navigation declarations
 *   definition, references, document highlight
 */

#ifndef XLSP_NAVIGATION_H
#define XLSP_NAVIGATION_H

#include "../../base/xjson.h"
#include "xlsp_types.h"

typedef struct XrLspServer XrLspServer;
typedef struct XrLspDocument XrLspDocument;

// Go to definition
XR_FUNC XrJsonValue *xlsp_analyze_definition(XrLspServer *server,
                                              XrLspDocument *doc,
                                              XrLspPosition pos);

// Find references (cross-file, scope-aware)
XR_FUNC XrJsonValue *xlsp_analyze_references(XrLspServer *server,
                                              XrLspDocument *doc,
                                              XrLspPosition pos);

// Document highlight (scope-aware, single-file)
XR_FUNC XrJsonValue *xlsp_analyze_document_highlight(XrLspServer *server,
                                                      XrLspDocument *doc,
                                                      XrLspPosition pos);

#endif // XLSP_NAVIGATION_H
