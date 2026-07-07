/*
 * Unit tests for Class Hierarchy Analysis (CHA)
 *
 * Covers: build / leaf query / single-implementor / invalidation / subclass count
 */

#include "../../../src/frontend/analyzer/xanalyzer_cha.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/class/xclass_info.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

enum {
    SYM_RUN = 12,
    SYM_DRAW = 13,
    SYM_SPECIAL = 14,
};

static XaMethodSlot make_slot(const char *name, int32_t symbol_id) {
    XaMethodSlot s;
    memset(&s, 0, sizeof(s));
    s.name = name;
    s.symbol_id = symbol_id;
    return s;
}

static XrClassInfo make_info(const char *name, const char *base_name) {
    XrClassInfo info;
    memset(&info, 0, sizeof(info));
    info.name = name;
    info.base_name = base_name;
    return info;
}

/* ========== Tests ========== */

static void test_empty_build(void) {
    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, NULL, 0);
    assert(ok);
    assert(cha.nnodes == 0);
    xa_cha_free(&cha);
}

static void test_single_class_is_leaf(void) {
    XrClassInfo info = make_info("Foo", NULL);
    XrClassInfo *infos[] = {&info};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 1);
    assert(ok);
    assert(cha.nnodes == 1);
    assert(xa_cha_is_leaf(&cha, &info));
    assert(xa_cha_subclass_count(&cha, &info) == 0);
    xa_cha_free(&cha);
}

static void test_parent_child_link(void) {
    XrClassInfo base = make_info("Animal", NULL);
    XrClassInfo child = make_info("Dog", "Animal");
    child.base = &base;
    XrClassInfo *infos[] = {&base, &child};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 2);
    assert(ok);
    assert(cha.nnodes == 2);
    assert(!xa_cha_is_leaf(&cha, &base));
    assert(xa_cha_is_leaf(&cha, &child));
    assert(xa_cha_subclass_count(&cha, &base) == 1);
    assert(xa_cha_subclass_count(&cha, &child) == 0);
    assert(base.has_subclass);
    assert(child.base == &base);
    xa_cha_free(&cha);
}

static void test_diamond_hierarchy(void) {
    /*  Base
     *  / \
     * A   B
     *  \ /
     *   C
     */
    XrClassInfo base = make_info("Base", NULL);
    XrClassInfo a = make_info("A", "Base");
    XrClassInfo b = make_info("B", "Base");
    XrClassInfo c_cls = make_info("C", "A");
    a.base = &base;
    b.base = &base;
    c_cls.base = &a;
    XrClassInfo *infos[] = {&base, &a, &b, &c_cls};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 4);
    assert(ok);
    assert(cha.nnodes == 4);
    assert(!xa_cha_is_leaf(&cha, &base));
    assert(!xa_cha_is_leaf(&cha, &a));
    assert(xa_cha_is_leaf(&cha, &b));
    assert(xa_cha_is_leaf(&cha, &c_cls));
    assert(xa_cha_subclass_count(&cha, &base) == 3);
    assert(xa_cha_subclass_count(&cha, &a) == 1);
    xa_cha_free(&cha);
}

static void test_single_implementor_found(void) {
    XaMethodSlot base_slots[1];
    base_slots[0] = make_slot("run", SYM_RUN);

    XrClassInfo base = make_info("Shape", NULL);
    base.vtable = base_slots;
    base.vtable_size = 1;

    XrClassInfo child = make_info("Circle", "Shape");
    child.base = &base;

    XrClassInfo *infos[] = {&base, &child};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 2);
    assert(ok);

    const XrClassInfo *impl = xa_cha_single_implementor(&cha, &base, SYM_RUN);
    assert(impl == &base);
    xa_cha_free(&cha);
}

