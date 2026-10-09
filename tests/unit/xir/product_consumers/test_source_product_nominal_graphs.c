/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_nominal_graphs.c - Detached exact recursive and deep class descriptors
 *
 * KEY CONCEPT:
 *   Original nominal cycles survive producer destruction and verified lowering.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include "source_product_nominal_graphs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_instance_compile_observer.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Actual Checked identity");

typedef struct NominalRun {
    XrXirStatus status;
    XrCompileResourceStats stats;
    size_t attempts;
} NominalRun;

static XrCompileResourceLimits graph_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
}

static void graph_deep_shape(const XrXirModule *module) {
    const XrXirTypes *types = module->types;
    const XrXirNominalTable *table = types->nominals;
    XrXirType ids[80] = {0};
    unsigned count = 0;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        if (!xr_xir_type_is_class(types, type)) continue;
        const XrXirTypeNode *node = xr_xir_type_node(types, type);
        CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.field_count == 1);
        XrXirLiteral name = table->declarations ? table->declarations[node->nominal.declaration].name :
            table->identities[node->nominal.declaration].name;
        unsigned identity = 0;
        for (; identity < 80; ++identity) {
            char expected[16];
            int length = snprintf(expected, sizeof(expected), "C%u", identity);
            CHECK(length > 0 && (size_t)length < sizeof(expected));
            if (name.length == (uint32_t)length && !memcmp(name.bytes, expected, (size_t)length)) break;
        }
        CHECK(identity < 80 && !ids[identity] && count < 80);
        CHECK(xr_xir_type_is_owned(types, type));
        ids[identity] = type;
        ++count;
    }
    CHECK(count == 80);
    for (unsigned i = 0; i < 80; ++i) {
        CHECK(ids[i]);
        const XrXirTypeNode *node = xr_xir_type_node(types, ids[i]);
        XrXirLiteral field = table->declarations ? table->declarations[node->nominal.declaration].fields[0].name :
            table->identities[node->nominal.declaration].fields[0].name;
        XrXirType value = node->nominal.fields[0];
        if (i == 79) {
            CHECK(field.length == 5 && !memcmp(field.bytes, "value", 5));
            CHECK(value == XR_XIR_I64);
        } else {
            CHECK(field.length == 4 && !memcmp(field.bytes, "next", 4));
            CHECK(xr_xir_type_is_nullable(types, value) && xr_xir_type_is_owned(types, value));
            CHECK(xr_xir_nullable_element(types, value) == ids[i + 1]);
        }
    }
}

static void graph_shape(const XrXirModule *module, unsigned expected) {
    CHECK(module && module->types && module->types->nominals);
    if (expected == 80) {
        graph_deep_shape(module);
        return;
    }
    CHECK(expected == 1 || expected == 2);
    const XrXirTypes *types = module->types;
    XrXirType ids[2] = {0};
    unsigned count = 0;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        if (xr_xir_type_is_class(types, type)) {
            CHECK(count < expected && count < 2);
            ids[count++] = type;
        }
    }
    CHECK(count == expected);
    bool left_seen = false, right_seen = false;
    for (unsigned i = 0; i < count; ++i) {
        const XrXirTypeNode *node = xr_xir_type_node(types, ids[i]);
        CHECK(node && node->kind == XR_XIR_TYPE_NOMINAL && node->nominal.field_count == 1);
        const XrXirNominalTable *table = types->nominals;
        XrXirLiteral name = table->declarations ? table->declarations[node->nominal.declaration].name :
            table->identities[node->nominal.declaration].name;
        XrXirLiteral field = table->declarations ? table->declarations[node->nominal.declaration].fields[0].name :
            table->identities[node->nominal.declaration].fields[0].name;
        if (count == 1) {
            CHECK(name.length == 4 && !memcmp(name.bytes, "Node", 4));
            CHECK(field.length == 4 && !memcmp(field.bytes, "next", 4));
        } else if (name.length == 4 && !memcmp(name.bytes, "Left", 4)) {
            CHECK(!left_seen); left_seen = true;
            CHECK(field.length == 5 && !memcmp(field.bytes, "right", 5));
        } else {
            CHECK(!right_seen); right_seen = true;
            CHECK(name.length == 5 && !memcmp(name.bytes, "Right", 5));
            CHECK(field.length == 4 && !memcmp(field.bytes, "left", 4));
        }
        CHECK(xr_xir_type_is_owned(types, ids[i]));
        XrXirType nullable = node->nominal.fields[0];
        CHECK(xr_xir_type_is_nullable(types, nullable) && xr_xir_type_is_owned(types, nullable));
        CHECK(xr_xir_nullable_element(types, nullable) == ids[count == 1 ? 0 : 1 - i]);
    }
    CHECK(count == 1 || (left_seen && right_seen));
}

