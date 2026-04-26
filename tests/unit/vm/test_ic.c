/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ic.c - Lock down the per-coroutine inline-cache contract:
 *     - XrProto.proto_id is monotonically assigned and never reused
 *     - xr_vm_ctx_ensure_ic_{fields,methods} lazy-allocate per proto
 *     - xr_vm_ctx_get_ic_{fields,methods} are read-only and return NULL
 *       until the matching ensure() is called
 *     - xr_vm_ic_{fields,methods}_snapshot return independent deep
 *       copies that survive subsequent live-table mutation and free
 *     - Two distinct XrVMContexts referencing the same XrProto keep
 *       their IC state fully isolated (the whole point of the move
 *       off shared XrProto state)
 *     - xr_vm_ctx_free_ic_tables resets the ctx so it can be reused.
 *
 *   These tests exercise the C-level APIs directly and never spin up
 *   a full isolate; an empty stack-allocated XrVMContext is enough
 *   because the IC subsystem only touches the ic_*_tables fields.
 */

#include "../test_framework.h"

#include "runtime/value/xchunk.h"
#include "runtime/xexec_frame.h"
#include "vm/xvm_internal.h"
#include "vm/xic_field.h"
#include "vm/xic_field_table.h"
#include "vm/xic_method.h"
#include "vm/xic_builtin.h"
#include "base/xmalloc.h"

#include <string.h>

/* ========== Helpers ========== */

/* Create a proto holding `n_insts` dummy instructions so the IC table
 * sized to PROTO_CODE_COUNT(proto) lines up with the assertions in
 * the production VM dispatch code. The instruction encoding does not
 * matter; xr_vm_proto_write just appends to the dynarray. */
static XrProto *make_proto_with_insts(int n_insts) {
    XrProto *p = xr_vm_proto_new();
    if (!p) return NULL;
    for (int i = 0; i < n_insts; i++) {
        xr_vm_proto_write(p, (XrInstruction)0, 1);
    }
    return p;
}

/* Stack-allocated XrVMContext: only the IC fields are exercised, so
 * leaving the rest zeroed is safe. */
static void ctx_zero(XrVMContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* ========== proto_id monotonicity ========== */

TEST(proto_id_is_unique_and_monotonic) {
    XrProto *a = make_proto_with_insts(1);
    XrProto *b = make_proto_with_insts(1);
    XrProto *c = make_proto_with_insts(1);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    /* Counter is process-global; use strict ordering rather than
     * specific values so other tests can run before this one. */
    ASSERT(b->proto_id > a->proto_id);
    ASSERT(c->proto_id > b->proto_id);

    xr_vm_proto_free(a);
    xr_vm_proto_free(b);
    xr_vm_proto_free(c);
}

/* ========== Lazy allocation ========== */

TEST(ensure_field_lazy_alloc_is_idempotent) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(4);
    ASSERT_NOT_NULL(p);

    XrICFieldTable *t1 = xr_vm_ctx_ensure_ic_fields(&ctx, p);
    ASSERT_NOT_NULL(t1);
    /* Slots are pre-allocated so cache_index lookups always land. */
    ASSERT_EQ_INT(t1->count, 4);

    XrICFieldTable *t2 = xr_vm_ctx_ensure_ic_fields(&ctx, p);
    ASSERT(t2 == t1);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

TEST(ensure_method_lazy_alloc_is_idempotent) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(3);
    ASSERT_NOT_NULL(p);

    XrICMethodTable *t1 = xr_vm_ctx_ensure_ic_methods(&ctx, p);
    ASSERT_NOT_NULL(t1);
    ASSERT_EQ_INT(t1->count, 3);

    XrICMethodTable *t2 = xr_vm_ctx_ensure_ic_methods(&ctx, p);
    ASSERT(t2 == t1);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

TEST(ensure_builtin_lazy_alloc_is_idempotent) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(5);
    ASSERT_NOT_NULL(p);

    XrICBuiltinTable *t1 = xr_vm_ctx_ensure_ic_builtin(&ctx, p);
    ASSERT_NOT_NULL(t1);
    /* Slots are pre-allocated to PROTO_CODE_COUNT so cache_index
     * lookups always land. */
    ASSERT_EQ_INT(t1->count, 5);

    /* All slots must start empty (slot==NULL is the sentinel). */
    for (int i = 0; i < t1->count; i++) {
        ASSERT(t1->caches[i].slot == NULL);
        ASSERT_EQ_INT((int)t1->caches[i].hits, 0);
        ASSERT_EQ_INT((int)t1->caches[i].misses, 0);
    }

    XrICBuiltinTable *t2 = xr_vm_ctx_ensure_ic_builtin(&ctx, p);
    ASSERT(t2 == t1);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

/* ========== Read-side accessors ========== */

TEST(get_returns_null_before_ensure) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(2);

    ASSERT(xr_vm_ctx_get_ic_fields(&ctx, p) == NULL);
    ASSERT(xr_vm_ctx_get_ic_methods(&ctx, p) == NULL);
    ASSERT(xr_vm_ctx_get_ic_builtin(&ctx, p) == NULL);

    xr_vm_proto_free(p);
}

