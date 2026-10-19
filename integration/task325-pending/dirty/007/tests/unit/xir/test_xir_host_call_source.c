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
#include "xir_bounded_instance_cases.h"
static XrXirArtifact *host_source(void) {
    XrCompileResources *resources=NULL;
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_HOST_CALL_FIXTURES};
    XrXirSourceRequest request = {session, XR_HOST_CALL_FIXTURES "/root.xr", &authority,
        &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "host source %u at %d:%d: %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL) == XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(resources);
    return lowered;
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 2);
    XrXirArtifact *lowered = host_source();
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const char *names[] = {"normal", "failed", "cleanupAllocated", "cleanupFatal", "current", "spin", "reader"};
    uint32_t entries[7];
    for (unsigned n = 0; n < 7; ++n) {
        entries[n] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            if (module->functions[f].name_length == strlen(names[n]) &&
                !memcmp(module->functions[f].name, names[n], strlen(names[n]))) entries[n] = f;
        CHECK(entries[n] != UINT32_MAX);
    }
    bool fatal = argc == 2 && !strcmp(argv[1], "--fatal-drop");
    if (argc == 2 && !fatal) {
        XrXirCSource source = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "host_call_source", 4194304, &source) == XR_XIR_OK);
        CHECK(!strstr(source.text, "({"));
        FILE *file = fopen(argv[1], "wb"); CHECK(file);
        CHECK(fwrite(source.text, 1, source.length, file) == source.length);
        CHECK(fprintf(file, "\nconst uint32_t host_call_entries[3]={%uu,%uu,%uu};\n",
            entries[0], entries[1], entries[2]) > 0);
        CHECK(fprintf(file, "const uint32_t host_call_fatal_entry=%uu;\n", entries[3]) > 0);
        CHECK(fprintf(file, "const uint32_t host_call_bounded_entries[3]={%uu,%uu,%uu};\n",entries[4],entries[5],entries[6]) > 0);
        CHECK(fclose(file) == 0); xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    CHECK(!lowered);
    if (fatal) host_fatal_cleanup(program, entries[3]);
    bounded_instances(program,entries[0],entries[4],entries[5],entries[6]);
    host_cases(program, entries);
    return 0;
}
