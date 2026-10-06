/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_go_safe.inc.c - Reverified direct worker execution permissions
 *
 * KEY CONCEPT:
 *   Local nonSendable storage is legal; module mutable authority cannot cross.
 */
static XrXirStatus effect_go_seed(const XrXirModule *module, uint32_t f,
    const XrXirInstruction *op, const XrXirCompileContext *work) {
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE || op->op == XR_XIR_SLOT_GROUP_INIT ||
        op->op == XR_XIR_SLOT_PLACE || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT ||
        op->op == XR_XIR_CALL_REQUIREMENT) return XR_XIR_BAD_TYPE;
    if (op->op != XR_XIR_SLOT_LOAD) return XR_XIR_OK;
    const XrXirSlot *slot = &module->declarations->slots[op->immediate];
    if (slot->mutable) return XR_XIR_BAD_TYPE;
    XrXirProofContext proof = {module, {XR_XIR_CONTEXT_FUNCTION, f, 0}};
    return xr_xir_compile_type_markers_prove(work, &proof, slot->type, XR_XIR_CONSTRAINT_SENDABLE);
}
static XrXirStatus effect_go_safe(const XrXirModule *module, XrXirEffects *effects,
    EffectGraph *graph, const XrXirCompileContext *work) {
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
        /* Permission to enter through GO is a separate shape obligation.
         * A safe lexical cleanup body may execute inside an ordinary worker. */
        XrXirStatus qualified = XR_XIR_OK;
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; qualified == XR_XIR_OK && i < function->instruction_count; ++i) {
            if (!xir_compile_work(work, 1)) return XR_XIR_BUDGET;
            qualified = effect_go_seed(module, f, &function->instructions[i], work);
            if (qualified == XR_XIR_BUDGET || qualified == XR_XIR_OUT_OF_MEMORY) return qualified;
        }
        effects->go_safe[f] = qualified;
        if (qualified != XR_XIR_OK) graph->queue[back++] = f;
    }
    /* Every function changes from qualified at most once. Reverse execution
     * edges include direct calls, defaults, cleanup and recursive components. */
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
    return XR_XIR_OK;
}
