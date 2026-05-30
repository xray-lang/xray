/*
 * Unit tests for xm_pic — polymorphic inline cache.
 */

#include "../../../src/jit/xm_pic.h"
#include "../../../src/vm/xic_method.h"
#include "../../../src/runtime/class/xclass.h"

#include <stdio.h>
#include <string.h>

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

TEST(init_empty) {
    XmPic pic;
    xm_pic_init(&pic);
    ASSERT(pic.nentries == 0);
    ASSERT(pic.state == XM_PIC_EMPTY);
    ASSERT(pic.hit_count == 0);
    ASSERT(pic.miss_count == 0);
}

TEST(null_safe) {
    xm_pic_init(NULL);
    ASSERT(!xm_pic_record(NULL, 1, 0, NULL));
    ASSERT(xm_pic_lookup(NULL, 1) == NULL);
    xm_pic_reset(NULL);
}

TEST(single_entry_monomorphic) {
    XmPic pic;
    xm_pic_init(&pic);

    void *code = (void *) 0xCAFEBABE;
    bool hit = xm_pic_record(&pic, 42, 0, code);
    ASSERT(!hit); /* first time is a miss */
    ASSERT(pic.nentries == 1);
    ASSERT(pic.state == XM_PIC_MONOMORPHIC);

    /* Lookup should find it. */
    ASSERT(xm_pic_lookup(&pic, 42) == code);
    ASSERT(xm_pic_lookup(&pic, 99) == NULL);
}

TEST(second_hit_returns_true) {
    XmPic pic;
    xm_pic_init(&pic);

    void *code = (void *) 0xDEADBEEF;
    xm_pic_record(&pic, 42, 0, code);
    bool hit = xm_pic_record(&pic, 42, 0, code);
    ASSERT(hit);
    ASSERT(pic.hit_count == 1);
    ASSERT(pic.nentries == 1);
}

TEST(two_types_polymorphic) {
    XmPic pic;
    xm_pic_init(&pic);

    xm_pic_record(&pic, 1, 0, (void *) 0x1);
    xm_pic_record(&pic, 2, 1, (void *) 0x2);
    ASSERT(pic.nentries == 2);
    ASSERT(pic.state == XM_PIC_POLYMORPHIC);

    ASSERT(xm_pic_lookup(&pic, 1) == (void *) 0x1);
    ASSERT(xm_pic_lookup(&pic, 2) == (void *) 0x2);
}

TEST(overflow_to_megamorphic) {
    XmPic pic;
    xm_pic_init(&pic);

    for (uint32_t i = 1; i <= XM_PIC_MAX_ENTRIES; i++) {
        xm_pic_record(&pic, i, 0, (void *) (uintptr_t) i);
    }
    ASSERT(pic.nentries == XM_PIC_MAX_ENTRIES);
    ASSERT(pic.state == XM_PIC_POLYMORPHIC);

    /* Next addition triggers megamorphic. */
    bool hit = xm_pic_record(&pic, XM_PIC_MAX_ENTRIES + 1, 0, (void *) 0xFF);
    ASSERT(!hit);
    ASSERT(pic.state == XM_PIC_MEGAMORPHIC);
    ASSERT(pic.nentries == XM_PIC_MAX_ENTRIES); /* no growth */
}

TEST(megamorphic_rejects_new) {
    XmPic pic;
    xm_pic_init(&pic);

    for (uint32_t i = 1; i <= XM_PIC_MAX_ENTRIES + 1; i++) {
        xm_pic_record(&pic, i, 0, (void *) (uintptr_t) i);
    }
    ASSERT(pic.state == XM_PIC_MEGAMORPHIC);

    uint32_t misses_before = pic.miss_count;
    bool hit = xm_pic_record(&pic, 999, 0, (void *) 0x999);
    ASSERT(!hit);
    ASSERT(pic.miss_count == misses_before + 1);
}

TEST(reset_clears_all) {
    XmPic pic;
    xm_pic_init(&pic);

    xm_pic_record(&pic, 1, 0, (void *) 0x1);
    xm_pic_record(&pic, 2, 1, (void *) 0x2);
    ASSERT(pic.nentries == 2);

    xm_pic_reset(&pic);
    ASSERT(pic.nentries == 0);
    ASSERT(pic.state == XM_PIC_EMPTY);
    ASSERT(xm_pic_lookup(&pic, 1) == NULL);
}

TEST(zero_type_id_rejected) {
    XmPic pic;
    xm_pic_init(&pic);

    bool hit = xm_pic_record(&pic, 0, 0, (void *) 0x1);
    ASSERT(!hit);
    ASSERT(pic.nentries == 0);
    ASSERT(xm_pic_lookup(&pic, 0) == NULL);
}

TEST(import_poly_ic) {
    XrClass klass_a;
    XrClass klass_b;
    memset(&klass_a, 0, sizeof(klass_a));
    memset(&klass_b, 0, sizeof(klass_b));

    XrICMethod ic;
    memset(&ic, 0, sizeof(ic));
    ic.count = 2;
    ic.entries[0].klass = &klass_a;
    ic.entries[0].hit_count = 70;
    ic.entries[1].klass = &klass_b;
    ic.entries[1].hit_count = 30;

    XmPic pic;
    xm_pic_import_ic_method(&pic, &ic);
    ASSERT(pic.nentries == 2);
    ASSERT(pic.state == XM_PIC_POLYMORPHIC);
    ASSERT(xm_pic_lookup(&pic, (uint32_t) (uintptr_t) &klass_a) == NULL);
}

TEST(code_ptr_update) {
    XmPic pic;
    xm_pic_init(&pic);

    xm_pic_record(&pic, 42, 0, (void *) 0x1);
    ASSERT(xm_pic_lookup(&pic, 42) == (void *) 0x1);

    xm_pic_record(&pic, 42, 0, (void *) 0x2);
    ASSERT(xm_pic_lookup(&pic, 42) == (void *) 0x2);
}

int main(void) {
    printf("=== XM PIC Tests ===\n\n");

    run_init_empty();
    run_null_safe();
    run_single_entry_monomorphic();
    run_second_hit_returns_true();
    run_two_types_polymorphic();
    run_overflow_to_megamorphic();
    run_megamorphic_rejects_new();
    run_reset_clears_all();
    run_zero_type_id_rejected();
    run_import_poly_ic();
    run_code_ptr_update();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
