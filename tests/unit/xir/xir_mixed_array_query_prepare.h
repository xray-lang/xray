/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_array_query_prepare.h - Shared failure-aware Source query preparation
 */
#ifndef XIR_MIXED_ARRAY_QUERY_PREPARE_H
#define XIR_MIXED_ARRAY_QUERY_PREPARE_H
static const char *const array_query_bodies[] = {
    "export fn inspect()->i64 { return 42 }",
    "export fn inspect()->Array<i64> { return [3,1,2] }",
    "export fn inspect()->Array<i64> { var values:Array<i64>=[3,1,2]; return values }",
    "export fn inspect()->i64 { var values:Array<i64>=[3,1,2]; values.reverse(); return len(values) }",
    "export fn inspect()->Array<i64> { var values:Array<i64>=[3,1,2]; return values.reverse() }",
    "export fn inspect()->i64 { var values:Array<i64>=[3,1,2]; values.reserve(8); return len(values) }"
};
static const char *const array_query_members[] = {NULL, NULL, NULL, "reverse", "reverse", "reserve"};
typedef struct ArrayQueryCompileResult {
    XrXirStatus status;
    XrCompileResourceStats stats;
    size_t sites;
    unsigned case_index, stage, completed;
    bool injected;
} ArrayQueryCompileResult;
typedef struct ArrayQueryCompileState {
    XrXirCompileContext context;
    XrCompilerSession *session;
    char *body;
    size_t bytes;
    XrXirSourceResult source;
    XrXirCheckedPacket packet;
    XrXirArtifact *checked, *closed, *lowered;
    XrXirCSource c;
    ArrayQueryCompileResult result;
} ArrayQueryCompileState;
static XrCompileResourceLimits array_query_compile_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
}
static XrXirStatus array_query_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static bool array_query_stage(ArrayQueryCompileState *state, unsigned stage, XrXirStatus status) {
    state->result.stage = stage; state->result.status = status; return status == XR_XIR_OK;
}
static bool array_query_prepare_source(ArrayQueryCompileState *state) {
    XrCompilerSessionStatus created = xr_compile_session_new(state->context.resources, &state->session);
    CHECK(created == XR_COMPILER_SESSION_OK || created == XR_COMPILER_SESSION_BUDGET || created == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (!array_query_stage(state, 2, status)) { CHECK(!state->session); return false; }
    const char *body = array_query_bodies[state->result.case_index]; state->bytes = strlen(body);
    XrCompileResourceStatus allocation = xr_compile_resources_calloc(state->context.resources, state->bytes+1, 1, (void **)&state->body);
    if (!array_query_stage(state, 3, array_query_resource_status(allocation))) { CHECK(!state->body); return false; }
    memcpy(state->body, body, state->bytes);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "p20-array-query", NULL};
    XrXirSourceRequest request = {state->session, NULL, &authority, &state->context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceText source = {NULL, state->body, state->bytes}; XrXirSourceDiagnostic diagnostic = {0};
    status = xr_xir_compile_source_check_text(&request, &source, &state->source, &diagnostic, NULL);
    if (!array_query_stage(state, 4, status)) {
        XrXirSourceResult empty = {0}; CHECK(!memcmp(&state->source, &empty, sizeof(empty))); return false;
    }
    CHECK(state->source.checked && state->source.snapshot);
    xr_compile_session_free(state->session); state->session = NULL;
    memset(state->body, 0xCC, state->bytes); xr_compile_resources_free(state->body); state->body = NULL;
    array_query_schema(&state->source, array_query_members[state->result.case_index]); return true;
}
static bool array_query_prepare_checked(ArrayQueryCompileState *state) {
    XrXirStatus status = xr_xir_compile_checked_write(state->source.checked, &state->packet, NULL);
    if (!array_query_stage(state, 5, status)) { CHECK(!state->packet.bytes && !state->packet.length); return false; }
    xr_xir_compile_artifact_free(state->source.checked); state->source.checked = NULL;
    status = xr_xir_compile_checked_read(&state->context, state->packet.bytes, state->packet.length, &state->checked, NULL);
    memset(state->packet.bytes, 0xA5, state->packet.length); xr_xir_compile_checked_packet_free(&state->packet);
    xr_xir_compile_source_result_free(&state->source);
    if (!array_query_stage(state, 6, status)) { CHECK(!state->checked); return false; }
    array_query_closed(state->checked);
    return array_query_stage(state, 7, xr_xir_compile_artifact_verify(state->checked, NULL));
}
static bool array_query_prepare_c(ArrayQueryCompileState *state) {
    XrXirStatus status = xr_xir_compile_specialize(state->checked, &state->closed, NULL);
    if (!array_query_stage(state, 8, status)) { CHECK(!state->closed); return false; }
    xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(state->closed, &target, NULL, &state->lowered, NULL);
    if (!array_query_stage(state, 9, status)) { CHECK(!state->lowered); return false; }
    xr_xir_compile_artifact_free(state->closed); state->closed = NULL;
    array_query_closed(state->lowered);
    status = xr_xir_compile_emit_c(state->lowered, "array_query_fixture", 4194304, &state->c);
    if (!array_query_stage(state, 10, status)) { CHECK(!state->c.text && !state->c.length); return false; }
    CHECK(state->c.text && state->c.length && !strstr(state->c.text, "({") && !strstr(state->c.text, "xr_xir_vm")); return true;
}
static void array_query_prepare_clear(ArrayQueryCompileState *state) {
    xr_xir_compile_source_result_free(&state->source); xr_compile_session_free(state->session); state->session = NULL;
    if (state->body) { memset(state->body, 0xCC, state->bytes); xr_compile_resources_free(state->body); state->body = NULL; }
    xr_xir_compile_checked_packet_free(&state->packet); xr_xir_compile_artifact_free(state->checked); state->checked = NULL;
    xr_xir_compile_artifact_free(state->closed); state->closed = NULL;
    xr_xir_compile_c_source_free(&state->c); xr_xir_compile_artifact_free(state->lowered); state->lowered = NULL;
}
static ArrayQueryCompileResult array_query_compile_operation(size_t failure, XrCompileResourceLimits caps, bool verbose) {
    CHECK(!effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    CHECK(caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    ArrayQueryCompileState state = {0}; state.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus created = xr_compile_resources_new(&caps, &state.context.resources);
    if (array_query_stage(&state, 1, array_query_resource_status(created))) {
        XrCompileResourceStats previous = {0}; CHECK(xr_compile_resources_stats(state.context.resources, &previous) == XR_COMPILE_RESOURCE_OK);
        uint64_t baseline = previous.live_bytes;
        for (unsigned c = 0; c < 6; ++c) {
            state.result.case_index = c;
            bool complete = array_query_prepare_source(&state) && array_query_prepare_checked(&state) && array_query_prepare_c(&state);
            array_query_prepare_clear(&state);
            XrCompileResourceStats after = {0}; CHECK(xr_compile_resources_stats(state.context.resources, &after) == XR_COMPILE_RESOURCE_OK);
            CHECK(after.live_bytes == baseline && after.allocated_bytes >= previous.allocated_bytes && after.work >= previous.work);
            if (!complete) break;
            CHECK(after.allocated_bytes > previous.allocated_bytes && after.work > previous.work); previous = after;
            ++state.result.completed;
            if (verbose) printf("Array query member=%s SourceSessionTextQueryDead CheckedPacketRead Lowered CGen liveBaseline\n",
                array_query_members[c] ? array_query_members[c] : "control");
        }
        CHECK(xr_compile_resources_stats(state.context.resources, &state.result.stats) == XR_COMPILE_RESOURCE_OK);
    } else CHECK(!state.context.resources);
    size_t before = effects_compile_attempts; xr_compile_resources_release(state.context.resources);
    CHECK(effects_compile_attempts == before && !effects_compile_live && !effects_compile_bytes);
    state.result.sites = effects_compile_attempts; state.result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX; return state.result;
}
#endif // XIR_MIXED_ARRAY_QUERY_PREPARE_H
