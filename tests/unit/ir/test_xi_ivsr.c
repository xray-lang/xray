/*
 * Unit tests for Xi induction variable strength reduction (IVSR).
 *
 * Construction template (used by every test):
 *
 *   preheader → header(phi i) → body(j = i*c …) → header (back-edge)
 *                              ↘ exit
 *
 * Each test builds the IR by hand, runs xi_opt_ivsr, and asserts the
 * expected rewrite or non-rewrite.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_opt_ivsr.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
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

/* Build a counted loop with a basic IV (i = phi(start, i+step)) and a
 * single derived IV (j = i * scale + offset).  Returns the func and
 * exposes key pointers via out-params for the test body. */
typedef struct {
    XiFunc *f;
    XiBlock *preheader;
    XiBlock *header;
    XiBlock *body;
    XiBlock *exit_b;
    XiPhi *iphi;
    XiValue *istart;
    XiValue *inext;
    XiValue *j; /* the derived value to be reduced */
} LoopFixture;

static LoopFixture build_loop(int64_t start, int64_t step, int64_t scale, int64_t offset,
                              bool include_offset) {
    LoopFixture fx = {0};
    XiFunc *f = xi_func_new("ivsr_test", &stub_int);
    XiBlock *pre = xi_block_new(f);
    XiBlock *hdr = xi_block_new(f);
    XiBlock *bdy = xi_block_new(f);
    XiBlock *ex = xi_block_new(f);
    pre->sealed = hdr->sealed = bdy->sealed = ex->sealed = true;

    /* preheader: i_start = const(start) */
    XiValue *istart = xi_const_int(f, pre, start, &stub_int);
    xi_block_set_jump(pre, hdr);

    /* header: i_phi = phi(istart, inext) — 2-arg phi (preheader, latch) */
    XiPhi *iphi = xi_phi_new(f, hdr, &stub_int, 2);
    /* Cond is irrelevant for IVSR but the header must be a real IF. */
    XiValue *cond = xi_const_bool(f, hdr, true, &stub_bool);
    xi_block_set_if(hdr, cond, bdy, ex);

    /* body: j = i * scale (+ offset),  i_next = i + step */
    XiValue *scale_c = xi_const_int(f, bdy, scale, &stub_int);
    XiValue *j = xi_binary(f, bdy, XI_MUL, &stub_int, &iphi->value, scale_c);
    if (include_offset) {
        XiValue *off_c = xi_const_int(f, bdy, offset, &stub_int);
        j = xi_binary(f, bdy, XI_ADD, &stub_int, j, off_c);
    }
    XiValue *step_c = xi_const_int(f, bdy, step, &stub_int);
    XiValue *inext = xi_binary(f, bdy, XI_ADD, &stub_int, &iphi->value, step_c);
    xi_block_set_jump(bdy, hdr);

    /* Wire iphi: header preds are [pre, bdy] in insertion order. */
    iphi->value.args[0] = istart;
    iphi->value.args[1] = inext;

    xi_block_set_return(ex, xi_const_int(f, ex, 0, &stub_int));

    fx.f = f;
    fx.preheader = pre;
    fx.header = hdr;
    fx.body = bdy;
    fx.exit_b = ex;
    fx.iphi = iphi;
    fx.istart = istart;
    fx.inext = inext;
    fx.j = j;
    return fx;
}

/* Count phis in a block. */
static uint32_t phi_count(const XiBlock *blk) {
    uint32_t n = 0;
    for (const XiPhi *p = blk->phis; p; p = p->next)
        n++;
    return n;
}

static const XiPassStats *find_stats(const XiPipelineStats *stats, const char *name) {
    for (uint32_t i = 0; i < stats->npass; i++) {
        if (strcmp(stats->passes[i].name, name) == 0)
            return &stats->passes[i];
    }
    return NULL;
}

static XiFunc *build_live_counted_loop(void) {
    XiFunc *f = xi_func_new("ivsr_live", &stub_int);
    XiBlock *pre = xi_block_new(f);
    XiBlock *hdr = xi_block_new(f);
    XiBlock *bdy = xi_block_new(f);
    XiBlock *ex = xi_block_new(f);
    pre->sealed = hdr->sealed = bdy->sealed = ex->sealed = true;

    XiValue *istart = xi_const_int(f, pre, 0, &stub_int);
    XiValue *sum_start = xi_const_int(f, pre, 0, &stub_int);
    XiValue *limit = xi_const_int(f, pre, 10, &stub_int);
    xi_block_set_jump(pre, hdr);

    XiPhi *iphi = xi_phi_new(f, hdr, &stub_int, 2);
    XiPhi *sum_phi = xi_phi_new(f, hdr, &stub_int, 2);
    XiValue *cond = xi_binary(f, hdr, XI_LT, &stub_bool, &iphi->value, limit);
    xi_block_set_if(hdr, cond, bdy, ex);

    XiValue *scale_c = xi_const_int(f, bdy, 4, &stub_int);
    XiValue *mul = xi_binary(f, bdy, XI_MUL, &stub_int, &iphi->value, scale_c);
    XiValue *off_c = xi_const_int(f, bdy, 7, &stub_int);
    XiValue *j = xi_binary(f, bdy, XI_ADD, &stub_int, mul, off_c);
    XiValue *sum_next = xi_binary(f, bdy, XI_ADD, &stub_int, &sum_phi->value, j);
    XiValue *one = xi_const_int(f, bdy, 1, &stub_int);
    XiValue *inext = xi_binary(f, bdy, XI_ADD, &stub_int, &iphi->value, one);
    xi_block_set_jump(bdy, hdr);

    iphi->value.args[0] = istart;
    iphi->value.args[1] = inext;
    sum_phi->value.args[0] = sum_start;
    sum_phi->value.args[1] = sum_next;
    xi_block_set_return(ex, &sum_phi->value);
    return f;
}

