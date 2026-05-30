/*
 * Unit tests for xi_opt_slp — SLP vectorization.
 */

#include "../../../src/ir/xi_opt_slp.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_op_name.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>

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

TEST(null_safe) {
    XiPassChange c = xi_opt_slp(NULL);
    ASSERT(!c.values_changed);
}

TEST(empty_func_noop) {
    XiFunc *f = xi_func_new("test_slp_empty", &stub_int);
    XiBlock *entry = xi_block_new(f);
    XiValue *c = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    c->aux_int = 0;
    xi_block_set_return(entry, c);

    XiPassChange chg = xi_opt_slp(f);
    ASSERT(!chg.values_changed);

    xi_func_free(f);
}

TEST(no_adjacent_stores_noop) {
    XiFunc *f = xi_func_new("test_slp_no_adj", &stub_int);
    XiBlock *entry = xi_block_new(f);

    XiValue *arr = xi_value_new(f, entry, XI_PARAM, &stub_int, 0);
    XiValue *idx = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    idx->aux_int = 0;
    XiValue *val = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    val->aux_int = 42;

    XiValue *store = xi_value_new(f, entry, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = idx;
    store->args[2] = val;

    xi_block_set_return(entry, store);

    XiPassChange chg = xi_opt_slp(f);
    ASSERT(!chg.values_changed);

    xi_func_free(f);
}

TEST(vec_ops_in_enum) {
    /* Verify the vector op names are registered. */
    const char *name_load = xi_op_name(XI_VEC_LOAD);
    const char *name_store = xi_op_name(XI_VEC_STORE);
    const char *name_add = xi_op_name(XI_VEC_ADD);
    const char *name_sub = xi_op_name(XI_VEC_SUB);
    const char *name_mul = xi_op_name(XI_VEC_MUL);

    ASSERT(name_load != NULL);
    ASSERT(name_store != NULL);
    ASSERT(name_add != NULL);
    ASSERT(name_sub != NULL);
    ASSERT(name_mul != NULL);
}

int main(void) {
    printf("=== Xi SLP Tests ===\n\n");

    run_null_safe();
    run_empty_func_noop();
    run_no_adjacent_stores_noop();
    run_vec_ops_in_enum();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
