/*
 * test_effect_db.c - Unit tests for analyzer-owned error effect summaries
 */

#include "xa_effect_db.h"
#include <stdio.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Running %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASSED\n");                                                                        \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

TEST(empty_complete_is_real_summary) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_is_nothrow(&summary));

    XaEffectId id = xa_effect_db_intern(db, &summary);
    XaEffectId id_again = xa_effect_db_intern(db, &summary);
    ASSERT(id != XA_EFFECT_NONE);
    ASSERT(id == id_again);
    ASSERT(xa_effect_db_summary_count(db) == 1);

    const XaEffectSummary *stored = xa_effect_db_get(db, id);
    ASSERT(stored != NULL);
    ASSERT(stored->escaping.count == 0);
    ASSERT(stored->completeness == XA_EFFECT_COMPLETE);
    ASSERT(stored->unknown_reasons == XA_UNKNOWN_NONE);
    ASSERT(stored->fingerprint != 0);
    ASSERT(xa_effect_summary_is_nothrow(stored));

    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(dynamic_bitset_accepts_variant_above_64) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type_id = xa_effect_db_register_error_type(db, 0x101u, NULL);
    ASSERT(type_id != XA_ERROR_TYPE_NONE);
    for (uint32_t i = 0; i <= 72; i++) {
        XaErrorVariantId v = xa_effect_db_register_error_variant(db, type_id, 0x1000u + i);
        ASSERT(v != XA_ERROR_VARIANT_INVALID);
    }

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_add_variant(db, &summary, type_id, 72));
    XaEffectId id = xa_effect_db_intern(db, &summary);
    ASSERT(id != XA_EFFECT_NONE);
    const XaEffectSummary *stored = xa_effect_db_get(db, id);
    ASSERT(stored != NULL);
    ASSERT(stored->escaping.count == 1);
    ASSERT(xa_bitset_word_count(&stored->escaping.types[0].variants) >= 2);
    ASSERT(xa_bitset_test(&stored->escaping.types[0].variants, 72));
    ASSERT(!xa_bitset_test(&stored->escaping.types[0].variants, 1));
    ASSERT(!xa_effect_summary_is_nothrow(stored));

    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(interning_ignores_variant_insertion_order) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type_id = xa_effect_db_register_error_type(db, 0x202u, NULL);
    ASSERT(type_id != XA_ERROR_TYPE_NONE);
    XaErrorVariantId a = xa_effect_db_register_error_variant(db, type_id, 0xaaaau);
    XaErrorVariantId b = xa_effect_db_register_error_variant(db, type_id, 0xbbbbu);
    ASSERT(a != XA_ERROR_VARIANT_INVALID);
    ASSERT(b != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary first;
    XaEffectSummary second;
    xa_effect_summary_init(&first);
    xa_effect_summary_init(&second);
    ASSERT(xa_effect_summary_add_variant(db, &first, type_id, b));
    ASSERT(xa_effect_summary_add_variant(db, &first, type_id, a));
    ASSERT(xa_effect_summary_add_variant(db, &second, type_id, a));
    ASSERT(xa_effect_summary_add_variant(db, &second, type_id, b));

    XaEffectId first_id = xa_effect_db_intern(db, &first);
    XaEffectId second_id = xa_effect_db_intern(db, &second);
    ASSERT(first_id != XA_EFFECT_NONE);
    ASSERT(first_id == second_id);
    ASSERT(xa_effect_db_summary_count(db) == 1);

    xa_effect_summary_clear(&first);
    xa_effect_summary_clear(&second);
    xa_effect_db_free(db);
}

