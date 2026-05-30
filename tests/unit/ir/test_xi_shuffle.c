/*
 * Regression coverage for the XRAY_XI_SHUFFLE debug mode.
 *
 * The shuffle path in xi_opt.c must keep two cross-cutting invariants
 * intact across every pass invocation:
 *
 *   1. block->id == its index in f->blocks[] (SCCP / xi_loop / codegen
 *      index per-block scratch arrays by id; a permuted blocks[] without
 *      id resync silently mis-routes work to the wrong block).
 *   2. cfg_version is bumped after the permutation so cached RPO /
 *      dominator / loop info recomputes lazily on the next ensure() call.
 *
 * setenv() is used to enable XRAY_XI_SHUFFLE before xi_opt_run_pipeline
 * captures it into a static cache.  Run this binary as a single ctest
 * invocation; it must never share a process with other Xi tests.
 */
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/ir/xi_opt.h"
#include "../../../src/ir/xi_verify.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/base/xmalloc.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 3, .frozen = true};
static XrType stub_array_int = {
    .kind = XR_KIND_ARRAY, .id = 4, .frozen = true, .container = {.element_type = &stub_int}};

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__);                            \
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

/*
 * Build a counted loop with a back-edge (entry -> hdr -> body -> hdr -> exit).
 * Multiple blocks + a SCCP-eligible IF terminator is enough to exercise the
 * shuffle->id-resync->cfg_invalidate chain across SCCP, xi_loop, dom cache.
 */
static XiFunc *build_counted_loop(void) {
    XiFunc *f = xi_func_new("shuffle_loop", &stub_int);
    XiBlock *pre = xi_block_new(f);
    XiBlock *hdr = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *ex = xi_block_new(f);
    pre->sealed = hdr->sealed = body->sealed = ex->sealed = true;

    XiValue *istart = xi_const_int(f, pre, 0, &stub_int);
    XiValue *limit = xi_const_int(f, pre, 4, &stub_int);
    xi_block_set_jump(pre, hdr);

    XiPhi *iphi = xi_phi_new(f, hdr, &stub_int, 2);
    XiValue *cond = xi_binary(f, hdr, XI_LT, &stub_bool, &iphi->value, limit);
    xi_block_set_if(hdr, cond, body, ex);

    XiValue *one = xi_const_int(f, body, 1, &stub_int);
    XiValue *inext = xi_binary(f, body, XI_ADD, &stub_int, &iphi->value, one);
    xi_block_set_jump(body, hdr);

    iphi->value.args[0] = istart;
    iphi->value.args[1] = inext;
    xi_block_set_return(ex, &iphi->value);
    return f;
}

TEST(shuffle_pipeline_keeps_block_id_invariant) {
    XiFunc *f = build_counted_loop();
    ASSERT(f != NULL);

    xi_opt_run_pipeline(f, XI_OPT_FULL);

    /* Whatever the pipeline did, xi_verify (which now enforces
     * blocks[i]->id == i) must accept the result. */
    char err[256] = {0};
    bool ok = xi_verify(f, err, sizeof(err));
    if (!ok)
        fprintf(stderr, "  verify error: %s\n", err);
    ASSERT(ok);

    for (uint32_t b = 0; b < f->nblocks; b++) {
        ASSERT(f->blocks[b] != NULL);
        ASSERT(f->blocks[b]->id == b);
    }
    xi_func_free(f);
}

TEST(shuffle_pipeline_recomputes_dom_after_permutation) {
    XiFunc *f = build_counted_loop();
    ASSERT(f != NULL);

    xi_opt_run_pipeline(f, XI_OPT_FULL);

    /* dom version must equal cfg_version: any pass that bumped one but
     * not the other (e.g. forgot to xi_cfg_invalidate after permuting
     * blocks[]) would surface here as a stale dom_version. */
    xi_ensure_dominators(f);
    ASSERT(f->dom_version == f->cfg_version);

    /* Entry block keeps the canonical idom contract. */
    ASSERT(f->entry == f->blocks[0]);
    ASSERT(f->entry->idom == NULL);
    xi_func_free(f);
}

