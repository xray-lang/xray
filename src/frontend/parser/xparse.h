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
 *         * xr_compile_parse
 *         * xr_compile_parse_with_source
 *         * xr_compile_parse_with_trivia
 *         * xr_compile_parse_expression_string  (REPL / DAP eval)
 *
 *     - 1 LSP recovery entry that takes a pre-initialised Parser:
 *         * xr_compile_parse_recoverable
 *
 *     - The Parser struct (callers stack-allocate it) plus the two LSP
 *       setup helpers it needs:
 *         * xr_compile_parser_open
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
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xdefs.h"

/* ========== Public Forward Declarations ========== */

typedef struct Parser Parser;
typedef enum XrParseStatus {
    XR_PARSE_OK,
    XR_PARSE_SYNTAX,
    XR_PARSE_RECOVERED,
    XR_PARSE_BAD_ARGUMENT,
    XR_PARSE_BUDGET,
    XR_PARSE_OUT_OF_MEMORY,
    XR_PARSE_IO
} XrParseStatus;
typedef struct XrTypeScope XrTypeScope;  // Defined in xtype_scope.h
struct XrArena;                          // Defined in base/xarena.h

// Error callback for LSP integration: each lexer/parser diagnostic is
// reported by invoking this on `user_data`. `end_line`/`end_column`
// describe the affected range; `message` is owned by the parser and is
// only valid for the duration of the call.
typedef void (*XrParseErrorCallback)(void *user_data, int line, int column, int end_line,
                                     int end_column, const char *message);

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
    XrCompileState *state;                 // Borrowed from the exact parse arena.
    struct XrParserOwner *owner;           // Shared ownership record; copies only borrow it.
    Scanner scanner;                      // Lexical scanner
    Token current;                        // Current token
    Token previous;                       // Previous token
    int had_error;                        // Whether there was a syntax error
    int panic_mode;                       // Whether in panic mode (error recovery)
    XrCompilerSession *compiler_session;  // Active toolchain session for AST allocation.
    struct XrArena *arena;                // Required owner of AST, strings and trivia.
    XrTypeScope *type_scope;              // Parser-owned scope for type aliases / generic params
    const char *source_file;              // Source file path (for error reporting)

    // Error callback (for LSP)
    XrParseErrorCallback error_callback;
    void *error_callback_data;
    int error_count;  // Number of errors collected
    int max_errors;   // Max errors before stopping (0 = unlimited)

    // True while parsing a bodyless function declaration inside an extern block.
    // (foreign implementation), so no `{ }` block follows the signature.
    bool parsing_extern_fn;

    // Nesting depth for module-level restriction checks.
    // 0 = top-level (import/export allowed), >0 = inside function/class body.
    int scope_depth;

    // Recursion depth guard: prevents deeply nested expressions / types /
    // patterns from exhausting the C call stack. Incremented on entry
    // to each recursive parse entry point, decremented on exit; exceeding
    // XR_PARSER_MAX_DEPTH reports a clean error and stops recursion instead of
    // crashing with SIGSEGV.
    int recursion_depth;

    // Open-bracket bitstack driven purely by xr_parser_advance(): bit i records
    // what the i-th still-open bracket was — 1 for `(` / `[` (a *group*, where
    // no statement can begin), 0 for `{` (a brace scope, where statements can).
    // xr_parser_in_group() reads the top bit and is the single gate for
    // suppressing line-break statement termination (L-04, see xparse.c).
    //
    // Maintained by the token stream itself so that no `(`-consuming call site
    // has to remember to bump a counter, and so that the parser-state
    // checkpoint/rollback used by generic-call and struct-literal lookahead
    // restores it for free along with the rest of the struct.
    //
    // Nesting past XR_PARSER_MAX_BRACKET_BITS stops being tracked and reads as
    // "not in a group", i.e. the conservative statement-terminating behaviour.
    uint64_t bracket_bits;
    int bracket_depth;

    // True for units whose contract is "evaluate and report" — the REPL prints
    // the value of a trailing bare expression, so an expression statement there
    // is not a discarded result and E0208 must not fire. False for scripts and
    // modules, where a bare expression really is dead code.
    bool expr_value_observed;
    // A match arm consumes the next block's tail value; nested blocks reset it.
    bool match_value_block_pending;
    bool block_tail_value_observed;

    // Nesting count of match-arm expression bodies currently being parsed.
    // `is` has no prefix rule (it is infix-only), so a line break before a
    // leading `is` would not normally terminate the previous expression; when
    // the previous arm body is still open, that leading `is` starts the next
    // arm's type pattern and must end the body instead.
    int match_arm_body_depth;

    // Record braces after a qualified path belong to the enclosing pattern
    // parser while this flag is set; expression parsing must leave them unread.
    bool parsing_pattern;

    // Stable across formatting and restored with parser lookahead checkpoints.
    uint32_t tuple_head_sequence;
};

