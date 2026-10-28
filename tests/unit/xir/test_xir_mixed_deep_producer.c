/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_producer.c - Authentic recursive Source to portable resumable C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "deep producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static uint32_t deep_entry(const XrXirModule *module, const char *name) {
    uint32_t entry = UINT32_MAX; const size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name, name, length)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX); return entry;
}
static XrXirCheckedPacket deep_packet(const XrXirCompileContext *context) {
    char path[1024]; int length = snprintf(path, sizeof(path), "%s/root.xr", XR_DEEP_FIXTURES);
    CHECK(length > 0 && (size_t)length < sizeof(path)); XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_DEEP_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_DEEP_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "deep Source status=%u at%d:%d: %s\n", status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked); XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0}; CHECK(xr_xir_compile_checked_write(owned, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(owned); return packet;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirCheckedPacket packet = deep_packet(&context); XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t root = deep_entry(module, "run"), descend = deep_entry(module, "descend");
    CHECK(module->declarations->functions[root].module == module->declarations->root_module);
    CHECK(module->declarations->functions[descend].module != module->declarations->root_module);
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, "worker_deep_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file && fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t worker_deep_entry = %uu;\nconst uint32_t worker_deep_descend = %uu;\n", root, descend) > 0 && !fclose(file));
    xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered); effects_source_owners_free(); return 0;
}
