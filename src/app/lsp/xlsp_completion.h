/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_completion.h - Code completion for LSP
 *
 * KEY CONCEPT:
 *   Provides code completion by analyzing symbols, modules, and types.
 */

#ifndef XLSP_COMPLETION_H
#define XLSP_COMPLETION_H

#include "xlsp_types.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_builtins.h"

// Unified completion: keywords, builtins, symbols, modules, classes, enums
XR_FUNC XrJsonValue *xlsp_analyze_completion(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos);

// Infer variable type from declaration (also used by hover)
XR_FUNC XlspBuiltinType xlsp_infer_variable_type(XrLspServer *server, XrLspDocument *doc, const char *var_name);

#endif // XLSP_COMPLETION_H
