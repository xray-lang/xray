/*
 * Unit tests for Xi CFG-derived analysis cache (xi_ensure_rpo /
 * xi_ensure_dominators / xi_cfg_invalidate).
 *
 * These verify the cache semantics independently of any specific pass:
 *   - First ensure() after xi_func_new always recomputes (miss).
 *   - A second ensure() with no CFG change is a no-op (hit).
 *   - xi_cfg_invalidate() forces the next ensure() to recompute.
 *   - The dominator ensure() depends on RPO: invalidating CFG forces
 *     both to recompute on the next call.
 */

#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_analysis.h"
#include "../../../src/ir/xi_loop.h"
#include "../../../src/runtime/value/xtype.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

/* Build a minimal function with a single return block — enough to
 * exercise the RPO / dominator computation. */
static XiFunc *make_trivial(void) {
    XiFunc *f = xi_func_new("dom_cache_test", &stub_int);
    XiBlock *entry = xi_block_new(f);
    entry->sealed = true;
    entry->kind = XI_BLOCK_RETURN;
    xi_block_set_return(entry, xi_const_int(f, entry, 0, &stub_int));
    return f;
}

TEST(initial_state_forces_recompute) {
    XiFunc *f = make_trivial();
    /* Fresh function: cfg_version=1, rpo/dom_version=0 (calloc). */
    ASSERT(f->cfg_version == 1);
    ASSERT(f->rpo_version == 0);
    ASSERT(f->dom_version == 0);

    ASSERT(xi_ensure_rpo(f) == true); /* miss */
    ASSERT(f->rpo_version == f->cfg_version);
    xi_func_free(f);
}

TEST(second_call_is_cache_hit) {
    XiFunc *f = make_trivial();
    xi_ensure_rpo(f);
    ASSERT(xi_ensure_rpo(f) == false); /* no CFG change -> hit */

    xi_ensure_dominators(f);
    ASSERT(xi_ensure_dominators(f) == false); /* hit */
    xi_func_free(f);
}

TEST(invalidate_forces_recompute) {
    XiFunc *f = make_trivial();
    xi_ensure_dominators(f);
    uint64_t v_before = f->cfg_version;

    xi_cfg_invalidate(f);
    ASSERT(f->cfg_version == v_before + 1);

    ASSERT(xi_ensure_rpo(f) == true); /* miss after invalidation */
    ASSERT(xi_ensure_dominators(f) == true);
    xi_func_free(f);
}

TEST(dominators_imply_rpo_recompute) {
    XiFunc *f = make_trivial();
    /* Fresh: neither RPO nor dom is computed. */
    ASSERT(f->rpo_version == 0);
    ASSERT(f->dom_version == 0);

    /* Ensuring dominators must also bring RPO up to date. */
    ASSERT(xi_ensure_dominators(f) == true);
    ASSERT(f->rpo_version == f->cfg_version);
    ASSERT(f->dom_version == f->cfg_version);
    xi_func_free(f);
}

/* ========== Loop info cache ========== */

/* Build a function with a standard natural loop:
 *   preheader → header → body → header (back-edge)
 *   header → exit  (loop exit)
 * This produces exactly one natural loop with body == {header, body}. */
static XiFunc *make_with_loop(void) {
    XiFunc *f = xi_func_new("with_loop", &stub_int);
    /* The first xi_block_new becomes f->entry (xi_func_new alone
     * leaves f->entry == NULL). */
    XiBlock *preheader = xi_block_new(f);
    XiBlock *header = xi_block_new(f);
    XiBlock *body = xi_block_new(f);
    XiBlock *exit_b = xi_block_new(f);
    preheader->sealed = true;
    header->sealed = true;
    body->sealed = true;
    exit_b->sealed = true;

    xi_block_set_jump(preheader, header);
    XiValue *cond = xi_const_int(f, header, 1, &stub_int);
    xi_block_set_if(header, cond, body, exit_b);
    xi_block_set_jump(body, header);
    xi_block_set_return(exit_b, xi_const_int(f, exit_b, 0, &stub_int));
    return f;
}

