/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_pure_permissions.c - One Catalog admits pure GO and rejects Root GO
 */
#define XR_LIBRARY_COUNTER_PRODUCER_CUSTOM_MAIN
#include "test_xir_mixed_library_producer.c"

static XrXirLibraryCatalog *library_pure_catalog(const XrXirCompileContext *context) {
    XrXirArtifact *library = counter_source(context, "library.xr", XR_XIR_LIBRARY, NULL);
    const XrXirModule *module = xr_xir_compile_artifact_module(library);
    CHECK(module->declarations->module_count == 1 && module->declarations->root_module == UINT32_MAX);
    CHECK(module->declarations->slot_count == 1 && module->declarations->slots[0].mutable);
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(library, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(library);
    XrXirLibraryModuleInput binding = {{XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT}, "library.xr"};
    XrXirLibraryInput input = {.packet=packet.bytes, .length=packet.length, .modules=&binding, .module_count=1};
    xr_sha256(input.packet, input.length, input.sha256); XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context, &input, 1, &catalog) == XR_XIR_OK);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    memset(&input, 0, sizeof(input)); memset(&binding, 0, sizeof(binding)); return catalog;
}
static void library_pure_reject_root(const XrXirCompileContext *context, const XrXirLibraryCatalog *catalog) {
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/reject-root.xr", XR_COUNTER_LOGICAL_ROOT);
    CHECK(length > 0 && (size_t)length < sizeof(path) && !fopen(path, "rb"));
    char *body = counter_text(context, "reject-root.xr"); size_t bytes = strlen(body);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, XR_XIR_PROGRAM, catalog};
    XrXirSourceText text = {"reject-root.xr", body, bytes}; XrXirSourceResult result = {0}, empty = {0};
    XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, &failure);
    printf("Catalog Root GO rejection status=%u diagnostic=%u checked=%u snapshot=%u at=%d:%d message=%s\n",
        status, diagnostic.status, (unsigned)(result.checked != NULL), (unsigned)(result.snapshot != NULL),
        diagnostic.line, diagnostic.column, diagnostic.message); fflush(stdout);
    CHECK(status == XR_XIR_BAD_TYPE && diagnostic.status == status && diagnostic.line == 2 && diagnostic.column > 0);
    CHECK(!memcmp(&result, &empty, sizeof(result)) && failure && !strcmp(diagnostic.message, "GO target requires the current instance root execution"));
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); memset(body, 0xCC, bytes); xr_compile_resources_free(body);
    const char *slash = strrchr(failure, '/'), *backslash = strrchr(failure, '\\');
    const char *base = slash && (!backslash || slash > backslash) ? slash+1 : backslash ? backslash+1 : failure;
    CHECK(!strcmp(base, "reject-root.xr")); xr_compile_resources_free(failure);
}
static void library_pure_ledger(const XrXirCompileContext *context, uint64_t live, XrCompileResourceStats *previous) {
    XrCompileResourceStats cost = {0}; CHECK(xr_compile_resources_stats(context->resources, &cost) == XR_COMPILE_RESOURCE_OK);
    CHECK(cost.live_bytes == live && cost.allocated_bytes > previous->allocated_bytes && cost.work > previous->work);
    CHECK(cost.peak_bytes >= previous->peak_bytes); *previous = cost;
    printf("Catalog permission ledger allocated=%llu peak=%llu work=%llu live=%llu, sameOwner cumulativeNoRefund catalogBaseline\n",
        (unsigned long long)cost.allocated_bytes, (unsigned long long)cost.peak_bytes,
        (unsigned long long)cost.work, (unsigned long long)cost.live_bytes);
}
int main(void) {
    (void)counter_producer_main;
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirLibraryCatalog *catalog = library_pure_catalog(context); XrCompileResourceStats cost = {0};
    CHECK(xr_compile_resources_stats(context->resources, &cost) == XR_COMPILE_RESOURCE_OK); uint64_t live = cost.live_bytes;
    for (unsigned round = 0; round < 3; ++round) {
        if (round == 1) library_pure_reject_root(context, catalog);
        else {
            XrXirArtifact *checked = counter_source(context, "root.xr", XR_XIR_PROGRAM, catalog);
            CHECK(checked && xr_xir_compile_artifact_module(checked)->declarations->module_count == 2);
            xr_xir_compile_artifact_free(checked); printf("Catalog pure GO admitted round=%u afterSourceSessionDeath\n", round);
        }
        library_pure_ledger(context, live, &cost);
    }
    xr_xir_compile_library_catalog_free(catalog); effects_source_owners_free();
    puts("Catalog permissions sameLibrary sameCatalog pureOK RootRejected pureOK failurePathOwned physical0/0"); return 0;
}
