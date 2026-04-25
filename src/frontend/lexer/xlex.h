/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlex.h - Lexical analyzer (tokenizer)
 *
 * KEY CONCEPT:
 *   Converts source code text into a stream of tokens for the parser.
 *   Supports trivia (comments) preservation for code formatting.
 */

#ifndef XLEX_H
#define XLEX_H

#include <stdbool.h>
#include "../../base/xdefs.h"

// ============================================================================
// Trivia System - Comment Preservation
// ============================================================================

// Trivia types (non-semantic tokens attached to real tokens)
typedef enum {
    TRIVIA_LINE_COMMENT,    // // comment
    TRIVIA_BLOCK_COMMENT,   // /* comment */
} XrTriviaType;

// Single trivia item (comment)
typedef struct XrTrivia {
    XrTriviaType type;
    const char *start;      // Pointer to start of comment text
    int length;             // Length of comment text (excluding delimiters)
    int line;               // Line number where comment starts
    struct XrTrivia *next;  // Linked list for multiple comments
} XrTrivia;



// Token type definitions
// Contains all lexical tokens for the Xray language
typedef enum {
    // Single character tokens
    TK_LPAREN = '(',    // (
    TK_RPAREN = ')',    // )
    TK_LBRACE = '{',    // {
    TK_RBRACE = '}',    // }
    TK_LBRACKET = '[',  // [
    TK_RBRACKET = ']',  // ]
    TK_COMMA = ',',     // ,
    TK_DOT = '.',       // .
    TK_COLON = ':',     // :
    TK_SEMICOLON = ';', // ; used for for-loop separators etc.
    TK_PLUS = '+',      // +
    TK_MINUS = '-',     // -
    TK_STAR = '*',      // *
    TK_SLASH = '/',     // /
    TK_PERCENT = '%',   // %
    TK_HASH = '#',      // # (for Set literal)
    TK_AMP = '&',       // & (bitwise and)
    TK_CARET = '^',     // ^ (bitwise xor)
    TK_TILDE = '~',     // ~ (bitwise not)

    // Multi-character tokens (start from 256 to avoid ASCII collision)
    TK_EQ = 256,        // ==
    TK_NE,              // !=
    TK_EQ_STRICT,       // ===
    TK_NE_STRICT,       // !==
    TK_LT,              // <
    TK_LE,              // <=
    TK_GT,              // >
    TK_GE,              // >=
    TK_LSHIFT,          // <<
    TK_RSHIFT,          // >>
    TK_ASSIGN,          // =
    TK_PLUS_ASSIGN,     // +=
    TK_MINUS_ASSIGN,    // -=
    TK_MUL_ASSIGN,      // *=
    TK_DIV_ASSIGN,      // /=
    TK_MOD_ASSIGN,      // %=
    TK_AND_ASSIGN,      // &=
    TK_OR_ASSIGN,       // |=
    TK_XOR_ASSIGN,      // ^=
    TK_LSHIFT_ASSIGN,   // <<=
    TK_RSHIFT_ASSIGN,   // >>=
    TK_INC,             // ++
    TK_DEC,             // --
    TK_AND,             // &&
    TK_OR,              // ||
    TK_NOT,             // !

    // Keywords (TK_FIRST_KEYWORD must be first, TK_LAST_KEYWORD must be last)
    TK_FIRST_KEYWORD,
    TK_LET = TK_FIRST_KEYWORD, // let
    TK_CONST,           // const
    TK_SHARED,          // shared - stored in global heap, pass by reference (zero-copy across coroutines)
    TK_IF,              // if
    TK_ELSE,            // else
    TK_WHILE,           // while
    TK_FOR,             // for
    TK_IN,              // in (for for-in loop)
    TK_IS,              // is (runtime type check)
    TK_BREAK,           // break
    TK_CONTINUE,        // continue
    TK_RETURN,          // return
    TK_YIELD,           // yield (reserved for coroutines)
    TK_NULL,            // null
    TK_TRUE,            // true
    TK_FALSE,           // false
    TK_CLASS,           // class
    TK_STRUCT,          // struct
    TK_EXTENDS,         // extends
    TK_INTERFACE,       // interface
    TK_IMPLEMENTS,      // implements
    TK_FN,              // fn (unified function keyword)
    TK_NEW,             // new
    TK_THIS,            // this
    TK_SUPER,           // super
    TK_CONSTRUCTOR,     // constructor
    TK_STATIC,          // static
    TK_PRIVATE,         // private
    TK_PUBLIC,          // public
    TK_OPERATOR,        // operator
    TK_ABSTRACT,        // abstract
    TK_OVERRIDE,        // override
    TK_FINAL,           // final
    TK_ENUM,            // enum
    TK_MATCH,           // match
    TK_TYPE_ALIAS,      // type (type alias definition)

    // Exception handling keywords
    TK_TRY,             // try
    TK_CATCH,           // catch
    TK_FINALLY,         // finally
    TK_THROW,           // throw

    // Module system keywords
    TK_IMPORT,          // import
    TK_EXPORT,          // export
    TK_AS,              // as (infix operator: expr as Type)

    // Coroutine keywords
    TK_GO,              // go - spawn coroutine
    TK_AWAIT,           // await - wait for coroutine
    TK_SELECT,          // select - multiplexing
    TK_DEFER,           // defer - deferred execution
    TK_SCOPE,           // scope - structured concurrency

    // Type keywords
    TK_VOID,            // void
    TK_INT,             // int (= int64)
    TK_FLOAT,           // float (= float64)
    TK_STRING,          // string
    TK_BOOL,            // bool
    TK_INT8,            // int8
    TK_INT16,           // int16
    TK_INT32,           // int32
    TK_INT64,           // int64
    TK_UINT8,           // uint8
    TK_UINT16,          // uint16
    TK_UINT32,          // uint32
    TK_UINT64,          // uint64
    TK_FLOAT32,         // float32
    TK_FLOAT64,         // float64
    TK_TYPE_ARRAY,      // Array (container type)
    TK_TYPE_MAP,        // Map (container type)
    TK_TYPE_SET,        // Set (container type)
    TK_TYPE_JSON,       // Json (dynamic object type)
    TK_TYPE_CHANNEL,    // Channel (coroutine channel)
    TK_TYPE_BIGINT,     // BigInt (arbitrary precision integer)
    TK_TYPE_RANGE,      // Range (lazy integer range)
    TK_TYPE_DATETIME,   // DateTime (date/time type)
    TK_TYPE_BYTES,      // Bytes (byte array type)
    TK_UNKNOWN,         // unknown
    TK_LAST_KEYWORD = TK_UNKNOWN,

    // Contextual keywords (NOT in keyword range — can be used as identifiers)
    // These are recognized by the parser via string comparison, not by the lexer.
    TK_FROM,            // from (import/select context only)
    TK_TO,              // to (select send context only)
    TK_DEFAULT,         // default (reserved for future use)
    TK_CANCELLED,       // cancelled() (coroutine context only)
    TK_REF,             // ref (parameter modifier context only)
    TK_MOVE,            // move (ownership transfer context only)

    // Type operators
    TK_QUESTION,        // ? (optional type)
    TK_QUESTION_DOT,    // ?. (optional chaining)
    TK_PIPE,            // | (union type)
    TK_ARROW,           // => (arrow function)
    TK_DOT_DOT_DOT,     // ... (rest/spread operator)
    TK_RANGE,           // .. (range operator)
    TK_NULLISH_COALESCE,// ?? (nullish coalescing operator)
    TK_UNDERSCORE,      // _ (wildcard pattern, for match expression)

    // New syntax tokens
    TK_AT,              // @ - attribute marker (@test, @before_each etc.)
    TK_EMPTY_MAP_START, // #{ - empty Map literal start
    TK_SET_START,       // #[ - Set literal start

    // Literals and identifiers
    TK_LITERAL_INT,     // integer literal
    TK_LITERAL_FLOAT,   // float literal
    TK_LITERAL_BIGINT,  // bigint literal 123n
    TK_LITERAL_STRING,  // string literal
    TK_LITERAL_REGEX,   // regex literal /pattern/flags
    TK_NAME,            // identifier

    // Template string
    TK_TEMPLATE_STRING, // template string `hello ${name}`

    // Raw string (r prefix, no escape processing)
    TK_RAW_STRING,           // r"..." or r'...' - raw string, no escapes
    TK_RAW_TEMPLATE_STRING,  // r`...` - raw template string, no escapes but ${} interpolation

    // Special
    TK_EOF,             // end of file
    TK_ERROR            // error
} TokenType;

