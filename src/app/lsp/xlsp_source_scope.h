/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_scope.h - Owned editor overlays and closed workspace texts
 *
 * KEY CONCEPT:
 *   Capture real authority roots and complete directory results before exposing
 *   a source corpus. Open editor bytes replace disk files at the same path.
 */
#ifndef XLSP_SOURCE_SCOPE_H
#define XLSP_SOURCE_SCOPE_H
#include "xlsp_source_buffer.h"
#include "xlsp_ignore.h"
typedef struct XlspSourceScope XlspSourceScope;
typedef struct XlspSourceScopeOptions {
    const XrXirCompileContext *context;
    const XlspSourceDocument *documents;
    size_t count;
    const char *const *roots;
    size_t root_count,maximum_file_bytes;
    const XlspConfig *configuration;
} XlspSourceScopeOptions;
/* All inputs are synchronous borrows. Output owns validated UTF8 bytes, URIs,
 * versions and canonical paths on the original cumulative ledger. Every open
 * document contributes its nearest real authority root; explicit roots join
 * that union. Closed sources have unknown editor versions. Any discovery,
 * authority, file read or resource failure rejects the whole corpus.
 * This is capture, not semantic checking or an atomic filesystem snapshot. */
XR_FUNC XrXirStatus xlsp_source_scope_capture(const XlspSourceScopeOptions *,XlspSourceScope **);
XR_FUNC void xlsp_source_scope_free(XlspSourceScope *);
XR_FUNC size_t xlsp_source_scope_count(const XlspSourceScope *);
XR_FUNC const XlspSourceDocument *xlsp_source_scope_documents(const XlspSourceScope *);
XR_FUNC const bool *xlsp_source_scope_known_versions(const XlspSourceScope *);
XR_FUNC XrXirStatus xlsp_source_scope_find(const XlspSourceScope *,const char *,size_t,size_t *);
#endif // XLSP_SOURCE_SCOPE_H