TEST(loop_cache_first_call_recomputes) {
    XiFunc *f = make_with_loop();
    XiLoopInfo *info1 = xi_ensure_loops(f);
    ASSERT(info1 != NULL);
    ASSERT(info1->nloop == 1);
    ASSERT(f->loop_cache == info1);
    ASSERT(f->loop_version == f->cfg_version);
    xi_func_free(f);
}

TEST(loop_cache_second_call_is_hit) {
    XiFunc *f = make_with_loop();
    XiLoopInfo *info1 = xi_ensure_loops(f);
    XiLoopInfo *info2 = xi_ensure_loops(f);
    /* Same pointer == cache hit, no reallocation. */
    ASSERT(info1 == info2);
    xi_func_free(f);
}

TEST(loop_cache_invalidate_rebuilds) {
    XiFunc *f = make_with_loop();
    XiLoopInfo *info1 = xi_ensure_loops(f);
    ASSERT(info1 != NULL);

    xi_cfg_invalidate(f);
    /* After invalidation, the next ensure() must rebuild — but the
     * cache field may legitimately still point at the freshly built
     * structure, so we verify behaviour via loop_version. */
    XiLoopInfo *info2 = xi_ensure_loops(f);
    ASSERT(info2 != NULL);
    ASSERT(info2->nloop == 1);
    ASSERT(f->loop_version == f->cfg_version);
    xi_func_free(f);
}

TEST(loop_cache_freed_on_func_free) {
    /* This test exists mainly so AddressSanitizer / leak detectors
     * catch a missing free in xi_func_free.  No assertions beyond a
     * successful round-trip. */
    XiFunc *f = make_with_loop();
    (void) xi_ensure_loops(f);
    xi_func_free(f);
}

/* ========== Recompute counters ========== */

TEST(counters_track_cache_misses) {
    XiFunc *f = make_with_loop();
    /* Fresh function: no analyses yet. */
    ASSERT(f->rpo_recomputes == 0);
    ASSERT(f->dom_recomputes == 0);
    ASSERT(f->loop_recomputes == 0);

    /* First ensure() triggers one of each (loops chains through dom,
     * dom chains through rpo). */
    xi_ensure_loops(f);
    ASSERT(f->rpo_recomputes == 1);
    ASSERT(f->dom_recomputes == 1);
    ASSERT(f->loop_recomputes == 1);

    /* Cache hit: counters stay put. */
    xi_ensure_loops(f);
    xi_ensure_dominators(f);
    xi_ensure_rpo(f);
    ASSERT(f->rpo_recomputes == 1);
    ASSERT(f->dom_recomputes == 1);
    ASSERT(f->loop_recomputes == 1);

    /* After invalidation, the next ensure() chain recomputes everything. */
    xi_cfg_invalidate(f);
    xi_ensure_loops(f);
    ASSERT(f->rpo_recomputes == 2);
    ASSERT(f->dom_recomputes == 2);
    ASSERT(f->loop_recomputes == 2);

    xi_func_free(f);
}

TEST(compute_unconditional_still_updates_versions) {
    /* The legacy unconditional entry points must keep cache version
     * tags in sync, otherwise mixing them with ensure() would corrupt
     * cache semantics. */
    XiFunc *f = make_trivial();
    xi_compute_rpo(f);
    ASSERT(f->rpo_version == f->cfg_version);

    xi_compute_dominators(f);
    ASSERT(f->dom_version == f->cfg_version);

    /* Both ensures should now be hits. */
    ASSERT(xi_ensure_rpo(f) == false);
    ASSERT(xi_ensure_dominators(f) == false);
    xi_func_free(f);
}

int main(void) {
    printf("=== Xi dom/rpo cache tests ===\n\n");

    run_initial_state_forces_recompute();
    run_second_call_is_cache_hit();
    run_invalidate_forces_recompute();
    run_dominators_imply_rpo_recompute();
    run_compute_unconditional_still_updates_versions();
    run_loop_cache_first_call_recomputes();
    run_loop_cache_second_call_is_hit();
    run_loop_cache_invalidate_rebuilds();
    run_loop_cache_freed_on_func_free();
    run_counters_track_cache_misses();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
