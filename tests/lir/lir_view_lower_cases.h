/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_view_lower_cases.h - Cold Checked view contracts and receiving owners
 */
#include "xir/xxir_checked.h"
#include "lir/xlir_view.h"
#include "lir/xlir_string.h"

static void view_cold_checked_sources(const char *programs) {
    char directory[2048];
    int written = snprintf(directory, sizeof(directory), "%s/../../fixtures/lir_cli/array_lower", programs);
    CHECK(written > 0 && (size_t)written < sizeof(directory));
    const char *names[] = {"view_return", "view_module_alias", "view_phi", "view_conditional_cleanup",
        "view_copy_argument", "view_copy_unused_parameter", "string_append_values", "string_append_ref",
        "string_join", "string_append_loop", "string_parameter_loop", "panic_nullable_string_append", "readonly_byte_slice"};
    for (uint32_t n = 0; n < sizeof(names) / sizeof(*names); ++n) {
        char demo_directory[2048];
        written = snprintf(demo_directory, sizeof(demo_directory), "%s/../../demos/07-advanced", programs);
        CHECK(written > 0 && (size_t)written < sizeof(demo_directory));
        bool original_u8 = !strcmp(names[n], "readonly_byte_slice");
        char *text = read_file(original_u8 ? demo_directory : directory, names[n], "xr");
        if (!strcmp(names[n],"string_append_ref")) {
            source_fixture_rejected(text,
                "E0382: ref of module binding `published` conflicts with access by `actual` via `actual`");
            xr_free(text);
            continue;
        }
        SourceFixture fixture = source_fixture(text);
        xr_free(text);
        XrXirCheckedPacket packet = {0};
        CHECK(xr_xir_compile_checked_write(fixture.source.checked, &packet, NULL) == XR_XIR_OK);
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrXirCompileContext receiver = {0};
        CHECK(xr_compile_resources_new(&limits, &receiver.resources) == XR_COMPILE_RESOURCE_OK);
        receiver.limits = xr_xir_compile_default_limits();
        XrXirArtifact *decoded = NULL, *closed = NULL;
        CHECK(xr_xir_compile_checked_read(&receiver, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
        xr_xir_compile_checked_packet_free(&packet);
        CHECK(xr_xir_compile_specialize(decoded, &closed, NULL, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(decoded);
        XrLirLowered output = {0};
        XrLirLowerRequest request = {closed, fixture.source.snapshot, false};
        OK(xr_lir_lower_program_checked(&request, &output, NULL));
        xr_xir_compile_artifact_free(closed);
        source_fixture_free(&fixture);
        xr_compile_resources_release(receiver.resources);
        OK(xr_lir_verify(output.module, NULL));
        if (!strcmp(names[n],"string_parameter_loop")) {
            CHECK(!xr_lir_intrinsic_count(output.module));
            xr_lir_lowered_free(&output);
            continue;
        }
        if (!strncmp(names[n],"string_",7) || !strcmp(names[n],"panic_nullable_string_append")) {
            uint32_t string_operation=0;
            OK(xr_lir_string_operation(output.module,XR_LIR_STRING_OP_BORROW_CELL,&string_operation));
            XrLirIntrinsic string_entry={0};
            OK(xr_lir_intrinsic_view(output.module,string_operation,&string_entry));
            CHECK(string_entry.operation == XR_LIR_STRING_OP_BORROW_CELL &&
                string_entry.result == XR_LIR_STRING_REF && string_entry.parameter_count == 1);
            xr_lir_lowered_free(&output);
            continue;
        }
        uint32_t intrinsic;
        XrLirStatus role = xr_lir_slice_operation(output.module, XR_LIR_SLICE_OP_NEW,
            original_u8 ? XR_LIR_U8 : XR_LIR_I64, XR_LIR_ARRAY_CELL, &intrinsic);
        if (!strcmp(names[n], "view_copy_unused_parameter")) {
            CHECK(role == XR_LIR_UNSUPPORTED);
        } else {
            OK(role);
            XrLirIntrinsic entry;
            OK(xr_lir_intrinsic_view(output.module, intrinsic, &entry));
            XrLirSliceIntrinsic shape;
            OK(xr_lir_slice_intrinsic_view(output.module, intrinsic, &shape));
            CHECK(shape.operation == XR_LIR_SLICE_OP_NEW && entry.parameter_count == 3 &&
                entry.parameters[0] == XR_LIR_ARRAY_CELL && entry.parameters[1] == XR_LIR_I64 && entry.parameters[2] == XR_LIR_I64);
            for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
                XrLirFunction function = {0}; XrLirFunctionView view;
                OK(xr_lir_function_at(output.module, f, &function)); OK(xr_lir_function_view(output.module, function, &view));
                for (uint32_t b = 0; b < view.block_count; ++b) {
                    XrLirBlock block = {0}; XrLirBlockView body;
                    OK(xr_lir_block_at(output.module, function, b, &block)); OK(xr_lir_block_view(output.module, block, &body));
                    for (uint32_t i = 0; i < body.instruction_count; ++i) {
                        XrLirInstructionView instruction; OK(xr_lir_instruction_view(output.module, block, i, &instruction));
                        CHECK(instruction.op != XR_LIR_VIEW_FRAME_ENTER && instruction.op != XR_LIR_VIEW_FRAME_LEAVE &&
                            instruction.type != XR_LIR_VIEW_CELL && instruction.type != XR_LIR_SLICE_I64);
                    }
                }
            }
        }
        xr_lir_lowered_free(&output);
    }
}
