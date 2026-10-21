/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_encode.c - Checked fixed-width instruction encoding.
 *
 * KEY CONCEPT:
 *   Check before narrowing. A rejected operand must never become a different
 *   valid instruction by truncating its high bits.
 */

#include "xrbc.h"
#include "base/xchecks.h"

XR_STATIC_ASSERT(XR_RBC_OP_COUNT <= 256, "opcodes must fit their eight-bit field");

typedef enum {
    XR_FMT_NONE,
    XR_FMT_A,
    XR_FMT_AB,
    XR_FMT_ABC,
    XR_FMT_ABX,
    XR_FMT_ASBX,
    XR_FMT_SJ
} XrRbcFormat;

static const XrRbcFormat xr_rbc_formats[] = {
#define XR_RBC_FORMAT(name, format) XR_FMT_##format,
    XR_RBC_OPS(XR_RBC_FORMAT)
#undef XR_RBC_FORMAT
};

XR_FUNC bool xr_rbc_encode(uint32_t *out, XrRbcOperands o) {
    if (!out || (unsigned) o.op >= XR_RBC_OP_COUNT)
        return false;
    XrRbcFormat format = xr_rbc_formats[o.op];
    uint32_t payload = 0;
    if (format == XR_FMT_SJ) {
        if (o.a || o.b || o.c || o.immediate < XR_RBC_JUMP_MIN || o.immediate > XR_RBC_JUMP_MAX)
            return false;
        payload = ((uint32_t) o.immediate & UINT32_C(0xffffff)) << 8;
    } else if (format == XR_FMT_NONE) {
        if (o.a || o.b || o.c || o.immediate)
            return false;
    } else {
        if (o.a >= XR_RBC_REGISTER_COUNT_MAX)
            return false;
        payload = o.a << 8;
        if (format == XR_FMT_ABX || format == XR_FMT_ASBX) {
            int64_t low = format == XR_FMT_ABX ? 0 : INT16_MIN;
            int64_t high = format == XR_FMT_ABX ? UINT16_MAX : INT16_MAX;
            if (o.b || o.c || o.immediate < low || o.immediate > high)
                return false;
            payload |= ((uint32_t) o.immediate & UINT32_C(0xffff)) << 16;
        } else {
            if (o.immediate || o.b >= XR_RBC_REGISTER_COUNT_MAX ||
                o.c >= XR_RBC_REGISTER_COUNT_MAX || (format == XR_FMT_A && o.b) ||
                (format != XR_FMT_ABC && o.c))
                return false;
            payload |= o.b << 16 | o.c << 24;
        }
    }
    *out = (uint32_t) o.op | payload;
    return true;
}

XR_FUNC bool xr_rbc_jump(uint32_t *out, size_t from, size_t to, size_t code_count) {
    if (from >= code_count || to >= code_count)
        return false;
    int64_t offset;
    if (to > from) {
        size_t distance = to - from - 1;
        if (distance > XR_RBC_JUMP_MAX)
            return false;
        offset = (int64_t) distance;
    } else {
        size_t distance = from - to;
        if (distance >= (size_t) -XR_RBC_JUMP_MIN)
            return false;
        offset = -(int64_t) distance - 1;
    }
    return xr_rbc_encode(out, (XrRbcOperands) {XR_RBC_JMP, 0, 0, 0, offset});
}
