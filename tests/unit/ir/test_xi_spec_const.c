/*
 * Unit tests for xi_opt_spec_const — constant-on-branch specialization.
 */

#include "../../../src/ir/xi_opt_spec_const.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_ic.h"
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
    XiFunc *f = xi_func_new("test_spec_const", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

static void attach_mono_ic(XiFunc *f, uint32_t value_id, uint32_t type_id, uint32_t total,
                           uint32_t hits) {
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
    meta->total_count = total;
    meta->ntargets = 1;
    meta->targets[0].type_id = type_id;
    meta->targets[0].hit_count = hits;
    f->invariant_mask |= XI_INV_IC_ATTACHED;
}

TEST(null_safe) {
    XiPassChange c = xi_opt_spec_const(NULL);
    ASSERT(!c.values_changed);
}

TEST(no_ic_noop) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = recv;
    load->aux_int = 0;
    xi_block_set_return(entry, load);

    XiPassChange c = xi_opt_spec_const(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(mono_high_ratio_inserts_guard) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = recv;
    load->aux_int = 0;
    xi_block_set_return(entry, load);

    attach_mono_ic(f, load->id, 0xBEEF, 1000, 950);

    uint32_t nvalues_before = entry->nvalues;
    XiPassChange c = xi_opt_spec_const(f);
    ASSERT(c.values_changed);
    ASSERT(entry->nvalues == nvalues_before + 1);

    /* Find the guard. */
    bool found_guard = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] && entry->values[i]->op == XI_GUARD_TYPE) {
            found_guard = true;
            break;
        }
    }
    ASSERT(found_guard);

    xi_func_free(f);
}

TEST(low_ratio_no_guard) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = recv;
    load->aux_int = 0;
    xi_block_set_return(entry, load);

    /* 50% ratio — below threshold. */
    attach_mono_ic(f, load->id, 0xBEEF, 1000, 500);

    XiPassChange c = xi_opt_spec_const(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(non_load_field_skipped) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *call = xi_value_new(f, entry, XI_CALL_METHOD, &stub_int, 1);
    call->args[0] = recv;
    call->aux_int = 42;
    xi_block_set_return(entry, call);

    attach_mono_ic(f, call->id, 0xBEEF, 1000, 950);

    XiPassChange c = xi_opt_spec_const(f);
    ASSERT(!c.values_changed);

    xi_func_free(f);
}

TEST(already_guarded_skipped) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiValue *recv = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);

    /* Pre-existing guard. */
    XiValue *guard = xi_value_new(f, entry, XI_GUARD_TYPE, &stub_int, 1);
    guard->args[0] = recv;
    guard->aux_int = 0xBEEF;

    XiValue *load = xi_value_new(f, entry, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = recv;
    load->aux_int = 0;
    xi_block_set_return(entry, load);

    attach_mono_ic(f, load->id, 0xBEEF, 1000, 950);

    uint32_t nvalues_before = entry->nvalues;
    XiPassChange c = xi_opt_spec_const(f);
    ASSERT(!c.values_changed);
    ASSERT(entry->nvalues == nvalues_before);

    xi_func_free(f);
}

int main(void) {
    printf("=== Xi Spec Const Tests ===\n\n");

    run_null_safe();
    run_no_ic_noop();
    run_mono_high_ratio_inserts_guard();
    run_low_ratio_no_guard();
    run_non_load_field_skipped();
    run_already_guarded_skipped();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
