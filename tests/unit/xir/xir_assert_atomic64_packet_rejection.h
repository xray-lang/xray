/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_atomic64_packet_rejection.h - Complete prior assertion packet refusal
 */
#ifndef XIR_ASSERT_ATOMIC64_PACKET_REJECTION_H
#define XIR_ASSERT_ATOMIC64_PACKET_REJECTION_H
static void assert_previous63_packet_rejected(const uint8_t *bytes, size_t length) {
    CHECK(length >= 64 && bytes[8] == 24 && bytes[12] == 63);
    size_t attempts = source_program_compile_attempts;
    size_t blocks = source_program_compile_live, allocated = source_program_compile_bytes;
    size_t live = runtime_live, runtime_allocated = runtime_bytes;
    XrXirArtifact *empty = NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context, bytes, length, &empty, NULL) ==
        XR_XIR_BAD_STRUCTURE && !empty);
    XrXirArtifact *sentinel = (XrXirArtifact *)(uintptr_t)1;
    XrXirArtifact *occupied = sentinel;
    CHECK(xr_xir_compile_checked_read(assert_compile_context, bytes, length, &occupied, NULL) ==
        XR_XIR_BAD_STRUCTURE && occupied == sentinel);
    CHECK(source_program_compile_attempts == attempts && source_program_compile_live == blocks &&
        source_program_compile_bytes == allocated && runtime_live == live && runtime_bytes == runtime_allocated);
    puts("Complete prior24/63 assertion packet rejected before allocation; empty/occupied output preserved PASS");
}
#endif
