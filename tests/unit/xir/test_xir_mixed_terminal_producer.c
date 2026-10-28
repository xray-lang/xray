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

static char *terminal_owned_text(const XrXirCompileContext *context, const char *text) {
    size_t length = strlen(text);
    char *copy = NULL;
    CHECK(xr_compile_resources_work(context->resources, length + 1) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(context->resources, length + 1, (void **)&copy) == XR_COMPILE_RESOURCE_OK);
    memcpy(copy, text, length + 1); return copy;
}
static void terminal_release_text(char *text) {
    CHECK(text); memset(text, 0xCC, strlen(text) + 1); xr_compile_resources_free(text);
}
static void terminal_owned_identity(const XrXirCompileContext *context) {
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    char *name = terminal_owned_text(context, "terminal-normal");
    char *root = terminal_owned_text(context, XR_TERMINAL_FIXTURES);
    char *path = terminal_owned_text(context, XR_TERMINAL_FIXTURES "/root.xr");
    char *logical_input = terminal_owned_text(context, "root.xr");
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_PROJECT, name, root};
    char *identity = NULL, *logical = NULL, *independent = NULL;
    CHECK(xr_compile_module_identity_from_source(context->resources, &authority, path, &identity, &logical) == XR_MODULE_OK);
    CHECK(xr_compile_module_identity_from_logical(context->resources, &authority, logical_input, &independent) == XR_MODULE_OK);
    terminal_release_text(name); terminal_release_text(root);
    terminal_release_text(path); terminal_release_text(logical_input);
    const char expected[] = "module-id-v1:kind=7:project:namespace=15:terminal-normal:path=7:root.xr";
    CHECK(!strcmp(identity, expected) && !strcmp(independent, expected) && !strcmp(logical, "root.xr"));
    XrModuleIdentityKind kind = 0;
    CHECK(xr_module_identity_valid(identity, &kind) && kind == XR_MODULE_IDENTITY_PROJECT);
    xr_compile_resources_free(identity); xr_compile_resources_free(logical); xr_compile_resources_free(independent);
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK && after.live_bytes == before.live_bytes);
    puts("Owned project identity: exact root-relative framing survives caller input release, ledger restored");
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || (argc == 3 && !strcmp(argv[2], "--owned-project")));
    const bool owned_project = argc == 3;
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TERMINAL_FIXTURES};
    XrXirSourceRequest request = {session,XR_TERMINAL_FIXTURES "/root.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    char *owned[3] = {0};
    if (owned_project) {
        terminal_owned_identity(&context);
        owned[0] = terminal_owned_text(&context, "terminal-normal");
        owned[1] = terminal_owned_text(&context, XR_TERMINAL_FIXTURES);
        owned[2] = terminal_owned_text(&context, XR_TERMINAL_FIXTURES "/root.xr");
        authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_PROJECT, owned[0], owned[1]};
        request.entry_path = owned[2];
    }
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, NULL);
    if (status != XR_XIR_OK) fprintf(stderr, "terminal Source %u at %d:%d: %s\n",
        status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK);
    if (owned_project) {
        for (unsigned i = 0; i < 3; ++i) terminal_release_text(owned[i]);
        authority = (XrModuleIdentityAuthority){0}; request.entry_path = NULL;
        puts("Owned project Source: root/callee import checked; caller authority/path released before Checked serialization");
    }
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, NULL, &lowered, NULL) == XR_XIR_OK);
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
