/*
 * Unit tests for xi_opt_spec_narrow — speculative type narrowing.
 */

#include "../../../src/ir/xi_opt_spec_narrow.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ic.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_any = {.kind = XR_KIND_UNKNOWN, .id = 0, .frozen = true};

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

static XiFunc *make_func(const char *name) {
    XiFunc *f = xi_func_new(name, &stub_int);
    if (!f)
        return NULL;
    XiBlock *entry = xi_block_new(f);
    if (!entry) {
        xi_func_free(f);
        return NULL;
    }
    entry->sealed = true;
    return f;
}

/* Helper: inject a mono IC entry for a specific value_id. */
static void inject_mono_ic(XiFunc *f, uint32_t value_id, uint32_t type_id) {
    if (!f->ic_table) {
        ASSERT(xi_ic_attach(f, NULL, NULL));
    }
    XiIcTable *t = f->ic_table;
    if (t->nentries >= t->capacity) {
        uint32_t new_cap = t->capacity ? t->capacity * 2 : 8;
        XiIcMeta *grown = (XiIcMeta *) xr_realloc(t->entries, new_cap * sizeof(XiIcMeta));
        ASSERT(grown != NULL);
        t->entries = grown;
        t->capacity = new_cap;
    }
    XiIcMeta *meta = &t->entries[t->nentries++];
    memset(meta, 0, sizeof(*meta));
    meta->value_id = value_id;
    meta->kind = XI_IC_MONO;
    meta->total_count = 100;
    meta->ntargets = 1;
    meta->targets[0].type_id = type_id;
    meta->targets[0].hit_count = 100;
}

/* ========== Tests ========== */

TEST(no_ic_is_noop) {
    XiFunc *f = make_func("no_ic");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, v);

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(!c.values_changed);
    ASSERT(!c.cfg_changed);

    xi_func_free(f);
}

TEST(empty_ic_is_noop) {
    XiFunc *f = make_func("empty_ic");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, v);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(mono_call_method_gets_guard) {
    XiFunc *f = make_func("mono_call");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    /* Create a fake XI_CALL_METHOD: receiver=param, method="foo" */
    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &stub_any, 0);
    ASSERT(receiver != NULL);

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    ASSERT(call != NULL);
    call->args[0] = receiver;
    call->aux = "foo";
    call->aux_int = 42;

    xi_block_set_return(entry, call);

    /* Attach IC with mono type for the call value. */
    ASSERT(xi_ic_attach(f, NULL, NULL));
    inject_mono_ic(f, call->id, 0xBEEF);

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(c.values_changed);
    ASSERT(c.n_added >= 1);

    /* Verify: a GUARD_TYPE was inserted before the call. */
    bool found_guard = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i]->op == XI_GUARD_TYPE) {
            found_guard = true;
            ASSERT(entry->values[i]->args[0] == receiver);
            break;
        }
    }
    ASSERT(found_guard);

    /* The call's receiver should now be the guard result, not the original. */
    ASSERT(call->args[0] != receiver);
    ASSERT(call->args[0]->op == XI_GUARD_TYPE);

    xi_func_free(f);
}

TEST(guard_merging_same_receiver) {
    XiFunc *f = make_func("guard_merge");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &stub_any, 0);
    ASSERT(receiver != NULL);

    XiValue *load1 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ASSERT(load1 != NULL);
    load1->args[0] = receiver;
    load1->aux = "x";

    XiValue *load2 = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    ASSERT(load2 != NULL);
    load2->args[0] = receiver;
    load2->aux = "y";

    xi_block_set_return(entry, load2);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    inject_mono_ic(f, load1->id, 0xBEEF);
    inject_mono_ic(f, load2->id, 0xBEEF);

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(c.values_changed);

    /* Both loads should share the same guard (merging). */
    ASSERT(load1->args[0] == load2->args[0]);
    ASSERT(load1->args[0]->op == XI_GUARD_TYPE);

    /* Only one guard should be present in the block. */
    uint32_t guard_count = 0;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i]->op == XI_GUARD_TYPE)
            guard_count++;
    }
    ASSERT(guard_count == 1);

    xi_func_free(f);
}

TEST(poly_ic_skipped) {
    XiFunc *f = make_func("poly_skip");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &stub_any, 0);
    ASSERT(receiver != NULL);

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    ASSERT(call != NULL);
    call->args[0] = receiver;

    xi_block_set_return(entry, call);

    ASSERT(xi_ic_attach(f, NULL, NULL));

    /* Inject polymorphic IC (should be skipped). */
    XiIcTable *t = f->ic_table;
    XiIcMeta *meta = &t->entries[t->nentries++];
    memset(meta, 0, sizeof(*meta));
    meta->value_id = call->id;
    meta->kind = XI_IC_POLY;
    meta->ntargets = 2;

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(!c.values_changed);

    /* No guard should be inserted. */
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        ASSERT(entry->values[i]->op != XI_GUARD_TYPE);
    }

    xi_func_free(f);
}

TEST(mega_ic_skipped) {
    XiFunc *f = make_func("mega_skip");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &stub_any, 0);
    ASSERT(receiver != NULL);

    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    ASSERT(call != NULL);
    call->args[0] = receiver;

    xi_block_set_return(entry, call);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    XiIcTable *t = f->ic_table;
    XiIcMeta *meta = &t->entries[t->nentries++];
    memset(meta, 0, sizeof(*meta));
    meta->value_id = call->id;
    meta->kind = XI_IC_MEGA;

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(store_field_gets_guard) {
    XiFunc *f = make_func("store_guard");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;

    XiValue *receiver = xi_value_new(f, entry, XI_PARAM, &stub_any, 0);
    ASSERT(receiver != NULL);

    XiValue *val = xi_const_int(f, entry, 99, &stub_int);

    XiValue *store = xi_value_new(f, entry, XI_STORE_FIELD, &stub_int, 2);
    ASSERT(store != NULL);
    store->args[0] = receiver;
    store->args[1] = val;
    store->aux = "z";

    xi_block_set_return(entry, val);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    inject_mono_ic(f, store->id, 0xCAFE);

    XiPassChange c = xi_opt_spec_narrow(f);
    ASSERT(c.values_changed);
    ASSERT(store->args[0]->op == XI_GUARD_TYPE);

    xi_func_free(f);
}

TEST(null_func_is_safe) {
    XiPassChange c = xi_opt_spec_narrow(NULL);
    ASSERT(!c.values_changed);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Spec Narrow Tests ===\n\n");

    run_no_ic_is_noop();
    run_empty_ic_is_noop();
    run_mono_call_method_gets_guard();
    run_guard_merging_same_receiver();
    run_poly_ic_skipped();
    run_mega_ic_skipped();
    run_store_field_gets_guard();
    run_null_func_is_safe();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
