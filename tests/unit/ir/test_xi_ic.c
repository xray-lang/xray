/*
 * Unit tests for Xi IC snapshot metadata attachment.
 */

#include "../../../src/ir/xi_ic.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/vm/xic_field_table.h"
#include "../../../src/vm/xic_method.h"
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

/* ========== Basic Tests ========== */

TEST(attach_null_ic_sets_invariant) {
    XiFunc *f = make_func("null_ic");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, v);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    ASSERT(f->ic_table != NULL);
    ASSERT(f->ic_table->nentries == 0);
    ASSERT(f->invariant_mask & XI_INV_IC_ATTACHED);

    xi_func_free(f);
}

TEST(attach_creates_empty_table_no_call_sites) {
    XiFunc *f = make_func("no_calls");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *a = xi_const_int(f, entry, 1, &stub_int);
    XiValue *b = xi_const_int(f, entry, 2, &stub_int);
    XiValue *add = xi_binary(f, entry, XI_ADD, &stub_int, a, b);
    xi_block_set_return(entry, add);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    ASSERT(f->ic_table->nentries == 0);

    xi_func_free(f);
}

TEST(lookup_returns_null_for_missing) {
    XiFunc *f = make_func("lookup_miss");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    ASSERT(xi_ic_lookup(f, v->id) == NULL);
    ASSERT(xi_ic_lookup(f, 9999) == NULL);

    xi_func_free(f);
}

TEST(lookup_null_func_returns_null) {
    ASSERT(xi_ic_lookup(NULL, 0) == NULL);
}

TEST(table_free_null_safe) {
    xi_ic_table_free(NULL);
}

TEST(reattach_replaces_table) {
    XiFunc *f = make_func("reattach");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    ASSERT(f->ic_table != NULL);

    ASSERT(xi_ic_attach(f, NULL, NULL));
    ASSERT(f->ic_table != NULL);
    ASSERT(f->ic_table->nentries == 0);
    ASSERT(f->invariant_mask & XI_INV_IC_ATTACHED);

    xi_func_free(f);
}

/* ========== Classification Tests ========== */

TEST(ic_kind_helpers) {
    XiIcMeta mono = {.kind = XI_IC_MONO};
    XiIcMeta poly = {.kind = XI_IC_POLY};
    XiIcMeta mega = {.kind = XI_IC_MEGA};
    XiIcMeta none = {.kind = XI_IC_NONE};

    ASSERT(xi_ic_is_mono(&mono));
    ASSERT(!xi_ic_is_mono(&poly));
    ASSERT(!xi_ic_is_mono(&mega));
    ASSERT(!xi_ic_is_mono(&none));
    ASSERT(!xi_ic_is_mono(NULL));

    ASSERT(xi_ic_is_poly(&poly));
    ASSERT(!xi_ic_is_poly(&mono));

    ASSERT(xi_ic_is_mega(&mega));
    ASSERT(!xi_ic_is_mega(&mono));
}

TEST(generated_ic_site_policy) {
    ASSERT(xi_ic_site_kind(XI_CALL_METHOD) == XI_GEN_IC_SITE_METHOD);
    ASSERT(xi_ic_site_kind(XI_CALL_METHOD_DIRECT) == XI_GEN_IC_SITE_METHOD);
    ASSERT(xi_ic_site_kind(XI_LOAD_FIELD) == XI_GEN_IC_SITE_FIELD);
    ASSERT(xi_ic_site_kind(XI_STORE_FIELD) == XI_GEN_IC_SITE_FIELD);
    ASSERT(xi_ic_site_kind(XI_CALL) == XI_GEN_IC_SITE_NONE);
    ASSERT(xi_ic_site_kind(XI_OP_COUNT) == XI_GEN_IC_SITE_NONE);
}

/* ========== Verifier Integration ========== */

TEST(verifier_rejects_ic_bit_without_table) {
    XiFunc *f = make_func("ic_no_table");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);

    f->invariant_mask |= XI_INV_IC_ATTACHED;
    /* ic_table is NULL but invariant bit set → verifier should reject */
    char err[256] = {0};
    ASSERT(!xi_verify(f, err, sizeof(err)));
    ASSERT(err[0] != '\0');

    xi_func_free(f);
}

TEST(verifier_accepts_ic_bit_with_table) {
    XiFunc *f = make_func("ic_with_table");
    ASSERT(f != NULL);
    XiBlock *entry = f->entry;
    XiValue *v = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_return(entry, v);

    ASSERT(xi_ic_attach(f, NULL, NULL));

    char err[256] = {0};
    ASSERT(xi_verify(f, err, sizeof(err)));

    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi IC Metadata Tests ===\n\n");

    run_attach_null_ic_sets_invariant();
    run_attach_creates_empty_table_no_call_sites();
    run_lookup_returns_null_for_missing();
    run_lookup_null_func_returns_null();
    run_table_free_null_safe();
    run_reattach_replaces_table();
    run_ic_kind_helpers();
    run_generated_ic_site_policy();
    run_verifier_rejects_ic_bit_without_table();
    run_verifier_accepts_ic_bit_with_table();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
