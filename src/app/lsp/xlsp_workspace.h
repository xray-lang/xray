/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace.h - Workspace indexing for cross-file features
 *
 * KEY CONCEPT:
 *   Maintains a global symbol index across all workspace files.
 *   Enables Go to Definition, Find References, and Rename.
 */

#ifndef XLSP_WORKSPACE_H
#define XLSP_WORKSPACE_H

#include "xlsp_server.h"
#include "../../runtime/value/xtype.h"
#include <stdbool.h>

// Symbol location in workspace
typedef struct XrLspSymbolLocation {
    char *uri;              // File URI
    int line;               // 1-indexed line
    int column;             // 0-indexed column
    int end_line;
    int end_column;
} XrLspSymbolLocation;

// Symbol reference
typedef struct XrLspSymbolRef {
    XrLspSymbolLocation loc;
    bool is_definition;     // true if this is the definition
    bool is_write;          // true if this is a write reference
    struct XrLspSymbolRef *next;
} XrLspSymbolRef;

// Workspace symbol entry
typedef struct XrLspWorkspaceSymbol {
    char *name;
    XrType *type;
    bool is_exported;
    bool is_function;
    bool is_class;
    
    // Definition location
    XrLspSymbolLocation def_loc;
    
    // All references
    XrLspSymbolRef *refs;
    int ref_count;
    
    struct XrLspWorkspaceSymbol *next;  // Hash chain
} XrLspWorkspaceSymbol;

// Workspace index
typedef struct XrLspWorkspaceIndex {
    XrLspWorkspaceSymbol **symbols;  // Hash table
    int table_size;
    int symbol_count;
    
    // Indexed files
    char **indexed_files;
    int file_count;
    int file_capacity;
} XrLspWorkspaceIndex;

// Create/destroy workspace index
XR_FUNC XrLspWorkspaceIndex *xlsp_workspace_new(void);
XR_FUNC void xlsp_workspace_free(XrLspWorkspaceIndex *idx);

// Index a document
XR_FUNC void xlsp_workspace_index_document(XrLspWorkspaceIndex *idx, XrLspDocument *doc);

// Remove document from index
XR_FUNC void xlsp_workspace_remove_document(XrLspWorkspaceIndex *idx, const char *uri);

// Find symbol definition
XR_FUNC XrLspWorkspaceSymbol *xlsp_workspace_find_definition(XrLspWorkspaceIndex *idx, 
                                                       const char *name);

// Find all references to a symbol
XR_FUNC XrLspSymbolRef *xlsp_workspace_find_references(XrLspWorkspaceIndex *idx,
                                                 const char *name,
                                                 int *count);

// Find symbol at position in document
XR_FUNC XrLspWorkspaceSymbol *xlsp_workspace_symbol_at(XrLspWorkspaceIndex *idx,
                                                 const char *uri,
                                                 int line, int column);

// Get all workspace symbols (for workspace/symbol request)
XR_FUNC XrLspWorkspaceSymbol **xlsp_workspace_get_all_symbols(XrLspWorkspaceIndex *idx,
                                                        int *count);

// Background indexing support
typedef struct XrLspServer XrLspServer;

// Data for background indexing task
typedef struct XrLspIndexTaskData {
    XrLspServer *server;
    char *root_path;
    char **files;
    int file_count;
    int current_file;
} XrLspIndexTaskData;

// Start background workspace indexing
XR_FUNC void xlsp_workspace_start_background_index(XrLspServer *server, const char *root_path);

// Background task execute function (runs in worker thread)
XR_FUNC void xlsp_workspace_index_task_execute(void *data);

// Background task completion (runs in main thread)
XR_FUNC void xlsp_workspace_index_task_complete(void *data, void *result);

// Index a single file by path (for file watcher updates)
XR_FUNC void xlsp_workspace_index_file(XrLspServer *server, const char *uri, const char *path);

// ============================================================================
// Multi-Isolate Parallel Indexing API
// ============================================================================

// Forward declaration
typedef struct XrLspIndexResult XrLspIndexResult;

// Poll index pool and process results (call from main loop)
XR_FUNC void xlsp_workspace_poll_index_results(XrLspServer *server);

// Get index pool notify fd (for select/poll)
XR_FUNC int xlsp_workspace_get_index_notify_fd(XrLspServer *server);

// Merge index results into workspace analyzer
XR_FUNC void xlsp_workspace_merge_index_results(XrLspServer *server, XrLspIndexResult *results);

#endif // XLSP_WORKSPACE_H
