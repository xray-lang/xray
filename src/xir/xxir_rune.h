/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_rune.h - Allocation-free UTF-8 of one admitted Unicode scalar
 *
 * KEY CONCEPT:
 *   Rune payload validation precedes every byte; NUL has explicit length one.
 */
#ifndef XXIR_RUNE_H
#define XXIR_RUNE_H
#include "xxir_value.h"
static inline size_t xr_xir_rune_utf8(int64_t payload, char *output) {
    if (!output || !xr_xir_rune_payload_valid(payload)) return 0;
    uint32_t cp=(uint32_t)payload;
    if (cp<0x80) { output[0]=(char)cp; return 1; }
    if (cp<0x800) {
        output[0]=(char)(0xc0u|(cp>>6)); output[1]=(char)(0x80u|(cp&0x3fu)); return 2;
    }
    if (cp<0x10000) {
        output[0]=(char)(0xe0u|(cp>>12)); output[1]=(char)(0x80u|((cp>>6)&0x3fu));
        output[2]=(char)(0x80u|(cp&0x3fu)); return 3;
    }
    output[0]=(char)(0xf0u|(cp>>18)); output[1]=(char)(0x80u|((cp>>12)&0x3fu));
    output[2]=(char)(0x80u|((cp>>6)&0x3fu)); output[3]=(char)(0x80u|(cp&0x3fu)); return 4;
}
#endif // XXIR_RUNE_H
