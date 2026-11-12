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

static void view_cold_checked_sources(const char *programs) {
    char directory[2048];
    int written = snprintf(directory, sizeof(directory), "%s/../../fixtures/lir_cli/array_lower", programs);
    CHECK(written > 0 && (size_t)written < sizeof(directory));
    const char *names[] = {"view_return", "view_module_alias", "view_phi", "view_conditional_cleanup"};
    for (uint32_t n = 0; n < sizeof(names) / sizeof(*names); ++n) {
        char *text = read_file(directory, names[n], "xr");
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
        uint32_t intrinsic;
        OK(xr_lir_view_operation(output.module, XR_LIR_VIEW_OP_NEW, &intrinsic));
        XrLirIntrinsic entry;
        OK(xr_lir_intrinsic_view(output.module, intrinsic, &entry));
        CHECK(entry.view_operation == XR_LIR_VIEW_OP_NEW && entry.parameter_count == 6);
        xr_lir_lowered_free(&output);
    }
}