/*
 * Build a loop body whose two side-effect-free instructions only differ
 * by their var_id:
 *
 *   body:
 *     v_sum_new = ADD v_sum_phi v_i_phi   ; var_id = sum
 *     v_i_new   = ADD v_i_phi   v_one     ; var_id = i
 *
 * In SSA they are independent (both read phis, neither reads the other),
 * but in emit they share VM registers via var coalescing: v_sum_new aliases
 * v_sum_phi's reg and v_i_new aliases v_i_phi's reg.  Swapping their
 * relative order under shuffle would make v_sum_new add the *post-increment*
 * i value, silently changing the program's meaning.
 *
 * This is the exact pattern that 0410_while.xr exposed in shuffle mode.
 */
static XiFunc *build_sum_loop(void) {
    XiFunc *f = xi_func_new("sum_loop", &stub_int);
    XiBlock *pre = xi_block_new(f);
    XiBlock *hdr = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *ex = xi_block_new(f);
    pre->sealed = hdr->sealed = body->sealed = ex->sealed = true;

    XiValue *zero = xi_const_int(f, pre, 0, &stub_int);
    XiValue *five = xi_const_int(f, pre, 5, &stub_int);
    xi_block_set_jump(pre, hdr);

    XiPhi *iphi = xi_phi_new(f, hdr, &stub_int, 2);
    XiPhi *sphi = xi_phi_new(f, hdr, &stub_int, 2);
    iphi->value.var_id = 1; /* var i */
    sphi->value.var_id = 2; /* var sum */
    XiValue *cond = xi_binary(f, hdr, XI_LT, &stub_bool, &iphi->value, five);
    xi_block_set_if(hdr, cond, body, ex);

    XiValue *one = xi_const_int(f, body, 1, &stub_int);
    /* In source order, sum_new comes first, then i_new (matches
     * `sum += i; i++`). */
    XiValue *snew = xi_binary(f, body, XI_ADD, &stub_int, &sphi->value, &iphi->value);
    XiValue *inew = xi_binary(f, body, XI_ADD, &stub_int, &iphi->value, one);
    snew->var_id = 2; /* coalesced with sum_phi */
    inew->var_id = 1; /* coalesced with i_phi */
    xi_block_set_jump(body, hdr);

    iphi->value.args[0] = zero;
    iphi->value.args[1] = inew;
    sphi->value.args[0] = zero;
    sphi->value.args[1] = snew;
    xi_block_set_return(ex, &sphi->value);
    return f;
}

TEST(shuffle_preserves_var_coalesce_chain_in_loop_body) {
    /* Build the IR once, snapshot the (snew, inew) order, then run a
     * shuffle-mode pipeline.  Even after the randomized intra-block
     * reorder, snew must still appear before inew within the body block.
     * Otherwise emit's var coalescing would silently change the loop
     * semantics (sum += i+1 instead of sum += i). */
    XiFunc *f = build_sum_loop();
    ASSERT(f != NULL);

    xi_opt_run_pipeline(f, XI_OPT_FULL);

    char err[256] = {0};
    ASSERT(xi_verify(f, err, sizeof(err)));

    /* Locate the body block by walking succs from the header.  We can't
     * rely on a fixed index because the pipeline may have permuted
     * blocks[]; the structural relationship survives. */
    XiBlock *hdr = NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (blk->kind == XI_BLOCK_IF) {
            hdr = blk;
            break;
        }
    }
    ASSERT(hdr != NULL);
    XiBlock *body = hdr->succs[0];
    ASSERT(body != NULL);

    int snew_idx = -1, inew_idx = -1;
    for (uint32_t i = 0; i < body->nvalues; i++) {
        XiValue *v = body->values[i];
        if (v->op != XI_ADD)
            continue;
        if (v->var_id == 2)
            snew_idx = (int) i;
        else if (v->var_id == 1)
            inew_idx = (int) i;
    }
    ASSERT(snew_idx >= 0);
    ASSERT(inew_idx >= 0);
    /* Source order was snew before inew; shuffle must preserve it. */
    if (!(snew_idx < inew_idx))
        fprintf(stderr, "  var coalesce order violated: snew=%d inew=%d\n", snew_idx, inew_idx);
    ASSERT(snew_idx < inew_idx);
    xi_func_free(f);
}