static void test_single_implementor_ambiguous(void) {
    XaMethodSlot base_slots[1];
    base_slots[0] = make_slot("draw", SYM_DRAW);

    XaMethodSlot child_slots[1];
    child_slots[0] = make_slot("draw", SYM_DRAW);

    XrClassInfo base = make_info("Shape", NULL);
    base.vtable = base_slots;
    base.vtable_size = 1;

    XrClassInfo child = make_info("Circle", "Shape");
    child.base = &base;
    child.vtable = child_slots;
    child.vtable_size = 1;

    XrClassInfo *infos[] = {&base, &child};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 2);
    assert(ok);

    const XrClassInfo *impl = xa_cha_single_implementor(&cha, &base, SYM_DRAW);
    assert(impl == NULL);
    xa_cha_free(&cha);
}

static void test_invalidation_bumps_version(void) {
    XrClassInfo info = make_info("Foo", NULL);
    XrClassInfo *infos[] = {&info};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 1);
    assert(ok);
    assert(cha.version == 1);

    xa_cha_invalidate(&cha);
    assert(cha.version == 2);

    xa_cha_invalidate(&cha);
    assert(cha.version == 3);
    xa_cha_free(&cha);
}

static void test_find_node(void) {
    XrClassInfo info = make_info("Bar", NULL);
    XrClassInfo *infos[] = {&info};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 1);
    assert(ok);

    XaChaNode *node = xa_cha_find_node(&cha, &info);
    assert(node != NULL);
    assert(node->info == &info);
    assert(node->is_leaf);

    XrClassInfo unknown = make_info("Unknown", NULL);
    assert(xa_cha_find_node(&cha, &unknown) == NULL);
    xa_cha_free(&cha);
}

static void test_deep_hierarchy(void) {
    /* A -> B -> C -> D (linear chain) */
    XrClassInfo a = make_info("A", NULL);
    XrClassInfo b = make_info("B", "A");
    XrClassInfo c_cls = make_info("C", "B");
    XrClassInfo d = make_info("D", "C");
    b.base = &a;
    c_cls.base = &b;
    d.base = &c_cls;
    XrClassInfo *infos[] = {&a, &b, &c_cls, &d};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 4);
    assert(ok);
    assert(xa_cha_subclass_count(&cha, &a) == 3);
    assert(xa_cha_subclass_count(&cha, &b) == 2);
    assert(xa_cha_subclass_count(&cha, &c_cls) == 1);
    assert(xa_cha_is_leaf(&cha, &d));
    xa_cha_free(&cha);
}

static void test_method_only_in_grandchild(void) {
    XaMethodSlot child_slots[1];
    child_slots[0] = make_slot("special", SYM_SPECIAL);

    XrClassInfo base = make_info("Base", NULL);
    XrClassInfo mid = make_info("Mid", "Base");
    XrClassInfo leaf = make_info("Leaf", "Mid");
    mid.base = &base;
    leaf.base = &mid;
    leaf.vtable = child_slots;
    leaf.vtable_size = 1;

    XrClassInfo *infos[] = {&base, &mid, &leaf};

    XaClassHierarchy cha;
    bool ok = xa_cha_build(&cha, infos, 3);
    assert(ok);

    const XrClassInfo *impl = xa_cha_single_implementor(&cha, &base, SYM_SPECIAL);
    assert(impl == &leaf);
    xa_cha_free(&cha);
}

static void test_null_safety(void) {
    assert(!xa_cha_build(NULL, NULL, 0));
    assert(!xa_cha_is_leaf(NULL, NULL));
    assert(xa_cha_single_implementor(NULL, NULL, 0) == NULL);
    assert(xa_cha_subclass_count(NULL, NULL) == 0);
    assert(xa_cha_find_node(NULL, NULL) == NULL);
    xa_cha_invalidate(NULL);
}

int main(void) {
    test_empty_build();
    test_single_class_is_leaf();
    test_parent_child_link();
    test_diamond_hierarchy();
    test_single_implementor_found();
    test_single_implementor_ambiguous();
    test_invalidation_bumps_version();
    test_find_node();
    test_deep_hierarchy();
    test_method_only_in_grandchild();
    test_null_safety();
    fprintf(stderr, "xi cha tests passed\n");
    return 0;
}
