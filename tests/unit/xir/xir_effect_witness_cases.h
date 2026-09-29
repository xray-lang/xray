/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_witness_cases.h - Independent suspension-cause path checks
 */
#ifndef XIR_EFFECT_WITNESS_CASES_H
#define XIR_EFFECT_WITNESS_CASES_H
static void effect_witness_paths(const XrXirModule *module, const XrXirEffects *effects) {
    CHECK(!xr_xir_effects_suspend_witness(NULL, 0));
    CHECK(!xr_xir_effects_suspend_witness(effects, module->function_count));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirEffect fact = xr_xir_effects_function(effects, f)->suspend;
        const XrXirEffectWitness *w = xr_xir_effects_suspend_witness(effects, f);
        if (fact == XR_XIR_EFFECT_NONE) { CHECK(!w); continue; }
        CHECK(w && w->distance < module->function_count);
        uint32_t at = f, steps = w->distance;
        for (;;) {
            CHECK(w->instruction < module->functions[at].instruction_count);
            const XrXirInstruction *op = &module->functions[at].instructions[w->instruction];
            CHECK(xr_xir_effects_function(effects, at)->suspend == fact);
            CHECK(w->distance == steps);
            if (!steps) {
                CHECK(w->callee == UINT32_MAX);
                if (fact == XR_XIR_EFFECT_MAY)
                    CHECK(w->cause == XR_XIR_EFFECT_CAUSE_SUSPEND && op->op == XR_XIR_SUSPEND);
                else CHECK(w->cause == XR_XIR_EFFECT_CAUSE_INDIRECT &&
                    (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT));
                break;
            }
            CHECK(w->cause == XR_XIR_EFFECT_CAUSE_CALL &&
                (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE));
            CHECK(w->callee == (uint32_t)op->immediate && w->callee < module->function_count);
            at = w->callee; --steps;
            w = xr_xir_effects_suspend_witness(effects, at); CHECK(w);
        }
    }
}
#endif // XIR_EFFECT_WITNESS_CASES_H
