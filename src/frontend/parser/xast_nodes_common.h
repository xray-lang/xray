/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes_common.h - AST common base types
 *
 * KEY CONCEPT (P-03):
 *   Forward decls + the small "leaf" structs that every other AST topic
 *   header (expr / stmt / decl) reuses. Splitting xast_nodes.h was
 *   forced by the 800-line hard limit.
 *
 *   IMPORTANT: this header MUST NOT define struct AstNode itself; the
 *   union over all node payloads lives in xast_nodes.h, after all four
 *   topic headers have been pulled in.
 */

#ifndef XAST_NODES_COMMON_H
#define XAST_NODES_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "xast_types.h"
#include "../lexer/xlex.h"

typedef struct AstNode AstNode;
typedef struct XrType XrType;
typedef struct XrTypeRef XrTypeRef;
struct XrArena;

/* ========== Leaf / shared primitives ========== */

typedef struct BinaryNode {
    AstNode *left;
    AstNode *right;
} BinaryNode;

typedef struct UnaryNode {
    AstNode *operand;
} UnaryNode;

// Literal node
typedef struct LiteralNode {
    LiteralKind kind;
    union {
        int64_t int_val;
        double float_val;
        const char *bigint_val;
        const char *string_val;
        bool bool_val;
        struct {
            const char *pattern;
            const char *flags;
        } regex;
    } raw_value;
} LiteralNode;

// Attribute node (test framework)
typedef struct XrAttribute {
    AttributeKind kind;
    int timeout;
} XrAttribute;

// Destructuring pattern structure (flat only, no nesting)
typedef struct XrDestructurePattern {
    PatternType type;

    union {
        struct {
            struct XrDestructurePattern **elements;
            int element_count;
        } array;

        struct {
            char **field_names;
            struct XrDestructurePattern **patterns;
            int field_count;
            bool use_shorthand;
        } object;

        struct {
            char *name;
            XrTypeRef *type;
            uint32_t symbol_id;
        } identifier;
    } as;
} XrDestructurePattern;

/* ========== Program / Block ==========
 *
 * Program node arena rules:
 *   arena owns all AST memory for this program (set by xr_parse_*).
 *   When owns_arena is true, xr_program_destroy destroys it.
 *   When false (e.g. xr_parse_recoverable with caller-provided arena),
 *   xr_program_destroy is a no-op.
 */
typedef struct ProgramNode {
    AstNode **statements;
    int count;
    int capacity;
    struct XrArena *arena;
    bool owns_arena;
} ProgramNode;

// print statement node
typedef struct PrintNode {
    AstNode **exprs;
    int expr_count;
    /* Runtime behaviour modifier.  When true, every argument is
     * evaluated but arguments that evaluate to null are silently
     * skipped (no output, no newline).  Set by the REPL auto-echo
     * rewrite so `f()` returning null does not clutter the prompt
     * with "null".  User-written `print()` always has this as
     * false, preserving `print(null)` -> "null". */
    bool skip_null;
} PrintNode;

// Block node
typedef struct BlockNode {
    AstNode **statements;
    int count;
    int capacity;
} BlockNode;

#endif  // XAST_NODES_COMMON_H
