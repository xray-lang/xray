/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_diamond_producer.c - Authentic diamond import initialization to portable C
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "diamond producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
static uint32_t diamond_named(const XrXirModule *module, const char *name) {
    uint32_t found = UINT32_MAX; size_t length = strlen(name);
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == length && !memcmp(module->functions[f].name, name, length)) {
            CHECK(found == UINT32_MAX); found = f;
        }
    CHECK(found != UINT32_MAX); return found;
}
static void diamond_graph(const XrXirModule *module, uint32_t *initializers) {
    const XrXirDeclarations *declarations = module->declarations;
    CHECK(declarations->module_count == 4 && declarations->slot_count == 6);
    const char *names[] = {"mark", "left", "right", "run"}; uint32_t ids[4];
    for (unsigned i = 0; i < 4; ++i) {
        ids[i] = declarations->functions[diamond_named(module, names[i])].module;
        CHECK(ids[i] < 4); initializers[i] = declarations->modules[ids[i]].initializer;
        for (unsigned prior = 0; prior < i; ++prior) CHECK(ids[i] != ids[prior]);
    }
    CHECK(ids[3] == declarations->root_module);
    for (unsigned i = 0; i < 4; ++i) {
        const XrXirSourceModule *source = &declarations->modules[ids[i]]; uint32_t mask = 0;
        CHECK(source->dependency_count == (i == 3 ? 3u : i ? 1u : 0u));
        for (uint32_t d = 0; d < source->dependency_count; ++d) {
            CHECK(source->dependencies[d] < 4 && !(mask & (1u << source->dependencies[d])));
            mask |= 1u << source->dependencies[d];
        }
        CHECK(mask == (i == 3 ? (1u << ids[0]) | (1u << ids[1]) | (1u << ids[2]) : i ? 1u << ids[0] : 0u));
    }
}
static XrXirCheckedPacket diamond_packet(const XrXirCompileContext *context) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_DIAMOND_FIXTURES};
    XrXirSourceRequest request = {session, XR_DIAMOND_FIXTURES "/root.xr", &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "diamond Source %u at %d:%d: %s\n", status,
        diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owned, &packet, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(owned);
    return packet;
}
static void diamond_emit(XrXirArtifact *lowered, const char *path) {
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered); uint32_t initializers[4];
    diamond_graph(module, initializers); uint32_t entry = diamond_named(module, "run");
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, "diamond_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(path, "wb"); CHECK(file && fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t diamond_entry = %uu;\nconst uint32_t diamond_initializers[4] = {%uu,%uu,%uu,%uu};\n",
        entry, initializers[0], initializers[1], initializers[2], initializers[3]) > 0 && !fclose(file));
    xr_xir_compile_c_source_free(&source);
}
static int diamond_producer_main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirCheckedPacket packet = diamond_packet(&context);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    diamond_emit(lowered, argv[1]); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}
int main(int argc, char **argv) { return diamond_producer_main(argc, argv); }
