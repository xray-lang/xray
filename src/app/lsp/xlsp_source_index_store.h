/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_index_store.h - Transactional ownership of syntax display indexes
 */
#ifndef XLSP_SOURCE_INDEX_STORE_H
#define XLSP_SOURCE_INDEX_STORE_H
#include "xlsp_source_index.h"
typedef struct XlspSourceIndexStore XlspSourceIndexStore;
typedef struct XlspIndexedSymbol {
    const char *uri;
    const XlspSourceIndexSymbol *symbol;
    int64_t version;
} XlspIndexedSymbol;
typedef XrXirStatus (*XlspIndexVisitor)(const XlspIndexedSymbol *,void *);
/* The store owns transferred immutable indexes, never AST/type authority.
 * Its receiving allocations and scans use the supplied cumulative ledger.
 * Failure preserves the store and input owner. Views remain valid until their
 * file is removed/replaced or the store dies. Calls require caller exclusion;
 * visitor callbacks must not mutate or destroy the store. */
XR_FUNC XrXirStatus xlsp_source_index_store_new(XrCompileResources *,XlspSourceIndexStore **);
XR_FUNC void xlsp_source_index_store_free(XlspSourceIndexStore *);
XR_FUNC XrXirStatus xlsp_source_index_store_replace(XlspSourceIndexStore *,XlspSourceIndex **);
XR_FUNC XrXirStatus xlsp_source_index_store_remove(XlspSourceIndexStore *,const char *);
/* Prefixes are canonical file URIs, with a component boundary after the prefix.
 * The caller converts filesystem paths to URIs before invoking this operation. */
XR_FUNC XrXirStatus xlsp_source_index_store_remove_prefix(XlspSourceIndexStore *,const char *);
XR_FUNC XrXirStatus xlsp_source_index_store_find(XlspSourceIndexStore *,const char *,XlspIndexedSymbol *);
/* Visiting immutable views charges the receiving query ledger exclusively. */
XR_FUNC XrXirStatus xlsp_source_index_store_visit(const XlspSourceIndexStore *,XrCompileResources *,XlspIndexVisitor,void *);
XR_FUNC size_t xlsp_source_index_store_count(const XlspSourceIndexStore *);
#endif // XLSP_SOURCE_INDEX_STORE_H
