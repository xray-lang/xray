/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_symbol_index.h - Shallow, workspace-wide symbol index
 *
 * KEY CONCEPT:
 *   A lightweight inverted index of top-level declarations across the whole
 *   workspace. It holds only shallow metadata (name, kind, location,
 *   exported-ness) — never types, scopes, or AST — so it is cheap to build in
 *   parallel and cheap to keep for thousands of files.
 *
 *   Population:
 *     - background index-pool workers merge shallow symbols for CLOSED files
 *       (parsed in parallel, no main-thread analysis);
 *     - the live parse path refreshes the entry for each OPEN document so the
 *       index reflects unsaved edits.
 *
 *   Consumers: workspace/symbol (whole workspace), go-to-definition fallback
 *   (closed, non-imported files), and completion augmentation. Deep type-level
 *   features stay powered by the on-demand XaAnalyzer for open files + imports.
 */

#ifndef XLSP_SYMBOL_INDEX_H
#define XLSP_SYMBOL_INDEX_H

#include "xlsp_types.h"  // XrLspSymbolKind
#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

// Produced by the index pool; the index copies what it needs.
struct XrLspIndexSymbol;

// One shallow declaration occurrence.
typedef struct XlspIndexEntry {
    char *name;                        // Symbol name (owned)
    const char *uri;                   // File URI (borrowed from the owning file node)
    int line;                          // Declaration line (0-based)
    int column;                        // Declaration column (0-based)
    XrLspSymbolKind kind;              // Enum values match LSP SymbolKind numbers
    bool is_exported;                  // Exported from its module?
    struct XlspIndexEntry *file_next;  // Next entry in the same file
} XlspIndexEntry;

typedef struct XlspSymbolIndex XlspSymbolIndex;

XR_FUNC XlspSymbolIndex *xlsp_symbol_index_new(void);
XR_FUNC void xlsp_symbol_index_free(XlspSymbolIndex *idx);

// Replace all entries for `uri` with a copy of `symbols` (a borrowed
// XrLspIndexSymbol list). Idempotent — safe to call on every re-index / edit.
XR_FUNC void xlsp_symbol_index_replace_file(XlspSymbolIndex *idx, const char *uri,
                                            struct XrLspIndexSymbol *symbols);

// Drop all entries for a file.
XR_FUNC void xlsp_symbol_index_remove_file(XlspSymbolIndex *idx, const char *uri);

// Drop every file whose path is under `path_prefix` (used on workspace-folder
// removal). `path_prefix` is a filesystem path; a leading "file://" on stored
// URIs is ignored when matching.
XR_FUNC void xlsp_symbol_index_remove_prefix(XlspSymbolIndex *idx, const char *path_prefix);

// First entry whose name equals `name`, preferring an exported one.
// Returns NULL when absent. The pointer is owned by the index; do not free.
XR_FUNC const XlspIndexEntry *xlsp_symbol_index_find(XlspSymbolIndex *idx, const char *name);

// Visit every entry (unordered).
typedef void (*XlspSymbolIndexIter)(const XlspIndexEntry *entry, void *ctx);
XR_FUNC void xlsp_symbol_index_foreach(XlspSymbolIndex *idx, XlspSymbolIndexIter cb, void *ctx);

// Total live entries (for progress logging / diagnostics).
XR_FUNC size_t xlsp_symbol_index_entry_count(const XlspSymbolIndex *idx);

#endif  // XLSP_SYMBOL_INDEX_H
