/*
 * test_effect_db.c - Unit tests for analyzer-owned error effect summaries
 */

#include "xa_effect_db.h"
#include "xa_memory_effect_db.h"
#include "xmalloc.h"
#include <stdio.h>
#include <string.h>

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

TEST(provenance_roots_merge_without_changing_semantic_identity) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary dest;
    XaEffectSummary src;
    xa_effect_summary_init(&dest);
    xa_effect_summary_init(&src);
    ASSERT(!xa_effect_summary_add_root(&src, XA_EFFECT_EDGE_NONE));
    ASSERT(xa_effect_summary_add_root(&src, 9));
    ASSERT(xa_effect_summary_add_root(&src, 3));
    ASSERT(xa_effect_summary_add_root(&src, 9));
    ASSERT(src.root_count == 2);
    ASSERT(src.roots[0] == 3);
    ASSERT(src.roots[1] == 9);
    ASSERT(xa_effect_summary_add_root(&dest, 7));
    ASSERT(xa_effect_summary_add_summary(db, &dest, &src));
    ASSERT(dest.root_count == 3);
    ASSERT(dest.roots[0] == 3);
    ASSERT(dest.roots[1] == 7);
    ASSERT(dest.roots[2] == 9);

    uint64_t fingerprint = xa_effect_summary_fingerprint(db, &dest);
    XaEffectId id = xa_effect_db_intern(db, &dest);
    ASSERT(id != XA_EFFECT_NONE);
    const XaEffectSummary *stored = xa_effect_db_get(db, id);
    ASSERT(stored != NULL);
    uint32_t first_revision = stored->revision;

    XaEffectSummary equivalent;
    xa_effect_summary_init(&equivalent);
    ASSERT(xa_effect_summary_add_root(&equivalent, 11));
    ASSERT(xa_effect_summary_fingerprint(db, &equivalent) == fingerprint);
    XaEffectId equivalent_id = xa_effect_db_intern(db, &equivalent);
    ASSERT(equivalent_id == id);
    ASSERT(xa_effect_db_summary_count(db) == 1);
    stored = xa_effect_db_get(db, id);
    ASSERT(stored != NULL);
    ASSERT(stored->revision > first_revision);
    ASSERT(stored->root_count == 4);
    ASSERT(stored->roots[0] == 3);
    ASSERT(stored->roots[1] == 7);
    ASSERT(stored->roots[2] == 9);
    ASSERT(stored->roots[3] == 11);
    uint32_t merged_revision = stored->revision;
    ASSERT(xa_effect_db_intern(db, &equivalent) == id);
    ASSERT(xa_effect_db_get(db, id)->revision == merged_revision);

    xa_effect_summary_clear(&equivalent);
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

    XaEffectSummary copied;
    xa_effect_summary_init(&copied);
    ASSERT(xa_effect_summary_add_type_from_summary(db, &copied, &summary, second_type));
    ASSERT(copied.escaping.count == 1);
    ASSERT(copied.escaping.types[0].type_id == second_type);
    ASSERT(copied.escaping.types[0].all_variants);

    xa_effect_summary_clear_escaping(&summary);
    ASSERT(summary.escaping.count == 0);
    ASSERT(summary.completeness == XA_EFFECT_INCOMPLETE);
    ASSERT((summary.unknown_reasons & XA_UNKNOWN_DYNAMIC_CALL_TARGET) != 0);
    ASSERT(!xa_effect_summary_is_nothrow(&summary));

    xa_effect_summary_clear(&copied);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(diff_identical_summaries_are_compatible) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA0u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    ASSERT(xa_effect_summary_add_variant(db, &before, type, 0));
    ASSERT(xa_effect_summary_add_variant(db, &after, type, 0));

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_COMPATIBLE);
    ASSERT(!diff.added_escaping);
    ASSERT(!diff.removed_escaping);
    ASSERT(!diff.became_incomplete);
    ASSERT(!diff.became_complete);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_added_escaping_variant_is_breaking) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA0u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA1u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    ASSERT(xa_effect_summary_add_variant(db, &before, type, 0));
    ASSERT(xa_effect_summary_add_variant(db, &after, type, 0));
    ASSERT(xa_effect_summary_add_variant(db, &after, type, 1));

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(diff.added_escaping);
    ASSERT(!diff.removed_escaping);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_removed_escaping_variant_is_improvement) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA0u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA1u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    ASSERT(xa_effect_summary_add_variant(db, &before, type, 0));
    ASSERT(xa_effect_summary_add_variant(db, &before, type, 1));
    ASSERT(xa_effect_summary_add_variant(db, &after, type, 0));

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_IMPROVEMENT);
    ASSERT(!diff.added_escaping);
    ASSERT(diff.removed_escaping);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_added_new_error_type_is_breaking) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId a = xa_effect_db_register_error_type(db, 0x111u, NULL);
    XaErrorTypeId b = xa_effect_db_register_error_type(db, 0x222u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, a, 0xA0u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, b, 0xB0u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    ASSERT(xa_effect_summary_add_variant(db, &before, a, 0));
    ASSERT(xa_effect_summary_add_variant(db, &after, a, 0));
    ASSERT(xa_effect_summary_add_variant(db, &after, b, 0));

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(diff.added_escaping);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_complete_to_incomplete_is_breaking) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    xa_effect_summary_mark_incomplete(&after, XA_UNKNOWN_DYNAMIC_CALL_TARGET);

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(diff.became_incomplete);
    ASSERT(diff.widened_unknown);
    ASSERT(!diff.became_complete);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_incomplete_to_complete_is_improvement) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    xa_effect_summary_mark_incomplete(&before, XA_UNKNOWN_DYNAMIC_CALL_TARGET);

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_IMPROVEMENT);
    ASSERT(diff.became_complete);
    ASSERT(diff.narrowed_unknown);
    ASSERT(!diff.became_incomplete);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(diff_all_variants_widening_and_narrowing) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA0u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA1u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary specific;
    XaEffectSummary all;
    xa_effect_summary_init(&specific);
    xa_effect_summary_init(&all);
    ASSERT(xa_effect_summary_add_variant(db, &specific, type, 0));
    ASSERT(xa_effect_summary_add_all_variants(db, &all, type));

    XaEffectDiff widening;
    ASSERT(xa_effect_summary_diff(db, &specific, &all, &widening) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(widening.added_escaping);
    ASSERT(!widening.removed_escaping);

    XaEffectDiff narrowing;
    ASSERT(xa_effect_summary_diff(db, &all, &specific, &narrowing) == XA_EFFECT_DIFF_IMPROVEMENT);
    ASSERT(narrowing.removed_escaping);
    ASSERT(!narrowing.added_escaping);

    xa_effect_summary_clear(&specific);
    xa_effect_summary_clear(&all);
    xa_effect_db_free(db);
}

