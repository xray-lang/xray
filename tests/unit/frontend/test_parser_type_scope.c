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

#include "base/xcompile_state.h"

static XrCompileResources *scope_resources;
static XrCompileState *scope_state;
static uint64_t scope_baseline;

static void scope_owner_check(bool condition) {
    if (!condition) {
        fprintf(stderr, "Type scope fixture ownership failed\n");
        abort();
    }
}

static void scope_fixture_open(void) {
    const XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResourceStats stats = {0};
    scope_owner_check(xr_compile_resources_new(&limits, &scope_resources) == XR_COMPILE_RESOURCE_OK);
    scope_owner_check(xr_compile_resources_stats(scope_resources, &stats) == XR_COMPILE_RESOURCE_OK);
    scope_baseline = stats.held_bytes;
    scope_owner_check(xr_compile_state_new(scope_resources, &scope_state) == XR_COMPILE_RESOURCE_OK);
}

static void scope_fixture_close(void) {
    XrCompileResourceStats stats = {0};
    scope_owner_check(xr_compile_state_status(scope_state) == XR_COMPILE_RESOURCE_OK);
    xr_compile_state_release(scope_state);
    scope_owner_check(xr_compile_resources_stats(scope_resources, &stats) == XR_COMPILE_RESOURCE_OK);
    scope_owner_check(stats.held_bytes == scope_baseline);
    xr_compile_resources_release(scope_resources);
}


/* ====================================================================== */
/* Dummy types -- pointer-equality is all the test needs                   */
/* ====================================================================== */

// xtype_scope stores `XrTypeRef *`; it never derefs. We use stack-
// allocated dummies cast through an explicit pointer so the type
// system stays happy.
typedef struct DummyType {
    int marker;
} DummyType;

static struct XrTypeRef *as_tref(DummyType *p) {
    return (struct XrTypeRef *) p;
}

/* ====================================================================== */
/* Tests                                                                   */
/* ====================================================================== */

TEST(define_then_lookup_returns_same_type) {
    XrTypeScope *scope = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &scope), XR_COMPILE_RESOURCE_OK);
    ASSERT_NOT_NULL(scope);

    DummyType t = {1};
    XrTypeAlias *e = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(scope, "Foo", as_tref(&t), &e), XR_TYPE_SCOPE_OK);
    ASSERT_NOT_NULL(e);
    ASSERT_STR_EQ(e->name, "Foo");
    ASSERT_EQ_PTR(e->type_ref, &t);

    // lookup chain (no parent) and resolve match.
    XrTypeAlias *got = xr_type_scope_lookup(scope, "Foo");
    ASSERT_EQ_PTR(got, e);

    XrTypeRef *resolved = xr_type_scope_resolve(scope, "Foo");
    ASSERT_EQ_PTR(resolved, &t);

    xr_type_scope_free(scope);
}

TEST(duplicate_define_returns_null) {
    XrTypeScope *scope = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &scope), XR_COMPILE_RESOURCE_OK);
    ASSERT_NOT_NULL(scope);

    DummyType t1 = {1}, t2 = {2};
    XrTypeAlias *first = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(scope, "T", as_tref(&t1), &first), XR_TYPE_SCOPE_OK);
    ASSERT_NOT_NULL(first);

    // Duplicate in the SAME scope must fail -- the parser uses this
    // signal to emit a "duplicate alias" diagnostic.
    XrTypeAlias *dup = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(scope, "T", as_tref(&t2), &dup), XR_TYPE_SCOPE_DUPLICATE);
    ASSERT_NULL(dup);

    // Original entry must still be intact.
    XrTypeAlias *got = xr_type_scope_lookup(scope, "T");
    ASSERT_EQ_PTR(got, first);
    ASSERT_EQ_PTR(got->type_ref, &t1);

    xr_type_scope_free(scope);
}

