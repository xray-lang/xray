/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_terminal_producer.c - Owned Source pipeline for terminal fixtures
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "terminal producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

static uint32_t terminal_entry(const XrXirModule *module, const char *name) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (module->declarations->functions[f].module != module->declarations->root_module) continue;
        if (module->functions[f].name_length != strlen(name) ||
            memcmp(module->functions[f].name, name, strlen(name))) continue;
        CHECK(found == UINT32_MAX); found = f;
    }
    CHECK(found != UINT32_MAX);
    return found;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TERMINAL_FIXTURES};
    XrXirSourceRequest request = {session,XR_TERMINAL_FIXTURES "/root.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "terminal Source %u at %d:%d: %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const char *names[] = {"returned","thrown","panicked","crossed","escaped"};
    uint32_t entries[5];
    for (unsigned i = 0; i < 5; ++i) entries[i] = terminal_entry(module, names[i]);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "terminal_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t terminal_fixture_functions[5] = {%uu,%uu,%uu,%uu,%uu};\n",
        entries[0], entries[1], entries[2], entries[3], entries[4]) > 0);
    CHECK(fclose(file) == 0);
    xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free();
    return 0;
}
