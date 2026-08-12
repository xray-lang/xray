/*
 * test_typed_frame_runtime_archive.c - Runtime-only typed frame link boundary
 */

#include "vm/xr_typed_frame.h"
#include "vm/xr_typed_dispatch.h"
#include <stdio.h>

int main(void) {
    XrTypedFrameLimits limits;
    xr_typed_frame_limits_default(&limits);
    XrFingerprint fingerprint = {{0}};
    XrTypedFrame *frame = (XrTypedFrame *) (uintptr_t) 1;
    int64_t result = 1;
    if (XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION != UINT32_C(11) ||
        XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK != XR_TARGET_REQUIRED_FAMILIES ||
        limits.max_arena_bytes != XR_TYPED_FRAME_MAX_ARENA_BYTES ||
        xr_typed_frame_create(NULL, &fingerprint, 0, &limits, &frame) !=
            XR_TYPED_FRAME_INVALID_ARGUMENT ||
        frame != NULL ||
        xr_typed_frame_cleanup(NULL) != XR_TYPED_FRAME_INVALID_ARGUMENT ||
        xr_typed_dispatch_execute_i64(NULL, &fingerprint, 0, &result) !=
            XR_TYPED_DISPATCH_INVALID_ARGUMENT ||
        result != 0) {
        fputs("runtime-only typed frame boundary failed\n", stderr);
        return 1;
    }
    xr_typed_frame_free(NULL);
    puts("runtime-only typed frame boundary passed");
    return 0;
}