static XiFunc *build_try_finally_func(XiValue **out_try, XiBlock **out_finally) {
    *out_try = NULL;
    *out_finally = NULL;
    XiFunc *f = xi_func_new("try_finally_remap", &stub_unit);
    XiBlock *pre = xi_block_new(f);
    XiBlock *hdr = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *fin = xi_block_new(f);
    XiBlock *ex = xi_block_new(f);
    pre->sealed = hdr->sealed = body->sealed = fin->sealed = ex->sealed = true;

    XiValue *arg = xi_param(f, pre, 0, &stub_int);
    XiValue *limit = xi_const_int(f, pre, 10, &stub_int);
    xi_block_set_jump(pre, hdr);

    XiValue *cond = xi_binary(f, hdr, XI_LT, &stub_bool, arg, limit);
    if (!cond)
        return f;
    xi_block_set_if(hdr, cond, body, ex);

    XiValue *try_v = xi_value_new(f, body, XI_TRY, &stub_unit, 0);
    if (!try_v)
        return f;
    try_v->aux = NULL;
    try_v->aux_int = (int64_t) fin->id;
    try_v->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(body, fin);

    XiValue *finally_v = xi_value_new(f, fin, XI_FINALLY, &stub_unit, 0);
    if (!finally_v)
        return f;
    finally_v->flags |= XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(fin, hdr);
    xi_block_set_return(ex, xi_const_int(f, ex, 0, &stub_int));

    *out_try = try_v;
    *out_finally = fin;
    return f;
}

TEST(shuffle_remaps_try_finally_block_id) {
    XiValue *try_v = NULL;
    XiBlock *fin = NULL;
    XiFunc *f = build_try_finally_func(&try_v, &fin);
    ASSERT(f != NULL);
    ASSERT(try_v != NULL);
    ASSERT(fin != NULL);

    xi_opt_run_pipeline(f, XI_OPT_LIGHT);

    ASSERT(try_v->aux_int >= 0);
    ASSERT((uint32_t) try_v->aux_int < f->nblocks);
    ASSERT((uint32_t) try_v->aux_int == fin->id);
    ASSERT(f->blocks[(uint32_t) try_v->aux_int] == fin);
    xi_func_free(f);
}

TEST(heap_equality_reads_memory) {
    XiFunc *f = xi_func_new("heap_equality_reads_memory", &stub_bool);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;

    XiValue *lhs = xi_param(f, entry, 0, &stub_array_int);
    XiValue *rhs = xi_param(f, entry, 1, &stub_array_int);
    XiValue *eq = xi_binary(f, entry, XI_EQ, &stub_bool, lhs, rhs);
    XiValue *ne = xi_binary(f, entry, XI_NE, &stub_bool, lhs, rhs);
    XiValue *i0 = xi_const_int(f, entry, 0, &stub_int);
    XiValue *i1 = xi_const_int(f, entry, 1, &stub_int);
    XiValue *primitive_eq = xi_binary(f, entry, XI_EQ, &stub_bool, i0, i1);
    ASSERT(eq != NULL);
    ASSERT(ne != NULL);
    ASSERT(primitive_eq != NULL);
    ASSERT((eq->flags & XI_FLAG_READS_MEM) != 0);
    ASSERT((ne->flags & XI_FLAG_READS_MEM) != 0);
    ASSERT((primitive_eq->flags & XI_FLAG_READS_MEM) == 0);

    xi_block_set_return(entry, eq);
    xi_func_free(f);
}

int main(void) {
    /* Force shuffle + check ON before xi_opt_run_pipeline captures the
     * env into its static cache.  The test binary is single-process so
     * the cache state is private to this run. */
    setenv("XRAY_XI_SHUFFLE", "1", 1);
    setenv("XRAY_XI_CHECK", "1", 1);
    setenv("XRAY_XI_SHUFFLE_SEED", "1", 1);

    printf("=== Xi Shuffle Invariant Tests ===\n\n");

    run_shuffle_pipeline_keeps_block_id_invariant();
    run_shuffle_pipeline_recomputes_dom_after_permutation();
    run_shuffle_preserves_var_coalesce_chain_in_loop_body();
    run_shuffle_remaps_try_finally_block_id();
    run_heap_equality_reads_memory();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
