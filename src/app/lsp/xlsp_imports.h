/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_imports.h - Cross-file import resolution for LSP
 *
 * KEY CONCEPT:
 *   Resolves local file imports (import "./utils") and extracts
 *   exported symbols from imported files.
 */

#ifndef XLSP_IMPORTS_H
#define XLSP_IMPORTS_H

#include "xlsp_server.h"
#include "../../base/xjson.h"
#include <stdbool.h>
#include <time.h>

// Import types
typedef enum {
    XLSP_IMPORT_STDLIB,     // import time
    XLSP_IMPORT_LOCAL,      // import "./utils" or import "../lib/math"
    XLSP_IMPORT_PACKAGE,    // import "somepackage" (future)
} XlspImportType;

// Imported module info
typedef struct XlspImportInfo {
    char *module_name;          // Alias used in code (e.g. "utils")
    char *import_path;          // Original path (e.g. "./utils")
    char *resolved_path;        // Absolute file path
    XlspImportType type;
    struct XlspImportInfo *next;
} XlspImportInfo;

// Exported symbol from a file
typedef struct XlspExportedSymbol {
    char *name;
    int kind;                   // LSP SymbolKind
    char *signature;            // For functions
    char *documentation;
    int line;
    struct XlspExportedSymbol *next;
} XlspExportedSymbol;

// Cached file exports
typedef struct XlspFileExports {
    char *file_path;
    XlspExportedSymbol *symbols;
    time_t mtime;               // File modification time for cache invalidation
    struct XlspFileExports *next;
} XlspFileExports;

// Parse imports from document content
// Returns linked list of imports (caller must free)
XR_FUNC XlspImportInfo *xlsp_parse_imports(const char *content, const char *doc_uri);

// Free import info list
XR_FUNC void xlsp_free_imports(XlspImportInfo *imports);

// Resolve import path to absolute file path
// base_uri: URI of the importing file
// import_path: the path in import statement
// Returns allocated string or NULL
XR_FUNC char *xlsp_resolve_import_path(const char *base_uri, const char *import_path);

// Extract exported symbols from a file (uses server's exports cache)
// Returns linked list of symbols (caller must free)
XR_FUNC XlspExportedSymbol *xlsp_extract_exports(XrLspServer *server, const char *file_path);

// Free exported symbols list
XR_FUNC void xlsp_free_exports(XlspExportedSymbol *symbols);

// Get completion items for an imported module
XR_FUNC XrJsonValue *xlsp_get_import_completions(XrLspDocument *doc, const char *module_name);

// Get hover info for an imported symbol
XR_FUNC const char *xlsp_get_import_hover(XrLspDocument *doc, const char *module_name,
                                   const char *symbol_name, char *buf, size_t buf_size);

// Go to definition for an imported symbol
XR_FUNC XrJsonValue *xlsp_get_import_definition(XrLspDocument *doc, const char *module_name,
                                         const char *symbol_name);

// Get file location for a module name (for import statement goto definition)
XR_FUNC XrJsonValue *xlsp_get_module_file_location(XrLspDocument *doc, const char *module_name);

// Exports cache hash table for O(1) lookup by file path
#define EXPORTS_CACHE_BUCKETS 64

typedef struct XlspExportsCache {
    XlspFileExports *buckets[EXPORTS_CACHE_BUCKETS];
} XlspExportsCache;

// Free exports cache (called from xlsp_server_free)
XR_FUNC void xlsp_free_exports_cache(XrLspServer *server);

// Remove a single entry from exports cache by file path
XR_FUNC void xlsp_exports_cache_remove(XrLspServer *server, const char *file_path);

// Remove all exports cache entries whose file_path starts with prefix
XR_FUNC void xlsp_exports_cache_remove_prefix(XrLspServer *server, const char *prefix);

// Invalidate document's import cache (call when content changes)
XR_FUNC void xlsp_invalidate_import_cache(XrLspDocument *doc);

#endif // XLSP_IMPORTS_H
