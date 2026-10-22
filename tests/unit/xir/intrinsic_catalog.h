/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * intrinsic_catalog.h - Owned library transport for canonical string calls
 *
 * KEY CONCEPT:
 *   Library functions relocate while intrinsic identities remain canonical.
 *   Both the packet and Source producers die before the receiver executes.
 */
#ifndef INTRINSIC_CATALOG_H
#define INTRINSIC_CATALOG_H
#include "xir/xxir_library_catalog.h"
#include "../test_win_compat.h"

static void intrinsic_catalog_source(const XrXirSourceRequest *request, const char *name,
    const char *text, XrXirSourceResult *result) {
    XrXirSourceText input = {name, text, strlen(text)};
    XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check_text(request, &input, result, &diagnostic, NULL);
    if (status != XR_XIR_OK)
        fprintf(stderr, "catalog source %s %u:%d:%d %s\n", name, (unsigned)status,
            diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result->checked && result->snapshot);
}

static void intrinsic_catalog_invalid(const XrXirCompileContext *context,
    const XrXirLibraryInput *input, const XrXirInstruction *instruction) {
    uint8_t pattern[40] = {0};
    put32(pattern, XR_XIR_CALL_INTRINSIC);
    put32(pattern + 4, XR_XIR_BOOL);
    put32(pattern + 8, instruction->args[0]);
    put32(pattern + 12, instruction->args[1]);
    put32(pattern + 24, (uint32_t)instruction->immediate);
    uint8_t *bytes = xr_malloc(input->length);
    CHECK(bytes);
    memcpy(bytes, input->packet, input->length);
    size_t at = 64;
    while (at + sizeof(pattern) <= input->length && memcmp(bytes + at, pattern, sizeof(pattern))) ++at;
    CHECK(at + sizeof(pattern) <= input->length);
    put32(bytes + at + 24, 0);
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, bytes, 32);
    xr_sha256_update(&hash, bytes + 64, input->length - 64);
    xr_sha256_final(&hash, bytes + 32);
    XrXirLibraryInput invalid = *input;
    invalid.packet = bytes;
    xr_sha256(bytes, input->length, invalid.sha256);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context, &invalid, 1, &catalog) ==
        XR_XIR_BAD_STRUCTURE && !catalog);
    xr_free(bytes);
}

