/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_types.h - LSP protocol types and structures
 *
 * KEY CONCEPT:
 *   Defines all LSP protocol types used for communication with editors.
 */

#ifndef XLSP_TYPES_H
#define XLSP_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// LSP Protocol Constants
// ============================================================================

// TextDocumentSyncKind
#define LSP_SYNC_NONE 0
#define LSP_SYNC_FULL 1
#define LSP_SYNC_INCREMENTAL 2

// FileChangeType (for didChangeWatchedFiles)
#define LSP_FILE_CHANGE_CREATED 1
#define LSP_FILE_CHANGE_CHANGED 2
#define LSP_FILE_CHANGE_DELETED 3

// WatchKind (bitmask for file watcher registration)
#define LSP_WATCH_CREATE 1
#define LSP_WATCH_CHANGE 2
#define LSP_WATCH_DELETE 4
#define LSP_WATCH_ALL 7

// DocumentHighlightKind
#define LSP_HIGHLIGHT_TEXT 1
#define LSP_HIGHLIGHT_READ 2
#define LSP_HIGHLIGHT_WRITE 3

// SymbolKind constants (used in various handlers)
#define LSP_SYMBOL_FILE 1
#define LSP_SYMBOL_MODULE 2
#define LSP_SYMBOL_NAMESPACE 3
#define LSP_SYMBOL_PACKAGE 4
#define LSP_SYMBOL_CLASS 5
#define LSP_SYMBOL_METHOD 6
#define LSP_SYMBOL_PROPERTY 7
#define LSP_SYMBOL_FIELD 8
#define LSP_SYMBOL_CONSTRUCTOR 9
#define LSP_SYMBOL_ENUM 10
#define LSP_SYMBOL_INTERFACE 11
#define LSP_SYMBOL_FUNCTION 12
#define LSP_SYMBOL_VARIABLE 13
#define LSP_SYMBOL_CONSTANT 14
#define LSP_SYMBOL_STRING 15
#define LSP_SYMBOL_NUMBER 16
#define LSP_SYMBOL_BOOLEAN 17
#define LSP_SYMBOL_ARRAY 18
#define LSP_SYMBOL_ENUM_MEMBER 22

// ============================================================================
// LSP Protocol Types
// ============================================================================

// Position in a text document (0-indexed)
typedef struct XrLspPosition {
    uint32_t line;
    uint32_t character;
} XrLspPosition;

// Range in a text document
typedef struct XrLspRange {
    XrLspPosition start;
    XrLspPosition end;
} XrLspRange;

// Location (file + range)
typedef struct XrLspLocation {
    char *uri;
    XrLspRange range;
} XrLspLocation;

// Diagnostic severity
typedef enum {
    XR_DIAG_ERROR = 1,
    XR_DIAG_WARNING = 2,
    XR_DIAG_INFO = 3,
    XR_DIAG_HINT = 4
} XrLspDiagnosticSeverity;

// Diagnostic
typedef struct XrLspDiagnostic {
    XrLspRange range;
    XrLspDiagnosticSeverity severity;
    char *code;
    char *source;
    char *message;
} XrLspDiagnostic;

// Diagnostic list
typedef struct XrLspDiagnosticList {
    XrLspDiagnostic *items;
    int count;
    int capacity;
} XrLspDiagnosticList;

// Text document identifier
typedef struct XrLspTextDocumentIdentifier {
    char *uri;
} XrLspTextDocumentIdentifier;

// Versioned text document identifier
typedef struct XrLspVersionedTextDocumentIdentifier {
    char *uri;
    int version;
} XrLspVersionedTextDocumentIdentifier;

// Text document content change event
typedef struct XrLspTextDocumentContentChangeEvent {
    XrLspRange *range;  // NULL for full sync
    char *text;
} XrLspTextDocumentContentChangeEvent;

