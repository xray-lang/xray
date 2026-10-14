/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_root_permissions.c - Source worker root exclusion with admitted controls
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "root permission %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
typedef struct RootPermissionCase { const char *name; bool admitted; } RootPermissionCase;
static const RootPermissionCase root_permission_cases[] = {
    {"pure-control", true}, {"local-worker-control", true}, {"snapshot-control", true},
    {"generic-pure-control", true}, {"alias-pure-control", true}, {"cross-control", true}, {"mutual-control", true},
    {"direct-read", false}, {"layered-read", false}, {"recursive-read", false}, {"generic-read", false},
    {"alias-read", false}, {"cross-read", false}, {"mutual-read", false}, {"nested-snapshot-read", false}, {"direct-write", false}
};
static void root_permission_packet(const XrXirCompileContext *context, XrXirArtifact *checked) {
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);
}
static void root_permission_check(const RootPermissionCase *oracle) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/%s.xr", XR_ROOT_PERMISSION_FIXTURES, oracle->name);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ROOT_PERMISSION_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}, empty = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    printf("Root permission case=%s status=%u checked=%u at=%d:%d message=%s\n",
        oracle->name, status, (unsigned)(result.checked != NULL), diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == (oracle->admitted ? XR_XIR_OK : XR_XIR_BAD_TYPE) && diagnostic.status == status);
    if (oracle->admitted) CHECK(result.checked && result.snapshot && !failure);
    else CHECK(!memcmp(&result, &empty, sizeof(result)) && failure &&
        !strcmp(diagnostic.message, "GO target requires the current instance root execution"));
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); xr_compile_resources_free(failure);
    if (checked) root_permission_packet(context, checked);
    effects_source_owners_free();
}
static const RootPermissionCase *root_permission_oracle(const char *name) {
    const RootPermissionCase *oracle = NULL;
    for (size_t i = 0; i < sizeof(root_permission_cases)/sizeof(*root_permission_cases); ++i)
        if (!strcmp(name, root_permission_cases[i].name)) { CHECK(!oracle); oracle = &root_permission_cases[i]; }
    CHECK(oracle); return oracle;
}
static int root_permission_main(int argc, char **argv) {
    CHECK(argc == 2); root_permission_check(root_permission_oracle(argv[1])); return 0;
}
int main(int argc, char **argv) { return root_permission_main(argc, argv); }
