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
#include "../../shared/xr_derive_flags.h"

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
    XrLiteralEscapeMode escape_mode;
    XrLiteralSourceForm source_form;
    /* Integer literals keep their original 64-bit bit pattern so uint64
     * contexts can distinguish 0xffff_ffff_ffff_ffff from a parse-time
     * signed overflow. raw_value.int_val remains the signed view used by
     * older call sites. */
    uint64_t int_bits;
    bool int_overflows_i64;
    /* String literals only: true when this node is a literal chunk of a
     * template string rather than a value in its own right. `"a${"b"}"` holds
     * two string literals — the chunk `a` and the interpolated `"b"` — and
     * nothing else tells them apart, so the formatter would print the second
     * one unquoted and silently turn the template into `"ab"`. */
    bool is_template_chunk;
    union {
        int64_t int_val;
        double float_val;
        const char *bigint_val;
        const char *string_val;
        uint32_t rune_val;
        bool bool_val;
        struct {
            const char *pattern;
            const char *flags;
        } regex;
    } raw_value;
} LiteralNode;

typedef struct FixedBytesLiteralNode {
    const uint8_t *payload;
    size_t payload_length;
    bool append_nul;
    XrLiteralEscapeMode escape_mode;
    XrLiteralSourceForm source_form;
} FixedBytesLiteralNode;

// Public attribute node.
typedef struct XrAttribute {
    AttributeKind kind;
    int timeout;
    // Optional @deprecated message. Arena-allocated, NUL-terminated; NULL when absent.
    const char *str_arg;
    // Bitmask of XR_DERIVE_* for ATTR_DERIVE; 0 otherwise.
    uint32_t derive_flags;
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

// Block node
typedef struct BlockNode {
    AstNode **statements;
    int count;
    int capacity;
    /* Canonicalizer value block: `{ var __t = recv; <expr using __t> }`,
     * produced when a place expression must be evaluated exactly once
     * (spec §3.0 E5/E6).  It is an expression, not a user scope: its last
     * statement produces the block's value and it declares no defer scope.
     * A user-written block never carries this, so an unflagged AST_BLOCK in
     * expression position stays a compiler bug. */
    bool is_canon_value_block;
} BlockNode;

#endif  // XAST_NODES_COMMON_H
