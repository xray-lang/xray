/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_lifetimes.inc.c - Staged lifetime consumption for admitted inputs
 */
typedef struct EffectLifetimePreparation {
    XrHirEscapeBuilder *builder;
    XrXirDiagnostic failure;
} EffectLifetimePreparation;

static XrXirStatus effect_lifetime_observe(void *context, const XrXirModule *module,
    uint32_t function, uint32_t instruction, const XrXirInstruction *op) {
    (void)module;
    EffectLifetimePreparation *prepare = context;
    return xr_hir_escape_builder_observe(prepare->builder, function, instruction, op);
}

static XrXirStatus effect_lifetime_points(void *context, const XirExecutionOwner *execution,
    XirStorageFlow *storage, bool final, bool *ready) {
    EffectLifetimePreparation *prepare = context;
    return xr_hir_escape_builder_prepare_points(prepare->builder, storage, execution, final, ready);
}

static XrXirStatus effect_lifetime_finish(void *context, const XirExecutionOwner *execution,
    XirStorageFlow *storage, const XirRestrictionReceiver *receiver) {
    EffectLifetimePreparation *prepare = context;
    XrHirEscapeFacts *facts = NULL;
    XrHirEscapeDiagnostic failure = {0};
    XrXirStatus status = xr_hir_escape_builder_finish(prepare->builder, storage, execution, &failure, &facts);
    if (status == XR_XIR_OK) {
        XirRestrictionView view = {0};
        status = xr_hir_escape_facts_view(facts, &view);
        if (status == XR_XIR_OK) status = receiver && receiver->accept ?
            receiver->accept(receiver->context, &view) : XR_XIR_BAD_STRUCTURE;
    }
    xr_hir_escape_facts_free(facts);
    xr_hir_escape_builder_free(prepare->builder); prepare->builder = NULL;
    if (status != XR_XIR_OK && failure.code) {
        prepare->failure.status = status;
        prepare->failure.function = failure.function;
        prepare->failure.instruction = failure.instruction;
    }
    return status;
}

XR_FUNC XrXirStatus xir_effects_infer_lifetimes_input(const XrXirCompileContext *context,
    XirEffectInput input, XrXirEffects **output, XrXirDiagnostic *diagnostic) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectLifetimePreparation prepare = {.failure =
        {XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE}};
    XrXirStatus status = xr_hir_escape_builder_new(context, input.module, NULL, input.control, &prepare.builder);
    if (status == XR_XIR_OK) {
        XrXirRootRefiner refiner = {.context = &prepare, .observe = effect_lifetime_observe,
            .points = effect_lifetime_points, .prepare = effect_lifetime_finish};
        status = effect_infer(context, input, &refiner, output, diagnostic);
    }
    xr_hir_escape_builder_free(prepare.builder);
    /* The general constructor reports its own location after the callback.
     * Preserve the sole lifetime producer's concrete rejection separately. */
    if (status != XR_XIR_OK && diagnostic && prepare.failure.status == status &&
        prepare.failure.function != UINT32_MAX) *diagnostic = prepare.failure;
    return status;
}