static Fixture intrinsic_catalog_fixture(void) {
    Fixture fixture = {0};
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    CHECK(xr_compile_resources_new(&limits, &fixture.context.resources) == XR_COMPILE_RESOURCE_OK);
    fixture.context.limits = xr_xir_compile_default_limits();
    char directory[XR_TEST_PATH_MAX] = "intrinsic-catalog-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX], root[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    int length = snprintf(library, sizeof(library), "%s/lib.xr", absolute);
    CHECK(length > 0 && (size_t)length < sizeof(library));
    length = snprintf(root, sizeof(root), "%s/root.xr", absolute);
    CHECK(length > 0 && (size_t)length < sizeof(root));
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(fixture.context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request = {.session = session, .entry_path = library, .authority = &authority,
        .context = &fixture.context, .linkage_kind = XR_XIR_LIBRARY};
    const char *body = strstr(source_text, "\nprint(");
    CHECK(body);
    size_t declaration_size = (size_t)(body + 1 - source_text);
    char *declarations = xr_malloc(declaration_size + 1);
    CHECK(declarations);
    memcpy(declarations, source_text, declaration_size);
    declarations[declaration_size] = 0;
    XrXirSourceResult produced = {0};
    intrinsic_catalog_source(&request, "lib.xr", declarations, &produced);
    memset(declarations, 0xa5, declaration_size);
    xr_free(declarations);
    const XrXirModule *module = xr_xir_compile_artifact_module(produced.checked);
    const char *names[] = {"probe", "prefix", "suffix"};
    uint32_t original[3];
    for (uint32_t i = 0; i < 3; ++i) original[i] = named(module, names[i]);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(produced.checked, &packet, NULL) == XR_XIR_OK);
    XrXirLibraryModuleInput binding = {authority, "lib.xr"};
    XrXirLibraryInput input = {packet.bytes, packet.length, {0}, &binding, 1};
    xr_sha256(packet.bytes, packet.length, input.sha256);
    const XrXirFunction *probe = &module->functions[original[0]];
    uint32_t call = 0;
    while (call < probe->instruction_count && probe->instructions[call].op != XR_XIR_CALL_INTRINSIC) ++call;
    CHECK(call < probe->instruction_count);
    intrinsic_catalog_invalid(&fixture.context, &input, &probe->instructions[call]);
    xr_xir_compile_source_result_free(&produced);
    xr_compile_session_free(session);
    session = NULL;
    XrXirLibraryCatalog *catalog = NULL;
    XrXirStatus status = xr_xir_compile_library_catalog_new_v2(&fixture.context, &input, 1, &catalog);
    if (status != XR_XIR_OK) fprintf(stderr, "canonical intrinsic catalog status=%u\n", (unsigned)status);
    CHECK(status == XR_XIR_OK && catalog);
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(fixture.context.resources, &session) == XR_COMPILER_SESSION_OK);
    request.session = session;
    request.entry_path = root;
    request.linkage_kind = XR_XIR_PROGRAM;
    request.libraries = catalog;
    const char imports[] = "import {probe,prefix,suffix,folded} from \"./lib\"\n"
        "fn caller0()->i64{return 11}\nfn caller1()->i64{return 13}\n";
    size_t size = sizeof(imports) - 1 + strlen(body + 1) + 1;
    char *consumer = xr_malloc(size);
    CHECK(consumer);
    memcpy(consumer, imports, sizeof(imports) - 1);
    memcpy(consumer + sizeof(imports) - 1, body + 1, strlen(body + 1) + 1);
    XrXirSourceResult received = {0};
    intrinsic_catalog_source(&request, "root.xr", consumer, &received);
    memset(consumer, 0xa5, size);
    xr_free(consumer);
    xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_specialize(received.checked, &fixture.closed, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&received);
    CHECK(xr_xir_compile_checked_write(fixture.closed, &fixture.packet, NULL) == XR_XIR_OK);
    xr_sha256(fixture.packet.bytes, fixture.packet.length, fixture.identity);
    xr_xir_compile_artifact_free(fixture.closed);
    fixture.closed = NULL;
    CHECK(xr_xir_compile_checked_read(&fixture.context, fixture.packet.bytes, fixture.packet.length,
        &fixture.closed, NULL) == XR_XIR_OK);
    memset(fixture.packet.bytes, 0xa5, fixture.packet.length);
    xr_xir_compile_checked_packet_free(&fixture.packet);
    module = xr_xir_compile_artifact_module(fixture.closed);
    uint32_t *functions[] = {&fixture.probe, &fixture.prefix, &fixture.suffix};
    for (uint32_t i = 0; i < 3; ++i) {
        *functions[i] = named(module, names[i]);
        CHECK(*functions[i] != original[i] && *functions[i] != i + 1);
        const XrXirFunction *function = &module->functions[*functions[i]];
        bool found = false;
        for (uint32_t n = 0; n < function->instruction_count; ++n)
            if (function->instructions[n].op == XR_XIR_CALL_INTRINSIC) {
                CHECK(function->instructions[n].immediate == (int64_t)i + 1);
                found = true;
            }
        CHECK(found);
    }
    fixture.folded = named(module, "folded");
    fixture.entry = module->declarations->entry_function;
    CHECK(xr_test_rmdir(directory) == 0);
    memset(absolute, 0xa5, sizeof(absolute));
    memset(library, 0xa5, sizeof(library));
    memset(root, 0xa5, sizeof(root));
    return fixture;
}
#endif // INTRINSIC_CATALOG_H
