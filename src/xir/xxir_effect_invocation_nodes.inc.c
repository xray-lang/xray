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

/* The formula graph's queue is shared by every invocation equation. All
 * live entries retain source order during growth; no second queue solves ROOT. */

/* This construction-only delta requests identity/Class reevaluation on the
 * existing queue. ROOT-only joins need no new origin replay; the independent
 * receiving proof still derives every actual input and effect clause. */

#include "xxir_effect_invocation_node_index.inc.c"

/* Neither a producer pointer nor a numeric target alone is a key. Exact
 * physical origin rows are copied into this owner before the node is linked. */

#include "xxir_effect_invocation_edge_index.inc.c"
