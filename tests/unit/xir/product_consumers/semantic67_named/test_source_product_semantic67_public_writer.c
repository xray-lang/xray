/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * writer_public_pipeline_candidate.c - Legal producer and rejected synthetic names
 *
 * KEY CONCEPT:
 *   Private codec sizing inputs are not valid public Checked artifacts. Public
 *   admission stays fail-closed; original private sizing responsibility is OPEN.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "semantic67_public_writer_goldens.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25 && XR_XIR_CHECKED_CONTRACT == 67 &&
    XR_XIR_OP_COUNT == 148, "current definition and writer contract");
_Static_assert(XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33, "named instruction ordinals");
_Static_assert(sizeof(writer_named67_0) == 224 && sizeof(writer_named67_1) == 224 &&
    sizeof(writer_named67_2) == 220, "complete independently framed packet lengths");
static const XrCompileResourceLimits writer_caps = {67108864, 8388608, 128000000};
static const XrXirInstruction writer_ops[] = {
    {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42},
    {.op = XR_XIR_RETURN, .type = XR_XIR_UNIT}
};
static const XrXirBlock writer_block = {.count = 2};

static XrXirFunction writer_function(const char *name, uint32_t length) {
    return (XrXirFunction) {.name = name, .name_length = length, .result = XR_XIR_I64,
        .blocks = &writer_block, .block_count = 1, .instructions = writer_ops,
        .instruction_count = 2};
}
static XrXirModule writer_built(const XrXirFunction *function) {
    return (XrXirModule) {.stage = XR_XIR_BUILT, .functions = function,
        .function_count = 1, .linkage_kind = XR_XIR_PROGRAM};
}
static XrCompileResourceStats writer_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(context->resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static XrXirArtifact *writer_legal(const XrXirCompileContext *context) {
    char name[] = {'m', 'a', 'i', 'n'};
    XrXirFunction function = writer_function(name, 4);
    XrXirModule built = writer_built(&function);
    XrXirArtifact *checked = NULL, *decoded = NULL;
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_check(context, &built, &checked, NULL) == XR_XIR_OK && checked);
    memset(name, 0xcc, sizeof(name));
    CHECK(xr_xir_compile_artifact_module(checked)->functions[0].name != name);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(writer_named67_0));
    CHECK(!memcmp(packet.bytes, writer_named67_0, packet.length));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(decoded)->functions[0].instructions[0].immediate == 42);
    xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);
    return checked;
}
static void writer_bad_diagnostic(const XrXirDiagnostic *diagnostic) {
    CHECK(diagnostic->status == XR_XIR_BAD_STRUCTURE && diagnostic->function == 0);
    CHECK(diagnostic->block == UINT32_MAX && diagnostic->instruction == UINT32_MAX);
    CHECK(diagnostic->reason == XR_XIR_DIAGNOSTIC_NONE);
}
static void writer_bad_built(const XrXirCompileContext *context,
    XrXirArtifact *occupied, unsigned variant) {
    static const char embedded[] = {'m', 0, 'i', 'n'};
    const char *name = variant == 1 ? embedded : (const char *)(uintptr_t)1;
    XrXirFunction function = writer_function(name, variant == 1 ? 4 : 0);
    XrXirModule built = writer_built(&function);
    for (unsigned state = 0; state < 2; ++state) {
        XrXirArtifact *output = state ? occupied : NULL;
        XrXirArtifact *expected = output;
        XrCompileResourceStats before = writer_stats(context);
        XrXirDiagnostic diagnostic = {XR_XIR_OK, 17, 18, 19, XR_XIR_DIAGNOSTIC_NONE};
        CHECK(xr_xir_compile_check(context, &built, &output, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        writer_bad_diagnostic(&diagnostic);
        CHECK(output == expected && writer_stats(context).live_bytes == before.live_bytes);
        CHECK(xr_xir_compile_artifact_verify(occupied, NULL) == XR_XIR_OK);
    }
}
static void writer_bad_packet(const XrXirCompileContext *context,
    XrXirArtifact *occupied, unsigned variant) {
    const uint8_t *bytes = variant == 1 ? writer_named67_1 : writer_named67_2;
    const size_t size = variant == 1 ? sizeof(writer_named67_1) : sizeof(writer_named67_2);
    for (unsigned state = 0; state < 2; ++state) {
        XrXirArtifact *output = state ? occupied : NULL;
        XrXirArtifact *expected = output;
        XrCompileResourceStats before = writer_stats(context);
        XrXirDiagnostic diagnostic = {XR_XIR_OK, 17, 18, 19, XR_XIR_DIAGNOSTIC_NONE};
        size_t attempts = source_program_compile_attempts;
        CHECK(xr_xir_compile_checked_read(context, bytes, size, &output, &diagnostic) == XR_XIR_BAD_STRUCTURE);
        writer_bad_diagnostic(&diagnostic);
        CHECK(output == expected && source_program_compile_attempts > attempts);
        CHECK(writer_stats(context).live_bytes == before.live_bytes);
        CHECK(xr_xir_compile_artifact_verify(occupied, NULL) == XR_XIR_OK);
    }
}
int main(void) {
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&writer_caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = writer_stats(&context);
    XrXirArtifact *legal = writer_legal(&context);
    for (unsigned variant = 1; variant <= 2; ++variant) {
        writer_bad_built(&context, legal, variant);
        writer_bad_packet(&context, legal, variant);
    }
    xr_xir_compile_artifact_free(legal);
    CHECK(writer_stats(&context).live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    puts("Legal named Built/Checked/writer full224B; two synthetic-name public admissions refused; private writer-only sizing remains OPEN");
    return 0;
}
