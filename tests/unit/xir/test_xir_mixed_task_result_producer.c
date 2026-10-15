/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_result_producer.c - Real two-module compound Task code
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task result producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

static XrXirArtifact *task_result_source(const XrXirCompileContext *context) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_MIXED_TASK_RESULT_FIXTURES};
    XrXirSourceRequest request = {session, XR_MIXED_TASK_RESULT_FIXTURES "/root.xr", &authority,
        context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "compound Task Source status=%u at=%d:%d %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *decoded = NULL;
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    return decoded;
}
static void task_result_entries(const XrXirModule *module, uint32_t entries[3]) {
    static const char *const names[] = {"run", "observed", "words"};
    CHECK(module->declarations->module_count == 2 && module->declarations->slot_count == 1);
    CHECK(module->declarations->slots[0].module == module->declarations->root_module);
    for (unsigned n = 0; n < 3; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f) {
            const XrXirFunction *function = &module->functions[f];
            if (function->name_length != strlen(names[n]) || memcmp(function->name, names[n], function->name_length)) continue;
            CHECK(entries[n] == UINT32_MAX && !function->parameter_count);
            CHECK((module->declarations->functions[f].module == module->declarations->root_module) == (n != 2));
            entries[n] = f;
        }
        CHECK(entries[n] != UINT32_MAX);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = task_result_source(context), *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    uint32_t entries[3]; task_result_entries(xr_xir_compile_artifact_module(lowered), entries);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "p20_task_result_fixture", 4194304, &source) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file && fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t p20_task_result_entries[3] = {%uu,%uu,%uu};\n", entries[0], entries[1], entries[2]) > 0);
    CHECK(!fclose(file));
    printf("P20 compound Task real Source modules=2 C11bytes=%zu SourceDead independentPacketDead VMexecuted=0\n", source.length);
    xr_xir_compile_c_source_free(&source); effects_source_owners_free(); return 0;
}
