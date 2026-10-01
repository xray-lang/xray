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
static bool effect_witness_call_matches(const XrXirModule *module, uint32_t caller,
    const XrXirEffectWitness *w) {
    if (caller >= module->function_count || w->cause != XR_XIR_EFFECT_CAUSE_CALL ||
        w->instruction >= module->functions[caller].instruction_count || w->callee >= module->function_count)
        return false;
    const XrXirInstruction *op = &module->functions[caller].instructions[w->instruction];
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE)
        return op->immediate >= 0 && (uint64_t)op->immediate == w->callee;
    if ((op->op != XR_XIR_CALL_DEFAULT && op->op != XR_XIR_INVOKE_DEFAULT) || !module->defaults) return false;
    uint32_t owner=op->op==XR_XIR_CALL_DEFAULT ? op->targets[0] : op->args[0];
    uint32_t ordinal=op->op==XR_XIR_CALL_DEFAULT ? op->targets[1] : op->args[1];
    uint32_t expected = UINT32_MAX;
    for (uint32_t d = 0; d < module->defaults->count; ++d) {
        const XrXirDefaultBinding *binding = &module->defaults->records[d];
        if (binding->owner == owner && binding->ordinal == ordinal) {
            if (expected != UINT32_MAX || binding->owner_kind != XR_XIR_DEFAULT_PARAMETER) return false;
            expected = binding->function;
        }
    }
    return expected == w->callee;
}
static void effect_default_witness_poison(const XrXirModule *module, uint32_t caller,
    const XrXirEffectWitness *w) {
    const XrXirInstruction *op = &module->functions[caller].instructions[w->instruction];
    if (op->op != XR_XIR_CALL_DEFAULT && op->op != XR_XIR_INVOKE_DEFAULT) return;
    uint32_t owner=op->op==XR_XIR_CALL_DEFAULT ? op->targets[0] : op->args[0];
    uint32_t ordinal=op->op==XR_XIR_CALL_DEFAULT ? op->targets[1] : op->args[1];
    XrXirEffectWitness changed = *w;
    changed.instruction = module->functions[caller].instruction_count;
    CHECK(!effect_witness_call_matches(module, caller, &changed));
    changed = *w; changed.callee = module->function_count;
    CHECK(!effect_witness_call_matches(module, caller, &changed));
    XrXirDefaultBinding binding = {XR_XIR_DEFAULT_PARAMETER,owner,ordinal,w->callee};
    XrXirDefaultTable table = {&binding,1}; XrXirModule poisoned = *module; poisoned.defaults = &table;
    CHECK(effect_witness_call_matches(&poisoned, caller, w));
    binding.function = (w->callee + 1) % module->function_count;
    CHECK(!effect_witness_call_matches(&poisoned, caller, w));
    binding.function = w->callee; binding.ordinal ^= 1u;
    CHECK(!effect_witness_call_matches(&poisoned, caller, w));
    binding.ordinal = ordinal; binding.owner ^= 1u;
    CHECK(!effect_witness_call_matches(&poisoned, caller, w));
}
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
            CHECK(effect_witness_call_matches(module, at, w));
            effect_default_witness_poison(module, at, w);
            at = w->callee; --steps;
            w = xr_xir_effects_suspend_witness(effects, at); CHECK(w);
        }
    }
}
#endif // XIR_EFFECT_WITNESS_CASES_H
