/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse.h - Pratt parser for Xray
 *
 * KEY CONCEPT:
 *   Implements a Pratt parser to convert token stream into AST.
*/

#ifndef XPARSE_H
#define XPARSE_H


#include "../lexer/xlex.h"
#include "xast.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xdefs.h"

// Operator precedence
// Higher value = higher precedence
typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,       // = (lowest)
    PREC_TERNARY,          // ? : (ternary, just above assignment)
    PREC_NULLISH_COALESCE, // ?? (nullish coalescing)
    PREC_OR,               // ||
    PREC_AND,              // &&
    PREC_BIT_OR,           // | (bitwise or)
    PREC_BIT_XOR,          // ^ (bitwise xor)
    PREC_BIT_AND,          // & (bitwise and)
    PREC_EQUALITY,         // == !=
    PREC_COMPARISON,       // < > <= >=
    PREC_SHIFT,            // << >> (shift)
    PREC_TERM,             // + -
    PREC_FACTOR,           // * / %
    PREC_UNARY,            // ! - ~ (bitwise not)
    PREC_CALL,             // . () []
    PREC_POSTFIX,          // ++ -- (postfix increment/decrement)
    PREC_PRIMARY           // literals, parentheses (highest)
} Precedence;

// Forward declarations
typedef struct Parser Parser;
typedef struct XrTypeScope XrTypeScope;

// Parse function types
// prefix: prefix parse function (handles prefix operators and literals)
// infix: infix parse function (handles binary operators)
typedef AstNode *(*PrefixParseFn)(Parser *parser);
typedef AstNode *(*InfixParseFn)(Parser *parser, AstNode *left);

// Parse rule
// Each token type has a corresponding parse rule
typedef struct ParseRule {
    PrefixParseFn prefix;   // Prefix parse function
    InfixParseFn infix;     // Infix parse function
    Precedence precedence;  // Precedence
} ParseRule;

// Error callback for LSP integration
typedef void (*XrParseErrorCallback)(void *user_data, int line, int column,
                                      int end_line, int end_column,
                                      const char *message);

// Forward declaration
struct XrArena;

// Parser state
// Manages all state information during parsing
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

/* ========== Parser Main Interface ========== */

// Parse source code, return AST
// This is the main entry function of the parser
XR_FUNC AstNode *xr_parse(XrayIsolate *X, const char *source);

// Parse source code (with filename), return AST
// source_file is used for error reporting, can be NULL
XR_FUNC AstNode *xr_parse_with_source(XrayIsolate *X, const char *source, const char *source_file);

// Parse source code with trivia collection (for formatter)
// Collects comments as trivia attached to AST nodes
XR_FUNC AstNode *xr_parse_with_trivia(XrayIsolate *X, const char *source, const char *source_file);

// Initialize parser
// parser: parser pointer
// X: Xray isolate
// source: source code string
// source_file: source file path (for error reporting, can be NULL)
// arena: optional arena for AST allocation (NULL = use malloc)
XR_FUNC void xr_parser_init(Parser *parser, XrayIsolate *X, const char *source,
                    const char *source_file, struct XrArena *arena);

// Set error callback for LSP integration
// callback: error callback function (NULL to disable)
// user_data: user data passed to callback
// max_errors: max errors before stopping (0 = unlimited)
XR_FUNC void xr_parser_set_error_callback(Parser *parser, XrParseErrorCallback callback,
                                   void *user_data, int max_errors);

// Parse with error recovery (for LSP)
// Returns partial AST even if there are errors
// Errors are reported via callback
XR_FUNC AstNode *xr_parse_recoverable(Parser *parser);

/* ========== Internal Parse Functions ========== */

// Token operation functions
XR_FUNC void xr_parser_advance(Parser *parser);
XR_FUNC int xr_parser_check(Parser *parser, TokenType type);
XR_FUNC int xr_parser_match(Parser *parser, TokenType type);
XR_FUNC void xr_parser_consume(Parser *parser, TokenType type, const char *message);

// Expression parse functions
XR_FUNC AstNode *xr_parse_expression(Parser *parser);
XR_FUNC AstNode *xr_parse_precedence(Parser *parser, Precedence precedence);

