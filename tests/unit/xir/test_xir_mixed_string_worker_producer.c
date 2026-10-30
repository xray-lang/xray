/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_string_worker_producer.c - Authentic owned string worker Source to portable C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "string worker producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

static uint32_t string_worker_entry(const XrXirModule *module) {
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 3 && !memcmp(module->functions[f].name, "run", 3)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX);
    return entry;
}
static void string_worker_emit(XrXirArtifact *lowered, uint32_t entry, const char *path) {
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "worker_string_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t worker_string_entry = %uu;\n", entry) > 0);
    CHECK(fclose(file) == 0);
    xr_xir_compile_c_source_free(&source);
}
static XrXirCheckedPacket string_worker_packet(const XrXirCompileContext *context) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_STRING_WORKER_FIXTURES};
    XrXirSourceRequest request = {session, XR_STRING_WORKER_FIXTURES "/root.xr", &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "string worker Source %u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owned, &packet, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(owned);
    return packet;
}
static int string_worker_producer_main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirCheckedPacket packet = string_worker_packet(&context);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    uint32_t entry = string_worker_entry(xr_xir_compile_artifact_module(lowered));
    string_worker_emit(lowered, entry, argv[1]); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}

int main(int argc, char **argv) { return string_worker_producer_main(argc, argv); }