TEST(diff_addition_dominates_removal_as_breaking) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA0u) != XA_ERROR_VARIANT_INVALID);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xA1u) != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    ASSERT(xa_effect_summary_add_variant(db, &before, type, 1));
    ASSERT(xa_effect_summary_add_variant(db, &after, type, 0));

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(diff.added_escaping);
    ASSERT(diff.removed_escaping);

    xa_effect_summary_clear(&before);
    xa_effect_summary_clear(&after);
    xa_effect_db_free(db);
}

TEST(error_type_and_variant_names_round_trip) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x909u, NULL);
    ASSERT(type != XA_ERROR_TYPE_NONE);
    XaErrorVariantId v0 = xa_effect_db_register_error_variant(db, type, 0x9A0u);
    XaErrorVariantId v1 = xa_effect_db_register_error_variant(db, type, 0x9A1u);
    ASSERT(v0 != XA_ERROR_VARIANT_INVALID);
    ASSERT(v1 != XA_ERROR_VARIANT_INVALID);

    ASSERT(xa_effect_db_error_type_name(db, type) == NULL);
    ASSERT(xa_effect_db_error_variant_name(db, type, v0) == NULL);

    xa_effect_db_set_error_type_name(db, type, "std.fs::IoError");
    xa_effect_db_set_error_variant_name(db, type, v0, "NotFound");
    xa_effect_db_set_error_variant_name(db, type, v1, "PermissionDenied");

    ASSERT(strcmp(xa_effect_db_error_type_name(db, type), "std.fs::IoError") == 0);
    ASSERT(strcmp(xa_effect_db_error_variant_name(db, type, v0), "NotFound") == 0);
    ASSERT(strcmp(xa_effect_db_error_variant_name(db, type, v1), "PermissionDenied") == 0);

    /* First assignment wins; the DB owns a private copy. */
    xa_effect_db_set_error_type_name(db, type, "shadowed");
    ASSERT(strcmp(xa_effect_db_error_type_name(db, type), "std.fs::IoError") == 0);

    /* Out-of-range variant name query is safe. */
    ASSERT(xa_effect_db_error_variant_name(db, type, 99) == NULL);

    xa_effect_db_free(db);
}