// Completion item kind
typedef enum {
    XR_COMPLETION_TEXT = 1,
    XR_COMPLETION_METHOD = 2,
    XR_COMPLETION_FUNCTION = 3,
    XR_COMPLETION_CONSTRUCTOR = 4,
    XR_COMPLETION_FIELD = 5,
    XR_COMPLETION_VARIABLE = 6,
    XR_COMPLETION_CLASS = 7,
    XR_COMPLETION_INTERFACE = 8,
    XR_COMPLETION_MODULE = 9,
    XR_COMPLETION_PROPERTY = 10,
    XR_COMPLETION_UNIT = 11,
    XR_COMPLETION_VALUE = 12,
    XR_COMPLETION_ENUM = 13,
    XR_COMPLETION_KEYWORD = 14,
    XR_COMPLETION_SNIPPET = 15,
    XR_COMPLETION_COLOR = 16,
    XR_COMPLETION_FILE = 17,
    XR_COMPLETION_REFERENCE = 18,
    XR_COMPLETION_FOLDER = 19,
    XR_COMPLETION_ENUM_MEMBER = 20,
    XR_COMPLETION_CONSTANT = 21,
    XR_COMPLETION_STRUCT = 22,
    XR_COMPLETION_EVENT = 23,
    XR_COMPLETION_OPERATOR = 24,
    XR_COMPLETION_TYPE_PARAMETER = 25
} XrLspCompletionItemKind;

// Completion item
typedef struct XrLspCompletionItem {
    char *label;
    XrLspCompletionItemKind kind;
    char *detail;
    char *documentation;
    char *insertText;
} XrLspCompletionItem;

// Completion list
typedef struct XrLspCompletionList {
    XrLspCompletionItem *items;
    int count;
    int capacity;
    bool is_incomplete;
} XrLspCompletionList;

// Hover result
typedef struct XrLspHover {
    char *contents;  // Markdown string
    XrLspRange *range;
} XrLspHover;

// Symbol kind
typedef enum {
    XR_SYMBOL_FILE = 1,
    XR_SYMBOL_MODULE = 2,
    XR_SYMBOL_NAMESPACE = 3,
    XR_SYMBOL_PACKAGE = 4,
    XR_SYMBOL_CLASS = 5,
    XR_SYMBOL_METHOD = 6,
    XR_SYMBOL_PROPERTY = 7,
    XR_SYMBOL_FIELD = 8,
    XR_SYMBOL_CONSTRUCTOR = 9,
    XR_SYMBOL_ENUM = 10,
    XR_SYMBOL_INTERFACE = 11,
    XR_SYMBOL_FUNCTION = 12,
    XR_SYMBOL_VARIABLE = 13,
    XR_SYMBOL_CONSTANT = 14,
    XR_SYMBOL_STRING = 15,
    XR_SYMBOL_NUMBER = 16,
    XR_SYMBOL_BOOLEAN = 17,
    XR_SYMBOL_ARRAY = 18,
    XR_SYMBOL_OBJECT = 19,
    XR_SYMBOL_KEY = 20,
    XR_SYMBOL_NULL = 21,
    XR_SYMBOL_ENUM_MEMBER = 22,
    XR_SYMBOL_STRUCT = 23,
    XR_SYMBOL_EVENT = 24,
    XR_SYMBOL_OPERATOR = 25,
    XR_SYMBOL_TYPE_PARAMETER = 26
} XrLspSymbolKind;

// Document symbol
typedef struct XrLspDocumentSymbol {
    char *name;
    char *detail;
    XrLspSymbolKind kind;
    XrLspRange range;
    XrLspRange selection_range;
    struct XrLspDocumentSymbol *children;
    int child_count;
} XrLspDocumentSymbol;

// Message type for window/showMessage
typedef enum {
    XR_MSG_ERROR = 1,
    XR_MSG_WARNING = 2,
    XR_MSG_INFO = 3,
    XR_MSG_LOG = 4
} XrLspMessageType;

#endif  // XLSP_TYPES_H
