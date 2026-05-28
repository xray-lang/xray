/*
 * test_xi_tbaa.c - Unit tests for TBAA lattice and Memory SSA
 *
 * Tests alias query disjointness, annotation pass correctness,
 * and Memory SSA construction.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_memssa.h"
#include "../../../src/ir/xi_pass.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* Minimal XrType stubs */
static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_str = {.kind = XR_KIND_STRING, .id = 5, .frozen = true};
static XrType stub_any = {.kind = XR_KIND_UNKNOWN, .id = 10, .frozen = true};

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    static void test_##name(void);                                                                 \
    static void run_##name(void) {                                                                 \
        printf("--- " #name " ---\n");                                                             \
        test_##name();                                                                             \
        printf("  PASS\n");                                                                        \
        tests_passed++;                                                                            \
    }                                                                                              \
    static void test_##name(void)

/* Helper: create a function with a sealed entry block. */
static XiFunc *make_func(const char *name) {
    XiFunc *f = xi_func_new(name, &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* ========== TBAA Classification Tests ========== */

TEST(is_memory_load) {
    assert(xi_is_memory_load(XI_LOAD_FIELD));
    assert(xi_is_memory_load(XI_INDEX_GET));
    assert(xi_is_memory_load(XI_STRUCT_GET));
    assert(xi_is_memory_load(XI_JSON_GET_F));
    assert(xi_is_memory_load(XI_TUPLE_GET));
    assert(xi_is_memory_load(XI_LOAD_UPVAL));
    assert(xi_is_memory_load(XI_GET_SHARED));
    assert(xi_is_memory_load(XI_GET_GLOBAL));
    assert(xi_is_memory_load(XI_CHAN_RECV));
    assert(!xi_is_memory_load(XI_ADD));
    assert(!xi_is_memory_load(XI_CONST));
    assert(!xi_is_memory_load(XI_CALL));
}

TEST(is_memory_store) {
    assert(xi_is_memory_store(XI_STORE_FIELD));
    assert(xi_is_memory_store(XI_INDEX_SET));
    assert(xi_is_memory_store(XI_STRUCT_SET));
    assert(xi_is_memory_store(XI_JSON_SET_F));
    assert(xi_is_memory_store(XI_STORE_UPVAL));
    assert(xi_is_memory_store(XI_SET_SHARED));
    assert(xi_is_memory_store(XI_SET_GLOBAL));
    assert(xi_is_memory_store(XI_CHAN_SEND));
    assert(!xi_is_memory_store(XI_ADD));
    assert(!xi_is_memory_store(XI_LOAD_FIELD));
}

TEST(is_memory_op) {
    assert(xi_is_memory_op(XI_LOAD_FIELD));
    assert(xi_is_memory_op(XI_STORE_FIELD));
    assert(!xi_is_memory_op(XI_ADD));
    assert(!xi_is_memory_op(XI_CONST));
    assert(!xi_is_memory_op(XI_PHI));
}

/* ========== TBAA Disjointness Tests ========== */

TEST(disjoint_field_vs_array) {
    /* Field and array accesses can never alias. */
    XiFunc *f = make_func("disjoint_test");
    XiBlock *blk = f->entry;

    XiValue *obj = xi_param(f, blk, 0, &stub_any);
    XiValue *load_f = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load_f->args[0] = obj;
    load_f->aux_int = 0;

    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *load_a = xi_value_new(f, blk, XI_INDEX_GET, &stub_any, 2);
    load_a->args[0] = obj;
    load_a->args[1] = idx;

    xi_block_set_return(blk, load_f);

    /* Annotate TBAA. */
    xi_tbaa_annotate(f);

    assert(load_f->mem_group != XI_MEM_NONE);
    assert(load_a->mem_group != XI_MEM_NONE);
    assert(load_f->mem_group != load_a->mem_group);
    assert(!xi_tbaa_may_alias(load_f, load_a));

    xi_func_free(f);
}

TEST(disjoint_shared_vs_upval) {
    /* Shared variables and upvalues never alias. */
    XiFunc *f = make_func("shared_upval");
    XiBlock *blk = f->entry;

    XiValue *get_s = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get_s->aux_int = 0;

    XiValue *get_u = xi_value_new(f, blk, XI_LOAD_UPVAL, &stub_int, 0);
    get_u->aux_int = 0;

    xi_block_set_return(blk, get_s);

    xi_tbaa_annotate(f);

    assert(!xi_tbaa_may_alias(get_s, get_u));

    xi_func_free(f);
}

TEST(same_group_may_alias) {
    /* Two field loads from same group may alias. */
    XiFunc *f = make_func("same_group");
    XiBlock *blk = f->entry;

    XiValue *obj = xi_param(f, blk, 0, &stub_any);

    XiValue *load1 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load1->args[0] = obj;
    load1->aux_int = 0;

    XiValue *load2 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load2->args[0] = obj;
    load2->aux_int = 0;

    xi_block_set_return(blk, load1);

    xi_tbaa_annotate(f);

    /* Same field id → may alias. */
    assert(xi_tbaa_may_alias(load1, load2));

    xi_func_free(f);
}

TEST(different_field_id_no_alias) {
    /* Two field loads with different known field IDs are disjoint. */
    XiFunc *f = make_func("field_id");
    XiBlock *blk = f->entry;

    XiValue *obj = xi_param(f, blk, 0, &stub_any);

    XiValue *load1 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load1->args[0] = obj;
    load1->aux_int = 0; /* field 0 */

    XiValue *load2 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load2->args[0] = obj;
    load2->aux_int = 1; /* field 1 */

    xi_block_set_return(blk, load1);

    xi_tbaa_annotate(f);

    /* Different field IDs → no alias. */
    assert(!xi_tbaa_may_alias(load1, load2));

    xi_func_free(f);
}

TEST(different_shared_slot_no_alias) {
    /* Two shared accesses with different slot indices are disjoint. */
    XiFunc *f = make_func("shared_slot");
    XiBlock *blk = f->entry;

    XiValue *get0 = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get0->aux_int = 0;

    XiValue *get1 = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get1->aux_int = 1;

    xi_block_set_return(blk, get0);

    xi_tbaa_annotate(f);

    assert(!xi_tbaa_may_alias(get0, get1));

    xi_func_free(f);
}

TEST(non_memory_never_aliases) {
    /* Non-memory ops never alias anything. */
    XiFunc *f = make_func("non_mem");
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 42, &stub_int);
    XiValue *get = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;

    xi_block_set_return(blk, c1);

    xi_tbaa_annotate(f);

    assert(!xi_tbaa_may_alias(c1, get));

    xi_func_free(f);
}

/* ========== TBAA Annotate Pass Tests ========== */

TEST(annotate_sets_invariant) {
    XiFunc *f = make_func("inv_test");
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 1, &stub_int);
    xi_block_set_return(blk, c);

    assert(!(f->invariant_mask & XI_INV_TBAA_ANNOTATED));
    xi_tbaa_annotate(f);
    assert(f->invariant_mask & XI_INV_TBAA_ANNOTATED);

    xi_func_free(f);
}

