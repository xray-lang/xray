/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * checked_semantic_scalar.inc.c - Authentic frames with invalid scalar roles
 *
 * KEY CONCEPT:
 *   Correct framing reaches allocation and semantic verification. Failure
 *   releases the rejected graph while preserving a separate real owner.
 */
#include "base/xsha256.h"
#include "checked_semantic_scalar_cases.h"

_Static_assert(XR_XIR_I8 == 5 && XR_XIR_I32 == 7 && XR_XIR_U8 == 8 &&
    XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33 && XR_XIR_OP_COUNT == 148,
    "Independent scalar model ordinals");
_Static_assert(sizeof(semantic_scalar_cases) / sizeof(semantic_scalar_cases[0]) == 71,
    "Complete named scalar matrix");

static void semantic_frame(const uint8_t bytes[224]) {
    CHECK(!memcmp(bytes, "XRCHK\0\0\0", 8));
    const uint8_t identity[24] = {25,0,0,0,67,0,0,0,2,0,0,0,0,0,0,0,160,0,0,0,0,0,0,0};
    CHECK(!memcmp(bytes + 8, identity, sizeof(identity)));
    uint8_t digest[32];
    XrSHA256Context sha;
    xr_sha256_init(&sha);
    xr_sha256_update(&sha, bytes, 32);
    xr_sha256_update(&sha, bytes + 64, 160);
    xr_sha256_final(&sha, digest);
    CHECK(!memcmp(digest, bytes + 32, sizeof(digest)));
}

static void semantic_positive(const SemanticScalarCase *test, XrXirArtifact *output) {
    const XrXirModule *module = xr_xir_compile_artifact_module(output);
    CHECK(module && module->function_count == 1 && module->stage == XR_XIR_CHECKED);
    const XrXirFunction *function = &module->functions[0];
    CHECK(function->result == test->result && !function->parameter_count);
    CHECK(function->instruction_count == 2 && function->instructions[0].op == XR_XIR_CONST_INT);
    CHECK(function->instructions[0].type == test->constant_type);
    CHECK(function->instructions[0].immediate == test->immediate && function->instructions[1].op == XR_XIR_RETURN);
    CHECK(xr_xir_compile_artifact_verify(output, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(output, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(test->bytes) && !memcmp(packet.bytes, test->bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}

static void semantic_scalar_cases_run(const XrXirCompileContext *context, XrXirArtifact *owner) {
    size_t accepted = 0, rejected = 0;
    for (size_t index = 0; index < 71; ++index) {
        const SemanticScalarCase *test = &semantic_scalar_cases[index];
        semantic_frame(test->bytes);
        uint8_t input[224];
        memcpy(input, test->bytes, sizeof(input));
        for (unsigned occupied = 0; occupied < 2; ++occupied) {
            XrXirArtifact *output = occupied ? owner : NULL, *before_output = output;
            XrCompileResourceStats before = {0}, after = {0};
            CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
            size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
            size_t attempts = source_program_compile_attempts;
            XrXirDiagnostic diagnostic = {0};
            XrXirStatus status = xr_xir_compile_checked_read(context, input, sizeof(input), &output, &diagnostic);
            printf("semantic-case name=%s occupied=%u status=%u expected=%u f=%u b=%u i=%u reason=%u\n",
                test->name, occupied, status, test->expected, diagnostic.function, diagnostic.block,
                diagnostic.instruction, diagnostic.reason);
            CHECK(status == test->expected && diagnostic.status == status);
            CHECK(source_program_compile_attempts > attempts);
            if (status == XR_XIR_OK) {
                CHECK(output && output != owner && output != before_output);
                semantic_positive(test, output);
                xr_xir_compile_artifact_free(output);
                ++accepted;
            } else {
                CHECK(output == before_output);
                ++rejected;
            }
            CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
            CHECK(after.allocated_bytes > before.allocated_bytes && after.allocation_count > before.allocation_count);
            CHECK(after.live_bytes == before.live_bytes && after.work > before.work);
            CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
            CHECK(!memcmp(input, test->bytes, sizeof(input)));
            owner_preserved(owner);
        }
    }
    CHECK(accepted == 10 && rejected == 132);
    puts("semantic-scalar packets=71 accepted=10 rejected=132 correct-digest=1 decoded-allocation=1 "
        "occupied=REAL_OWNER empty=1 input-preserved=1 owner-preserved=1 physical-refund=1 result=PASS");
}