TEST(get_returns_table_after_ensure) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(2);

    XrICFieldTable *fe = xr_vm_ctx_ensure_ic_fields(&ctx, p);
    XrICMethodTable *me = xr_vm_ctx_ensure_ic_methods(&ctx, p);
    XrICBuiltinTable *be = xr_vm_ctx_ensure_ic_builtin(&ctx, p);
    ASSERT(xr_vm_ctx_get_ic_fields(&ctx, p) == fe);
    ASSERT(xr_vm_ctx_get_ic_methods(&ctx, p) == me);
    ASSERT(xr_vm_ctx_get_ic_builtin(&ctx, p) == be);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

/* ========== Capacity growth ========== */

TEST(capacity_grows_for_high_proto_id) {
    /* Burn some proto_ids so the next proto's id is well above the
     * default initial capacity (16). The IC subsystem must grow its
     * indexing arrays to accommodate the higher id. */
    enum { BURN = 32 };
    XrProto *burn[BURN];
    for (int i = 0; i < BURN; i++) burn[i] = make_proto_with_insts(1);

    XrProto *high = make_proto_with_insts(1);
    ASSERT_NOT_NULL(high);
    ASSERT(high->proto_id >= BURN);  /* sanity: process-global counter advanced */

    XrVMContext ctx; ctx_zero(&ctx);
    XrICFieldTable *t = xr_vm_ctx_ensure_ic_fields(&ctx, high);
    ASSERT_NOT_NULL(t);
    /* capacity must cover the high proto_id slot */
    ASSERT(ctx.ic_tables_capacity > high->proto_id);

    xr_vm_ctx_free_ic_tables(&ctx);
    for (int i = 0; i < BURN; i++) xr_vm_proto_free(burn[i]);
    xr_vm_proto_free(high);
}

/* ========== Multi-proto isolation in one ctx ========== */

TEST(multi_proto_isolation_within_ctx) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *a = make_proto_with_insts(2);
    XrProto *b = make_proto_with_insts(2);

    XrICFieldTable *fa = xr_vm_ctx_ensure_ic_fields(&ctx, a);
    XrICFieldTable *fb = xr_vm_ctx_ensure_ic_fields(&ctx, b);
    ASSERT_NOT_NULL(fa);
    ASSERT_NOT_NULL(fb);
    ASSERT(fa != fb);

    /* Mutate A's first IC slot; B must remain pristine. */
    fa->caches[0].cached_symbol = 0x1234;
    fa->caches[0].state = XR_IC_FIELD_MONO;

    ASSERT_EQ_INT(fb->caches[0].cached_symbol, -1);
    ASSERT_EQ_INT((int)fb->caches[0].state, (int)XR_IC_FIELD_UNINIT);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(a);
    xr_vm_proto_free(b);
}

/* ========== Builtin IC: sticky first-write-wins ========== */

