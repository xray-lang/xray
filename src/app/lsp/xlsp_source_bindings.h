/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_bindings.h - Canonical Source binding observation owner
 *
 * KEY CONCEPT:
 *   Canonical Source owns binding semantics. Consumers retain exact owned facts
 *   and reject unavailable evidence without publishing partial edits.
 */
#ifndef XLSP_SOURCE_BINDINGS_H
#define XLSP_SOURCE_BINDINGS_H
#include "xlsp_source_query.h"
typedef struct XlspSourceBindings XlspSourceBindings;
/* Complete immutable Source views for consumers that must distinguish the
 * syntactic binding from its navigation/call target. The canonical Source
 * overlay checker owns every semantic decision. No alternate inference runs.
 * Views and canonical input paths outlive all parser/Catalog/authority/input
 * producers and pin the original supplied cumulative ledger. */
XR_FUNC XrXirStatus xlsp_source_bindings_build(const XrXirCompileContext *,const XlspSourceDocument *,size_t,XlspSourceBindings **);
XR_FUNC void xlsp_source_bindings_free(XlspSourceBindings *);
XR_FUNC size_t xlsp_source_bindings_count(const XlspSourceBindings *);
XR_FUNC const XrXirSourceView *xlsp_source_bindings_view(const XlspSourceBindings *,size_t);
XR_FUNC const XrXirSourceSyntaxView *xlsp_source_bindings_syntax(const XlspSourceBindings *,size_t);
XR_FUNC const char *xlsp_source_bindings_path(const XlspSourceBindings *,size_t);
#endif // XLSP_SOURCE_BINDINGS_H