/* ========== Tests ========== */

TEST(reduces_mul_by_const) {
    /* j = i * 2  with  i = phi(0, i+1)
     * After IVSR: j becomes COPY(j_phi), j_phi = phi(0*2=0, j_phi+1*2=2). */
    LoopFixture fx = build_loop(0, 1, 2, 0, false);

    XiPassChange chg = xi_opt_ivsr(fx.f);
    ASSERT(chg.values_changed);
    ASSERT(!chg.cfg_changed);

    /* Original mul has been rewritten to COPY referencing the new phi. */
    ASSERT(fx.j->op == XI_COPY);
    ASSERT(fx.j->nargs == 1);
    XiValue *jphi_val = fx.j->args[0];
    ASSERT(jphi_val != NULL);
    ASSERT(jphi_val->op == XI_PHI);
    ASSERT(jphi_val->block == fx.header);

    /* Exactly one new phi added to the header (i + j). */
    ASSERT(phi_count(fx.header) == 2);

    xi_func_free(fx.f);
}

TEST(reduces_with_offset) {
    /* j = i * 3 + 5 with i = phi(0, i+1) */
    LoopFixture fx = build_loop(0, 1, 3, 5, true);

    XiPassChange chg = xi_opt_ivsr(fx.f);
    ASSERT(chg.values_changed);

    /* The outer XI_ADD (j = mul + 5) is what gets rewritten. */
    ASSERT(fx.j->op == XI_COPY);
    XiValue *jphi_val = fx.j->args[0];
    ASSERT(jphi_val->op == XI_PHI);
    xi_func_free(fx.f);
}

TEST(skips_non_const_step) {
    /* i_next = i + param   — step is not a constant, so IVSR must bail. */
    LoopFixture fx = build_loop(0, 1, 2, 0, false);
    /* Replace the step operand of i_next with a parameter to make the
     * step loop-invariant but non-constant. */
    XiValue *param = xi_param(fx.f, fx.preheader, 0, &stub_int);
    fx.inext->args[1] = param;

    XiPassChange chg = xi_opt_ivsr(fx.f);
    /* The IV detector requires a constant step; with the modified IR
     * there is no basic IV, so the pass should report no change. */
    ASSERT(!chg.values_changed);
    ASSERT(fx.j->op == XI_MUL); /* untouched */
    xi_func_free(fx.f);
}

TEST(no_loop_no_change) {
    XiFunc *f = xi_func_new("no_loop", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    XiValue *c = xi_const_int(f, entry, 42, &stub_int);
    xi_block_set_return(entry, c);

    XiPassChange chg = xi_opt_ivsr(f);
    ASSERT(!chg.values_changed);
    ASSERT(!chg.cfg_changed);
    xi_func_free(f);
}

TEST(j_next_step_is_correct) {
    /* j = i * 4  with  i = phi(0, i+3) → j_next should be j_phi + 12. */
    LoopFixture fx = build_loop(0, 3, 4, 0, false);

    XiPassChange chg = xi_opt_ivsr(fx.f);
    ASSERT(chg.values_changed);

    XiValue *jphi_val = fx.j->args[0];
    /* The phi has two operands; one is the latch value j_next.
     * j_next must be XI_ADD(j_phi, const(12)). */
    bool found_correct = false;
    for (uint16_t i = 0; i < jphi_val->nargs; i++) {
        XiValue *arg = jphi_val->args[i];
        if (!arg)
            continue;
        if (arg->op == XI_ADD && arg->nargs == 2 && arg->args[0] == jphi_val) {
            XiValue *step = arg->args[1];
            if (step && step->op == XI_CONST && step->aux_int == 12) {
                found_correct = true;
                break;
            }
        }
    }
    ASSERT(found_correct);
    xi_func_free(fx.f);
}

TEST(pipeline_stats_include_child_ivsr) {
    XiFunc *parent = xi_func_new("parent", &stub_int);
    XiBlock *entry = xi_block_new(parent);
    entry->sealed = true;
    xi_block_set_return(entry, xi_const_int(parent, entry, 0, &stub_int));

    XiFunc *child = build_live_counted_loop();
    parent->children = (XiFunc **) xr_calloc(1, sizeof(XiFunc *));
    ASSERT(parent->children != NULL);
    parent->children[0] = child;
    parent->nchildren = 1;
    parent->children_cap = 1;

    XiPipelineStats stats;
    XiPassChange chg = xi_opt_run_pipeline_ex(parent, XI_OPT_FULL, &stats, 0);

    ASSERT(chg.values_changed);
    const XiPassStats *ivsr = find_stats(&stats, "ivsr");
    ASSERT(ivsr != NULL);
    ASSERT(ivsr->n_added >= 6);

    const XiPassStats *block_simplify = find_stats(&stats, "block_simplify");
    ASSERT(block_simplify != NULL);
    ASSERT(block_simplify->invocations > 0);

    xi_func_free(parent);
}

int main(void) {
    printf("=== Xi IVSR tests ===\n\n");

    run_reduces_mul_by_const();
    run_reduces_with_offset();
    run_skips_non_const_step();
    run_no_loop_no_change();
    run_j_next_step_is_correct();
    run_pipeline_stats_include_child_ivsr();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