// Token structure
typedef struct Token {
    TokenType type;
    const char *start;
    int length;
    int line;
    int column;             // 1-indexed column number
    bool has_leading_space; // true if whitespace before this token (for generic disambiguation)
    XrTrivia *leading_trivia;   // Comments before this token
    XrTrivia *trailing_trivia;  // Inline comment after this token (same line)
} Token;

// Scanner state
typedef struct Scanner {
    const char *source;  // source code string
    const char *start;   // current token start position
    const char *current; // current scan position
    const char *end;     // end of source (past last char)
    int line;            // current line number
    const char *line_start; // start of current line (for column calculation)
    bool had_leading_space; // tracks if whitespace was skipped before current token
    bool collect_trivia;    // whether to collect comments as trivia
    XrTrivia *pending_trivia; // collected trivia for next token
    XrTrivia *trivia_tail;     // tail pointer for O(1) append
    const char *pending_error; // error detected during comment scanning (e.g. unterminated)
} Scanner;

// Scanner functions
XR_FUNC void xr_scanner_init(Scanner *scanner, const char *source);
XR_FUNC void xr_scanner_init_with_trivia(Scanner *scanner, const char *source, bool collect_trivia);
XR_FUNC Token xr_scanner_scan(Scanner *scanner);

// Try to scan regex literal when expecting expression
XR_FUNC Token xr_scanner_try_regex(Scanner *scanner);

XR_FUNC const char *xr_token_name(TokenType type);

// Trivia functions
XR_FUNC XrTrivia *xr_trivia_new(XrTriviaType type, const char *start, int length, int line);
XR_FUNC void xr_trivia_free(XrTrivia *trivia);
XR_FUNC void xr_trivia_free_chain(XrTrivia *head);

#endif // XLEX_H