TEST(json_empty_complete_summary_is_canonical) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaEffectSummary summary;
    xa_effect_summary_init(&summary);

    char *json = xa_effect_summary_to_json(db, &summary, "app::boot");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"schema\":\"xray.effect-summary.v4\"") != NULL);
    ASSERT(strstr(json, "\"symbol\":\"app::boot\"") != NULL);
    ASSERT(strstr(json, "\"complete\":true") != NULL);
    ASSERT(strstr(json, "\"errors\":[]") != NULL);
    ASSERT(strstr(json, "\"unknownReasons\":[]") != NULL);
    ASSERT(strstr(json, "\"fingerprint\":\"0x") != NULL);

    xr_free(json);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(json_named_variants_are_labeled) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x111u, NULL);
    XaErrorVariantId v0 = xa_effect_db_register_error_variant(db, type, 0xA0u);
    XaErrorVariantId v1 = xa_effect_db_register_error_variant(db, type, 0xA1u);
    ASSERT(v0 != XA_ERROR_VARIANT_INVALID);
    ASSERT(v1 != XA_ERROR_VARIANT_INVALID);
    xa_effect_db_set_error_type_name(db, type, "std.fs::IoError");
    xa_effect_db_set_error_variant_name(db, type, v0, "NotFound");
    xa_effect_db_set_error_variant_name(db, type, v1, "PermissionDenied");

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_add_variant(db, &summary, type, v0));
    ASSERT(xa_effect_summary_add_variant(db, &summary, type, v1));

    char *json = xa_effect_summary_to_json(db, &summary, NULL);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"symbol\":") == NULL);
    ASSERT(strstr(json, "\"type\":\"std.fs::IoError\"") != NULL);
    ASSERT(strstr(json, "\"variant\":\"NotFound\"") != NULL);
    ASSERT(strstr(json, "\"variant\":\"PermissionDenied\"") != NULL);

    xr_free(json);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(json_all_variants_marks_all) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x222u, NULL);
    ASSERT(xa_effect_db_register_error_variant(db, type, 0xB0u) != XA_ERROR_VARIANT_INVALID);
    xa_effect_db_set_error_type_name(db, type, "app::ParseError");

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_add_all_variants(db, &summary, type));

    char *json = xa_effect_summary_to_json(db, &summary, NULL);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"type\":\"app::ParseError\"") != NULL);
    ASSERT(strstr(json, "\"allVariants\":true") != NULL);
    ASSERT(strstr(json, "\"variant\":") == NULL);

    xr_free(json);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(json_incomplete_lists_reasons_in_fixed_order) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_DYNAMIC_CALL_TARGET);
    xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_OPEN_VIRTUAL_DISPATCH);

    char *json = xa_effect_summary_to_json(db, &summary, NULL);
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"complete\":false") != NULL);
    const char *open = strstr(json, "openVirtualDispatch");
    const char *dyn = strstr(json, "dynamicCallTarget");
    ASSERT(open != NULL);
    ASSERT(dyn != NULL);
    /* Reasons are emitted in fixed bit order regardless of mark order. */
    ASSERT(open < dyn);

    xr_free(json);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(json_sorts_variants_by_stable_key) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaErrorTypeId type = xa_effect_db_register_error_type(db, 0x333u, NULL);
    /* v0 has the larger stable key, v1 the smaller: output must be key-sorted. */
    XaErrorVariantId v0 = xa_effect_db_register_error_variant(db, type, 0xB0u);
    XaErrorVariantId v1 = xa_effect_db_register_error_variant(db, type, 0xA0u);
    ASSERT(v0 != XA_ERROR_VARIANT_INVALID);
    ASSERT(v1 != XA_ERROR_VARIANT_INVALID);

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ASSERT(xa_effect_summary_add_variant(db, &summary, type, v0));
    ASSERT(xa_effect_summary_add_variant(db, &summary, type, v1));

    char *json = xa_effect_summary_to_json(db, &summary, NULL);
    ASSERT(json != NULL);
    const char *lo = strstr(json, "0x00000000000000a0");
    const char *hi = strstr(json, "0x00000000000000b0");
    ASSERT(lo != NULL);
    ASSERT(hi != NULL);
    ASSERT(lo < hi);

    xr_free(json);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(semantic_product_participates_in_identity_join_and_json) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);

    XaEffectSummary base;
    XaEffectSummary semantic;
    xa_effect_summary_init(&base);
    xa_effect_summary_init(&semantic);
    xa_effect_summary_add_semantic_effects(&semantic,
                                           XA_SEM_EFFECT_ALLOC | XA_SEM_EFFECT_SCHED_SUSPEND |
                                               XA_SEM_EFFECT_GEN_SUSPEND | XA_SEM_EFFECT_IO);
    xa_effect_summary_mark_contains_unsafe(&semantic);
    xa_effect_summary_mark_requires_unsafe(&semantic);
    xa_effect_summary_mark_semantic_incomplete(&semantic, XA_SEM_EFFECT_MAY_BLOCK,
                                               XA_UNKNOWN_DYNAMIC_CALL_TARGET);

    XaEffectId base_id = xa_effect_db_intern(db, &base);
    XaEffectId semantic_id = xa_effect_db_intern(db, &semantic);
    ASSERT(base_id != XA_EFFECT_NONE);
    ASSERT(semantic_id != XA_EFFECT_NONE);
    ASSERT(base_id != semantic_id);

    ASSERT(xa_effect_summary_add_summary(db, &base, &semantic));
    ASSERT(xa_effect_summary_has_semantic_effect(&base, XA_SEM_EFFECT_ALLOC));
    ASSERT(xa_effect_summary_has_semantic_effect(&base, XA_SEM_EFFECT_SCHED_SUSPEND));
    ASSERT(xa_effect_summary_has_semantic_effect(&base, XA_SEM_EFFECT_GEN_SUSPEND));
    ASSERT(!xa_effect_summary_has_semantic_effect(&base, XA_SEM_EFFECT_PANIC));
    ASSERT(base.contains_unsafe_op);
    ASSERT(base.requires_unsafe_at_call);

    char *json = xa_effect_summary_to_json(db, &base, "app::run");
    ASSERT(json != NULL);
    ASSERT(strstr(json, "\"schema\":\"xray.effect-summary.v4\"") != NULL);
    ASSERT(strstr(json, "\"semanticAlloc\"") != NULL);
    ASSERT(strstr(json, "\"schedSuspend\"") != NULL);
    ASSERT(strstr(json, "\"generatorSuspend\"") != NULL);
    ASSERT(strstr(json, "\"io\"") != NULL);
    ASSERT(strstr(json, "\"unknownSemanticEffects\":[\"mayBlock\"]") != NULL);
    ASSERT(strstr(json, "\"errorSetComplete\":true") != NULL);
    ASSERT(strstr(json, "\"containsUnsafeOp\":true") != NULL);
    ASSERT(strstr(json, "\"requiresUnsafeAtCall\":true") != NULL);

    xr_free(json);
    xa_effect_summary_clear(&semantic);
    xa_effect_summary_clear(&base);
    xa_effect_db_free(db);
}

