/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse.h - Pratt parser PUBLIC API
 *
 * KEY CONCEPT (P-02):
 *   This header exposes ONLY the contracts that downstream subsystems
 *   (LSP / DAP / formatter / CLI / tests / module loader) actually need:
 *
 *     - 4 entry points that take a source string and return an AST_PROGRAM
 *       node whose owning arena is released by `xr_program_destroy()`:
 *         * xr_parse
 *         * xr_parse_with_source
 *         * xr_parse_with_trivia
 *         * xr_parse_expression_string  (REPL / DAP eval)
 *
 *     - 1 LSP recovery entry that takes a pre-initialised Parser:
 *         * xr_parse_recoverable
 *
 *     - The Parser struct (callers stack-allocate it) plus the two LSP
 *       setup helpers it needs:
 *         * xr_parser_init
 *         * xr_parser_set_error_callback
 *
 *   All Pratt-table internals (Precedence, ParseRule, parse-helper fns,
 *   token-stream helpers, error helpers, AST-builder helpers) live in
 *   xparse_internal.h and are NOT part of the public surface.
 *
 *   Type pretty-printing is provided by runtime/value/xtype_format.c
 *   (xr_type_to_string); codegen calls that directly. The parser no
 *   longer re-exports it.
 */

#ifndef XPARSE_H
#define XPARSE_H

#include "../lexer/xlex.h"
#include "xast.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xdefs.h"

/* ========== Public Forward Declarations ========== */

typedef struct Parser Parser;
typedef struct XrTypeScope XrTypeScope;  // Defined in xtype_scope.h
struct XrArena;                          // Defined in base/xarena.h

// Error callback for LSP integration: each lexer/parser diagnostic is
// reported by invoking this on `user_data`. `end_line`/`end_column`
// describe the affected range; `message` is owned by the parser and is
// only valid for the duration of the call.
typedef void (*XrParseErrorCallback)(void *user_data,
                                     int line, int column,
                                     int end_line, int end_column,
                                     const char *message);

/* ========== Parser State ==========
 *
 * Callers stack-allocate this struct. After parsing, callers may read:
 *   - parser.had_error    (was any error reported?)
 *   - parser.error_count  (how many)
 *   - parser.max_errors   (writable: 0 = unlimited)
 *
 * Every other field is parser-internal and should not be touched.
 */
struct Parser {
    Scanner scanner;        // Lexical scanner
    Token current;          // Current token
    Token previous;         // Previous token
    int had_error;          // Whether there was a syntax error
    int panic_mode;         // Whether in panic mode (error recovery)
    XrayIsolate *X;         // Xray isolate
    struct XrArena *arena;  // Optional arena for AST allocation (NULL = use malloc)
    XrTypeScope *type_scope;    // Parser-owned scope for type aliases / generic params
    const char *source_file; // Source file path (for error reporting)

    // Error callback (for LSP)
    XrParseErrorCallback error_callback;
    void *error_callback_data;
    int error_count;        // Number of errors collected
    int max_errors;         // Max errors before stopping (0 = unlimited)

    // Allow bare container types (Array, Map, Set, Channel) without generic params.
    // Set temporarily by 'is'/'as' parsers where runtime type checks don't need
    // element type info.
    bool allow_bare_container;
};

/* ========== Public Entry Points ========== */

// Parse a complete program. Returns AST_PROGRAM node owning its arena;
// release with xr_program_destroy(). Returns NULL on parse error.
XR_FUNC AstNode *xr_parse(XrayIsolate *X, const char *source);

// Same as xr_parse but tags diagnostics with the given file path.
XR_FUNC AstNode *xr_parse_with_source(XrayIsolate *X, const char *source,
                                      const char *source_file);

// Parse a program AND collect comments as trivia attached to AST nodes.
// Used by the formatter; otherwise prefer xr_parse_with_source.
XR_FUNC AstNode *xr_parse_with_trivia(XrayIsolate *X, const char *source,
                                      const char *source_file);

// Parse a single expression as a self-contained translation unit.
// Returns an AST_PROGRAM node whose first declaration is the expression
// (so xr_program_destroy() releases everything uniformly). Returns NULL
// on parse error. Used by REPL completeness check and DAP eval.
XR_FUNC AstNode *xr_parse_expression_string(XrayIsolate *X,
                                            const char *source,
                                            const char *source_file);

/* ========== LSP Recoverable Parsing ==========
 *
 * For the LSP path the caller wants to keep parsing across errors and
 * receive structured diagnostics via callback. Sequence:
 *
 *   Parser parser;
 *   xr_parser_init(&parser, X, source, source_file, arena);
 *   xr_parser_set_error_callback(&parser, cb, user_data, max_errors);
 *   AstNode *program = xr_parse_recoverable(&parser);
 *   ... inspect program / parser.had_error ...
 */

// Initialise a stack-allocated Parser. `arena` is the arena to use for
// AST allocation; it MUST already be installed on the isolate via
// xr_isolate_set_current_arena. Pass NULL to leave the isolate's current
// arena in place (caller manages lifetime).
XR_FUNC void xr_parser_init(Parser *parser, XrayIsolate *X,
                            const char *source, const char *source_file,
                            struct XrArena *arena);

// Install a diagnostic callback. Pass NULL to disable. `max_errors == 0`
// means "no limit".
XR_FUNC void xr_parser_set_error_callback(Parser *parser,
                                          XrParseErrorCallback callback,
                                          void *user_data, int max_errors);

// Parse with error recovery. Returns a (possibly partial) AST_PROGRAM
// even when errors are present; callers should consult parser.had_error
// before using the result. For the LSP / incremental path the caller
// owns the arena, so the returned node is NOT auto-released by
// xr_program_destroy.
XR_FUNC AstNode *xr_parse_recoverable(Parser *parser);

#endif // XPARSE_H
