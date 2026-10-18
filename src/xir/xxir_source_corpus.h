/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_corpus.h - Check captured sources under one actual program entry
 */
#ifndef XXIR_SOURCE_CORPUS_H
#define XXIR_SOURCE_CORPUS_H
#include "xxir_source.h"
typedef struct XrXirSourceCorpusRequest {
    const XrXirSourceRequest *entry;
    const XrModuleOverlayInput *overlays;
    size_t overlay_count;
    const size_t *included;
    size_t included_count;
} XrXirSourceCorpusRequest;
/* Include the indexed captured sources and their imports in the checking graph
 * without changing entry authority, adding import edges or extending execution
 * closure. Each index selects an exact overlay authority/path/text tuple.
 * Repeated indices are idempotent. Ordinary module-storage and Sendable rules
 * remain in force for every non-entry module. All coordinates are synchronous
 * borrows; results own their facts on the original cumulative ledger. Overlay
 * validation and failure publication are identical to source_check_overlay. */
XR_FUNC XrXirStatus xr_xir_compile_source_check_corpus(const XrXirSourceCorpusRequest *,
    XrXirSourceResult *, XrXirSourceDiagnostic *, XrXirSourceFailure *failure);
#endif /* XXIR_SOURCE_CORPUS_H */
