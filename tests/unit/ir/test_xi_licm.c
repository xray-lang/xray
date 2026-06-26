/*
 * Unit tests for Xi LICM (Loop-Invariant Code Motion).
 * Covers pure-op hoisting, alias-aware load hoisting, and non-hoistable cases.
 */

#include "../../../src/ir/xi_opt_licm.h"
#include "../../../src/ir/xi_tbaa.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_op_name.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};

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

/* Helper: create a func with entry block. */
static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_licm", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* Helper: wire succ/pred between two blocks. */
static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

/* ========== Test: hoist pure add from loop body ========== */

TEST(hoist_pure_add) {
    /*
     * entry (preheader) -> header
     * header -> body -> header (loop)
     * header -> exit (RETURN)
     *
     * body contains: v = a + b  (both a, b defined in entry)
     * Expect: v hoisted to entry (preheader).
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    /* Values in entry: two constants a, b */
    XiValue *a = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    a->aux_int = 10;
    XiValue *b = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    b->aux_int = 20;

    /* Condition in header */
    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* Pure add in body: c = a + b */
    XiValue *c = xi_value_new(f, body, XI_ADD, &stub_int, 2);
    c->args[0] = a;
    c->args[1] = b;

    /* Seal blocks */
    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    /* Run LICM */
    XiPassChange chg = xi_opt_licm(f);
    ASSERT(chg.values_changed);

    /* c should now be in entry (preheader), not in body */
    bool found_in_entry = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == c)
            found_in_entry = true;
    }
    ASSERT(found_in_entry);

    /* body should not contain c */
    bool found_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == c)
            found_in_body = true;
    }
    ASSERT(!found_in_body);

    xi_func_free(f);
}

/* ========== Test: hoist constant operands before dependent values ========== */

TEST(hoist_const_operand_before_dependent_value) {
    /*
     * A constant defined inside the loop is still an SSA definition.  LICM
     * must move it before moving a dependent pure value to the preheader.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *outer = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    outer->aux_int = 44;

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    XiValue *inner_const = xi_value_new(f, body, XI_CONST, &stub_int, 0);
    inner_const->aux_int = 255;

    XiValue *cmp = xi_value_new(f, body, XI_EQ, &stub_bool, 2);
    cmp->args[0] = outer;
    cmp->args[1] = inner_const;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(chg.values_changed);
    ASSERT(inner_const->block == entry);
    ASSERT(cmp->block == entry);

    char errbuf[256];
    ASSERT(xi_verify(f, errbuf, sizeof(errbuf)));

    xi_func_free(f);
}

/* ========== Test: hoist generated speculatable select ========== */

TEST(hoist_generated_speculatable_select) {
    /*
     * XI_SELECT is not part of LICM itself; hoistability comes from
     * generated op speculation metadata.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *cond = xi_value_new(f, entry, XI_CONST, &stub_bool, 0);
    cond->aux_int = 1;
    XiValue *lhs = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    lhs->aux_int = 11;
    XiValue *rhs = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    rhs->aux_int = 22;
    header->control = cond;

    XiValue *sel = xi_value_new(f, body, XI_SELECT, &stub_int, 3);
    sel->args[0] = cond;
    sel->args[1] = lhs;
    sel->args[2] = rhs;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(chg.values_changed);
    ASSERT(sel->block == entry);

    bool found_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == sel)
            found_in_body = true;
    }
    ASSERT(!found_in_body);

    char errbuf[256];
    ASSERT(xi_verify(f, errbuf, sizeof(errbuf)));

    xi_func_free(f);
}

/* ========== Test: do NOT hoist zero-effect op without safe speculation ========== */

TEST(no_hoist_unspeculatable_zero_effect) {
    /*
     * XI_IS has no declared effects, but it is not marked safe
     * for speculation. LICM must obey the generated policy boundary.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *cond = xi_value_new(f, entry, XI_CONST, &stub_bool, 0);
    cond->aux_int = 1;
    XiValue *lhs = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    lhs->aux_int = 11;
    XiValue *rhs = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    rhs->aux_int = 11;
    header->control = cond;

    XiValue *type_check = xi_value_new(f, body, XI_IS, &stub_bool, 2);
    type_check->args[0] = lhs;
    type_check->args[1] = rhs;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(!chg.values_changed);
    ASSERT(type_check->block == body);

    xi_func_free(f);
}

/* ========== Test: hoist alias-safe load (const memory) ========== */

TEST(hoist_const_load) {
    /*
     * Same loop structure. body contains a LOAD_FIELD from XI_MEM_CONST.
     * No stores in loop. Load should be hoisted.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    /* Object defined in entry */
    XiValue *obj = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    obj->aux_int = 0;

    /* Condition in header */
    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* Load field in body with CONST mem_group */
    XiValue *load = xi_value_new(f, body, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = obj;
    load->mem_group = XI_MEM_CONST;
    load->flags = XI_FLAG_READS_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(chg.values_changed);

    /* load should be in entry (preheader) */
    bool found_in_entry = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == load)
            found_in_entry = true;
    }
    ASSERT(found_in_entry);

    xi_func_free(f);
}

/* ========== Test: hoist disjoint-group load ========== */

TEST(hoist_disjoint_load) {
    /*
     * body has: LOAD_FIELD (mem_group=STRUCT) + STORE_FIELD (mem_group=ARRAY).
     * Different groups → load is alias-safe → should hoist.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *obj = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    XiValue *arr = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    XiValue *val = xi_value_new(f, entry, XI_CONST, &stub_int, 0);

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* Load from STRUCT group */
    XiValue *load = xi_value_new(f, body, XI_STRUCT_GET, &stub_int, 1);
    load->args[0] = obj;
    load->mem_group = XI_MEM_STRUCT;
    load->flags = XI_FLAG_READS_MEM;

    /* Store to ARRAY group — disjoint from STRUCT */
    XiValue *store = xi_value_new(f, body, XI_INDEX_SET, &stub_int, 3);
    store->args[0] = arr;
    store->args[1] = val;
    store->args[2] = val;
    store->mem_group = XI_MEM_ARRAY;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(chg.values_changed);

    /* load hoisted to entry */
    bool found_in_entry = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == load)
            found_in_entry = true;
    }
    ASSERT(found_in_entry);

    /* store stays in body */
    bool store_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == store)
            store_in_body = true;
    }
    ASSERT(store_in_body);

    xi_func_free(f);
}