TEST(stable_variant_keys_survive_session_variant_order) {
    XaEffectDatabase *db1 = xa_effect_db_new();
    XaEffectDatabase *db2 = xa_effect_db_new();
    ASSERT(db1 != NULL);
    ASSERT(db2 != NULL);

    XaErrorTypeId t1 = xa_effect_db_register_error_type(db1, 0x303u, NULL);
    XaErrorVariantId b1 = xa_effect_db_register_error_variant(db1, t1, 0x20u);
    XaErrorVariantId a1 = xa_effect_db_register_error_variant(db1, t1, 0x10u);
    ASSERT(a1 != XA_ERROR_VARIANT_INVALID);
    ASSERT(b1 != XA_ERROR_VARIANT_INVALID);

    XaErrorTypeId t2 = xa_effect_db_register_error_type(db2, 0x303u, NULL);
    XaErrorVariantId a2 = xa_effect_db_register_error_variant(db2, t2, 0x10u);
    XaErrorVariantId b2 = xa_effect_db_register_error_variant(db2, t2, 0x20u);
    ASSERT(a2 != XA_ERROR_VARIANT_INVALID);
    ASSERT(b2 != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary s1;
    XaEffectSummary s2;
    xa_effect_summary_init(&s1);
    xa_effect_summary_init(&s2);
    ASSERT(xa_effect_summary_add_variant(db1, &s1, t1, a1));
    ASSERT(xa_effect_summary_add_variant(db2, &s2, t2, a2));

    XaEffectId id1 = xa_effect_db_intern(db1, &s1);
    XaEffectId id2 = xa_effect_db_intern(db2, &s2);
    ASSERT(id1 != XA_EFFECT_NONE);
    ASSERT(id2 != XA_EFFECT_NONE);
    ASSERT(xa_effect_db_get(db1, id1)->fingerprint == xa_effect_db_get(db2, id2)->fingerprint);

    xa_effect_summary_clear(&s1);
    xa_effect_summary_clear(&s2);
    xa_effect_db_free(db1);
    xa_effect_db_free(db2);
}

TEST(incomplete_empty_is_not_nothrow) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary complete;
    XaEffectSummary incomplete;
    xa_effect_summary_init(&complete);
    xa_effect_summary_init(&incomplete);
    xa_effect_summary_mark_incomplete(&incomplete, XA_UNKNOWN_NATIVE_CONTRACT_MISSING);

    XaEffectId complete_id = xa_effect_db_intern(db, &complete);
    XaEffectId incomplete_id = xa_effect_db_intern(db, &incomplete);
    ASSERT(complete_id != XA_EFFECT_NONE);
    ASSERT(incomplete_id != XA_EFFECT_NONE);
    ASSERT(complete_id != incomplete_id);

    const XaEffectSummary *c = xa_effect_db_get(db, complete_id);
    const XaEffectSummary *i = xa_effect_db_get(db, incomplete_id);
    ASSERT(c != NULL);
    ASSERT(i != NULL);
    ASSERT(xa_effect_summary_is_nothrow(c));
    ASSERT(!xa_effect_summary_is_nothrow(i));
    ASSERT(i->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((i->unknown_reasons & XA_UNKNOWN_NATIVE_CONTRACT_MISSING) != 0);
    ASSERT(c->fingerprint != i->fingerprint);

    xa_effect_summary_clear(&complete);
    xa_effect_summary_clear(&incomplete);
    xa_effect_db_free(db);
}

TEST(summary_merge_preserves_variants_and_incomplete_reason) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId first_type = xa_effect_db_register_error_type(db, 0x404u, NULL);
    XaErrorTypeId second_type = xa_effect_db_register_error_type(db, 0x505u, NULL);
    ASSERT(first_type != XA_ERROR_TYPE_NONE);
    ASSERT(second_type != XA_ERROR_TYPE_NONE);
    ASSERT(xa_effect_db_register_error_variant(db, first_type, 0x11u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, second_type, 0x22u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, second_type, 0x33u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary dest;
    XaEffectSummary src;
    xa_effect_summary_init(&dest);
    xa_effect_summary_init(&src);
    ASSERT(xa_effect_summary_add_all_variants(db, &dest, first_type));
    ASSERT(xa_effect_summary_add_variant(db, &src, second_type, 1));
    xa_effect_summary_mark_incomplete(&src, XA_UNKNOWN_UNRESOLVED_CALLEE);

    ASSERT(xa_effect_summary_add_summary(db, &dest, &src));
    XaEffectId id = xa_effect_db_intern(db, &dest);
    ASSERT(id != XA_EFFECT_NONE);
    const XaEffectSummary *stored = xa_effect_db_get(db, id);
    ASSERT(stored != NULL);
    ASSERT(stored->escaping.count == 2);
    ASSERT(stored->escaping.types[0].all_variants);
    ASSERT(!stored->escaping.types[1].all_variants);
    ASSERT(xa_bitset_test(&stored->escaping.types[1].variants, 1));
    ASSERT(stored->completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((stored->unknown_reasons & XA_UNKNOWN_UNRESOLVED_CALLEE) != 0);

    xa_effect_summary_clear(&dest);
    xa_effect_summary_clear(&src);
    xa_effect_db_free(db);
}

TEST(error_type_handle_is_bound_by_stable_key) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XrType *fake_type = (XrType *) &tests_passed;
    XaErrorTypeId first = xa_effect_db_register_error_type(db, 0x606u, NULL);
    XaErrorTypeId second = xa_effect_db_register_error_type(db, 0x606u, fake_type);
    ASSERT(first != XA_ERROR_TYPE_NONE);
    ASSERT(first == second);
    ASSERT(xa_effect_db_error_type_handle(db, first) == fake_type);
    xa_effect_db_free(db);
}

TEST(summary_subtract_type_and_clear_escaping_preserve_incomplete) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId first_type = xa_effect_db_register_error_type(db, 0x707u, NULL);
    XaErrorTypeId second_type = xa_effect_db_register_error_type(db, 0x808u, NULL);
    ASSERT(first_type != XA_ERROR_TYPE_NONE);
    ASSERT(second_type != XA_ERROR_TYPE_NONE);
    ASSERT(xa_effect_db_register_error_variant(db, first_type, 0x71u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, second_type, 0x81u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_add_variant(db, &summary, first_type, 0));
    ASSERT(xa_effect_summary_add_all_variants(db, &summary, second_type));
    xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_DYNAMIC_CALL_TARGET);

    ASSERT(xa_effect_summary_subtract_type(&summary, first_type));
    ASSERT(summary.escaping.count == 1);
    ASSERT(summary.escaping.types[0].type_id == second_type);
    ASSERT(!xa_effect_summary_is_nothrow(&summary));

    xa_effect_summary_clear_escaping(&summary);
    ASSERT(summary.escaping.count == 0);
    ASSERT(summary.completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((summary.unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!xa_effect_summary_is_nothrow(&summary));

    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

int main(void) {
    printf("Running effect database tests...\n");
    RUN_TEST(empty_complete_is_real_summary);
    RUN_TEST(dynamic_bitset_accepts_variant_above_64);
    RUN_TEST(interning_ignores_variant_insertion_order);
    RUN_TEST(stable_variant_keys_survive_session_variant_order);
    RUN_TEST(incomplete_empty_is_not_nothrow);
    RUN_TEST(summary_merge_preserves_variants_and_incomplete_reason);
    RUN_TEST(error_type_handle_is_bound_by_stable_key);
    RUN_TEST(summary_subtract_type_and_clear_escaping_preserve_incomplete);

    printf("\n%d tests passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
