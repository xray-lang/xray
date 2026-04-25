/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_parser_type_scope.c - P-01 acceptance tests
 *
 * KEY CONCEPT:
 *   Pre-Phase-1 the parser borrowed XaScope (analyzer/) for the
 *   tiny name->type mapping it needs while parsing `type` aliases
 *   and `fn<T>` generic parameters. That made parser/ depend on
 *   analyzer/ purely for a 60-line table.
 *
 *   Phase 1 (P-01) replaced the dependency with src/frontend/parser/
 *   xtype_scope.{c,h}. These tests pin down the contract the parser
 *   relies on:
 *
 *     1. define / lookup round trip (single scope).
 *     2. duplicate define in the same scope returns NULL (caller
 *        must report a "duplicate alias" error -- this is the only
 *        signal it gets).
 *     3. nested scope lookup walks the chain to the parent;
 *        lookup_local stays local.
 *     4. inner scope SHADOWS an outer alias: inner lookup wins, and
 *        when the inner scope is freed the outer alias is unaffected
 *        (the canonical generic-parameter shadow case).
 *     5. NULL safety: every entry point absorbs NULL inputs without
 *        crashing.
 *     6. forward-declared aliases (type==NULL at define time) can
 *        be patched by mutating entry->type, and the patch is
 *        observable through subsequent lookups.
 *
 *   The test does NOT exercise XrType internals -- it only stores
 *   pointers and compares them. We can therefore use opaque dummy
 *   structs as type values, decoupling the test from any future
 *   XrType layout change.
 */

#include "../test_framework.h"
#include "frontend/parser/xtype_scope.h"

#include <stdint.h>
#include <stdlib.h>

/* ====================================================================== */
/* Dummy types -- pointer-equality is all the test needs                   */
/* ====================================================================== */

// xtype_scope stores `XrType *`; it never derefs. We use stack-
// allocated dummies cast through an explicit pointer so the type
// system stays happy.
typedef struct DummyType { int marker; } DummyType;

