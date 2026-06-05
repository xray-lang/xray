/*
 * Unit tests for Xi if-conversion pass.
 *
 * Covers diamond CFG flattening into XI_SELECT, eligibility gating
 * (instruction budget, phi count, side-effects), and CFG rewiring.
 */

#include "../../../src/ir/xi_opt_ifconv.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Allocate a function with a sealed entry block. */
static XiFunc *make_func(void) {
    XiFunc *f = xi_func_new("test_ifconv", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    return f;
}

/* Wire from->succs[slot] = to and add from to to->preds. */
static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

/* Build: entry(IF cond) -> then -> join, entry -> else -> join (RETURN).
 * then/else each contain one pure ADD; join has one phi merging them. */
static void build_diamond(XiFunc *f, XiBlock **out_entry, XiBlock **out_then, XiBlock **out_else,
                          XiBlock **out_join, XiPhi **out_phi, XiValue **out_cond,
                          XiValue **out_then_val, XiValue **out_else_val) {
    XiBlock *entry = f->entry;
    XiBlock *then_b = xi_block_new(f);
    XiBlock *else_b = xi_block_new(f);
    XiBlock *join = xi_block_new(f);

    entry->kind = XI_BLOCK_IF;
    then_b->kind = XI_BLOCK_PLAIN;
    else_b->kind = XI_BLOCK_PLAIN;
    join->kind = XI_BLOCK_RETURN;

    XiValue *cond = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    cond->aux_int = 1;
    entry->control = cond;
    wire(entry, then_b, 0);
    wire(entry, else_b, 1);

    XiValue *a = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    a->aux_int = 10;
    XiValue *b = xi_value_new(f, entry, XI_CONST, &stub_int, 0);
    b->aux_int = 20;

    /* then: t = a + a */
    XiValue *tv = xi_value_new(f, then_b, XI_ADD, &stub_int, 2);
    tv->args[0] = a;
    tv->args[1] = a;
    wire(then_b, join, 0);

    /* else: e = b + b */
    XiValue *ev = xi_value_new(f, else_b, XI_ADD, &stub_int, 2);
    ev->args[0] = b;
    ev->args[1] = b;
    wire(else_b, join, 0);

    /* join: phi(t from then, e from else) */
    XiPhi *phi = xi_phi_new(f, join, &stub_int, 2);
    phi->value.args[0] = tv;
    phi->value.args[1] = ev;
    xi_block_set_return(join, &phi->value);

    then_b->sealed = true;
    else_b->sealed = true;
    join->sealed = true;

    if (out_entry)
        *out_entry = entry;
    if (out_then)
        *out_then = then_b;
    if (out_else)
        *out_else = else_b;
    if (out_join)
        *out_join = join;
    if (out_phi)
        *out_phi = phi;
    if (out_cond)
        *out_cond = cond;
    if (out_then_val)
        *out_then_val = tv;
    if (out_else_val)
        *out_else_val = ev;
}

/* ========== Test: basic diamond is converted to SELECT ========== */

TEST(basic_diamond) {
    XiFunc *f = make_func();
    XiBlock *entry, *then_b, *else_b, *join;
    XiPhi *phi;
    XiValue *cond, *tv, *ev;
    build_diamond(f, &entry, &then_b, &else_b, &join, &phi, &cond, &tv, &ev);

    XiPassChange chg = xi_opt_ifconv(f);
    ASSERT(chg.cfg_changed);
    ASSERT(chg.values_changed);

    /* The if-block flattens to PLAIN with a single successor. */
    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(entry->control == NULL);
    ASSERT(entry->succs[0] == join);
    ASSERT(entry->succs[1] == NULL);

    /* Both arms collapse to UNREACHABLE. */
    ASSERT(then_b->kind == XI_BLOCK_UNREACHABLE);
    ASSERT(else_b->kind == XI_BLOCK_UNREACHABLE);
    ASSERT(tv->block == entry);
    ASSERT(ev->block == entry);

    /* The phi is gone and join now has a single predecessor (the if-block). */
    ASSERT(join->phis == NULL);
    ASSERT(join->npreds == 1);
    ASSERT(join->preds[0] == entry);

    /* The if-block grew an XI_SELECT(cond, tv, ev) and the return now uses it. */
    bool found_select = false;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        XiValue *v = entry->values[i];
        if (v && v->op == XI_SELECT) {
            ASSERT(v->nargs == 3);
            ASSERT(v->args[0] == cond);
            ASSERT(v->args[1] == tv);
            ASSERT(v->args[2] == ev);
            ASSERT(join->control == v);
            found_select = true;
        }
    }
    ASSERT(found_select);

    xi_func_free(f);
}

