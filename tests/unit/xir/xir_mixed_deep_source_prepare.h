/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_deep_source_prepare.h - Failure-safe Source and observed image preparation
 */
#ifndef XIR_MIXED_DEEP_SOURCE_PREPARE_H
#define XIR_MIXED_DEEP_SOURCE_PREPARE_H
typedef struct DeepSourcePrepared { DeepCompileResult compile; XrXirProgram *program; } DeepSourcePrepared;
typedef struct DeepSourcePreparation {
    DeepCompileState compile;
    DeepCode *owner;
    XrXirProgram *program;
} DeepSourcePreparation;

static bool deep_source_wrap(DeepSourcePreparation *preparation, unsigned backend) {
    DeepCompileState *state = &preparation->compile;
    const XrXirModule *module = xr_xir_compile_artifact_module(state->lowered);
    CHECK(module->function_count == worker_deep_fixture_program.entry_count && module->function_count <= WORKER_FUNCTIONS);
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(state->context.resources, 1,
        sizeof(*preparation->owner), (void **)&preparation->owner);
    if (!deep_compile_stage(state, 11, deep_compile_resource_status(allocated))) { CHECK(!preparation->owner); return false; }
    WorkerControlCode *code = &preparation->owner->base;
    allocated = xr_compile_resources_calloc(state->context.resources, module->function_count,
        sizeof(*code->entries), (void **)&code->entries);
    if (!deep_compile_stage(state, 12, deep_compile_resource_status(allocated))) { CHECK(!code->entries); return false; }
    code->functions = module->function_count; code->root = worker_deep_entry; worker_control_targets(code, module);
    XrXirProgramProof proof = xr_xir_compile_program_proof(state->lowered);
    if (!deep_compile_prepare(state)) return false;
    CHECK(state->root == worker_deep_entry && state->descend == worker_deep_descend);
    code->vm = state->vm; state->vm = NULL;
    for (uint32_t f = 0; f < code->functions; ++f) {
        code->native[f] = backend == 3 || (backend < 2 && (f == code->root) == (backend == 1));
        CHECK(state->native->entries[f].resume != code->vm->entries[f].resume);
        code->entries[f] = code->native[f] ? state->native->entries[f] : code->vm->entries[f];
        code->delegates[f] = code->entries[f].resume; code->entries[f].resume = deep_resume;
    }
    XrXirProgramSpec spec = worker_deep_fixture_program;
    spec.entries = code->entries; spec.types = state->native->types; spec.declarations = state->native->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){preparation->owner, deep_code_free};
    XrXirStatus status = xr_xir_compile_program_seal(&state->context, &spec, &preparation->program);
    if (!deep_compile_stage(state, 13, status)) {
        CHECK(!preparation->program && !deep_code && !worker_control_code && !worker_code_releases); return false;
    }
    deep_code = preparation->owner; worker_control_code = code; preparation->owner = NULL; return true;
}

static DeepSourcePrepared deep_source_prepare(unsigned backend, size_t failure, XrCompileResourceLimits caps) {
    CHECK(backend < 4 && !effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    CHECK(!deep_code && !worker_control_code && !worker_code_releases && !runtime_live);
    CHECK(caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    DeepSourcePreparation preparation = {0}; DeepCompileState *state = &preparation.compile;
    state->context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus created = xr_compile_resources_new(&caps, &state->context.resources);
    if (deep_compile_stage(state, 1, deep_compile_resource_status(created))) {
        if (deep_compile_source(state) && deep_compile_lower(state)) (void)deep_source_wrap(&preparation, backend);
    } else CHECK(!state->context.resources);
    xr_xir_compile_source_result_free(&state->source); xr_compile_session_free(state->session);
    xr_xir_compile_checked_packet_free(&state->packet); xr_xir_compile_artifact_free(state->checked);
    xr_xir_compile_artifact_free(state->closed); xr_xir_compile_artifact_free(state->lowered);
    xr_xir_compile_c_source_free(&state->c); xr_xir_compile_program_drop(state->native); xr_xir_compile_program_drop(state->vm);
    if (preparation.owner) {
        WorkerControlCode *unpublished = &preparation.owner->base;
        CHECK(!preparation.program && !deep_code && !worker_control_code && !worker_code_releases);
        xr_xir_compile_program_drop(unpublished->vm); xr_compile_resources_free(unpublished->entries);
        xr_compile_resources_free(preparation.owner);
    }
    if (state->context.resources) CHECK(xr_compile_resources_stats(state->context.resources, &state->result.stats) == XR_COMPILE_RESOURCE_OK);
    size_t before_release = effects_compile_attempts;
    xr_compile_resources_release(state->context.resources); state->context.resources = NULL;
    CHECK(effects_compile_attempts == before_release && (state->result.status == XR_XIR_OK) == (preparation.program != NULL));
    if (preparation.program) deep_compile_product(preparation.program, state->root, state->descend);
    else CHECK(!effects_compile_live && !effects_compile_bytes);
    state->result.sites = effects_compile_attempts; state->result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX;
    return (DeepSourcePrepared){state->result, preparation.program};
}
#endif // XIR_MIXED_DEEP_SOURCE_PREPARE_H