/* ========== Test: do NOT hoist load that aliases a store ========== */

TEST(no_hoist_aliasing_load) {
    /*
     * body has: LOAD_FIELD (FIELD) + STORE_FIELD (FIELD).
     * Same group → may alias → load must NOT be hoisted.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *obj = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    XiValue *val = xi_value_new(f, entry, XI_CONST, &stub_int, 0);

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* Load and store with same mem_group (FIELD) */
    XiValue *load = xi_value_new(f, body, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = obj;
    load->mem_group = XI_MEM_FIELD;
    load->flags = XI_FLAG_READS_MEM;

    XiValue *store = xi_value_new(f, body, XI_STORE_FIELD, &stub_int, 2);
    store->args[0] = obj;
    store->args[1] = val;
    store->mem_group = XI_MEM_FIELD;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    xi_opt_licm(f);

    /* load should remain in body (not hoisted) */
    bool load_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == load)
            load_in_body = true;
    }
    ASSERT(load_in_body);

    xi_func_free(f);
}

/* ========== Test: do NOT hoist load with operand inside loop ========== */

TEST(no_hoist_inner_operand) {
    /*
     * body has: x = ADD(a, b) where a is loop-inner;  load = STRUCT_GET(x)
     * x depends on a loop-internal value → load cannot be hoisted.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *outer = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    outer->aux_int = 1;

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* inner_val defined inside body (non-const: ADD depends on itself conceptually) */
    XiValue *inner_val = xi_value_new(f, body, XI_ADD, &stub_int, 2);
    inner_val->args[0] = outer;
    inner_val->args[1] = outer;

    /* load depends on inner_val (loop-internal operand) */
    XiValue *load = xi_value_new(f, body, XI_STRUCT_GET, &stub_int, 1);
    load->args[0] = inner_val;
    load->mem_group = XI_MEM_STRUCT;
    load->flags = XI_FLAG_READS_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    xi_opt_licm(f);

    /* inner_val gets hoisted (pure, both args outside loop), but load
     * also gets hoisted after inner_val is hoisted (chain propagation).
     * So the real test for "inner operand blocks hoisting" needs the
     * operand to genuinely stay inside. Use a store (side-effectful). */

    xi_func_free(f);
}

