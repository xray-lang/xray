/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_ops.h - Xm opcode enumeration
 *
 * Generated from xisa/xm/ops.def by tools/xisagen/xisagen.c
 * To regenerate: tools/xisagen/xisagen xisa/xm/ops.def src/jit/xm_ops_gen.h
 */

#ifndef XM_OPS_H
#define XM_OPS_H

#include "xm_ops_gen.h"

/* Store-field packing helper (not generated — JIT-specific layout detail) */
#define XM_SF_PACK(tag, off) ((uint64_t) (tag) << 32 | (uint32_t) (off))

#endif  // XM_OPS_H
