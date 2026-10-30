/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_class_producer.c - Owned Source class metadata for real C and VM
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "class producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CLASS_LIFETIME_FIXTURES};
    XrXirSourceRequest request = {session, XR_CLASS_LIFETIME_FIXTURES "/root.xr", &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "class Source %u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = UINT32_MAX, array_entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 8 && !memcmp(module->functions[f].name, "retained", 8)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 14 && !memcmp(module->functions[f].name, "retained_array", 14)) {
            CHECK(array_entry == UINT32_MAX); array_entry = f;
        }
    CHECK(entry != UINT32_MAX && array_entry != UINT32_MAX);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "class_lifetime_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t class_lifetime_entry = %uu;\n", entry) > 0);
    CHECK(fprintf(file, "const uint32_t class_lifetime_array_entry = %uu;\n", array_entry) > 0);
    CHECK(fclose(file) == 0);
    xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}
