/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_worker_producer.c - Real Source worker controls for portable C and VM
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "worker producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

static unsigned worker_producer_index(const char *text) {
    CHECK(strlen(text) == 1 && text[0] >= '0' && text[0] <= '6'); return (unsigned)(text[0] - '0');
}
static uint32_t worker_producer_entry(const XrXirModule *module) {
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 3 && !memcmp(module->functions[f].name, "run", 3)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX);
    return entry;
}
static void worker_producer_emit(XrXirArtifact *lowered, const char *prefix, unsigned index, const char *path, uint32_t entry) {
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, prefix, 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t worker_control_%u_entry = %uu;\n", index, entry) > 0);
    CHECK(fclose(file) == 0);
    xr_xir_compile_c_source_free(&source);
}
static int worker_producer_main(int argc, char **argv) {
    CHECK(argc == 3); unsigned index = worker_producer_index(argv[1]);
    const char *names[] = {"pure-control", "local-worker-control", "snapshot-control", "generic-pure-control",
        "alias-pure-control", "cross-control", "mutual-control"};
    char path[1024], prefix[64];
    int length = snprintf(path, sizeof(path), "%s/%s.xr", XR_WORKER_CONTROL_FIXTURES, names[index]);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    CHECK(snprintf(prefix, sizeof(prefix), "worker_control_%u", index) > 0);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_WORKER_CONTROL_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "worker Source %u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owned, &packet, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(owned);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = worker_producer_entry(module);
    worker_producer_emit(lowered, prefix, index, argv[2], entry); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}

int main(int argc, char **argv) { return worker_producer_main(argc, argv); }
