/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_wait_producer.c - Authentic yielding and timed child strings to portable C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "worker wait producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static uint32_t worker_wait_entry(const XrXirModule *module) {
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 3 && !memcmp(module->functions[f].name, "run", 3)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX); return entry;
}
static XrXirCheckedPacket worker_wait_packet(const XrXirCompileContext *context, unsigned index) {
    const char *names[] = {"yield", "timer"}; char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s/root.xr", XR_WORKER_WAIT_FIXTURES, names[index]);
    CHECK(length > 0 && (size_t)length < sizeof(path)); XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_WORKER_WAIT_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_WORKER_WAIT_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "worker wait Source case=%s status=%u at%d:%d: %s\n",
        names[index], status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked); XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owned, &packet, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(owned);
    return packet;
}
static void worker_wait_emit(XrXirArtifact *lowered, unsigned index, const char *path) {
    char prefix[48]; CHECK(snprintf(prefix, sizeof(prefix), "worker_wait_%u", index) > 0);
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, prefix, 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    uint32_t entry = worker_wait_entry(xr_xir_compile_artifact_module(lowered));
    FILE *file = fopen(path, "wb"); CHECK(file && fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t worker_wait_%u_entry = %uu;\n", index, entry) > 0 && !fclose(file));
    xr_xir_compile_c_source_free(&source);
}
static int worker_wait_producer_main(int argc, char **argv) {
    CHECK(argc == 3 && strlen(argv[1]) == 1 && (argv[1][0] == '0' || argv[1][0] == '1'));
    unsigned index = (unsigned)(argv[1][0] - '0');
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirCheckedPacket packet = worker_wait_packet(&context, index); XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    worker_wait_emit(lowered, index, argv[2]); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}
int main(int argc, char **argv) { return worker_wait_producer_main(argc, argv); }