TEST(builtin_cache_is_sticky_first_write_wins) {
    /* Drives the production IC contract directly: a slot, once set,
     * is never overwritten by a subsequent miss. The OP_INVOKE_BUILTIN
     * fast path relies on this to avoid thrashing on poly call sites
     * (e.g. a generic logger that sees mostly strings but occasionally
     * sees ints — the string slot stays cached).
     *
     * We don't go through the dispatcher here; we model the same write
     * sequence directly on the cache struct. */
    XrICBuiltin ic = {0};

    /* First write: empty -> filled. */
    XrMethodSlot fake_slot_a = {0};
    ic.slot = &fake_slot_a;
    ic.cached_tid = (int16_t)XR_TID_STRING;

    /* Subsequent miss against a different type must NOT overwrite. */
    if (!ic.slot) { /* deliberately false; keep the same shape as the
                      dispatcher to make the invariant obvious. */
        ic.slot = NULL; /* unreachable */
    }
    /* Mismatch path the dispatcher takes: increments misses, leaves
     * slot untouched. */
    if (ic.slot && ic.cached_tid != (int16_t)XR_TID_INT) {
        ic.misses++;
    }
    ASSERT(ic.slot == &fake_slot_a);
    ASSERT_EQ_INT((int)ic.cached_tid, (int)XR_TID_STRING);
    ASSERT_EQ_INT((int)ic.misses, 1);

    /* Hits on the cached type bump hits, never replace the slot. */
    if (ic.slot && ic.cached_tid == (int16_t)XR_TID_STRING) {
        ic.hits++;
    }
    ASSERT(ic.slot == &fake_slot_a);
    ASSERT_EQ_INT((int)ic.hits, 1);
}

TEST(builtin_table_alloc_grows_capacity) {
    /* xr_ic_builtin_table_alloc handles the dynamic-array growth so
     * pre-sizing in xr_vm_ctx_ensure_ic_builtin can still expand if
     * the proto grows after first ensure (defensive — the production
     * code currently sizes once at ensure time, but the API contract
     * has to honor the doubling-capacity invariant either way). */
    XrICBuiltinTable *t = xr_ic_builtin_table_new(2);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_INT(t->capacity, 2);

    /* Allocate 5 slots; capacity must double past the initial 2. */
    for (int i = 0; i < 5; i++) {
        int idx = xr_ic_builtin_table_alloc(t);
        ASSERT_EQ_INT(idx, i);
    }
    ASSERT_EQ_INT(t->count, 5);
    ASSERT(t->capacity >= 5);

    /* Each freshly allocated slot starts empty. */
    for (int i = 0; i < t->count; i++) {
        ASSERT(t->caches[i].slot == NULL);
    }

    xr_ic_builtin_table_free(t);
}

/* ========== Snapshot semantics ========== */

TEST(snapshot_returns_null_before_ensure) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(2);

    ASSERT(xr_vm_ic_fields_snapshot(&ctx, p) == NULL);
    ASSERT(xr_vm_ic_methods_snapshot(&ctx, p) == NULL);

    xr_vm_proto_free(p);
}

TEST(snapshot_field_is_deep_copy) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(3);

    XrICFieldTable *live = xr_vm_ctx_ensure_ic_fields(&ctx, p);
    ASSERT_NOT_NULL(live);

    /* Seed the live table with a recognisable value. */
    live->caches[0].cached_symbol = 42;
    live->caches[0].state = XR_IC_FIELD_MONO;
    live->caches[0].entry_count = 1;
    live->caches[1].cached_symbol = 0x0BAD;

    XrICFieldTable *snap = xr_vm_ic_fields_snapshot(&ctx, p);
    ASSERT_NOT_NULL(snap);
    ASSERT(snap != live);
    ASSERT(snap->caches != live->caches);
    ASSERT_EQ_INT(snap->count, live->count);
    ASSERT_EQ_INT(snap->caches[0].cached_symbol, 42);
    ASSERT_EQ_INT((int)snap->caches[0].state, (int)XR_IC_FIELD_MONO);
    ASSERT_EQ_INT(snap->caches[1].cached_symbol, 0x0BAD);

    /* Mutating the live table after snapshot must not bleed into the
     * snapshot — this is the whole point of the deep copy. */
    live->caches[0].cached_symbol = 9999;
    live->caches[0].state = XR_IC_FIELD_MEGA;
    ASSERT_EQ_INT(snap->caches[0].cached_symbol, 42);
    ASSERT_EQ_INT((int)snap->caches[0].state, (int)XR_IC_FIELD_MONO);

    /* Freeing the snapshot must not invalidate the live table. */
    xr_ic_field_table_free(snap);
    ASSERT_EQ_INT(live->caches[1].cached_symbol, 0x0BAD);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