// Statement parse functions
XR_FUNC AstNode *xr_parse_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_expr_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_print_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_block(Parser *parser);

// Control flow statement parse functions
XR_FUNC AstNode *xr_parse_if_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_while_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_for_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_for_in_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_break_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_continue_statement(Parser *parser);

// Variable related parse functions
XR_FUNC AstNode *xr_parse_var_declaration(Parser *parser, int is_const);
XR_FUNC AstNode *xr_parse_variable(Parser *parser);
XR_FUNC AstNode *xr_parse_assignment(Parser *parser, AstNode *left);
XR_FUNC AstNode *xr_parse_compound_assignment(Parser *parser, AstNode *left);

// Function related parse functions
XR_FUNC AstNode *xr_parse_function_declaration(Parser *parser);
XR_FUNC AstNode *xr_parse_fn_expression(Parser *parser);  // fn() {} anonymous function expression
XR_FUNC AstNode *xr_parse_call_expr(Parser *parser, AstNode *callee);
XR_FUNC AstNode *xr_parse_return_statement(Parser *parser);
XR_FUNC AstNode *xr_parse_yield_expr(Parser *parser);  // yield expression (reserved for coroutines)

// Get parse rule for token
XR_FUNC const ParseRule *xr_get_rule(TokenType type);

// Error handling functions
XR_FUNC void xr_parser_error(Parser *parser, const char *message);
XR_FUNC void xr_parser_error_at_current(Parser *parser, const char *message);
XR_FUNC void xr_parser_error_at_previous(Parser *parser, const char *message);
XR_FUNC void xr_parser_synchronize(Parser *parser);

// Report "expected <context>" with keyword detection.
// If current token is a keyword, reports "'xxx' is a keyword and cannot be used as <context>".
// Otherwise reports the fallback message as-is.
XR_FUNC void xr_parser_error_expected_name(Parser *parser, const char *context);

// Contextual keyword helpers: check/match TK_NAME with specific string content.
// Used for soft keywords (from, to, default, ref, etc.) that can also be identifiers.
XR_FUNC bool xr_parser_check_name(Parser *parser, const char *name);
XR_FUNC bool xr_parser_match_name(Parser *parser, const char *name);

// Report ASI error with detection of common cross-language mistakes.
// Returns true if a specific error was reported, false if generic ASI error should be used.
XR_FUNC bool xr_parser_check_asi_hint(Parser *parser);

/* ========== Destructuring ========== */

// Destructuring pattern parse functions
XR_FUNC XrDestructurePattern* xr_parse_array_pattern(Parser *parser);
XR_FUNC XrDestructurePattern* xr_parse_object_pattern(Parser *parser);
XR_FUNC XrDestructurePattern* xr_parse_destructure_pattern(Parser *parser);
XR_FUNC AstNode* xr_parse_destructure_declaration(Parser *parser, bool is_const);

/* ========== Specific Parse Functions ========== */

// Prefix parse functions (handle literals and prefix operators)
XR_FUNC AstNode *xr_parse_literal(Parser *parser);        // Literals: numbers, strings, etc.
XR_FUNC AstNode *xr_parse_grouping(Parser *parser);       // Parenthesized expression: (expr)
XR_FUNC AstNode *xr_parse_unary(Parser *parser);          // Unary operators: -expr, !expr
XR_FUNC AstNode *xr_parse_array_literal(Parser *parser);  // Array literal: [1, 2, 3]
XR_FUNC AstNode *xr_parse_object_literal(Parser *parser); // Object literal: {name: "Alice"} or Map: {key => value}
XR_FUNC AstNode *xr_parse_empty_map_literal(Parser *parser); // Map literal: #{} or #{"key" => value}
XR_FUNC AstNode *xr_parse_set_literal_new(Parser *parser); // Set literal: #[a, b]
// Keep old function for compatibility
XR_FUNC AstNode *xr_parse_set_literal(Parser *parser);    // Set literal old syntax: #{1, 2, 3}

// Infix parse functions (handle binary operators)
XR_FUNC AstNode *xr_parse_binary(Parser *parser, AstNode *left);    // Binary operation: left op right
XR_FUNC AstNode *xr_parse_index_access(Parser *parser, AstNode *array);  // Index access: arr[0]
XR_FUNC AstNode *xr_parse_member_access(Parser *parser, AstNode *object);  // Member access: arr.length

