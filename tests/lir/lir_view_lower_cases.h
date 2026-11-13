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
        "string_join", "string_append_loop", "string_parameter_loop", "panic_nullable_string_append"};
    for (uint32_t n = 0; n < sizeof(names) / sizeof(*names); ++n) {
        char *text = read_file(directory, names[n], "xr");
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
        XrLirStatus role = xr_lir_view_operation(output.module, XR_LIR_VIEW_OP_NEW, &intrinsic);
        if (!strcmp(names[n], "view_copy_unused_parameter")) {
            CHECK(role == XR_LIR_UNSUPPORTED);
        } else {
            OK(role);
            XrLirIntrinsic entry;
            OK(xr_lir_intrinsic_view(output.module, intrinsic, &entry));
            CHECK(entry.operation == XR_LIR_VIEW_OP_NEW && entry.parameter_count == 6);
        }
        xr_lir_lowered_free(&output);
    }
}
