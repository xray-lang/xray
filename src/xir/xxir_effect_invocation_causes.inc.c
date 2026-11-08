/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_causes.inc.c - Rooted certificates on the existing equation queue
 *
 * KEY CONCEPT:
 *   Equation outputs are already closed. Certificate propagation changes no
 *   permission fact and requires a strictly descending path to a real terminal.
 */
static bool effect_invocation_atom(const EffectInvocationNode *node,
    uint32_t parameters,uint32_t atom,bool local) {
    if (atom<2) return !!((local?node->local_mask:node->intrinsic_mask)&
        (atom?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED));
    if (atom-2<parameters) {
        uint32_t p=atom-2;
        return !!(effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,local)[p/64]&(UINT64_C(1)<<(p%64)));
    }
    if (atom-2-parameters<parameters) {
        uint32_t p=atom-2-parameters;
        return !!((local?node->local_cells:node->cells)[p/64]&(UINT64_C(1)<<(p%64)));
    }
    return atom==(uint64_t)parameters*2+2 && (local?node->local_deferred:node->deferred);
}

/* Exact-key interning is finished before cause propagation. Reuse that
 * private allocation for root-ordered node numbers; nothing enters Final. */

/* These records describe the full equation key, not only a numeric callee.
 * The same root basis indexes every actual edge on this certificate walk. */
