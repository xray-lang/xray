/*
 * Unit tests for Xi self-tail-call optimization.
 * Covers self-tail-call loop conversion and conservative no-op cases.
 */

#include "../../../src/ir/xi_opt_tail_call.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi.h"
#include "../../../src/runtime/value/xtype.h"
#include "../../../src/base/xmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_func = {.kind = XR_KIND_FUNCTION, .id = 3, .frozen = true};

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

static void setup_params(XiFunc *f, XiValue **params, uint16_t nparams) {
    f->nparams = nparams;
    f->params = (XiValue **) xr_calloc(nparams, sizeof(XiValue *));
    ASSERT(f->params != NULL);
    for (uint16_t i = 0; i < nparams; i++)
        f->params[i] = params[i];
}

static void wire(XiBlock *from, XiBlock *to, int slot) {
    from->succs[slot] = to;
    xi_block_add_pred(to, from);
}

static uint32_t count_phis(const XiBlock *blk) {
    uint32_t n = 0;
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
        n++;
    return n;
}

/* ========== Test: self-tail-call converts to loop ========== */

TEST(self_tail_call_to_loop) {
    XiFunc *f = make_func("self_tail");
    XiBlock *entry = f->entry;

    XiValue *n = xi_param(f, entry, 0, &stub_int);
    XiValue *acc = xi_param(f, entry, 1, &stub_int);
    XiValue *params[2] = {n, acc};
    setup_params(f, params, 2);

    XiBlock *recur = xi_block_new(f);
    XiBlock *exit_blk = xi_block_new(f);

    XiValue *zero = xi_const_int(f, entry, 0, &stub_int);
    XiValue *cond = xi_binary(f, entry, XI_EQ, &stub_bool, n, zero);
    entry->kind = XI_BLOCK_IF;
    entry->control = cond;
    wire(entry, exit_blk, 0);
    wire(entry, recur, 1);

    exit_blk->kind = XI_BLOCK_RETURN;
    exit_blk->control = acc;
    exit_blk->sealed = true;

    XiValue *one = xi_const_int(f, recur, 1, &stub_int);
    XiValue *next_n = xi_binary(f, recur, XI_SUB, &stub_int, n, one);
    XiValue *next_acc = xi_binary(f, recur, XI_ADD, &stub_int, acc, n);
    XiValue *callee = xi_value_new(f, recur, XI_CLOSURE_NEW, &stub_func, 0);
    callee->aux = f;
    XiValue *call = xi_value_new(f, recur, XI_CALL, &stub_int, 3);
    call->args[0] = callee;
    call->args[1] = next_n;
    call->args[2] = next_acc;
    recur->kind = XI_BLOCK_RETURN;
    recur->control = call;
    recur->sealed = true;

    XiPassChange chg = xi_opt_tail_call(f);
    ASSERT(chg.cfg_changed);
    ASSERT(chg.values_changed);

    /* A new loop header is inserted after entry. */
    ASSERT(f->nblocks == 4);
    XiBlock *header = entry->succs[0];
    ASSERT(header != NULL);
    ASSERT(header != entry);
    ASSERT(entry->kind == XI_BLOCK_PLAIN);
    ASSERT(header->kind == XI_BLOCK_IF);
    ASSERT(header->control == cond);
    ASSERT(count_phis(header) == 2);
    ASSERT(header->npreds == 2);
    ASSERT(header->preds[0] == entry);
    ASSERT(header->preds[1] == recur);

    /* Recursive return block is now a back-edge to header and no longer returns. */
    ASSERT(recur->kind == XI_BLOCK_PLAIN);
    ASSERT(recur->control == NULL);
    ASSERT(recur->succs[0] == header);

    /* The call value is removed from the recursive block. */
    for (uint32_t i = 0; i < recur->nvalues; i++)
        ASSERT(recur->values[i] != call);

    /* Header phis receive initial params and recursive arguments. */
    bool saw_n_phi = false;
    bool saw_acc_phi = false;
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        ASSERT(phi->value.nargs == 2);
        if (phi->value.aux_int == 0) {
            saw_n_phi = true;
            ASSERT(phi->value.args[0] == n);
            ASSERT(phi->value.args[1] == next_n);
        } else if (phi->value.aux_int == 1) {
            saw_acc_phi = true;
            ASSERT(phi->value.args[0] == acc);
            ASSERT(phi->value.args[1] == next_acc);
        }
    }
    ASSERT(saw_n_phi);
    ASSERT(saw_acc_phi);

    char err[256];
    ASSERT(xi_verify(f, err, sizeof(err)));

    xi_func_free(f);
}

/* ========== Test: non-self call is not converted ========== */

TEST(non_self_call_no_change) {
    XiFunc *f = make_func("caller");
    XiFunc *g = xi_func_new("callee", &stub_int);
    ASSERT(g != NULL);
    XiBlock *g_entry = xi_block_new(g);
    ASSERT(g_entry != NULL);

    XiBlock *entry = f->entry;
    XiValue *n = xi_param(f, entry, 0, &stub_int);
    XiValue *params[1] = {n};
    setup_params(f, params, 1);

    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    callee->aux = g;
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    call->args[0] = callee;
    call->args[1] = n;
    entry->kind = XI_BLOCK_RETURN;
    entry->control = call;

    XiPassChange chg = xi_opt_tail_call(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_RETURN);
    ASSERT(entry->control == call);

    xi_func_free(g);
    xi_func_free(f);
}

/* ========== Test: call not in tail position is not converted ========== */

TEST(non_tail_call_no_change) {
    XiFunc *f = make_func("non_tail");
    XiBlock *entry = f->entry;
    XiValue *n = xi_param(f, entry, 0, &stub_int);
    XiValue *params[1] = {n};
    setup_params(f, params, 1);

    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    callee->aux = f;
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 2);
    call->args[0] = callee;
    call->args[1] = n;
    XiValue *plus_one = xi_binary(f, entry, XI_ADD, &stub_int, call, n);
    entry->kind = XI_BLOCK_RETURN;
    entry->control = plus_one;

    XiPassChange chg = xi_opt_tail_call(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_RETURN);
    ASSERT(entry->control == plus_one);

    xi_func_free(f);
}

/* ========== Test: self tail call with zero params is skipped ========== */

TEST(zero_params_no_change) {
    XiFunc *f = make_func("zero_params");
    XiBlock *entry = f->entry;
    f->nparams = 0;
    f->params = NULL;

    XiValue *callee = xi_value_new(f, entry, XI_CLOSURE_NEW, &stub_func, 0);
    callee->aux = f;
    XiValue *call = xi_value_new(f, entry, XI_CALL, &stub_int, 1);
    call->args[0] = callee;
    entry->kind = XI_BLOCK_RETURN;
    entry->control = call;

    XiPassChange chg = xi_opt_tail_call(f);
    ASSERT(!chg.cfg_changed);
    ASSERT(entry->kind == XI_BLOCK_RETURN);

    xi_func_free(f);
}

/* ========== Main ========== */

int main(void) {
    printf("=== Xi Tail Call Tests ===\n\n");

    run_self_tail_call_to_loop();
    run_non_self_call_no_change();
    run_non_tail_call_no_change();
    run_zero_params_no_change();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
