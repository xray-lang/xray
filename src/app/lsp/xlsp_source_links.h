/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_LINKS_H
#define XLSP_SOURCE_LINKS_H
#include "xlsp_source_imports.h"

typedef struct XlspSourceLinks XlspSourceLinks;
/* Resolves real parser-admitted imports with the canonical Catalog, nearest
 * project/lockfile authority and every open document overlay. No semantic
 * checking or recursive import execution is implied. Any unresolved target
 * rejects the complete result. The owner pins the original context ledger
 * after all authorities, resolver, Catalog, AST and caller inputs die. */
XR_FUNC XrXirStatus xlsp_source_links_build(const XrXirCompileContext *,const XlspSourceDocument *,size_t,size_t,XlspSourceLinks **);
/* Complete independently owned JSON; failure preserves an empty output. */
XR_FUNC XrXirStatus xlsp_source_links_json(const XlspSourceLinks *,XrJsonValue **);
XR_FUNC void xlsp_source_links_free(XlspSourceLinks *);
#endif // XLSP_SOURCE_LINKS_H
