/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_detachment.c - Independent source products and readers
 *
 * KEY CONCEPT:
 *   Checked owners remain valid after independent producers and inputs die.
 */
#include "base/xmalloc.h"
#include "os/os_fs.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_instance_compile_observer.h"

static XrXirSourceProduct *produce(const XrXirCompileContext *context, const char *root, const char *file) {
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    const XrXirSourceProductRequest request = {
        {session, file, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic error = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &error);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK)
        fprintf(stderr, "source status=%u stage=%u line=%d: %s\n", status, error.stage,
            error.source.line, error.source.message);
    if (status != XR_XIR_OK) {
        CHECK(!product);
        xr_xir_compile_source_product_diagnostic_free(&error);
        return NULL;
    }
    CHECK(product && !error.snapshot && !error.source_path);
    xr_xir_compile_source_product_diagnostic_free(&error);
    CHECK(xr_xir_compile_source_product_view(product)->complete);
    return product;
}

static void facts_equal(const XrXirSourceProductFacts *a, const XrXirSourceProductFacts *b) {
    CHECK(a && b && a != b);
    CHECK(a->entry == b->entry && a->function_count == b->function_count && a->module_count == b->module_count);
    CHECK(a->target.architecture == b->target.architecture && a->target.abi_version == b->target.abi_version);
    CHECK(!memcmp(a->source_digest, b->source_digest, 32));
    CHECK(!memcmp(a->closed_digest, b->closed_digest, 32));
    CHECK(!memcmp(a->lowered_layout_digest, b->lowered_layout_digest, 32));
}

static void modules(const XrXirArtifact *artifact, bool paired) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *declarations = module->declarations;
    CHECK(declarations && declarations->module_count == (paired ? 2u : 1u));
    CHECK(declarations->root_module < declarations->module_count);
    const XrXirSourceModule *root = &declarations->modules[declarations->root_module];
    CHECK(root->dependency_count == (paired ? 1u : 0u));
    CHECK(root->initializer != declarations->entry_function && root->initializer < module->function_count);
    CHECK(declarations->slot_count == (paired ? 1u : 0u));
    if (paired) {
        uint32_t library = root->dependencies[0];
        CHECK(library < declarations->module_count && library != declarations->root_module);
        const XrXirSourceModule *dependency = &declarations->modules[library];
        CHECK(!dependency->dependency_count && dependency->initializer < module->function_count);
        CHECK(dependency->initializer != root->initializer && dependency->initializer != declarations->entry_function);
        CHECK(xr_xir_module_imports(declarations, declarations->root_module, library));
        CHECK(!xr_xir_module_imports(declarations, library, declarations->root_module));
        CHECK(declarations->slots[0].module == library && declarations->slots[0].type == XR_XIR_I64);
        CHECK(declarations->slots[0].mutable == 1);
    }
}

static void reader_budget(const XrXirSourceProductPacketView *packet) {
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(16777216), 1};
    XrXirCompileContext context = {0};
    context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *rejected = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet->bytes, packet->length, &rejected, NULL) == XR_XIR_BUDGET);
    CHECK(!rejected);
    xr_compile_resources_release(context.resources);
}

static bool private_root(const char *root) {
    const char *prefix = XR_DETACHMENT_SCRATCH;
    while (*prefix && *root) {
        char a = *prefix++, b = *root++;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return false;
    }
    if (*prefix || (*root != '/' && *root != '\\')) return false;
    ++root;
    if (!*root) return false;
    for (; *root; ++root)
        if (!((*root >= 'a' && *root <= 'z') || (*root >= '0' && *root <= '9') || *root == '-' || *root == '_'))
            return false;
    return true;
}

static void delete_sources(const XrXirCompileContext *context, const char *root, const char *file, bool paired) {
    CHECK(private_root(root));
    XrOsIoPolicy policy = xr_compile_io_policy(context->resources);
    CHECK(xr_os_io_remove(&policy, file) == XR_OS_IO_OK);
    CHECK(xr_file_probe_owned(&policy, file, false) == XR_OS_IO_NOT_FOUND);
    if (paired) {
        char library[1056];
        int length = snprintf(library, sizeof(library), "%s/library.xr", root);
        CHECK(length > 0 && (size_t)length < sizeof(library));
        CHECK(xr_os_io_remove(&policy, library) == XR_OS_IO_OK);
        CHECK(xr_file_probe_owned(&policy, library, false) == XR_OS_IO_NOT_FOUND);
    }
    puts("physical source copies deleted and probed missing before Checked reader admission");
}

