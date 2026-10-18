/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_exports.h - Exact module queries for analyzer export collection
 */
#ifndef XANALYZER_EXPORTS_H
#define XANALYZER_EXPORTS_H
#include "../../base/xdefs.h"
struct XaAnalyzer;
struct AstNode;
struct XrModuleSpec;

/* Results borrow the unchanged graph. NULL denotes absence only while the
 * analyzer's original resource state remains healthy. */
XR_FUNC struct XrModuleSpec *xa_graph_spec_for_ast(struct XaAnalyzer *analyzer,
    const struct AstNode *ast);
XR_FUNC struct XrModuleSpec *xa_graph_spec_for_identity(struct XaAnalyzer *analyzer,
    const char *module_identity);
#endif // XANALYZER_EXPORTS_H
