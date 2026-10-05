/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_float_transport_fixture.h - Typed floating payload transport
 *
 * KEY CONCEPT:
 *   COPY has independent raw-bit expectations, separate from numeric kernels.
 */
#ifndef XIR_FLOAT_TRANSPORT_FIXTURE_H
#define XIR_FLOAT_TRANSPORT_FIXTURE_H
static XrXirArtifact *float_transport_fixture(const XrXirCompileContext *context) {
    const XrXirType wide = XR_XIR_F64, narrow = XR_XIR_F32;
    const XrXirBlock block = {0, 2, 0, 0};
    const XrXirInstruction wide_ops[] = {
        {XR_XIR_COPY, XR_XIR_F64, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}
    };
    const XrXirInstruction narrow_ops[] = {
        {XR_XIR_COPY, XR_XIR_F32, {0}, {0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1}, {0}, 0, {0}}
    };
    const XrXirFunction functions[] = {
        {"wide", 4, &wide, 1, XR_XIR_F64, &block, 1, wide_ops, 2, NULL, 0},
        {"narrow", 6, &narrow, 1, XR_XIR_F32, &block, 1, narrow_ops, 2, NULL, 0}
    };
    const XrXirModule built = {XR_XIR_BUILT, functions, 2, NULL, NULL, NULL,
        NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *decoded = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_check(context, &built, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(decoded, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(checked);
    return lowered;
}
#endif // XIR_FLOAT_TRANSPORT_FIXTURE_H
