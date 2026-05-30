/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pic_emit.h - Machine-code PIC stub emission
 */

#ifndef XM_PIC_EMIT_H
#define XM_PIC_EMIT_H

#include "xm_pic.h"
#include "../base/xdefs.h"
#include <stdint.h>

typedef struct XmPicEmitResult {
    uint32_t code_size;
    bool used_pic;
    bool megamorphic_fallback;
} XmPicEmitResult;

/* Emit a PIC dispatch stub for x64 into code buffer at *cursor.
 * Compares receiver type against pic entries and jumps to code_ptr
 * or falls through to slow_path when no match.
 *
 * Returns bytes written; 0 on failure or megamorphic state. */
XR_FUNC uint32_t xm_pic_emit_x64(uint8_t *code, uint32_t cap, const XmPic *pic,
                                 uint32_t slow_path_offset, XmPicEmitResult *result);

#endif /* XM_PIC_EMIT_H */