/* ========== Type Parse Functions ========== */

// Parse type annotation to XrType (directly use Token type, efficient)
// e.g.: int, float, string, int[], Array<int>, Map<string, int>, T?
XR_FUNC XrType* xr_parse_type_annotation(Parser *parser);

// Helper: Convert XrType to string (for debugging/XrProto)
XR_FUNC const char* xr_compile_type_to_string(XrType *type);

/* ========== Type Alias Parse Functions ========== */

// Parse type alias declaration
// type Point = { x: float, y: float }
// type BinaryOp = (int, int) => int
XR_FUNC AstNode *xr_parse_type_alias_declaration(Parser *parser);

/* ========== Enum Parse Functions ========== */

// Parse enum declaration
// enum Status : int { Success = 200, Error = 500 }
XR_FUNC AstNode *xr_parse_enum_declaration(Parser *parser);

/* ========== OOP Parse Functions ========== */

// Parse class declaration
// class Dog extends Animal { fields... methods... }
XR_FUNC AstNode *xr_parse_class_declaration(Parser *parser);

// Parse struct declaration (value type)
// struct Point { x: float, y: float }
XR_FUNC AstNode *xr_parse_struct_declaration(Parser *parser);

// Parse interface declaration
// interface Drawable extends Shape { ... }
XR_FUNC AstNode *xr_parse_interface_declaration(Parser *parser);

// Parse interface method signature
// draw(): void; or area(): float;
XR_FUNC AstNode *xr_parse_interface_method(Parser *parser);

// Parse field declaration
// name: string or private age: int = 0
XR_FUNC AstNode *xr_parse_field_declaration(Parser *parser, bool *is_method_out);

// Parse method declaration
// greet() { ... } or constructor(name) { ... }
//
// @param name         method name (already parsed)
// @param name_line    1-indexed line of the identifier token
// @param name_column  1-indexed column of the identifier token
// @param is_private whether private
// @param is_static whether static
XR_FUNC AstNode *xr_parse_method_declaration(Parser *parser, const char *name,
                                     int name_line, int name_column,
                                     bool is_private, bool is_static, bool is_abstract);

// Parse operator method declaration
// operator +(other: Type): Type { ... }
XR_FUNC AstNode *xr_parse_operator_method(Parser *parser, bool is_private, bool is_static);

// Parse static constructor
// static constructor() { ... }
XR_FUNC AstNode *xr_parse_static_constructor(Parser *parser, bool is_private);

// Parse new expression (prefix)
// new Dog("Rex")
XR_FUNC AstNode *xr_parse_new_expression(Parser *parser);

// Parse this expression (prefix)
// this
XR_FUNC AstNode *xr_parse_this_expression(Parser *parser);

// Parse super call (prefix)
// super.greet() or super(args)
XR_FUNC AstNode *xr_parse_super_expression(Parser *parser);

/* ========== Exception Handling Parse Functions ========== */

// Parse try-catch-finally statement
// try { ... } catch (e) { ... } finally { ... }
XR_FUNC AstNode *xr_parse_try_statement(Parser *parser);

// Parse throw statement
// throw expr
XR_FUNC AstNode *xr_parse_throw_statement(Parser *parser);

/* ========== Match Expression Parse Functions ========== */

// Parse match expression (prefix)
// match x { 1 => "one", _ => "other" }
XR_FUNC AstNode *xr_parse_match_expr(Parser *parser);

/* ========== Module System Parse Functions ========== */

// Parse import declaration
// import time
// import time as t
// import { now, sleep } from time
// import * as time from time
XR_FUNC AstNode *xr_parse_import_declaration(Parser *parser);

// Parse selective import
// import { now, sleep } from time
// import { now as currentTime } from time
XR_FUNC AstNode *xr_parse_import_from_declaration(Parser *parser, int line);

// Parse export declaration
// export fn add() {}
// export let PI = 3.14
// export default fn() {}
XR_FUNC AstNode *xr_parse_export_declaration(Parser *parser);

#endif // XPARSE_H
