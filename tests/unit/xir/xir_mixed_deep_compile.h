/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_deep_compile.h - One finite owner spans recursive Source and both images
 */
#ifndef XIR_MIXED_DEEP_COMPILE_H
#define XIR_MIXED_DEEP_COMPILE_H
typedef struct DeepCompileResult {
    XrXirStatus status;
    XrCompileResourceStats stats;
    size_t sites, c_bytes;
    unsigned stage;
    bool injected;
} DeepCompileResult;
typedef struct DeepCompileState {
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrXirSourceResult source;
    XrXirArtifact *checked, *closed, *lowered;
    XrXirCheckedPacket packet;
    XrXirCSource c;
    XrXirProgram *native, *vm;
    DeepCompileResult result;
    uint32_t root, descend;
} DeepCompileState;
static XrCompileResourceLimits deep_compile_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
}
static XrXirStatus deep_compile_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static bool deep_compile_stage(DeepCompileState *state, unsigned stage, XrXirStatus status) {
    state->result.stage = stage; state->result.status = status; return status == XR_XIR_OK;
}
static bool deep_compile_source(DeepCompileState *state) {
    XrCompilerSessionStatus created = xr_compile_session_new(state->context.resources, &state->session);
    CHECK(created == XR_COMPILER_SESSION_OK || created == XR_COMPILER_SESSION_BUDGET || created == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (!deep_compile_stage(state, 2, status)) { CHECK(!state->session); return false; }
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/root.xr", XR_DEEP_FIXTURES);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_DEEP_FIXTURES};
    XrXirSourceRequest request = {state->session, path, &authority, &state->context, XR_DEEP_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceDiagnostic diagnostic = {0};
    status = xr_xir_compile_source_check(&request, &state->source, &diagnostic, NULL);
    if (!deep_compile_stage(state, 3, status)) { CHECK(!state->source.checked && !state->source.snapshot); return false; }
    CHECK(state->source.checked); state->checked = state->source.checked; state->source.checked = NULL;
    xr_xir_compile_source_result_free(&state->source); xr_compile_session_free(state->session); state->session = NULL;
    return true;
}
static bool deep_compile_lower(DeepCompileState *state) {
    XrXirStatus status = xr_xir_compile_checked_write(state->checked, &state->packet, NULL);
    if (!deep_compile_stage(state, 4, status)) { CHECK(!state->packet.bytes && !state->packet.length); return false; }
    xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    status = xr_xir_compile_checked_read(&state->context, state->packet.bytes, state->packet.length, &state->checked, NULL);
    memset(state->packet.bytes, 0xCC, state->packet.length); xr_xir_compile_checked_packet_free(&state->packet);
    if (!deep_compile_stage(state, 5, status)) { CHECK(!state->checked); return false; }
    status = xr_xir_compile_specialize(state->checked, &state->closed, NULL);
    if (!deep_compile_stage(state, 6, status)) { CHECK(!state->closed); return false; }
    xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    status = xr_xir_compile_lower(state->closed, &worker_deep_fixture_program.target, NULL, &state->lowered, NULL);
    if (!deep_compile_stage(state, 7, status)) { CHECK(!state->lowered); return false; }
    xr_xir_compile_artifact_free(state->closed); state->closed = NULL; return true;
}
static uint32_t deep_compile_entry(const XrXirModule *module, const char *name) {
    uint32_t entry = UINT32_MAX; size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name, name, length)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX); return entry;
}
static bool deep_compile_prepare(DeepCompileState *state) {
    const XrXirModule *module = xr_xir_compile_artifact_module(state->lowered);
    state->root = deep_compile_entry(module, "run"); state->descend = deep_compile_entry(module, "descend");
    CHECK(module->declarations->functions[state->root].module == module->declarations->root_module);
    CHECK(module->declarations->functions[state->descend].module != module->declarations->root_module);
    XrXirStatus status = xr_xir_compile_emit_c(state->lowered, "worker_deep_fixture", 4194304, &state->c);
    if (!deep_compile_stage(state, 8, status)) { CHECK(!state->c.text && !state->c.length); return false; }
    CHECK(state->c.text && state->c.length && !strstr(state->c.text, "({") && !strstr(state->c.text, "xr_xir_vm"));
    state->result.c_bytes = state->c.length;
    XrXirProgramSpec spec = worker_deep_fixture_program;
    spec.types = module->types; spec.declarations = module->declarations; spec.proof = xr_xir_compile_program_proof(state->lowered);
    CHECK(!spec.code.owner && !spec.code.release);
    status = xr_xir_compile_program_seal(&state->context, &spec, &state->native);
    if (!deep_compile_stage(state, 9, status)) { CHECK(!state->native); return false; }
    XrXirArtifact *original = state->lowered;
    status = xr_xir_compile_vm_program_take(&state->lowered, &state->vm);
    if (!deep_compile_stage(state, 10, status)) { CHECK(!state->vm && state->lowered == original); return false; }
    CHECK(!state->lowered && state->native && state->vm); return true;
}
static void deep_compile_product(const XrXirProgram *program, uint32_t root, uint32_t descend) {
    CHECK(program && root < program->entry_count && descend < program->entry_count);
    CHECK(program->entries[root].parameter_count == 1 && program->entries[root].parameters[0] == XR_XIR_I64);
    CHECK(program->entries[root].result == XR_XIR_I64 && program->entries[descend].parameter_count == 1);
    CHECK(program->declarations->functions[root].module == program->declarations->root_module);
    CHECK(program->declarations->functions[descend].module != program->declarations->root_module);
}
static DeepCompileResult deep_compile_operation(unsigned keeper, size_t failure, XrCompileResourceLimits caps, const char *dump) {
    CHECK(keeper < 2 && !effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    CHECK(caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    DeepCompileState state = {0}; state.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus created = xr_compile_resources_new(&caps, &state.context.resources);
    if (deep_compile_stage(&state, 1, deep_compile_resource_status(created))) {
        if (deep_compile_source(&state) && deep_compile_lower(&state) && deep_compile_prepare(&state)) {
            deep_compile_product(state.vm, state.root, state.descend); deep_compile_product(state.native, state.root, state.descend);
            if (dump) {
                FILE *file = fopen(dump, "wb"); CHECK(file && fwrite(state.c.text, 1, state.c.length, file) == state.c.length);
                CHECK(fprintf(file, "\nconst uint32_t worker_deep_entry = %uu;\nconst uint32_t worker_deep_descend = %uu;\n", state.root, state.descend) > 0 && !fclose(file));
            }
        }
    } else CHECK(!state.context.resources);
    xr_xir_compile_source_result_free(&state.source); xr_compile_session_free(state.session);
    xr_xir_compile_checked_packet_free(&state.packet); xr_xir_compile_artifact_free(state.checked);
    xr_xir_compile_artifact_free(state.closed); xr_xir_compile_artifact_free(state.lowered); xr_xir_compile_c_source_free(&state.c);
    XrXirProgram *last = NULL;
    if (state.result.status == XR_XIR_OK) {
        last = keeper ? state.vm : state.native;
        if (keeper) state.vm = NULL; else state.native = NULL;
    }
    xr_xir_compile_program_drop(state.vm); xr_xir_compile_program_drop(state.native);
    if (state.context.resources) CHECK(xr_compile_resources_stats(state.context.resources, &state.result.stats) == XR_COMPILE_RESOURCE_OK);
    const size_t before_release = effects_compile_attempts;
    xr_compile_resources_release(state.context.resources); state.context.resources = NULL;
    if (last) { deep_compile_product(last, state.root, state.descend); xr_xir_compile_program_drop(last); }
    CHECK(effects_compile_attempts == before_release);
    state.result.sites = effects_compile_attempts; state.result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes); return state.result;
}
#endif // XIR_MIXED_DEEP_COMPILE_H