TEST(annotate_non_memory_is_none) {
    XiFunc *f = make_func("non_mem_ann");
    XiBlock *blk = f->entry;

    XiValue *c1 = xi_const_int(f, blk, 1, &stub_int);
    XiValue *c2 = xi_const_int(f, blk, 2, &stub_int);
    XiValue *add = xi_binary(f, blk, XI_ADD, &stub_int, c1, c2);
    xi_block_set_return(blk, add);

    xi_tbaa_annotate(f);

    assert(c1->mem_group == XI_MEM_NONE);
    assert(c2->mem_group == XI_MEM_NONE);
    assert(add->mem_group == XI_MEM_NONE);

    xi_func_free(f);
}

TEST(annotate_passes_verify) {
    /* After annotation, xi_verify should accept the function. */
    XiFunc *f = make_func("verify_ann");
    XiBlock *blk = f->entry;

    XiValue *get = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);

    char errbuf[256];
    bool ok = xi_verify(f, errbuf, sizeof(errbuf));
    if (!ok) {
        fprintf(stderr, "  verify failed: %s\n", errbuf);
    }
    assert(ok);

    xi_func_free(f);
}

/* ========== Memory SSA Tests ========== */

TEST(memssa_build_simple) {
    /* Build MemSSA for a simple function with one load. */
    XiFunc *f = make_func("memssa_simple");
    XiBlock *blk = f->entry;

    XiValue *get = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;
    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);
    assert(f->invariant_mask & XI_INV_MEM_SSA);

    /* The load should have an access node. */
    XiMemAccess *acc = xi_memssa_access(mssa, get);
    assert(acc != NULL);
    assert(acc->value == get);
    assert(acc->use_ver == XI_MEMVER_ENTRY);
    assert(acc->def_ver == XI_MEMVER_INVALID); /* load does not define */

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_store_defines_version) {
    /* A store should define a new memory version. */
    XiFunc *f = make_func("memssa_store");
    XiBlock *blk = f->entry;

    XiValue *val = xi_const_int(f, blk, 42, &stub_int);
    XiValue *set = xi_value_new(f, blk, XI_SET_SHARED, &stub_int, 1);
    set->args[0] = val;
    set->aux_int = 0;

    XiValue *get = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;

    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    /* Store defines a version. */
    XiMemAccess *store_acc = xi_memssa_access(mssa, set);
    assert(store_acc != NULL);
    assert(store_acc->def_ver != XI_MEMVER_INVALID);
    assert(store_acc->def_ver != XI_MEMVER_ENTRY);

    /* Load uses the version defined by the store. */
    XiMemAccess *load_acc = xi_memssa_access(mssa, get);
    assert(load_acc != NULL);
    assert(load_acc->use_ver == store_acc->def_ver);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_reaching_def) {
    /* xi_memssa_reaching_def should find the store that feeds a load. */
    XiFunc *f = make_func("memssa_reach");
    XiBlock *blk = f->entry;

    XiValue *val = xi_const_int(f, blk, 99, &stub_int);
    XiValue *set = xi_value_new(f, blk, XI_SET_SHARED, &stub_int, 1);
    set->args[0] = val;
    set->aux_int = 0;

    XiValue *get = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    get->aux_int = 0;

    xi_block_set_return(blk, get);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    XiValue *def = xi_memssa_reaching_def(mssa, get);
    assert(def == set);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_null_for_non_memory) {
    /* Non-memory values should have no access node. */
    XiFunc *f = make_func("memssa_null");
    XiBlock *blk = f->entry;

    XiValue *c = xi_const_int(f, blk, 1, &stub_int);
    xi_block_set_return(blk, c);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    XiMemAccess *acc = xi_memssa_access(mssa, c);
    assert(acc == NULL);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_phi_at_join) {
    /* Diamond CFG: entry → {then, else} → merge.
     * Store in then_blk, load in merge_blk → mem phi at merge. */
    XiFunc *f = make_func("memssa_phi");
    XiBlock *entry = f->entry;
    XiBlock *then_blk = xi_block_new(f);
    XiBlock *else_blk = xi_block_new(f);
    XiBlock *merge_blk = xi_block_new(f);

    XiValue *cond = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_if(entry, cond, then_blk, else_blk);

    /* then_blk: store shared[0] = 42, then jump to merge */
    XiValue *val42 = xi_const_int(f, then_blk, 42, &stub_int);
    XiValue *store = xi_value_new(f, then_blk, XI_SET_SHARED, &stub_int, 1);
    store->args[0] = val42;
    store->aux_int = 0;
    xi_block_set_jump(then_blk, merge_blk);

    /* else_blk: no store, jump to merge */
    xi_block_set_jump(else_blk, merge_blk);

    /* merge_blk: load shared[0] */
    XiValue *load = xi_value_new(f, merge_blk, XI_GET_SHARED, &stub_int, 0);
    load->aux_int = 0;
    xi_block_set_return(merge_blk, load);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    /* merge_blk must have a memory phi (2 predecessors). */
    XiMemPhi *mphi = xi_memssa_phis(mssa, merge_blk);
    assert(mphi != NULL);
    assert(mphi->npreds == 2);
    assert(mphi->def_ver != XI_MEMVER_INVALID);

    /* The load should consume the version defined by the mem phi. */
    XiMemAccess *load_acc = xi_memssa_access(mssa, load);
    assert(load_acc != NULL);
    assert(load_acc->use_ver == mphi->def_ver);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_call_clobbers_memory) {
    /* A call defines a new memory version (clobbers all memory). */
    XiFunc *f = make_func("memssa_call");
    XiBlock *blk = f->entry;

    XiValue *callee = xi_param(f, blk, 0, &stub_any);

    XiValue *load_before = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    load_before->aux_int = 0;

    XiValue *call = xi_value_new(f, blk, XI_CALL, &stub_any, 1);
    call->args[0] = callee;

    XiValue *load_after = xi_value_new(f, blk, XI_GET_SHARED, &stub_int, 0);
    load_after->aux_int = 0;

    xi_block_set_return(blk, load_after);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    XiMemAccess *call_acc = xi_memssa_access(mssa, call);
    assert(call_acc != NULL);
    assert(call_acc->def_ver != XI_MEMVER_INVALID);

    /* load_after should consume the version defined by the call,
     * not the same version as load_before. */
    XiMemAccess *before_acc = xi_memssa_access(mssa, load_before);
    XiMemAccess *after_acc = xi_memssa_access(mssa, load_after);
    assert(before_acc != NULL && after_acc != NULL);
    assert(after_acc->use_ver == call_acc->def_ver);
    assert(before_acc->use_ver != after_acc->use_ver);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_cross_block_version) {
    /* entry: store → blk2: load; load should consume the store's version. */
    XiFunc *f = make_func("memssa_xblock");
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);

    XiValue *val = xi_const_int(f, entry, 7, &stub_int);
    XiValue *store = xi_value_new(f, entry, XI_SET_SHARED, &stub_int, 1);
    store->args[0] = val;
    store->aux_int = 0;
    xi_block_set_jump(entry, blk2);

    XiValue *load = xi_value_new(f, blk2, XI_GET_SHARED, &stub_int, 0);
    load->aux_int = 0;
    xi_block_set_return(blk2, load);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    XiMemAccess *store_acc = xi_memssa_access(mssa, store);
    XiMemAccess *load_acc = xi_memssa_access(mssa, load);
    assert(store_acc != NULL && load_acc != NULL);
    assert(load_acc->use_ver == store_acc->def_ver);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_may_alias_disjoint_groups) {
    /* Two loads from disjoint TBAA groups: memssa_may_alias → false. */
    XiFunc *f = make_func("memssa_alias");
    XiBlock *blk = f->entry;

    XiValue *obj = xi_param(f, blk, 0, &stub_any);

    XiValue *load_field = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load_field->args[0] = obj;
    load_field->aux_int = 0;

    XiValue *idx = xi_const_int(f, blk, 0, &stub_int);
    XiValue *load_arr = xi_value_new(f, blk, XI_INDEX_GET, &stub_any, 2);
    load_arr->args[0] = obj;
    load_arr->args[1] = idx;

    xi_block_set_return(blk, load_field);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    /* Disjoint groups → no alias even through MemSSA. */
    assert(!xi_memssa_may_alias(mssa, load_field, load_arr));

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_may_alias_same_group) {
    /* Two loads from same TBAA group and same field: memssa_may_alias → true. */
    XiFunc *f = make_func("memssa_same");
    XiBlock *blk = f->entry;

    XiValue *obj = xi_param(f, blk, 0, &stub_any);

    XiValue *load1 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load1->args[0] = obj;
    load1->aux_int = 0;

    XiValue *load2 = xi_value_new(f, blk, XI_LOAD_FIELD, &stub_int, 1);
    load2->args[0] = obj;
    load2->aux_int = 0;

    xi_block_set_return(blk, load1);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    /* Same group + same field → may alias. */
    assert(xi_memssa_may_alias(mssa, load1, load2));

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

TEST(memssa_no_phi_single_pred) {
    /* Single-predecessor block should NOT get a memory phi. */
    XiFunc *f = make_func("memssa_nophi");
    XiBlock *entry = f->entry;
    XiBlock *blk2 = xi_block_new(f);

    XiValue *c = xi_const_int(f, entry, 1, &stub_int);
    xi_block_set_jump(entry, blk2);
    xi_block_set_return(blk2, c);

    xi_tbaa_annotate(f);
    XiMemSSA *mssa = xi_memssa_build(f);
    assert(mssa != NULL);

    /* blk2 has single predecessor → no mem phi. */
    XiMemPhi *mphi = xi_memssa_phis(mssa, blk2);
    assert(mphi == NULL);

    xi_memssa_destroy(mssa);
    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi TBAA + MemSSA Tests ===\n\n");

    /* Classification */
    run_is_memory_load();
    run_is_memory_store();
    run_is_memory_op();

    /* Disjointness */
    run_disjoint_field_vs_array();
    run_disjoint_shared_vs_upval();
    run_same_group_may_alias();
    run_different_field_id_no_alias();
    run_different_shared_slot_no_alias();
    run_non_memory_never_aliases();

    /* Annotate pass */
    run_annotate_sets_invariant();
    run_annotate_non_memory_is_none();
    run_annotate_passes_verify();

    /* Memory SSA */
    run_memssa_build_simple();
    run_memssa_store_defines_version();
    run_memssa_reaching_def();
    run_memssa_null_for_non_memory();
    run_memssa_phi_at_join();
    run_memssa_call_clobbers_memory();
    run_memssa_cross_block_version();
    run_memssa_may_alias_disjoint_groups();
    run_memssa_may_alias_same_group();
    run_memssa_no_phi_single_pred();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
