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

#define DETACH_TRY(expression) do { status = (expression); if (status != XR_XIR_OK) goto done; } while (0)

typedef struct DetachmentCase {
    const char *name;
    char root[1024], file[1056];
    uint8_t source[4096];
    size_t source_length;
    bool remove_source;
} DetachmentCase;

typedef struct DetachmentRun {
    XrXirStatus status;
    XrCompileResourceStats stats;
    size_t attempts;
    bool injected;
} DetachmentRun;

static XrCompileResourceLimits limits(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
}

static XrXirStatus resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    if (status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET);
    return XR_XIR_BUDGET;
}

static XrXirStatus io_status(XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return XR_XIR_OK;
    if (status == XR_OS_IO_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    if (status == XR_OS_IO_BUDGET) return XR_XIR_BUDGET;
    fprintf(stderr, "unexpected source I/O status=%u\n", status);
    CHECK(false);
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus checked_read(const XrXirCompileContext *context, const void *bytes,
    size_t length, XrXirArtifact **output) {
    CHECK(!*output);
    XrXirStatus status = xr_xir_compile_checked_read(context, bytes, length, output, NULL);
    if (status != XR_XIR_OK) CHECK(!*output);
    return status;
}

static XrXirStatus checked_write(const XrXirArtifact *artifact, XrXirCheckedPacket *output) {
    CHECK(!output->bytes && !output->length);
    XrXirStatus status = xr_xir_compile_checked_write(artifact, output, NULL);
    if (status != XR_XIR_OK) CHECK(!output->bytes && !output->length);
    return status;
}

static XrXirStatus emit(const XrXirSourceProduct *product, XrXirCSource *output) {
    CHECK(!output->text && !output->length);
    XrXirStatus status = xr_xir_compile_source_product_emit(product, "detached_product", 1048576, output);
    if (status != XR_XIR_OK) CHECK(!output->text && !output->length);
    return status;
}

static XrXirStatus produce(const XrXirCompileContext *context, const DetachmentCase *test,
    XrXirSourceProduct **product) {
    XrCompilerSession *session = NULL;
    XrCompilerSessionStatus opened = xr_compile_session_new(context->resources, &session);
    if (opened != XR_COMPILER_SESSION_OK) {
        CHECK(!session && !*product);
        CHECK(opened == XR_COMPILER_SESSION_OUT_OF_MEMORY || opened == XR_COMPILER_SESSION_BUDGET);
        return opened == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, test->root};
    const XrXirSourceProductRequest request = {
        {session, test->file, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProductDiagnostic error = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, product, &error);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK && status != XR_XIR_OUT_OF_MEMORY && status != XR_XIR_BUDGET)
        fprintf(stderr, "source status=%u stage=%u line=%d: %s\n", status, error.stage,
            error.source.line, error.source.message);
    if (status != XR_XIR_OK) {
        CHECK(!*product);
    } else {
        CHECK(*product && !error.snapshot && !error.source_path);
        CHECK(xr_xir_compile_source_product_view(*product)->complete);
    }
    xr_xir_compile_source_product_diagnostic_free(&error);
    CHECK(!error.snapshot && !error.source_path && !error.source.message[0]);
    return status;
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

static XrXirStatus reader_budget(const XrXirSourceProductPacketView *packet) {
    const XrCompileResourceLimits budget = {UINT64_C(67108864), UINT64_C(16777216), 1};
    XrXirCompileContext context = {0};
    context.limits = xr_xir_compile_default_limits();
    XrXirStatus status = resource_status(xr_compile_resources_new(&budget, &context.resources));
    if (status != XR_XIR_OK) return status;
    XrXirArtifact *rejected = NULL;
    status = checked_read(&context, packet->bytes, packet->length, &rejected);
    CHECK(!rejected);
    xr_compile_resources_release(context.resources);
    CHECK(status == XR_XIR_BUDGET || status == XR_XIR_OUT_OF_MEMORY);
    return status == XR_XIR_BUDGET ? XR_XIR_OK : status;
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

static XrXirStatus delete_sources(const XrXirCompileContext *context, const DetachmentCase *test) {
    CHECK(private_root(test->root));
    XrOsIoPolicy policy = xr_compile_io_policy(context->resources);
    XrXirStatus status = io_status(xr_os_io_remove(&policy, test->file));
    if (status != XR_XIR_OK) return status;
    XrOsIoStatus missing = xr_file_probe_owned(&policy, test->file, false);
    if (missing != XR_OS_IO_NOT_FOUND) {
        CHECK(missing != XR_OS_IO_OK);
        return io_status(missing);
    }
    if (!strcmp(test->name, "two_modules")) {
        char library[1056];
        int length = snprintf(library, sizeof(library), "%s/library.xr", test->root);
        CHECK(length > 0 && (size_t)length < sizeof(library));
        status = io_status(xr_os_io_remove(&policy, library));
        if (status != XR_XIR_OK) return status;
        missing = xr_file_probe_owned(&policy, library, false);
        if (missing != XR_OS_IO_NOT_FOUND) {
            CHECK(missing != XR_OS_IO_OK);
            return io_status(missing);
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus detach(const XrXirCompileContext *context, XrXirSourceProduct *products[2], const char *name) {
    XrXirStatus status = XR_XIR_OK;
    XrXirSourceProductPacketView packets[2][2] = {0};
    XrXirArtifact *readers[2] = {0};
    XrXirArtifact *rejected = NULL, *final = NULL;
    XrXirCSource generated[2] = {0};
    XrXirCheckedPacket retained = {0}, after = {0};
    uint8_t *corrupted = NULL;
    facts_equal(xr_xir_compile_source_product_facts(products[0]), xr_xir_compile_source_product_facts(products[1]));
    for (unsigned p = 0; p < 2; ++p) {
        for (unsigned kind = 0; kind < 2; ++kind)
            DETACH_TRY(xr_xir_compile_source_product_packet(products[p], (XrXirSourceProductPacketKind)kind,
                &packets[p][kind]));
        DETACH_TRY(checked_read(context, packets[p][1].bytes, packets[p][1].length, &readers[p]));
        DETACH_TRY(emit(products[p], &generated[p]));
    }
    for (unsigned kind = 0; kind < 2; ++kind) {
        CHECK(packets[0][kind].bytes != packets[1][kind].bytes);
        CHECK(packets[0][kind].length == packets[1][kind].length);
        CHECK(!memcmp(packets[0][kind].bytes, packets[1][kind].bytes, packets[0][kind].length));
    }
    CHECK(generated[0].text != generated[1].text && generated[0].length == generated[1].length);
    CHECK(!memcmp(generated[0].text, generated[1].text, generated[0].length));
    DETACH_TRY(checked_write(readers[0], &retained));
    CHECK(retained.bytes != packets[0][1].bytes && retained.length == packets[0][1].length);
    CHECK(!memcmp(retained.bytes, packets[0][1].bytes, retained.length));
    DETACH_TRY(reader_budget(&packets[0][1]));
    DETACH_TRY(resource_status(xr_compile_resources_alloc(context->resources, retained.length, (void **)&corrupted)));
    DETACH_TRY(resource_status(xr_compile_resources_work(context->resources, retained.length)));
    memcpy(corrupted, retained.bytes, retained.length);
    corrupted[0] ^= UINT8_C(0xff);
    status = checked_read(context, corrupted, retained.length, &rejected);
    CHECK(!rejected);
    if (status != XR_XIR_BAD_STRUCTURE) goto done;
    status = XR_XIR_OK;
    xr_compile_resources_free(corrupted);
    corrupted = NULL;
    for (unsigned p = 0; p < 2; ++p) {
        xr_xir_compile_source_product_free(products[p]);
        products[p] = NULL;
    }
    for (unsigned p = 0; p < 2; ++p) {
        DETACH_TRY(xr_xir_compile_artifact_verify(readers[p], NULL));
        if (!strcmp(name, "single_module") || !strcmp(name, "two_modules"))
            modules(readers[p], !strcmp(name, "two_modules"));
        xr_xir_compile_c_source_free(&generated[p]);
    }
    DETACH_TRY(checked_write(readers[1], &after));
    CHECK(after.bytes != retained.bytes && after.length == retained.length);
    CHECK(!memcmp(after.bytes, retained.bytes, retained.length));
    for (unsigned p = 0; p < 2; ++p) {
        xr_xir_compile_artifact_free(readers[p]);
        readers[p] = NULL;
    }
    DETACH_TRY(checked_read(context, retained.bytes, retained.length, &final));
    DETACH_TRY(xr_xir_compile_artifact_verify(final, NULL));
done:
    xr_xir_compile_artifact_free(final);
    xr_xir_compile_artifact_free(rejected);
    xr_compile_resources_free(corrupted);
    for (unsigned p = 0; p < 2; ++p) {
        xr_xir_compile_artifact_free(readers[p]);
        xr_xir_compile_c_source_free(&generated[p]);
    }
    xr_xir_compile_checked_packet_free(&after);
    xr_xir_compile_checked_packet_free(&retained);
    return status;
}

static DetachmentRun run(const DetachmentCase *test, size_t failure, XrCompileResourceLimits budget) {
    instance_compile_zero();
    instance_compile_attempts = 0;
    instance_compile_peak = 0;
    instance_compile_fail_at = failure;
    instance_compile_injected = false;
    DetachmentRun result = {XR_XIR_OUT_OF_MEMORY, {0}, 0, false};
    XrXirCompileContext context = {0};
    XrXirSourceProduct *products[2] = {0};
    XrCompileResourceStats baseline = {0};
    context.limits = xr_xir_compile_default_limits();
    result.status = resource_status(xr_compile_resources_new(&budget, &context.resources));
    if (result.status != XR_XIR_OK) goto done;
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    for (unsigned p = 0; p < 2; ++p) {
        result.status = produce(&context, test, &products[p]);
        if (result.status != XR_XIR_OK) goto release;
    }
    CHECK(products[0] != products[1]);
    if (test->remove_source) {
        result.status = delete_sources(&context, test);
        if (result.status != XR_XIR_OK) goto release;
    }
    result.status = detach(&context, products, test->name);
release:
    for (unsigned p = 0; p < 2; ++p) xr_xir_compile_source_product_free(products[p]);
    CHECK(xr_compile_resources_stats(context.resources, &result.stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.stats.live_bytes == baseline.live_bytes && result.stats.live_bytes == instance_compile_bytes);
    xr_compile_resources_release(context.resources);
done:
    result.attempts = instance_compile_attempts;
    result.injected = instance_compile_injected;
    instance_compile_fail_at = SIZE_MAX;
    instance_compile_zero();
    return result;
}

#include "source_product_detachment_faults.inc.c"

int main(int argc, char **argv) {
    CHECK(argc == 2 || (argc >= 4 && argc <= 7 && !strcmp(argv[3], "--delete-source") && private_root(argv[2])));
    CHECK(!strcmp(argv[1], "generics") || !strcmp(argv[1], "callables") ||
        !strcmp(argv[1], "single_module") || !strcmp(argv[1], "text_program") || !strcmp(argv[1], "two_modules"));
    DetachmentCase test = {0};
    test.name = argv[1];
    test.remove_source = argc >= 4;
    int length = test.remove_source ? snprintf(test.root, sizeof(test.root), "%s", argv[2]) :
        snprintf(test.root, sizeof(test.root), "%s/%s", XR_DETACHMENT_ROOT, test.name);
    CHECK(length > 0 && (size_t)length < sizeof(test.root));
    length = snprintf(test.file, sizeof(test.file), "%s/root.xr", test.root);
    CHECK(length > 0 && (size_t)length < sizeof(test.file));
    if (argc > 4) {
        CHECK(strcmp(test.name, "two_modules"));
        fixture_replay(&test, REPLAY_LOAD);
        fixture_replay(&test, REPLAY_WRITE);
    }
    DetachmentRun baseline = run(&test, SIZE_MAX, limits());
    if (baseline.status != XR_XIR_OK) {
        instance_compile_report();
        return 1;
    }
    CHECK(baseline.attempts && !baseline.injected);
    if (test.remove_source) puts("physical source copies deleted and probed missing before Checked reader admission");
    puts("two independent sessions/products; source and closed bytes and native C identical; readers survive all producers");
    printf("detachment-baseline case=%s sites=%zu allocated=%llu peak=%llu work=%llu physical=0/0\n",
        test.name, baseline.attempts, (unsigned long long)baseline.stats.allocated_bytes,
        (unsigned long long)baseline.stats.peak_bytes, (unsigned long long)baseline.stats.work);
    if (argc > 4) {
        exercise(&test, &baseline, argc, argv);
        fixture_replay(&test, REPLAY_REMOVE);
    }
    instance_compile_report();
    return 0;
}