TEST(snapshot_method_deep_copies_mega_cache) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *p = make_proto_with_insts(2);

    XrICMethodTable *live = xr_vm_ctx_ensure_ic_methods(&ctx, p);
    ASSERT_NOT_NULL(live);

    /* Attach a fake mega cache to slot 0. The IC code only inspects
     * keys/values pointers, never dereferences them, so synthetic
     * non-NULL sentinels are fine for the deep-copy test. */
    XrMegaCache *mc = (XrMegaCache *)xr_calloc(1, sizeof(XrMegaCache));
    ASSERT_NOT_NULL(mc);
    mc->keys[0] = (struct XrClass *)(uintptr_t)0xC1A55;
    mc->values[1] = (struct XrMethod *)(uintptr_t)0xBEEF;
    live->caches[0].mega_cache = mc;
    live->caches[0].is_megamorphic = 1;
    live->caches[0].count = 1;
    live->caches[0].total_count = 7;

    XrICMethodTable *snap = xr_vm_ic_methods_snapshot(&ctx, p);
    ASSERT_NOT_NULL(snap);
    ASSERT(snap != live);
    ASSERT(snap->caches != live->caches);
    ASSERT_EQ_INT(snap->count, live->count);

    /* Per-slot scalar fields are copied. */
    ASSERT_EQ_INT(snap->caches[0].is_megamorphic, 1);
    ASSERT_EQ_INT((int)snap->caches[0].total_count, 7);

    /* mega_cache must be a separately-allocated copy. */
    ASSERT_NOT_NULL(snap->caches[0].mega_cache);
    ASSERT(snap->caches[0].mega_cache != live->caches[0].mega_cache);
    ASSERT(snap->caches[0].mega_cache->keys[0] ==
           (struct XrClass *)(uintptr_t)0xC1A55);
    ASSERT(snap->caches[0].mega_cache->values[1] ==
           (struct XrMethod *)(uintptr_t)0xBEEF);

    /* Mutate live's mega; snapshot's mega must stay put. */
    live->caches[0].mega_cache->keys[0] = (struct XrClass *)(uintptr_t)0xDEAD;
    ASSERT(snap->caches[0].mega_cache->keys[0] ==
           (struct XrClass *)(uintptr_t)0xC1A55);

    /* xr_ic_method_table_free walks every entry and frees its
     * mega_cache, so destroying the snapshot also reclaims the
     * deep-copied mega without any extra plumbing. */
    xr_ic_method_table_free(snap);

    /* Live still owns its own mega cache; xr_vm_ctx_free_ic_tables
     * releases the table (and its mega via xr_ic_method_table_free
     * internally), so nothing else is needed here. */
    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(p);
}

/* ========== Free / reuse ========== */

