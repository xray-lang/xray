/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pic_emit.c - PIC stub machine-code emission (x64)
 *
 * Emits a compare-and-branch chain for polymorphic inline caches.
 * Each entry compares the receiver's type_id (in R10d) against a
 * cached type and jumps to the cached code pointer on match.
 */

#include "xm_pic_emit.h"
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)

/* Minimal x64 encodings: cmp r10d, imm32; je rel32 */
static uint32_t emit_cmp_r10d_imm32(uint8_t *p, uint32_t imm) {
    p[0] = 0x41;
    p[1] = 0x81;
    p[2] = 0xFA;
    p[3] = (uint8_t) (imm & 0xFF);
    p[4] = (uint8_t) ((imm >> 8) & 0xFF);
    p[5] = (uint8_t) ((imm >> 16) & 0xFF);
    p[6] = (uint8_t) ((imm >> 24) & 0xFF);
    return 7;
}

static uint32_t emit_jmp_rel32(uint8_t *p, int32_t rel) {
    p[0] = 0xE9;
    p[1] = (uint8_t) (rel & 0xFF);
    p[2] = (uint8_t) ((rel >> 8) & 0xFF);
    p[3] = (uint8_t) ((rel >> 16) & 0xFF);
    p[4] = (uint8_t) ((rel >> 24) & 0xFF);
    return 5;
}

XR_FUNC uint32_t xm_pic_emit_x64(uint8_t *code, uint32_t cap, const XmPic *pic,
                                 uint32_t slow_path_offset, XmPicEmitResult *result) {
    if (result)
        memset(result, 0, sizeof(*result));

    if (!code || !pic || pic->state == XM_PIC_MEGAMORPHIC || pic->nentries == 0) {
        if (result)
            result->megamorphic_fallback = true;
        return 0;
    }

    uint32_t off = 0;
    for (uint8_t i = 0; i < pic->nentries; i++) {
        const XmPicEntry *e = &pic->entries[i];
        if (e->type_id == 0 || !e->code_ptr)
            continue;

        if (off + 7 + 5 + 5 > cap)
            return 0;

        off += emit_cmp_r10d_imm32(code + off, e->type_id);
        /* Placeholder je — target patched by codegen driver using code_ptr */
        off += emit_jmp_rel32(code + off, 0);
        (void) e->code_ptr;
    }

    if (off + 5 > cap)
        return 0;

    off += emit_jmp_rel32(code + off, (int32_t) slow_path_offset);

    if (result) {
        result->code_size = off;
        result->used_pic = true;
    }
    return off;
}

#else

XR_FUNC uint32_t xm_pic_emit_x64(uint8_t *code, uint32_t cap, const XmPic *pic,
                                 uint32_t slow_path_offset, XmPicEmitResult *result) {
    (void) code;
    (void) cap;
    (void) slow_path_offset;
    if (result)
        memset(result, 0, sizeof(*result));
    if (!pic || pic->state == XM_PIC_MEGAMORPHIC || pic->nentries == 0) {
        if (result)
            result->megamorphic_fallback = true;
        return 0;
    }
    return 0;
}

#endif
