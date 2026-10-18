/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_INDEX_SCAN_H
#define XLSP_INDEX_SCAN_H
#include "xlsp_source_index.h"
typedef bool (*XlspIndexIgnore)(const char *,bool,void *);
typedef struct XlspIndexScanOptions {
    XrCompileResources *resources;
    const char *const *roots;
    size_t root_count;
    XlspIndexIgnore ignore;
    void *context;
} XlspIndexScanOptions;
typedef struct XlspIndexPaths {char **values;size_t count,capacity;} XlspIndexPaths;
/* Iterative traversal on the supplied ledger; complete output or no output.
 * Reject directories observed as reparse/symlink leaves. Native ancestor
 * resolution remains an OS operation; traversal stays resource bounded.
 * Ignore callbacks borrow names synchronously and carry no syntax authority. */
XR_FUNC XrXirStatus xlsp_index_scan(const XlspIndexScanOptions *,XlspIndexPaths *);
XR_FUNC void xlsp_index_paths_free(XlspIndexPaths *);
#endif // XLSP_INDEX_SCAN_H