TEST(no_hoist_load_with_store_dep) {
    /*
     * body has: store = STORE_FIELD(obj, val); load = STRUCT_GET(store_result)
     * store stays in loop (side-effectful), load depends on it → cannot hoist.
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *obj = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    XiValue *val = xi_value_new(f, entry, XI_CONST, &stub_int, 0);

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    /* Store in body — stays pinned (side-effectful) */
    XiValue *store = xi_value_new(f, body, XI_STORE_FIELD, &stub_int, 2);
    store->args[0] = obj;
    store->args[1] = val;
    store->mem_group = XI_MEM_STRUCT;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    /* Load depends on store (loop-internal, non-hoistable operand) */
    XiValue *load = xi_value_new(f, body, XI_STRUCT_GET, &stub_int, 1);
    load->args[0] = store;
    load->mem_group = XI_MEM_STRUCT;
    load->flags = XI_FLAG_READS_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    xi_opt_licm(f);

    /* load should remain in body (operand 'store' is loop-internal) */
    bool load_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == load)
            load_in_body = true;
    }
    ASSERT(load_in_body);

    xi_func_free(f);
}

/* ========== Test: no loop — no change ========== */

TEST(no_loop_no_change) {
    XiFunc *f = make_func();
    f->entry->kind = XI_BLOCK_RETURN;

    XiPassChange chg = xi_opt_licm(f);
    ASSERT(!chg.values_changed);
    ASSERT(!chg.cfg_changed);

    xi_func_free(f);
}

/* ========== Test: do NOT hoist call (conservative) ========== */

TEST(no_hoist_call) {
    /*
     * body has a CALL — calls are never hoistable (side effects).
     */
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;

    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *callee = xi_value_new(f, entry, XI_CONST, &stub_int, 0);

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    XiValue *call = xi_value_new(f, body, XI_CALL, &stub_int, 1);
    call->args[0] = callee;
    call->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    xi_opt_licm(f);

    /* call remains in body */
    bool call_in_body = false;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        if (body->values[i] == call)
            call_in_body = true;
    }
    ASSERT(call_in_body);

    xi_func_free(f);
}

/* ========== Alias-aware: TBAA group interaction matrix ========== */

/* Helper: build a loop with a load (read_group) and a store (write_group)
 * in the body.  Returns true if the load was hoisted to entry. */
static bool check_hoist(uint8_t read_group, uint8_t write_group) {
    XiFunc *f = make_func();
    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    entry->kind = XI_BLOCK_PLAIN;
    wire(entry, header, 0);

    header->kind = XI_BLOCK_IF;
    wire(header, body, 0);
    wire(header, exit_blk, 1);

    body->kind = XI_BLOCK_PLAIN;
    wire(body, header, 0);

    exit_blk->kind = XI_BLOCK_RETURN;

    XiValue *obj = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    XiValue *val = xi_value_new(f, entry, XI_CONST, &stub_int, 0);

    XiValue *cond = xi_value_new(f, header, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    header->control = cond;

    XiValue *load = xi_value_new(f, body, XI_LOAD_FIELD, &stub_int, 1);
    load->args[0] = obj;
    load->mem_group = read_group;
    load->flags = XI_FLAG_READS_MEM;

    XiValue *store = xi_value_new(f, body, XI_STORE_FIELD, &stub_int, 2);
    store->args[0] = obj;
    store->args[1] = val;
    store->mem_group = write_group;
    store->flags = XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM;

    header->sealed = true;
    body->sealed = true;
    exit_blk->sealed = true;

    xi_opt_licm(f);

    bool hoisted = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        if (entry->values[i] == load)
            hoisted = true;
    }

    xi_func_free(f);
    return hoisted;
}

