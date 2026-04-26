/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_cache.h - Document and analysis caching for performance
 *
 * KEY CONCEPT:
 *   Manages caching of parse results and semantic analysis to avoid
 *   redundant computation on unchanged documents.
 */

#ifndef XLSP_CACHE_H
#define XLSP_CACHE_H

#include "xlsp_server.h"
#include <stdint.h>

// Sentinel value for uninitialized content hash (avoids collision with empty string hash)
#define XLSP_CONTENT_HASH_UNINITIALIZED UINT64_MAX

// Compute hash of document content for change detection
XR_FUNC uint64_t xlsp_content_hash(const char *content, size_t length);

// Check if document content has changed (using hash)
XR_FUNC bool xlsp_content_changed(XrLspDocument *doc, const char *new_content, size_t new_length);

// Invalidate analyzer cache (triggers re-analysis on next parse)
XR_FUNC void xlsp_invalidate_semantic_cache(XrLspDocument *doc);

// Free all caches for a document
XR_FUNC void xlsp_free_document_cache(XrLspDocument *doc);

// Smart reparse - only reparse if content actually changed
XR_FUNC bool xlsp_smart_reparse(XrLspDocument *doc, XrLspServer *server);

#endif  // XLSP_CACHE_H