static struct XrType *as_type(DummyType *p) {
    return (struct XrType *)p;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(define_then_lookup_returns_same_type) {
    XrTypeScope *scope = xr_type_scope_new(NULL);
    ASSERT_NOT_NULL(scope);

    DummyType t = { 1 };
    XrTypeAlias *e = xr_type_scope_define(scope, "Foo", as_type(&t));
    ASSERT_NOT_NULL(e);
    ASSERT_STR_EQ(e->name, "Foo");
    ASSERT_EQ_PTR(e->type, &t);

    // lookup chain (no parent) and resolve match.
    XrTypeAlias *got = xr_type_scope_lookup(scope, "Foo");
    ASSERT_EQ_PTR(got, e);

    XrType *resolved = xr_type_scope_resolve(scope, "Foo");
    ASSERT_EQ_PTR(resolved, &t);

    xr_type_scope_free(scope);
}

TEST(duplicate_define_returns_null) {
    XrTypeScope *scope = xr_type_scope_new(NULL);
    ASSERT_NOT_NULL(scope);

    DummyType t1 = { 1 }, t2 = { 2 };
    XrTypeAlias *first  = xr_type_scope_define(scope, "T", as_type(&t1));
    ASSERT_NOT_NULL(first);

    // Duplicate in the SAME scope must fail -- the parser uses this
    // signal to emit a "duplicate alias" diagnostic.
    XrTypeAlias *dup = xr_type_scope_define(scope, "T", as_type(&t2));
    ASSERT_NULL(dup);

    // Original entry must still be intact.
    XrTypeAlias *got = xr_type_scope_lookup(scope, "T");
    ASSERT_EQ_PTR(got, first);
    ASSERT_EQ_PTR(got->type, &t1);

    xr_type_scope_free(scope);
}

TEST(lookup_walks_to_parent_scope) {
    XrTypeScope *outer = xr_type_scope_new(NULL);
    XrTypeScope *inner = xr_type_scope_new(outer);
    ASSERT_NOT_NULL(outer);
    ASSERT_NOT_NULL(inner);

    DummyType t = { 42 };
    xr_type_scope_define(outer, "Foo", as_type(&t));

    // Inner does not have Foo; chained lookup must find it on the
    // outer scope.
    XrTypeAlias *via_chain = xr_type_scope_lookup(inner, "Foo");
    ASSERT_NOT_NULL(via_chain);
    ASSERT_EQ_PTR(via_chain->type, &t);

    // lookup_local must NOT find it -- this is the API the parser
    // uses to detect re-declaration in the current scope only.
    ASSERT_NULL(xr_type_scope_lookup_local(inner, "Foo"));

    // Inner scope free must NOT affect the outer scope's entries.
    xr_type_scope_free(inner);
    XrTypeAlias *still_there = xr_type_scope_lookup(outer, "Foo");
    ASSERT_NOT_NULL(still_there);
    ASSERT_EQ_PTR(still_there->type, &t);

    xr_type_scope_free(outer);
}

TEST(generic_param_shadow_outer_alias) {
    // The canonical case the plan called out (P-01 §4):
    //   type T = int;       // outer scope
    //   fn foo<T>() { ... } // inner scope binds T as a parameter
    // Inside foo, T must resolve to the parameter, not the alias.
    // After foo finishes parsing the inner scope is freed and the
    // outer alias must remain bound.
    XrTypeScope *outer = xr_type_scope_new(NULL);

    DummyType outer_t = { 100 };
    DummyType inner_t = { 200 };

    xr_type_scope_define(outer, "T", as_type(&outer_t));

    XrTypeScope *inner = xr_type_scope_new(outer);
    xr_type_scope_define(inner, "T", as_type(&inner_t));

    // Inside the inner scope, T resolves to the inner type
    // (generic-parameter shadow).
    XrType *inner_view = xr_type_scope_resolve(inner, "T");
    ASSERT_EQ_PTR(inner_view, &inner_t);

    // lookup_local on inner returns the inner entry (not the outer).
    XrTypeAlias *local = xr_type_scope_lookup_local(inner, "T");
    ASSERT_NOT_NULL(local);
    ASSERT_EQ_PTR(local->type, &inner_t);

    // Pop the inner scope (mirroring the parser's "fn body finished"
    // step). The outer T must still resolve to the outer type.
    xr_type_scope_free(inner);

    XrType *outer_view = xr_type_scope_resolve(outer, "T");
    ASSERT_EQ_PTR(outer_view, &outer_t);

    xr_type_scope_free(outer);
}

TEST(lookup_unknown_is_null) {
    XrTypeScope *scope = xr_type_scope_new(NULL);
    ASSERT_NULL(xr_type_scope_lookup(scope, "Nope"));
    ASSERT_NULL(xr_type_scope_lookup_local(scope, "Nope"));
    ASSERT_NULL(xr_type_scope_resolve(scope, "Nope"));
    xr_type_scope_free(scope);
}

TEST(null_safe_read_api) {
    // The READ-side entry points (free / lookup / lookup_local /
    // resolve) absorb NULL without crashing -- they are exposed to
    // analyzer / formatter call sites that may not own a scope.
    //
    // `xr_type_scope_define` is intentionally NOT NULL-safe: it
    // ASSERTs on a NULL scope (XR_DCHECK in xtype_scope.c). The
    // contract there is "the parser always owns a scope when it
    // calls define"; passing NULL is a programmer error and should
    // fail loudly. We deliberately do NOT exercise that path here.
    xr_type_scope_free(NULL);
    ASSERT_NULL(xr_type_scope_lookup(NULL, "x"));
    ASSERT_NULL(xr_type_scope_lookup_local(NULL, "x"));
    ASSERT_NULL(xr_type_scope_resolve(NULL, "x"));
}

TEST(forward_declared_alias_can_be_patched) {
    // The parser uses NULL-typed entries as a forward-decl guard
    // (e.g. `type A = A` self-reference detection): it defines
    // `A` with type=NULL, parses the RHS, then patches the type.
    // The contract is that the entry pointer is stable across
    // the parse so the patch is visible to later lookups.
    XrTypeScope *scope = xr_type_scope_new(NULL);

    XrTypeAlias *e = xr_type_scope_define(scope, "A", NULL);
    ASSERT_NOT_NULL(e);
    ASSERT_NULL(e->type);

    // Patch.
    DummyType t = { 7 };
    e->type = as_type(&t);

    // Subsequent lookups see the patched value.
    XrType *now = xr_type_scope_resolve(scope, "A");
    ASSERT_EQ_PTR(now, &t);

    xr_type_scope_free(scope);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("P-01 parser-owned type scope");
    RUN_TEST(define_then_lookup_returns_same_type);
    RUN_TEST(duplicate_define_returns_null);
    RUN_TEST(lookup_walks_to_parent_scope);
    RUN_TEST(generic_param_shadow_outer_alias);
    RUN_TEST(lookup_unknown_is_null);
    RUN_TEST(null_safe_read_api);
    RUN_TEST(forward_declared_alias_can_be_patched);
TEST_MAIN_END()