static XrXirStatus graph_detached_packet(XrXirArtifact *checked, const XrXirCheckedPacket *golden) {
    XrXirCheckedPacket repeated = {0};
    XrXirStatus status = xr_xir_compile_checked_write(checked, &repeated, NULL);
    if (status == XR_XIR_OK)
        CHECK(repeated.length == golden->length && !memcmp(repeated.bytes, golden->bytes, golden->length));
    xr_xir_compile_checked_packet_free(&repeated);
    return status;
}

static NominalRun graph_build(const SourceProductNominalFixture *fixture,
                             const char *emit, size_t failure, XrCompileResourceLimits limits) {
    instance_compile_zero();
    instance_compile_attempts = 0;
    instance_compile_fail_at = failure;
    instance_compile_injected = false;
    NominalRun run = {XR_XIR_OUT_OF_MEMORY, {0}, 0};
    XrXirCompileContext context = {0};
    XrCompileResourceStats baseline = {0};
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirCheckedPacket rewrite = {0};
    XrXirCSource original = {0}, regenerated = {0};
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &context.resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        run.status = opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto release;
    }
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSessionStatus ss = xr_compile_session_new(context.resources, &session);
    if (ss != XR_COMPILER_SESSION_OK) {
        run.status = ss == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto release;
    }
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, fixture->root};
    XrXirSourceProductRequest request = {
        {session, fixture->file, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run.status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    if (run.status != XR_XIR_OK) {
        CHECK(!product);
        if (!instance_compile_injected)
            fprintf(stderr, "nominal source status=%u stage=%u line=%d: %s\n", run.status,
                diagnostic.stage, diagnostic.source.line, diagnostic.source.message);
        goto release;
    }
    XrXirSourceProductPacketView packet = {0};
    run.status = xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet);
    if (run.status != XR_XIR_OK) goto release;
    run.status = xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL);
    if (run.status != XR_XIR_OK) goto release;
    run.status = xr_xir_compile_checked_write(checked, &rewrite, NULL);
    if (run.status != XR_XIR_OK) goto release;
    CHECK(rewrite.length == packet.length && !memcmp(rewrite.bytes, packet.bytes, packet.length));
    run.status = xr_xir_compile_source_product_emit(product, "nominal_graph", UINT64_C(16777216), &original);
    if (run.status != XR_XIR_OK) goto release;
    xr_xir_compile_source_product_free(product);
    product = NULL;
    xr_compile_session_free(session);
    session = NULL;
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    graph_shape(xr_xir_compile_artifact_module(checked), fixture->classes);
    run.status = xr_xir_compile_artifact_verify(checked, NULL);
    if (run.status != XR_XIR_OK) goto release;
    run.status = graph_detached_packet(checked, &rewrite);
    if (run.status != XR_XIR_OK) goto release;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run.status = xr_xir_compile_lower(checked, &target, &lowered, NULL);
    if (run.status != XR_XIR_OK) goto release;
    xr_xir_compile_artifact_free(checked); checked = NULL;
    run.status = xr_xir_compile_artifact_verify(lowered, NULL);
    if (run.status != XR_XIR_OK) goto release;
    graph_shape(xr_xir_compile_artifact_module(lowered), fixture->classes);
    run.status = xr_xir_compile_emit_c(lowered, "nominal_graph", UINT64_C(16777216), &regenerated);
    if (run.status != XR_XIR_OK) goto release;
    CHECK(original.length == regenerated.length && !memcmp(original.text, regenerated.text, original.length));
    if (emit) {
        FILE *file = fopen(emit, "wb");
        CHECK(file && fwrite(original.text, 1, original.length, file) == original.length && !fclose(file));
    }
