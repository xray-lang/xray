/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_host_call_source.c - Real Source host suspension and owned outcomes
 */
#include "execution/xr_xir_host_execution.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_vm.c"
#include "execution/xr_xir_host_execution.c"
#include "xir_host_call_cases.h"
static XrXirArtifact *host_source(void) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_HOST_CALL_FIXTURES};
    XrXirSourceRequest request = {session, XR_HOST_CALL_FIXTURES "/root.xr", &authority,
        NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request, &result, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "host source %u at %d:%d: %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked, NULL, &packet, NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_checked_read(packet.bytes, packet.length, NULL, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_checked_packet_free(&packet);
    CHECK(xr_xir_specialize(checked, NULL, &closed, NULL) == XR_XIR_OK); xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_artifact_free(closed);
    return lowered;
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    XrXirArtifact *lowered = host_source();
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    const char *names[] = {"normal", "failed", "cleanupAllocated", "cleanupFatal"};
    uint32_t entries[4];
    for (unsigned n = 0; n < 4; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) entries[n] = f;
        CHECK(entries[n] != UINT32_MAX);
    }
    bool fatal = argc == 2 && !strcmp(argv[1], "--fatal-drop");
    if (argc == 2 && !fatal) {
        XrXirCSource source = {0};
        CHECK(xr_xir_emit_c(lowered, "host_call_source", 4194304, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({"));
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t host_call_entries[3]={%uu,%uu,%uu};\n",
            entries[0], entries[1], entries[2]) > 0);
        CHECK(fprintf(file, "const uint32_t host_call_fatal_entry=%uu;\n", entries[3]) > 0);
        CHECK(fclose(file) == 0); xr_xir_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered, (XrXirProgramBudget){16777216, 64000000}, &program) == XR_XIR_OK);
    CHECK(!lowered);
    if (fatal) host_fatal_cleanup(program, entries[3]);
    host_cases(program, entries);
    return 0;
}
