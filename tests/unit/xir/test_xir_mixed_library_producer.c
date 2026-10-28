/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_producer.c - Source Library state linked through an owned Catalog
 */
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "library producer %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"

static char *counter_text(const XrXirCompileContext *context, const char *name) {
    char path[1024]; int path_length = snprintf(path, sizeof(path), "%s/%s", XR_COUNTER_FIXTURES, name);
    CHECK(path_length > 0 && (size_t)path_length < sizeof(path));
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file); CHECK(length > 0 && length < 8192 && !fseek(file, 0, SEEK_SET));
    char *text = NULL;
    CHECK(xr_compile_resources_calloc(context->resources, (size_t)length + 1, 1, (void **)&text) == XR_COMPILE_RESOURCE_OK);
    CHECK(fread(text, 1, (size_t)length, file) == (size_t)length && !fclose(file));
    return text;
}
static XrXirArtifact *counter_source(const XrXirCompileContext *context, const char *name,
    XrXirLinkageKind kind, const XrXirLibraryCatalog *catalog) {
    char path[1024]; int path_length = snprintf(path, sizeof(path), "%s/%s", XR_COUNTER_LOGICAL_ROOT, name);
    CHECK(path_length > 0 && (size_t)path_length < sizeof(path) && !fopen(path, "rb"));
    char *body = counter_text(context, name);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT};
    XrXirSourceRequest request = {session, path, &authority, context, NULL, NULL, kind, catalog};
    XrXirSourceText text = {name, body, strlen(body)};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "counter Source kind=%u status=%u at=%d:%d: %s\n",
        kind, status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked = result.checked; result.checked = NULL;
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    memset(body, 0xCC, strlen(body)); xr_compile_resources_free(body);
    return checked;
}
static XrXirArtifact *counter_linked(const XrXirCompileContext *context) {
    XrXirArtifact *library = counter_source(context, "library.xr", XR_XIR_LIBRARY, NULL);
    const XrXirModule *module = xr_xir_compile_artifact_module(library);
    CHECK(module->declarations->module_count == 1 && module->declarations->root_module == UINT32_MAX &&
        module->declarations->slot_count == 1 && module->declarations->slots[0].mutable);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(library, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(library);
    XrXirLibraryModuleInput binding = {{XR_MODULE_IDENTITY_SCRIPT, NULL, XR_COUNTER_LOGICAL_ROOT}, "library.xr"};
    XrXirLibraryInput input = {0}; input.packet = packet.bytes; input.length = packet.length;
    input.modules = &binding; input.module_count = 1; xr_sha256(input.packet, input.length, input.sha256);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context, &input, 1, &catalog) == XR_XIR_OK);
    memset(packet.bytes, 0xA5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    memset(&input, 0, sizeof(input)); memset(&binding, 0, sizeof(binding));
    /* No entry Source files exist at the logical root; the import must use Catalog ownership. */
    XrXirArtifact *checked = counter_source(context, "root.xr", XR_XIR_PROGRAM, catalog);
    xr_xir_compile_library_catalog_free(catalog);
    module = xr_xir_compile_artifact_module(checked);
    CHECK(module->declarations->module_count == 2 && module->declarations->slot_count == 1 &&
        module->declarations->slots[0].module != module->declarations->root_module);
    return checked;
}
static int counter_producer_main(int argc, char **argv) {
    CHECK(argc == 2);
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = counter_linked(&context); XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered); uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->declarations->functions[f].module == module->declarations->root_module &&
            module->functions[f].name_length == 7 && !memcmp(module->functions[f].name, "advance", 7)) {
            CHECK(entry == UINT32_MAX); entry = f;
        }
    CHECK(entry != UINT32_MAX);
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered, "library_counter_fixture", 4194304, &source) == XR_XIR_OK);
    CHECK(source.text && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    FILE *file = fopen(argv[1], "wb"); CHECK(file && fwrite(source.text, 1, source.length, file) == source.length);
    CHECK(fprintf(file, "\nconst uint32_t library_counter_entry = %uu;\n", entry) > 0 && !fclose(file));
    xr_xir_compile_c_source_free(&source); xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); return 0;
}
#ifndef XR_LIBRARY_COUNTER_PRODUCER_CUSTOM_MAIN
int main(int argc, char **argv) { return counter_producer_main(argc, argv); }
#endif