release:
    xr_xir_compile_c_source_free(&regenerated);
    xr_xir_compile_c_source_free(&original);
    xr_xir_compile_checked_packet_free(&rewrite);
    xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(checked);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product);
    xr_compile_session_free(session);
    if (context.resources) {
        CHECK(xr_compile_resources_stats(context.resources, &run.stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(run.stats.live_bytes == baseline.live_bytes);
        xr_compile_resources_release(context.resources);
    }
    run.attempts = instance_compile_attempts;
    instance_compile_fail_at = SIZE_MAX;
    instance_compile_zero();
    return run;
}

static void graph_axes(const SourceProductNominalFixture *fixture) {
    NominalRun baseline = graph_build(fixture, NULL, SIZE_MAX, graph_limits());
    CHECK(baseline.status == XR_XIR_OK);
    uint64_t values[] = {baseline.stats.allocated_bytes, baseline.stats.peak_bytes, baseline.stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(values[axis]);
        for (unsigned below = 0; below < 2; ++below) {
            XrCompileResourceLimits limits = graph_limits();
            uint64_t value = values[axis] - below;
            if (!axis) limits.allocated_bytes = value;
            else if (axis == 1) limits.live_bytes = value;
            else limits.work = value;
            NominalRun run = graph_build(fixture, NULL, SIZE_MAX, limits);
            CHECK(run.status == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            printf("nominal-axis case=%s axis=%u limit=%llu below=%u status=%u physical=0/0\n",
                fixture->name, axis, (unsigned long long)value, below, run.status);
        }
    }
    printf("nominal-axes case=%s boundaries=6 physical=0/0\n", fixture->name);
}

int xr_source_product_nominal_main(const SourceProductNominalFixture *fixture, int argc, char **argv) {
    CHECK(fixture && argc >= 2 && argc <= 5 && !strcmp(argv[1], "0"));
    if (argc == 3 && !strcmp(argv[2], "--axes")) {
        graph_axes(fixture);
        return 0;
    }
    if (argc == 5) {
        CHECK(!strcmp(argv[2], "--compiler-shard"));
        size_t shard = (size_t)strtoull(argv[3], NULL, 10), shards = (size_t)strtoull(argv[4], NULL, 10);
        CHECK(shards && shard < shards);
        NominalRun baseline = graph_build(fixture, NULL, SIZE_MAX, graph_limits());
        CHECK(baseline.status == XR_XIR_OK && baseline.attempts);
        size_t covered = 0;
        for (size_t ordinal = shard; ordinal < baseline.attempts; ordinal += shards) {
            NominalRun fault = graph_build(fixture, NULL, ordinal, graph_limits());
            CHECK(instance_compile_injected && fault.status == XR_XIR_OUT_OF_MEMORY && fault.attempts > ordinal);
            printf("compiler ordinal=%zu physical=0/0\n", ordinal);
            ++covered;
        }
        printf("compiler-summary case=%s mode=0 sites=%zu shard=%zu shards=%zu covered=%zu physical=0/0\n",
            fixture->name, baseline.attempts, shard, shards, covered);
        return 0;
    }
    size_t failure = SIZE_MAX;
    const char *emit = argc == 3 ? argv[2] : NULL;
    if (argc == 4) {
        CHECK(!strcmp(argv[2], "--compiler-site"));
        failure = (size_t)strtoull(argv[3], NULL, 10);
    }
    NominalRun run = graph_build(fixture, emit, failure, graph_limits());
    if (failure == SIZE_MAX)
        CHECK(run.status == XR_XIR_OK);
    else
        CHECK(instance_compile_injected && run.status == XR_XIR_OUT_OF_MEMORY && run.attempts > failure);
    printf("consumer %s mode=0 status=%u compiler-sites=%zu allocated=%llu peak=%llu work=%llu\n",
        fixture->name, run.status, run.attempts, (unsigned long long)run.stats.allocated_bytes,
        (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    if (run.status == XR_XIR_OK) {
        puts("nominal-owned exact-Checked-packet-after-producers=PASS Lowered-after-Checked-death=PASS request-authority-dead=1");
        if (fixture->classes == 80)
            printf("nominal-graph case=%s classes=80 nullable-links=79 terminal-i64=1 Checked/Lowered readers survive producers; C identical\n",
                fixture->name);
        else
            printf("nominal-graph case=%s classes=%u nullable-cycles=%u Checked/Lowered readers survive producers; C identical\n",
                fixture->name, fixture->classes, fixture->classes);
    }
    instance_compile_report();
    return 0;
}