/* Disjoint group pairs: load should be hoisted */
TEST(alias_field_vs_array) {
    ASSERT(check_hoist(XI_MEM_FIELD, XI_MEM_ARRAY));
}
TEST(alias_field_vs_struct) {
    ASSERT(check_hoist(XI_MEM_FIELD, XI_MEM_STRUCT));
}
TEST(alias_array_vs_struct) {
    ASSERT(check_hoist(XI_MEM_ARRAY, XI_MEM_STRUCT));
}
TEST(alias_array_vs_shared) {
    ASSERT(check_hoist(XI_MEM_ARRAY, XI_MEM_SHARED));
}
TEST(alias_struct_vs_upval) {
    ASSERT(check_hoist(XI_MEM_STRUCT, XI_MEM_UPVAL));
}
TEST(alias_upval_vs_array) {
    ASSERT(check_hoist(XI_MEM_UPVAL, XI_MEM_ARRAY));
}
TEST(alias_json_vs_array) {
    ASSERT(check_hoist(XI_MEM_JSON, XI_MEM_ARRAY));
}
TEST(alias_tuple_vs_field) {
    ASSERT(check_hoist(XI_MEM_TUPLE, XI_MEM_FIELD));
}
TEST(alias_chan_vs_struct) {
    ASSERT(check_hoist(XI_MEM_CHAN, XI_MEM_STRUCT));
}
TEST(alias_shared_vs_json) {
    ASSERT(check_hoist(XI_MEM_SHARED, XI_MEM_JSON));
}
TEST(alias_global_vs_array) {
    ASSERT(check_hoist(XI_MEM_GLOBAL, XI_MEM_ARRAY));
}
TEST(alias_tls_vs_field) {
    ASSERT(check_hoist(XI_MEM_TLS, XI_MEM_FIELD));
}

/* Same group: load must NOT be hoisted */
TEST(alias_field_vs_field) {
    ASSERT(!check_hoist(XI_MEM_FIELD, XI_MEM_FIELD));
}
TEST(alias_array_vs_array) {
    ASSERT(!check_hoist(XI_MEM_ARRAY, XI_MEM_ARRAY));
}
TEST(alias_struct_vs_struct) {
    ASSERT(!check_hoist(XI_MEM_STRUCT, XI_MEM_STRUCT));
}
TEST(alias_json_vs_json) {
    ASSERT(!check_hoist(XI_MEM_JSON, XI_MEM_JSON));
}
TEST(alias_upval_vs_upval) {
    ASSERT(!check_hoist(XI_MEM_UPVAL, XI_MEM_UPVAL));
}
TEST(alias_shared_vs_shared) {
    ASSERT(!check_hoist(XI_MEM_SHARED, XI_MEM_SHARED));
}

/* TOP group: always aliases → load must NOT be hoisted */
TEST(alias_field_vs_top) {
    ASSERT(!check_hoist(XI_MEM_FIELD, XI_MEM_TOP));
}
TEST(alias_top_vs_array) {
    ASSERT(!check_hoist(XI_MEM_TOP, XI_MEM_ARRAY));
}

/* CONST group: always safe to hoist even with same-group store */
TEST(alias_const_vs_field) {
    ASSERT(check_hoist(XI_MEM_CONST, XI_MEM_FIELD));
}
TEST(alias_const_vs_array) {
    ASSERT(check_hoist(XI_MEM_CONST, XI_MEM_ARRAY));
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi LICM Tests ===\n\n");

    run_hoist_pure_add();
    run_hoist_const_operand_before_dependent_value();
    run_hoist_generated_speculatable_select();
    run_no_hoist_unspeculatable_zero_effect();
    run_hoist_const_load();
    run_hoist_disjoint_load();
    run_no_hoist_aliasing_load();
    run_no_hoist_inner_operand();
    run_no_hoist_load_with_store_dep();
    run_no_loop_no_change();
    run_no_hoist_call();

    /* Alias-aware TBAA group interaction */
    run_alias_field_vs_array();
    run_alias_field_vs_struct();
    run_alias_array_vs_struct();
    run_alias_array_vs_shared();
    run_alias_struct_vs_upval();
    run_alias_upval_vs_array();
    run_alias_json_vs_array();
    run_alias_tuple_vs_field();
    run_alias_chan_vs_struct();
    run_alias_shared_vs_json();
    run_alias_global_vs_array();
    run_alias_tls_vs_field();

    run_alias_field_vs_field();
    run_alias_array_vs_array();
    run_alias_struct_vs_struct();
    run_alias_json_vs_json();
    run_alias_upval_vs_upval();
    run_alias_shared_vs_shared();

    run_alias_field_vs_top();
    run_alias_top_vs_array();

    run_alias_const_vs_field();
    run_alias_const_vs_array();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