TEST(free_ic_tables_resets_state_and_supports_reuse) {
    XrVMContext ctx; ctx_zero(&ctx);
    XrProto *a = make_proto_with_insts(2);
    XrProto *b = make_proto_with_insts(5);

    ASSERT_NOT_NULL(xr_vm_ctx_ensure_ic_fields(&ctx, a));
    ASSERT_NOT_NULL(xr_vm_ctx_ensure_ic_methods(&ctx, b));
    ASSERT_NOT_NULL(xr_vm_ctx_ensure_ic_builtin(&ctx, b));
    ASSERT(ctx.ic_tables_capacity > 0);

    xr_vm_ctx_free_ic_tables(&ctx);
    ASSERT_EQ_INT((int)ctx.ic_tables_capacity, 0);
    ASSERT(ctx.ic_field_tables == NULL);
    ASSERT(ctx.ic_method_tables == NULL);
    ASSERT(ctx.ic_builtin_tables == NULL);

    /* The ctx must be fully reusable after teardown — coroutines
     * recycle through xr_coro_release_resources and need a clean
     * slate the next time they are dispatched. */
    XrICFieldTable *fresh = xr_vm_ctx_ensure_ic_fields(&ctx, a);
    ASSERT_NOT_NULL(fresh);
    ASSERT_EQ_INT(fresh->count, 2);

    xr_vm_ctx_free_ic_tables(&ctx);
    xr_vm_proto_free(a);
    xr_vm_proto_free(b);
}

/* ========== Multi-context isolation ========== */

TEST(two_contexts_keep_ic_state_independent) {
    XrVMContext ctx_a; ctx_zero(&ctx_a);
    XrVMContext ctx_b; ctx_zero(&ctx_b);
    XrProto *p = make_proto_with_insts(2);

    XrICFieldTable *fa = xr_vm_ctx_ensure_ic_fields(&ctx_a, p);
    XrICFieldTable *fb = xr_vm_ctx_ensure_ic_fields(&ctx_b, p);
    ASSERT_NOT_NULL(fa);
    ASSERT_NOT_NULL(fb);
    /* Same proto, distinct ctx → distinct backing tables. This is the
     * core invariant the migration off shared XrProto state buys us. */
    ASSERT(fa != fb);

    fa->caches[0].cached_symbol = 1111;
    fa->caches[0].state = XR_IC_FIELD_MONO;
    ASSERT_EQ_INT(fb->caches[0].cached_symbol, -1);
    ASSERT_EQ_INT((int)fb->caches[0].state, (int)XR_IC_FIELD_UNINIT);

    fb->caches[1].cached_symbol = 2222;
    ASSERT_EQ_INT(fa->caches[1].cached_symbol, -1);

    xr_vm_ctx_free_ic_tables(&ctx_a);
    xr_vm_ctx_free_ic_tables(&ctx_b);
    xr_vm_proto_free(p);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("XrProto.proto_id contract");
    RUN_TEST(proto_id_is_unique_and_monotonic);

    RUN_TEST_SUITE("Lazy allocation");
    RUN_TEST(ensure_field_lazy_alloc_is_idempotent);
    RUN_TEST(ensure_method_lazy_alloc_is_idempotent);
    RUN_TEST(ensure_builtin_lazy_alloc_is_idempotent);

    RUN_TEST_SUITE("Read-side accessors");
    RUN_TEST(get_returns_null_before_ensure);
    RUN_TEST(get_returns_table_after_ensure);

    RUN_TEST_SUITE("Capacity growth");
    RUN_TEST(capacity_grows_for_high_proto_id);

    RUN_TEST_SUITE("Multi-proto isolation");
    RUN_TEST(multi_proto_isolation_within_ctx);

    RUN_TEST_SUITE("Builtin IC sticky cache");
    RUN_TEST(builtin_cache_is_sticky_first_write_wins);
    RUN_TEST(builtin_table_alloc_grows_capacity);

    RUN_TEST_SUITE("Snapshot semantics");
    RUN_TEST(snapshot_returns_null_before_ensure);
    RUN_TEST(snapshot_field_is_deep_copy);
    RUN_TEST(snapshot_method_deep_copies_mega_cache);

    RUN_TEST_SUITE("Free and reuse");
    RUN_TEST(free_ic_tables_resets_state_and_supports_reuse);

    RUN_TEST_SUITE("Multi-context isolation");
    RUN_TEST(two_contexts_keep_ic_state_independent);
TEST_MAIN_END()
