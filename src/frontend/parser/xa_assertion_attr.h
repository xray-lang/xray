/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_assertion_attr.h - Shared registry for the @no_* system assertion
 * attribute family (task 217).  One table (xa_assertion_attr.def) drives
 * parsing, formatting, position validation, duplicate diagnostics, and the
 * "does this declaration carry the assertion" query for every member, so the
 * proof-source passes (allocation/throw/suspend) never re-derive attribute
 * plumbing.
 */

#ifndef XA_ASSERTION_ATTR_H
#define XA_ASSERTION_ATTR_H

#include "xast_types.h" /* AttributeKind */
#include <stdbool.h>
#include <stddef.h>

typedef struct AstNode AstNode;
typedef struct XrAttribute XrAttribute;

/* Which effect analysis proves an assertion holds (fail-closed source). */
typedef enum XaAssertionProofSource {
    XA_ASSERT_PROOF_ALLOC = 0,   /* allocation effect (task 207) */
    XA_ASSERT_PROOF_THROW = 1,   /* error/throw effect (task 216) */
    XA_ASSERT_PROOF_SUSPEND = 2, /* suspend effect (task 212) */
} XaAssertionProofSource;

/* Compiler stage that enforces the contract. */
typedef enum XaAssertionVerifyStage {
    XA_ASSERT_STAGE_ANALYZER = 0,
    XA_ASSERT_STAGE_AOT_BACKEND = 1,
} XaAssertionVerifyStage;

/* Source positions an assertion attribute may modify (bitmask). */
typedef enum XaAssertionPosition {
    XA_ASSERT_POS_FN = 1u << 0,      /* free / module-level / nested `fn` */
    XA_ASSERT_POS_METHOD = 1u << 1,  /* class / struct / enum method */
    XA_ASSERT_POS_FN_TYPE = 1u << 2, /* function-type prefix `@x fn(...)` */
} XaAssertionPosition;

typedef struct XaAssertionAttrInfo {
    const char *name; /* spelling without '@' */
    size_t name_length;
    AttributeKind kind;
    XaAssertionProofSource proof_source;
    XaAssertionVerifyStage verify_stage;
    unsigned positions; /* XaAssertionPosition bitmask */
    bool usable_in_function_type;
} XaAssertionAttrInfo;

/* Registry lookups. */
const XaAssertionAttrInfo *xa_assertion_attr_by_name(const char *name, int length);
const XaAssertionAttrInfo *xa_assertion_attr_by_kind(AttributeKind kind);
bool xa_attribute_kind_is_assertion(AttributeKind kind);

/* Iteration (stable order matching the .def registry). */
size_t xa_assertion_attr_count(void);
const XaAssertionAttrInfo *xa_assertion_attr_at(size_t index);

/* Position predicates driven by the registry. */
bool xa_assertion_attr_allows_position(AttributeKind kind, XaAssertionPosition position);

/* Shared "does this function-like declaration carry the attribute" query.
 * Handles AST_FUNCTION_DECL / AST_FUNCTION_EXPR / AST_METHOD_DECL uniformly so
 * the allocation and throw-effect passes stop duplicating the walk. */
bool xa_decl_attribute_list(const AstNode *node, XrAttribute ***out_attrs, int *out_count);
bool xa_decl_has_attribute(const AstNode *node, AttributeKind kind);

#endif /* XA_ASSERTION_ATTR_H */
