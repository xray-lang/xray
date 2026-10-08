/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_checked_hostile_frame.c - Exact bounded reader framing
 *
 * KEY CONCEPT:
 *   Invalid framing publishes no owner, allocates no compiler memory, and
 *   preserves a real occupied artifact and every caller input byte.
 */
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "semantic67_named/semantic67_named_cases.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u,
    "Exact current Checked framing");
_Static_assert(sizeof(named_packet_67_0) == 224, "Independent scalar packet extent");

static size_t caller_buffers, rejected_reads;

static void owner_preserved(const XrXirArtifact *owner) {
    const Semantic67NamedVector *v = &semantic67_named_vectors[0];
    const XrXirModule *module = xr_xir_compile_artifact_module(owner);
    CHECK(module && module->stage == XR_XIR_CHECKED && module->function_count == 1);
    const XrXirFunction *function = &module->functions[0];
    CHECK(function->name_length == 4 && !memcmp(function->name, "main", 4));
    CHECK(function->result == XR_XIR_I64 && !function->parameter_count);
    CHECK(function->instruction_count == 2 && function->instructions[0].op == XR_XIR_CONST_INT);
    CHECK(function->instructions[0].type == XR_XIR_I64 && function->instructions[0].immediate == 42);
    CHECK(function->instructions[1].op == XR_XIR_RETURN);
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == v->current_size && !memcmp(packet.bytes, v->current, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(!packet.bytes && !packet.length);
}

static void rejection(const XrXirCompileContext *context, XrXirArtifact *owner,
                      const uint8_t *data, size_t length, const char *family,
                      size_t ordinal, XrXirStatus expected) {
    uint8_t *input = NULL;
    if (data) {
        /* The actual heap extent equals the declared nonempty input extent.
         * Sanitizers can therefore detect reads beyond a truncated prefix. */
        input = xr_malloc(length ? length : 1);
        CHECK(input);
        ++caller_buffers;
        if (length) memcpy(input, data, length);
        else input[0] = UINT8_C(0xa5);
    }
    for (unsigned occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *output = occupied ? owner : NULL;
        XrXirArtifact *before_output = output;
        XrCompileResourceStats before = {0}, after = {0};
        CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
        size_t attempts = source_program_compile_attempts;
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, length, &output, &diagnostic);
        printf("frame-reject family=%s ordinal=%zu occupied=%u length=%zu status=%u expected=%u\n",
            family, ordinal, occupied, length, status, expected);
        CHECK(status == expected && diagnostic.status == expected && output == before_output);
        CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.allocation_count == before.allocation_count && after.allocated_bytes == before.allocated_bytes);
        CHECK(after.live_bytes == before.live_bytes && after.peak_bytes == before.peak_bytes);
        CHECK(after.work >= before.work && source_program_compile_attempts == attempts);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
        if (input) CHECK(length ? !memcmp(input, data, length) : input[0] == UINT8_C(0xa5));
        /* This verification is outside the observed rejection interval. */
        owner_preserved(owner);
        ++rejected_reads;
    }
    if (input) { xr_free(input); CHECK(caller_buffers); --caller_buffers; }
}

#include "checked_semantic_scalar.inc.c"
#include "checked_f64_wire.inc.c"

int main(void) {
    const Semantic67NamedVector *v = &semantic67_named_vectors[0];
    CHECK(v->index == 0 && v->reader_positive && v->current_size == 224);
    CHECK(!source_program_compile_live && !source_program_compile_bytes && !caller_buffers);
    const XrXirCompileContext *context = source_program_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, v->current, v->current_size, &owner, NULL) == XR_XIR_OK);
    owner_preserved(owner);
    for (size_t length = 0; length < v->current_size; ++length)
        rejection(context, owner, v->current, length, "truncated", length, XR_XIR_BAD_STRUCTURE);
    uint8_t mutated[225];
    for (size_t byte = 0; byte < v->current_size; ++byte) {
        memcpy(mutated, v->current, v->current_size);
        mutated[byte] ^= UINT8_C(1);
        XrXirStatus expected = byte >= 16 && byte < 20 ? XR_XIR_BAD_STAGE : XR_XIR_BAD_STRUCTURE;
        rejection(context, owner, mutated, v->current_size,
            byte < 32 ? "header" : byte < 64 ? "digest" : "body-integrity", byte, expected);
    }
    memcpy(mutated, v->current, v->current_size);
    mutated[v->current_size] = UINT8_C(0xa5);
    rejection(context, owner, mutated, sizeof(mutated), "trailing-byte", 0, XR_XIR_BAD_STRUCTURE);
    rejection(context, owner, NULL, v->current_size, "null-input", 0, XR_XIR_BAD_STRUCTURE);
    memcpy(mutated, v->current, v->current_size);
    memset(mutated + 24, 0xff, 8);
    rejection(context, owner, mutated, v->current_size, "max-payload", 0, XR_XIR_BAD_STRUCTURE);
    CHECK(rejected_reads == 902 && !caller_buffers);
    semantic_scalar_cases_run(context, owner);
    f64_wire_cases_run(context, owner);
    owner_preserved(owner);
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("hostile-frame cases=451 reads=%zu truncated=224 mutated=224 other=3 "
        "occupied=REAL_OWNER empty=1 input-preserved=1 owner-roundtrip=1 rejection-allocations=0 "
        "caller-buffers=0 compiler-physical=0/0 observer-table=0 result=PASS\n", rejected_reads);
    return 0;
}
