/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * checked_f64_wire.inc.c - Original binary64 bits at the Checked wire boundary
 *
 * KEY CONCEPT:
 *   Constant encoding admits canonical NaN only. Bitwise transport of arbitrary
 *   runtime NaNs is a separate responsibility and is not proved by rejection.
 */
#include "checked_f64_wire_cases.h"

_Static_assert(XR_XIR_F64 == 13 && XR_XIR_CONST_FLOAT == 63,
    "Independent binary64 wire model");
_Static_assert(sizeof(f64_wire_cases) / sizeof(f64_wire_cases[0]) == 21,
    "Original binary64 corpus and controls");

static void f64_wire_positive(const F64WireCase *test, XrXirArtifact *output) {
    const XrXirModule *module = xr_xir_compile_artifact_module(output);
    CHECK(module && module->function_count == 1 && module->stage == XR_XIR_CHECKED);
    const XrXirFunction *function = &module->functions[0];
    CHECK(function->result == XR_XIR_F64 && !function->parameter_count);
    CHECK(function->instruction_count == 2 && function->instructions[0].op == XR_XIR_CONST_FLOAT);
    CHECK(function->instructions[0].type == XR_XIR_F64);
    CHECK((uint64_t)function->instructions[0].immediate == test->bits);
    CHECK(function->instructions[1].op == XR_XIR_RETURN);
    CHECK(xr_xir_compile_artifact_verify(output, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(output, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(test->bytes) && !memcmp(packet.bytes, test->bytes, packet.length));
    xr_xir_compile_checked_packet_free(&packet);
}

static void f64_wire_cases_run(const XrXirCompileContext *context, XrXirArtifact *owner) {
    size_t accepted = 0, rejected = 0;
    for (size_t index = 0; index < 21; ++index) {
        const F64WireCase *test = &f64_wire_cases[index];
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
            printf("f64-wire name=%s occupied=%u status=%u expected=%u\n",
                test->name, occupied, status, test->expected);
            CHECK(status == test->expected && diagnostic.status == status);
            CHECK(source_program_compile_attempts > attempts);
            if (status == XR_XIR_OK) {
                CHECK(output && output != owner && output != before_output);
                f64_wire_positive(test, output);
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
    CHECK(accepted == 16 && rejected == 26);
    puts("f64-wire packets=21 accepted=16 rejected=26 original-bits=10 correct-digest=1 "
        "occupied=REAL_OWNER empty=1 input-preserved=1 owner-preserved=1 physical-refund=1 result=PASS");
}
