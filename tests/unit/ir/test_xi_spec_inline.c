/*
 * Unit tests for xi_opt_spec_inline — speculative devirtualization.
 */

#include "../../../src/ir/xi_opt_spec_inline.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ic.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                                     \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_spec_inline", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* Create a minimal IC table with a mono entry for a given value_id. */
static void attach_mono_ic(XiFunc *f, uint32_t value_id, uint32_t type_id) {
    if (!f->ic_table) {
        f->ic_table = (XiIcTable *) xr_malloc(sizeof(XiIcTable));
        memset(f->ic_table, 0, sizeof(XiIcTable));
    }

    if (f->ic_table->nentries >= f->ic_table->capacity) {
        uint32_t new_cap = f->ic_table->capacity ? f->ic_table->capacity * 2 : 4;
        f->ic_table->entries =
            (XiIcMeta *) xr_realloc(f->ic_table->entries, new_cap * sizeof(XiIcMeta));
        f->ic_table->capacity = new_cap;
    }

    XiIcMeta *meta = &f->ic_table->entries[f->ic_table->nentries++];
    memset(meta, 0, sizeof(XiIcMeta));
    meta->value_id = value_id;
    meta->kind = XI_IC_MONO;
    meta->total_count = 500;
    meta->ntargets = 1;
    meta->targets[0].type_id = type_id;
    meta->targets[0].hit_count = 500;

    f->invariant_mask |= XI_INV_IC_ATTACHED;
}

TEST(no_ic_is_noop) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(!c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD);

    xi_func_free(f);
}

TEST(mono_with_guard_devirtualizes) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    /* Guard: XI_GUARD_TYPE for receiver with type_id = 0xBEEF */
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    /* Call method using the original receiver (not guarded yet). */
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    /* Attach mono IC with matching type_id. */
    attach_mono_ic(f, call->id, 0xBEEF);

    /* Compute dominators for guard lookup. */
    xi_ensure_dominators(f);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD_DIRECT);
    ASSERT(call->args[0] == guard);

    xi_func_free(f);
}

TEST(no_guard_no_devirt) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    attach_mono_ic(f, call->id, 0xBEEF);
    xi_ensure_dominators(f);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(!c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD);

    xi_func_free(f);
}

TEST(type_mismatch_no_devirt) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    /* IC says type is 0xCAFE but guard says 0xBEEF — mismatch. */
    attach_mono_ic(f, call->id, 0xCAFE);
    xi_ensure_dominators(f);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(!c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD);

    xi_func_free(f);
}

static void attach_poly_ic(XiFunc *f, uint32_t value_id, uint32_t type_a, uint32_t type_b) {
    if (!f->ic_table) {
        f->ic_table = (XiIcTable *) xr_malloc(sizeof(XiIcTable));
        memset(f->ic_table, 0, sizeof(XiIcTable));
    }

    if (f->ic_table->nentries >= f->ic_table->capacity) {
        uint32_t new_cap = f->ic_table->capacity ? f->ic_table->capacity * 2 : 4;
        f->ic_table->entries =
            (XiIcMeta *) xr_realloc(f->ic_table->entries, new_cap * sizeof(XiIcMeta));
        f->ic_table->capacity = new_cap;
    }

    XiIcMeta *meta = &f->ic_table->entries[f->ic_table->nentries++];
    memset(meta, 0, sizeof(XiIcMeta));
    meta->value_id = value_id;
    meta->kind = XI_IC_POLY;
    meta->total_count = 500;
    meta->ntargets = 2;
    meta->targets[0].type_id = type_a;
    meta->targets[0].hit_count = 350;
    meta->targets[1].type_id = type_b;
    meta->targets[1].hit_count = 150;

    f->invariant_mask |= XI_INV_IC_ATTACHED;
}

TEST(poly_ic_devirt_when_guard_matches) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    attach_poly_ic(f, call->id, 0xBEEF, 0xCAFE);
    xi_ensure_dominators(f);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD_DIRECT);
    ASSERT(call->args[0] == guard);

    xi_func_free(f);
}

TEST(poly_ic_no_matching_guard_type) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    attach_poly_ic(f, call->id, 0xCAFE, 0xDEAD);
    xi_ensure_dominators(f);

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(!c.values_changed);
    ASSERT(call->op == XI_CALL_METHOD);

    xi_func_free(f);
}

TEST(null_func_safe) {
    XiPassChange c = xi_opt_spec_inline(NULL);
    ASSERT(!c.values_changed);
}

TEST(empty_ic_table_noop) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    xi_block_set_return(entry, call);

    /* Attach empty IC table. */
    f->ic_table = (XiIcTable *) xr_malloc(sizeof(XiIcTable));
    memset(f->ic_table, 0, sizeof(XiIcTable));
    f->invariant_mask |= XI_INV_IC_ATTACHED;

    XiPassChange c = xi_opt_spec_inline(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Spec Inline Tests ===\n\n");

    run_no_ic_is_noop();
    run_mono_with_guard_devirtualizes();
    run_no_guard_no_devirt();
    run_type_mismatch_no_devirt();
    run_poly_ic_devirt_when_guard_matches();
    run_poly_ic_no_matching_guard_type();
    run_null_func_safe();
    run_empty_ic_table_noop();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