// Logical nesting budget for expressions, types and match patterns. Recursive
// implementations must also fit the host stack in unoptimized and sanitized
// builds; the numeric budget alone does not establish stack safety.
#define XR_PARSER_MAX_DEPTH 1000

// Number of open brackets tracked by Parser::bracket_bits. Beyond this the
// tracker degrades to "statement level", which is the safe direction: line
// breaks terminate statements rather than silently gluing lines together.
#define XR_PARSER_MAX_BRACKET_BITS 64

/* ========== Statement Boundaries (L-04) ==========
 *
 * Xray terminates statements at line breaks. `xr_token_can_end_expr` is the
 * language-level predicate that decides whether a token may be the last token
 * of an expression; together with "the next line starts with a token that has a
 * prefix role" it defines where a line break ends a statement. The full rule
 * and its rationale live above xr_token_can_end_expr() in xparse.c.
 *
 * Exposed because the rule is part of the language surface, not a parser
 * detail: tests/unit/frontend/test_parser_asi.c cross-checks it against the
 * Pratt rules table so a newly added dual-role token cannot silently escape it.
 */
XR_FUNC bool xr_token_can_end_expr(XrTokenType type);

// True when the innermost still-open bracket is `(` or `[`. Inside such a group
// no statement can begin, so line breaks never terminate anything there.
XR_FUNC bool xr_parser_in_group(const Parser *parser);

/* ========== Speculative Lookahead ==========
 *
 * Everything the token stream owns, as one value. Speculative lookahead must
 * save and restore it through these two calls rather than copying individual
 * fields: the bracket bitstack advances with the stream, and a site that
 * restored only `scanner`/`current`/`previous` would silently leave it wrong —
 * which is how a `(` scanned during lookahead used to leak into the L-04
 * statement-boundary decision.
 *
 * Error state (had_error / panic_mode / error_count) is deliberately NOT part
 * of this: each site decides for itself whether a speculative parse's
 * diagnostics should survive.
 */
typedef struct XrParserStreamState {
    Scanner scanner;
    Token current;
    Token previous;
    uint64_t bracket_bits;
    int bracket_depth;
} XrParserStreamState;

XR_FUNC XrParserStreamState xr_parser_stream_save(const Parser *parser);
XR_FUNC void xr_parser_stream_restore(Parser *parser, const XrParserStreamState *saved);

/* ========== Public Entry Points ========== */

// Owning entry points require an empty output. Only OK publishes an AST;
// syntax or resource failure preserves it. The returned program owns its arena
// and keeps the shared state/ledger alive until xr_program_destroy().
XR_FUNC XrParseStatus xr_compile_parse(XrCompilerSession *session, const char *source, AstNode **output);
XR_FUNC XrParseStatus xr_compile_parse_repl_unit(XrCompilerSession *session, const char *source, AstNode **output);
XR_FUNC XrParseStatus xr_compile_parse_with_source(XrCompilerSession *session, const char *source,
                                           const char *source_file, AstNode **output);
XR_FUNC XrParseStatus xr_compile_parse_with_trivia(XrCompilerSession *session, const char *source,
                                           const char *source_file, AstNode **output);
XR_FUNC XrParseStatus xr_compile_parse_expression_string(XrCompilerSession *session, const char *source,
                                                  const char *source_file, AstNode **output);

// Initialize a zeroed parser borrowing the explicit arena. The session must
// remain alive until destroy or parse_recoverable closes the parser scope.
// Copies borrow the owner and must not be independently destroyed. Failure
// preserves the zeroed parser and leaves no active session scope.
XR_FUNC XrParseStatus xr_compile_parser_open(Parser *parser, XrCompilerSession *session, const char *source,
                                      const char *source_file, struct XrArena *arena);
XR_FUNC void xr_compile_parser_close(Parser *parser);
XR_FUNC XrParseStatus xr_compile_parser_status(const Parser *parser);

// Install a diagnostic callback. Pass NULL to disable. `max_errors == 0`
// means "no limit".
XR_FUNC void xr_parser_set_error_callback(Parser *parser, XrParseErrorCallback callback,
                                          void *user_data, int max_errors);

// Consumes the parser scope. OK or RECOVERED publishes an AST borrowing the
// caller's exact arena; resource failure never publishes a partial AST.
XR_FUNC XrParseStatus xr_compile_parse_recoverable(Parser *parser, AstNode **output);

#endif  // XPARSE_H
