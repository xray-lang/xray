/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_go_safe.inc.c - Reverified direct worker execution permissions
 *
 * KEY CONCEPT:
 *   Root permission consumes final facts; borrow shape is an independent rule.
 */
static XrXirStatus effect_go_shape(const XrXirFunction *function,
    const XrXirCompileContext *work) {
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        if (function->instructions[i].op == XR_XIR_SLOT_PLACE) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_go_safe(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, const XrXirCompileContext *work) {
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        /* Permission to enter through GO is a separate shape obligation.
         * A safe lexical cleanup body may execute inside an ordinary worker. */
        XrXirStatus qualified = effect_go_shape(&module->functions[f], work);
        if (qualified == XR_XIR_BUDGET) return qualified;
        effects->go_safe[f] = qualified;
        if (qualified != XR_XIR_OK) graph->queue[back++] = f;
    }
    /* Only the independent borrow shape propagates here. The shared solver
     * has already closed root facts over calls, defaults, cleanup and cycles. */
    while (front < back) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        uint32_t callee = graph->queue[front++];
        for (uint32_t e = graph->heads[callee]; e != UINT32_MAX; e = graph->edges[e].next) {
            if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
            uint32_t caller = graph->edges[e].caller;
            if (effects->go_safe[caller] == XR_XIR_OK) {
                effects->go_safe[caller] = XR_XIR_BAD_TYPE; graph->queue[back++] = caller;
            }
        }
    }
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        if (effects->root[f].requires_root || effects->root[f].unresolved)
            effects->go_safe[f] = XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
