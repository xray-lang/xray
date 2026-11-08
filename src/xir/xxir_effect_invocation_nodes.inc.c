/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_nodes.inc.c - Owned keys, fixed producer basis and one shared equation queue
 */
/* The producer basis is fixed before any equation is expanded. Capture
 * environments are flat rows for these real sites; a repeated nested site
 * cannot manufacture another basis atom or a recursively copied path. */
typedef struct EffectInvocationSiteRows {
    EffectInvocationSite *rows;
    uint32_t capacity,at,binding,function,instruction,captures;
    bool compare;
} EffectInvocationSiteRows;

static void effect_invocation_final_clear(EffectInvocationFinal *final) {
    if (!final) return;
    if (final->outcomes) {
        effect_outcomes_clear(final->outcomes,final->outcomes->count);xr_compile_resources_free(final->outcomes);
    }
    xr_compile_resources_free(final->capture.facts);
    effect_parameter_summary_clear(&final->parameter_summary);
    effect_invocation_result_clear(&final->summary);
    for (uint32_t n=0;n<final->count;++n) {
        xr_compile_resources_free(final->nodes[n].causes);
    }
    for (uint32_t d=0;d<final->deferred_count;++d)
        xr_compile_resources_free(final->deferred_calls[d].arguments);
    xr_compile_resources_free(final->deferred_calls);
    xr_compile_resources_free(final->nodes);xr_compile_resources_free(final->edges);
    xr_compile_resources_free(final->sites);
    xr_compile_resources_free(final->roots);xr_compile_resources_free(final->root_spaces);
    /* Physical rows and Cell proof share the material lifetime. */
}

static void effect_invocation_final_free(EffectInvocationFinal *final) {
    if (!final) return;
    effect_invocation_final_clear(final);xr_compile_resources_free(final);
}

/* The formula graph's queue is shared by every invocation equation. All
 * live entries retain source order during growth; no second queue solves ROOT. */

/* This construction-only delta requests identity/Class reevaluation on the
 * existing queue. ROOT-only joins need no new origin replay; the independent
 * receiving proof still derives every actual input and effect clause. */

#include "xxir_effect_invocation_node_index.inc.c"

/* Neither a producer pointer nor a numeric target alone is a key. Exact
 * physical origin rows are copied into this owner before the node is linked. */

#include "xxir_effect_invocation_edge_index.inc.c"