static void detach(const XrXirCompileContext *context, XrXirSourceProduct *first,
    XrXirSourceProduct *second, const char *name) {
    XrXirSourceProductPacketView packets[2][2] = {0};
    XrXirArtifact *readers[2] = {0};
    XrXirCSource generated[2] = {0};
    XrXirSourceProduct *products[] = {first, second};
    facts_equal(xr_xir_compile_source_product_facts(first), xr_xir_compile_source_product_facts(second));
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned kind = 0; kind < 2; ++kind)
            CHECK(xr_xir_compile_source_product_packet(products[p], (XrXirSourceProductPacketKind)kind,
                &packets[p][kind]) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context, packets[p][1].bytes, packets[p][1].length,
            &readers[p], NULL) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_emit(products[p], "detached_product", 1048576, &generated[p]) == XR_XIR_OK);
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
        CHECK(packets[0][kind].bytes != packets[1][kind].bytes);
        CHECK(packets[0][kind].length == packets[1][kind].length);
        CHECK(!memcmp(packets[0][kind].bytes, packets[1][kind].bytes, packets[0][kind].length));
    }
    CHECK(generated[0].text != generated[1].text && generated[0].length == generated[1].length);
    CHECK(!memcmp(generated[0].text, generated[1].text, generated[0].length));
    XrXirCheckedPacket retained = {0};
    CHECK(xr_xir_compile_checked_write(readers[0], &retained, NULL) == XR_XIR_OK);
    CHECK(retained.bytes != packets[0][1].bytes && retained.length == packets[0][1].length);
    CHECK(!memcmp(retained.bytes, packets[0][1].bytes, retained.length));
    reader_budget(&packets[0][1]);
    uint8_t *corrupted = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, retained.length, (void **)&corrupted) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context->resources, retained.length) == XR_COMPILE_RESOURCE_OK);
    memcpy(corrupted, retained.bytes, retained.length);
    corrupted[0] ^= UINT8_C(0xff);
    XrXirArtifact *rejected = NULL;
    CHECK(xr_xir_compile_checked_read(context, corrupted, retained.length, &rejected, NULL) == XR_XIR_BAD_STRUCTURE);
    CHECK(!rejected);
    xr_compile_resources_free(corrupted);
    xr_xir_compile_source_product_free(first);
    xr_xir_compile_source_product_free(second);
    for (unsigned p = 0; p < 2; ++p) {
        CHECK(xr_xir_compile_artifact_verify(readers[p], NULL) == XR_XIR_OK);
        if (!strcmp(name, "single_module") || !strcmp(name, "two_modules"))
            modules(readers[p], !strcmp(name, "two_modules"));
        xr_xir_compile_c_source_free(&generated[p]);
    }
    XrXirCheckedPacket after = {0};
    CHECK(xr_xir_compile_checked_write(readers[1], &after, NULL) == XR_XIR_OK);
    CHECK(after.bytes != retained.bytes && after.length == retained.length);
    CHECK(!memcmp(after.bytes, retained.bytes, retained.length));
    xr_xir_compile_artifact_free(readers[0]);
    xr_xir_compile_artifact_free(readers[1]);
    XrXirArtifact *final = NULL;
    CHECK(xr_xir_compile_checked_read(context, retained.bytes, retained.length, &final, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(final, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(final);
    xr_xir_compile_checked_packet_free(&after);
    xr_xir_compile_checked_packet_free(&retained);
    puts("two independent sessions/products; source and closed bytes and native C identical; readers survive all producers");
}

int main(int argc, char **argv) {
    CHECK(argc == 2 || (argc == 4 && !strcmp(argv[3], "--delete-source") && private_root(argv[2])));
    CHECK(!strcmp(argv[1], "generics") || !strcmp(argv[1], "callables") ||
        !strcmp(argv[1], "single_module") || !strcmp(argv[1], "text_program") || !strcmp(argv[1], "two_modules"));
    char root[1024], file[1056];
    int length = argc == 4 ? snprintf(root, sizeof(root), "%s", argv[2]) :
        snprintf(root, sizeof(root), "%s/%s", XR_DETACHMENT_ROOT, argv[1]);
    CHECK(length > 0 && (size_t)length < sizeof(root));
    length = snprintf(file, sizeof(file), "%s/root.xr", root);
    CHECK(length > 0 && (size_t)length < sizeof(file));
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
    XrXirCompileContext context = {0};
    context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *first = produce(&context, root, file);
    XrXirSourceProduct *second = first ? produce(&context, root, file) : NULL;
    bool complete = first && second;
    if (complete) {
        CHECK(first != second);
        if (argc == 4) delete_sources(&context, root, file, !strcmp(argv[1], "two_modules"));
        detach(&context, first, second, argv[1]);
    } else {
        xr_xir_compile_source_product_free(first);
        xr_xir_compile_source_product_free(second);
    }
    XrCompileResourceStats final = {0};
    CHECK(xr_compile_resources_stats(context.resources, &final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == baseline.live_bytes && final.live_bytes == instance_compile_bytes);
    xr_compile_resources_release(context.resources);
    instance_compile_report();
    return complete ? 0 : 1;
}