/* ========== Test: arm with > IFCONV_MAX_INS pure values is rejected ========== */

TEST(rejects_oversized_arm) {
    XiFunc *f = make_func();
    XiBlock *entry, *then_b, *else_b, *join;
    XiPhi *phi;
    XiValue *cond, *tv, *ev;
    build_diamond(f, &entry, &then_b, &else_b, &join, &phi, &cond, &tv, &ev);

    /* Add three extra pure ops to the then-arm: total = 1 + 3 = 4 > 2. */
    for (int i = 0; i < 3; i++) {
        XiValue *extra = xi_value_new(f, then_b, XI_ADD, &stub_int, 2);
        extra->args[0] = tv;
        extra->args[1] = tv;
    }

    XiPassChange chg = xi_opt_ifconv(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_IF);
    ASSERT(then_b->kind == XI_BLOCK_PLAIN);
    ASSERT(join->phis == phi);

    xi_func_free(f);
}

/* ========== Test: arm with side-effect op is rejected ========== */

TEST(rejects_side_effect_arm) {
    XiFunc *f = make_func();
    XiBlock *entry, *then_b, *else_b, *join;
    XiPhi *phi;
    XiValue *cond, *tv, *ev;
    build_diamond(f, &entry, &then_b, &else_b, &join, &phi, &cond, &tv, &ev);

    /* Mark the then-arm's add as side-effecting; ifconv must refuse to
     * speculate it past the branch. */
    tv->flags |= XI_FLAG_SIDE_EFFECT;

    XiPassChange chg = xi_opt_ifconv(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_IF);
    ASSERT(then_b->kind == XI_BLOCK_PLAIN);
    ASSERT(join->phis == phi);

    xi_func_free(f);
}

/* ========== Test: join with a third predecessor is rejected ========== */

TEST(rejects_three_pred_join) {
    XiFunc *f = make_func();
    XiBlock *entry, *then_b, *else_b, *join;
    XiPhi *phi;
    XiValue *cond, *tv, *ev;
    build_diamond(f, &entry, &then_b, &else_b, &join, &phi, &cond, &tv, &ev);

    /* Add an extra predecessor to join so npreds=3. The phi's argument
     * count stays at 2 — ifconv must bail on the arity mismatch. */
    XiBlock *extra = xi_block_new(f);
    extra->kind = XI_BLOCK_PLAIN;
    wire(extra, join, 0);
    extra->sealed = true;

    XiPassChange chg = xi_opt_ifconv(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_IF);
    ASSERT(join->phis == phi);
    ASSERT(join->npreds == 3);

    xi_func_free(f);
}

/* ========== Test: join without phi is left unchanged ========== */

TEST(no_phi_no_change) {
    /* Build the diamond, then drop the phi: no merge value to convert. */
    XiFunc *f = make_func();
    XiBlock *entry, *then_b, *else_b, *join;
    XiPhi *phi;
    XiValue *cond, *tv, *ev;
    build_diamond(f, &entry, &then_b, &else_b, &join, &phi, &cond, &tv, &ev);
    join->phis = NULL;
    join->kind = XI_BLOCK_PLAIN;
    join->control = NULL;

    XiPassChange chg = xi_opt_ifconv(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(!chg.values_changed);
    ASSERT(entry->kind == XI_BLOCK_IF);

    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi if-conversion tests ===\n\n");

    run_basic_diamond();
    run_rejects_oversized_arm();
    run_rejects_side_effect_arm();
    run_rejects_three_pred_join();
    run_no_phi_no_change();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