TEST(lookup_walks_to_parent_scope) {
    XrTypeScope *outer = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &outer), XR_COMPILE_RESOURCE_OK);
    XrTypeScope *inner = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, outer, &inner), XR_COMPILE_RESOURCE_OK);
    ASSERT_NOT_NULL(outer);
    ASSERT_NOT_NULL(inner);

    DummyType t = {42};
    XrTypeAlias *defined_alias_4 = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(outer, "Foo", as_tref(&t), &defined_alias_4), XR_TYPE_SCOPE_OK);

    // Inner does not have Foo; chained lookup must find it on the
    // outer scope.
    XrTypeAlias *via_chain = xr_type_scope_lookup(inner, "Foo");
    ASSERT_NOT_NULL(via_chain);
    ASSERT_EQ_PTR(via_chain->type_ref, &t);

    // lookup_local must NOT find it -- this is the API the parser
    // uses to detect re-declaration in the current scope only.
    ASSERT_NULL(xr_type_scope_lookup_local(inner, "Foo"));

    // Inner scope free must NOT affect the outer scope's entries.
    xr_type_scope_free(inner);
    XrTypeAlias *still_there = xr_type_scope_lookup(outer, "Foo");
    ASSERT_NOT_NULL(still_there);
    ASSERT_EQ_PTR(still_there->type_ref, &t);

    xr_type_scope_free(outer);
}

TEST(generic_param_shadow_outer_alias) {
    // The canonical case the plan called out (P-01 §4):
    //   type T = int;       // outer scope
    //   fn foo<T>() { ... } // inner scope binds T as a parameter
    // Inside foo, T must resolve to the parameter, not the alias.
    // After foo finishes parsing the inner scope is freed and the
    // outer alias must remain bound.
    XrTypeScope *outer = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &outer), XR_COMPILE_RESOURCE_OK);

    DummyType outer_t = {100};
    DummyType inner_t = {200};

    XrTypeAlias *defined_alias_5 = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(outer, "T", as_tref(&outer_t), &defined_alias_5), XR_TYPE_SCOPE_OK);

    XrTypeScope *inner = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, outer, &inner), XR_COMPILE_RESOURCE_OK);
    XrTypeAlias *defined_alias_6 = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(inner, "T", as_tref(&inner_t), &defined_alias_6), XR_TYPE_SCOPE_OK);

    // Inside the inner scope, T resolves to the inner type
    // (generic-parameter shadow).
    XrTypeRef *inner_view = xr_type_scope_resolve(inner, "T");
    ASSERT_EQ_PTR(inner_view, &inner_t);

    // lookup_local on inner returns the inner entry (not the outer).
    XrTypeAlias *local = xr_type_scope_lookup_local(inner, "T");
    ASSERT_NOT_NULL(local);
    ASSERT_EQ_PTR(local->type_ref, &inner_t);

    // Pop the inner scope (mirroring the parser's "fn body finished"
    // step). The outer T must still resolve to the outer type.
    xr_type_scope_free(inner);

    XrTypeRef *outer_view = xr_type_scope_resolve(outer, "T");
    ASSERT_EQ_PTR(outer_view, &outer_t);

    xr_type_scope_free(outer);
}

TEST(lookup_unknown_is_null) {
    XrTypeScope *scope = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &scope), XR_COMPILE_RESOURCE_OK);
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
    // (e.g. `type A = A` self-reference detection) -> it defines
    // `A` with type=NULL, parses the RHS, then patches the type.
    // The contract is that the entry pointer is stable across
    // the parse so the patch is visible to later lookups.
    XrTypeScope *scope = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_open(scope_state, NULL, &scope), XR_COMPILE_RESOURCE_OK);

    XrTypeAlias *e = NULL;
    ASSERT_EQ_INT(xr_compile_type_scope_define(scope, "A", NULL, &e), XR_TYPE_SCOPE_OK);
    ASSERT_NOT_NULL(e);
    ASSERT_NULL(e->type_ref);

    // Patch.
    DummyType t = {7};
    e->type_ref = as_tref(&t);

    // Subsequent lookups see the patched value.
    XrTypeRef *now = xr_type_scope_resolve(scope, "A");
    ASSERT_EQ_PTR(now, &t);

    xr_type_scope_free(scope);
}

/* ====================================================================== */
/* Driver                                                                  */
/* ====================================================================== */

TEST_MAIN_BEGIN()
scope_fixture_open();
RUN_TEST_SUITE("P-01 parser-owned type scope");
RUN_TEST(define_then_lookup_returns_same_type);
RUN_TEST(duplicate_define_returns_null);
RUN_TEST(lookup_walks_to_parent_scope);
RUN_TEST(generic_param_shadow_outer_alias);
RUN_TEST(lookup_unknown_is_null);
RUN_TEST(null_safe_read_api);
RUN_TEST(forward_declared_alias_can_be_patched);
scope_fixture_close();
TEST_MAIN_END()
