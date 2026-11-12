/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_summary.inc.c - Owned projections of numeric semantic control facts
 *
 * KEY CONCEPT:
 *   The shared numeric owner produces every control fact and cause.
 *   Retained projections own their arrays independently of all input storage.
 */
struct EffectControlSummary {
    XrCompileResources *resources;
    uint32_t functions;
    XrXirFunctionEffects *facts;
    XrXirEffectWitness *causes;
    XrXirEffect *task_creation;
};


XR_FUNC void xir_effect_control_free(EffectControlSummary *summary) {
    if (!summary) return;
    xr_compile_resources_free(summary->facts);
    xr_compile_resources_free(summary->causes);
    xr_compile_resources_free(summary->task_creation);
    xr_compile_resources_free(summary);
}