TEST(semantic_effect_diff_is_directional) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaEffectSummary before;
    XaEffectSummary after;
    xa_effect_summary_init(&before);
    xa_effect_summary_init(&after);
    xa_effect_summary_add_semantic_effects(&after, XA_SEM_EFFECT_MAY_BLOCK);

    XaEffectDiff diff;
    ASSERT(xa_effect_summary_diff(db, &before, &after, &diff) == XA_EFFECT_DIFF_BREAKING);
    ASSERT(diff.added_semantic_effects == XA_SEM_EFFECT_MAY_BLOCK);
    ASSERT(diff.removed_semantic_effects == XA_SEM_EFFECT_NONE);
    ASSERT(xa_effect_summary_diff(db, &after, &before, &diff) == XA_EFFECT_DIFF_IMPROVEMENT);
    ASSERT(diff.removed_semantic_effects == XA_SEM_EFFECT_MAY_BLOCK);

    xa_effect_summary_clear(&after);
    xa_effect_summary_clear(&before);
    xa_effect_db_free(db);
}

TEST(analysis_resource_failure_is_never_complete) {
    XaEffectDatabase *db = xa_effect_db_new();
    ASSERT(db != NULL);
    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    xa_effect_summary_mark_incomplete(&summary, XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
    ASSERT(!xa_effect_summary_is_complete(&summary));
    ASSERT(xa_effect_summary_has_resource_failure(&summary));
    ASSERT(!xa_effect_summary_is_nothrow(&summary));
    ASSERT(xa_effect_db_intern(db, &summary) == XA_EFFECT_NONE);
    xa_effect_summary_clear(&summary);
    xa_effect_db_free(db);
}

TEST(memory_effect_identity_is_order_independent) {
    XaMemoryEffectDatabase *db = xa_memory_effect_db_new();
    ASSERT(db != NULL);
    XaMemoryEffectSummary first;
    XaMemoryEffectSummary second;
    xa_memory_effect_summary_init(&first);
    xa_memory_effect_summary_init(&second);
    XaMemoryRootRef receiver = {.kind = XA_MEMORY_ROOT_RECEIVER, .index = 0};
    XaMemoryRootRef param = {.kind = XA_MEMORY_ROOT_PARAM, .index = 1};

    ASSERT(xa_memory_effect_summary_add_write(&first, receiver, 9));
    ASSERT(xa_memory_effect_summary_add_write(&first, receiver, 3));
    ASSERT(xa_memory_effect_summary_mark_relocation(&first, param));
    ASSERT(xa_memory_effect_summary_mark_descriptor_rebind(&first, receiver));

    ASSERT(xa_memory_effect_summary_mark_descriptor_rebind(&second, receiver));
    ASSERT(xa_memory_effect_summary_mark_relocation(&second, param));
    ASSERT(xa_memory_effect_summary_add_write(&second, receiver, 3));
    ASSERT(xa_memory_effect_summary_add_write(&second, receiver, 9));

    XaMemoryEffectId first_id = xa_memory_effect_db_intern(db, &first);
    XaMemoryEffectId second_id = xa_memory_effect_db_intern(db, &second);
    ASSERT(first_id != XA_MEMORY_EFFECT_NONE);
    ASSERT(first_id == second_id);
    ASSERT(xa_memory_effect_db_summary_count(db) == 1);
    ASSERT(xa_memory_effect_summary_invalidates_live_view(xa_memory_effect_db_get(db, first_id),
                                                          param));
    ASSERT(!xa_memory_effect_summary_invalidates_live_view(xa_memory_effect_db_get(db, first_id),
                                                           receiver));

    xa_memory_effect_summary_clear(&second);
    xa_memory_effect_summary_clear(&first);
    xa_memory_effect_db_free(db);
}

TEST(memory_effect_unknown_and_resource_failure_fail_closed) {
    XaMemoryEffectDatabase *db = xa_memory_effect_db_new();
    ASSERT(db != NULL);
    XaMemoryRootRef param = {.kind = XA_MEMORY_ROOT_PARAM, .index = 0};
    XaMemoryEffectSummary unknown;
    xa_memory_effect_summary_init(&unknown);
    xa_memory_effect_summary_mark_incomplete(&unknown, XA_UNKNOWN_VIEW_INVALIDATION);
    ASSERT(!xa_memory_effect_summary_is_complete(&unknown));
    ASSERT(xa_memory_effect_summary_invalidates_live_view(&unknown, param));
    ASSERT(xa_memory_effect_db_intern(db, &unknown) != XA_MEMORY_EFFECT_NONE);

    XaMemoryEffectSummary failed;
    xa_memory_effect_summary_init(&failed);
    xa_memory_effect_summary_mark_incomplete(&failed, XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
    ASSERT(xa_memory_effect_summary_has_resource_failure(&failed));
    ASSERT(xa_memory_effect_db_intern(db, &failed) == XA_MEMORY_EFFECT_NONE);

    xa_memory_effect_summary_clear(&failed);
    xa_memory_effect_summary_clear(&unknown);
    xa_memory_effect_db_free(db);
}

int main(void) {
    printf("Running effect database tests...\n");
    RUN_TEST(empty_complete_is_real_summary);
    RUN_TEST(dynamic_bitset_accepts_variant_above_64);
    RUN_TEST(interning_ignores_variant_insertion_order);
    RUN_TEST(stable_variant_keys_survive_session_variant_order);
    RUN_TEST(incomplete_empty_is_not_nothrow);
    RUN_TEST(summary_merge_preserves_variants_and_incomplete_reason);
    RUN_TEST(provenance_roots_merge_without_changing_semantic_identity);
    RUN_TEST(error_type_handle_is_bound_by_stable_key);
    RUN_TEST(summary_subtract_type_and_clear_escaping_preserve_incomplete);
    RUN_TEST(diff_identical_summaries_are_compatible);
    RUN_TEST(diff_added_escaping_variant_is_breaking);
    RUN_TEST(diff_removed_escaping_variant_is_improvement);
    RUN_TEST(diff_added_new_error_type_is_breaking);
    RUN_TEST(diff_complete_to_incomplete_is_breaking);
    RUN_TEST(diff_incomplete_to_complete_is_improvement);
    RUN_TEST(diff_all_variants_widening_and_narrowing);
    RUN_TEST(diff_addition_dominates_removal_as_breaking);
    RUN_TEST(error_type_and_variant_names_round_trip);
    RUN_TEST(json_empty_complete_summary_is_canonical);
    RUN_TEST(json_named_variants_are_labeled);
    RUN_TEST(json_all_variants_marks_all);
    RUN_TEST(json_incomplete_lists_reasons_in_fixed_order);
    RUN_TEST(json_sorts_variants_by_stable_key);
    RUN_TEST(semantic_product_participates_in_identity_join_and_json);
    RUN_TEST(semantic_effect_diff_is_directional);
    RUN_TEST(analysis_resource_failure_is_never_complete);
    RUN_TEST(memory_effect_identity_is_order_independent);
    RUN_TEST(memory_effect_unknown_and_resource_failure_fail_closed);

    printf("\n%d tests passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
