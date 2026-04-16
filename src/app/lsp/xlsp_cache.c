/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_cache.c - Document and analysis caching implementation
 */

#include "xlsp_cache.h"
#include "xlsp_analysis.h"
#include "../../base/xhash.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include <stdlib.h>
#include <string.h>

// Use unified hash function from xhash.h
uint64_t xlsp_content_hash(const char *content, size_t length) {
    return xr_hash_bytes64(content, length);
}

// Check if content changed
bool xlsp_content_changed(XrLspDocument *doc, const char *new_content, size_t new_length) {
    if (!doc) return true;
    
    // Quick length check
    if (doc->length != new_length) return true;
    
    // Hash comparison
    uint64_t new_hash = xlsp_content_hash(new_content, new_length);
    return doc->content_hash != new_hash;
}

// Invalidate analyzer cache (triggers re-analysis on next access)
void xlsp_invalidate_semantic_cache(XrLspDocument *doc) {
    if (!doc) return;
    doc->dirty = true;
}

// Free all document caches
void xlsp_free_document_cache(XrLspDocument *doc) {
    if (!doc) return;
    
    // Free diagnostics cache
    if (doc->cached_diagnostics) {
        xlsp_json_free((XrJsonValue *)doc->cached_diagnostics);
        doc->cached_diagnostics = NULL;
    }
}

// Smart reparse - only if content changed
bool xlsp_smart_reparse(XrLspDocument *doc, XrLspServer *server) {
    if (!doc || !doc->content || !server) return false;
    
    // Compute current hash
    uint64_t current_hash = xlsp_content_hash(doc->content, doc->length);
    
    // Check if unchanged
    if (doc->content_hash == current_hash && doc->ast && !doc->dirty) {
        return false;  // No reparse needed
    }
    
    // Update hash
    doc->content_hash = current_hash;
    
    // Invalidate caches
    xlsp_invalidate_semantic_cache(doc);
    
    // Reparse
    xlsp_parse_document(doc, server);
    
    return true;  // Reparsed
}
