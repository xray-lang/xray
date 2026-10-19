/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_library_permissions.h - Failure-aware Source permission sequence on one Catalog
 */
#ifndef XIR_MIXED_LIBRARY_PERMISSIONS_H
#define XIR_MIXED_LIBRARY_PERMISSIONS_H
typedef struct LibraryPermissionResult {
    XrXirStatus status;
    XrCompileResourceStats stats, rounds[3];
    size_t sites;
    unsigned phase, stage, completed;
    bool injected, root_rejected;
} LibraryPermissionResult;
typedef struct LibraryPermissionState {
    XrXirCompileContext context;
    XrXirLibraryCatalog *catalog;
    LibraryPermissionResult result;
} LibraryPermissionState;
static XrCompileResourceLimits library_permission_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
}
static XrXirStatus library_permission_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static bool library_permission_stage(LibraryPermissionState *state, unsigned stage, XrXirStatus status) {
    state->result.stage = stage; state->result.status = status; return status == XR_XIR_OK;
}
static char *library_permission_text(LibraryPermissionState *state, const char *name) {
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/%s", XR_COUNTER_FIXTURES, name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long bytes = ftell(file); CHECK(bytes > 0 && bytes < 8192 && !fseek(file, 0, SEEK_SET));
    char *text = NULL;
    XrCompileResourceStatus status = xr_compile_resources_calloc(state->context.resources, (size_t)bytes+1, 1, (void **)&text);
    if (!library_permission_stage(state, 2, library_permission_resource_status(status))) {
        CHECK(!text && !fclose(file)); return NULL;
    }
    CHECK(fread(text, 1, (size_t)bytes, file) == (size_t)bytes && !fclose(file)); return text;
}
static bool library_permission_source(LibraryPermissionState *state, const char *name, XrXirArtifact **output) {
    CHECK(!*output);
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/%s", XR_COUNTER_LOGICAL_ROOT, name);
    CHECK(length > 0 && (size_t)length < sizeof(path) && !fopen(path, "rb"));
    char *body = library_permission_text(state, name); if (!body) return false;
    size_t bytes = strlen(body); XrCompilerSession *session = NULL;
    XrCompilerSessionStatus created = xr_compile_session_new(state->context.resources, &session);
    CHECK(created == XR_COMPILER_SESSION_OK || created == XR_COMPILER_SESSION_BUDGET || created == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    if (!library_permission_stage(state, 3, status)) {
        CHECK(!session); memset(body, 0xCC, bytes); xr_compile_resources_free(body); return false;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT};
    XrXirSourceRequest request = {session, path, &authority, &state->context, NULL, NULL,
        state->result.phase ? XR_XIR_PROGRAM : XR_XIR_LIBRARY, state->catalog};
    XrXirSourceText text = {name, body, bytes}; XrXirSourceResult result = {0}, empty = {0};
    XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, &failure);
    library_permission_stage(state, 4, status);
    if (status == XR_XIR_OK) {
        CHECK(state->result.phase != 3 && result.checked && result.snapshot && !failure);
        const XrXirModule *module = xr_xir_compile_artifact_module(result.checked);
        CHECK(module->declarations->module_count == (state->result.phase ? 2u : 1u));
        CHECK(module->declarations->slot_count == 1 && module->declarations->slots[0].mutable);
        CHECK(state->result.phase || module->declarations->root_module == UINT32_MAX);
        *output = result.checked; result.checked = NULL;
    } else {
        CHECK(!memcmp(&result, &empty, sizeof(result)));
        if (state->result.phase == 3 && status == XR_XIR_BAD_TYPE) {
            CHECK(diagnostic.status == status && diagnostic.line == 2 && diagnostic.column == 11 && failure);
            CHECK(!strcmp(diagnostic.message, "GO target requires the current instance root execution"));
            state->result.root_rejected = true;
        }
    }
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    memset(body, 0xCC, bytes); xr_compile_resources_free(body);
    if (failure) {
        const char *slash = strrchr(failure, '/'), *backslash = strrchr(failure, '\\');
        const char *base = slash && (!backslash || slash > backslash) ? slash+1 : backslash ? backslash+1 : failure;
        CHECK(!strcmp(base, name)); xr_compile_resources_free(failure);
    }
    return status == XR_XIR_OK || (state->result.phase == 3 && status == XR_XIR_BAD_TYPE);
}
static bool library_permission_catalog(LibraryPermissionState *state) {
    XrXirArtifact *library = NULL; if (!library_permission_source(state, "library.xr", &library)) return false;
    XrXirCheckedPacket packet = {0}; XrXirStatus status = xr_xir_compile_checked_write(library, &packet, NULL);
    xr_xir_compile_artifact_free(library);
    if (!library_permission_stage(state, 5, status)) { CHECK(!packet.bytes && !packet.length); return false; }
    XrXirLibraryModuleInput binding = {{XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT}, "library.xr"};
    XrXirLibraryInput input = {.packet=packet.bytes, .length=packet.length, .modules=&binding, .module_count=1};
    xr_sha256(input.packet, input.length, input.sha256); state->result.phase = 1;
    status = xr_xir_compile_library_catalog_new_v2(&state->context, &input, 1, &state->catalog);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    memset(&input, 0, sizeof(input)); memset(&binding, 0, sizeof(binding));
    if (!library_permission_stage(state, 6, status)) { CHECK(!state->catalog); return false; }
    return true;
}
static void library_permission_sequence(LibraryPermissionState *state) {
    if (!library_permission_catalog(state)) return;
    XrCompileResourceStats previous = {0};
    CHECK(xr_compile_resources_stats(state->context.resources, &previous) == XR_COMPILE_RESOURCE_OK);
    uint64_t live = previous.live_bytes;
    for (unsigned round = 0; round < 3; ++round) {
        state->result.phase = round+2; XrXirArtifact *checked = NULL;
        bool success = library_permission_source(state, round == 1 ? "reject-root.xr" : "root.xr", &checked);
        if (!success) { CHECK(!checked); return; }
        CHECK(round == 1 ? !checked && state->result.root_rejected : checked != NULL);
        xr_xir_compile_artifact_free(checked);
        XrCompileResourceStats cost = {0}; CHECK(xr_compile_resources_stats(state->context.resources, &cost) == XR_COMPILE_RESOURCE_OK);
        CHECK(cost.live_bytes == live && cost.allocated_bytes > previous.allocated_bytes && cost.work > previous.work);
        CHECK(cost.peak_bytes >= previous.peak_bytes); previous = cost;
        state->result.rounds[round] = cost; ++state->result.completed;
    }
    CHECK(state->result.root_rejected && state->result.completed == 3);
    state->result.status = XR_XIR_OK;
}
static LibraryPermissionResult library_permission_operation(size_t failure, XrCompileResourceLimits caps) {
    CHECK(!effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    CHECK(caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    LibraryPermissionState state = {0}; state.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus status = xr_compile_resources_new(&caps, &state.context.resources);
    if (library_permission_stage(&state, 1, library_permission_resource_status(status))) {
        library_permission_sequence(&state); xr_xir_compile_library_catalog_free(state.catalog);
        CHECK(xr_compile_resources_stats(state.context.resources, &state.result.stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(state.result.stats.live_bytes == sizeof(XrCompileResources));
    } else CHECK(!state.context.resources);
    size_t before_release = effects_compile_attempts; xr_compile_resources_release(state.context.resources);
    CHECK(effects_compile_attempts == before_release);
    state.result.sites = effects_compile_attempts; state.result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX; CHECK(!effects_compile_live && !effects_compile_bytes); return state.result;
}
#endif // XIR_MIXED_LIBRARY_PERMISSIONS_H
