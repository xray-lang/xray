/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_blueprint.h - Compiler-generated JIT metadata (loop liveness for OSR)
 *
 * KEY CONCEPT:
 *   Loop live maps provide precise OSR slot lists so OSR stubs know which
 *   bytecode registers to load from the interpreter's register array.
 *
 * RELATED MODULES:
 *   - xcompiler.c: generates Blueprint during bytecode compilation
 *   - xm_codegen.c: consumes loop live maps for OSR stub generation
 */

#ifndef XM_BLUEPRINT_H
#define XM_BLUEPRINT_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

/* ========== Loop Live Map ========== */

// Max live slots per loop header (matches XM_MAX_OSR_SLOTS)
#define XR_BP_MAX_LOOP_LIVE 32

// Per-slot liveness entry at a loop header
typedef struct {
    uint8_t slot;  // bytecode register index
    uint8_t tag;   // XR_TAG_* at this point
} XrBpLiveSlot;

// Live variable map for a single loop header
typedef struct {
    uint16_t header_pc;                      // bytecode PC of loop header
    uint8_t nlive;                           // number of live slots
    XrBpLiveSlot live[XR_BP_MAX_LOOP_LIVE];  // live slot entries
} XrBpLoopInfo;

/* ========== Complete Blueprint ========== */

typedef struct XrBlueprint {
    // Per-loop-header live variable maps (for OSR)
    XrBpLoopInfo *loops;  // [nloops]
    uint16_t nloops;
} XrBlueprint;

/* ========== API ========== */

struct XrProto;

// Generate Blueprint from compiled proto metadata.
// Called from xr_compiler_end() after bytecode + metadata generation.
// Returns NULL if proto has no code or allocation fails.
XR_FUNC XrBlueprint *xr_blueprint_generate(struct XrProto *proto);

// Free Blueprint and all owned memory.
XR_FUNC void xr_blueprint_free(XrBlueprint *bp);

#endif  // XM_BLUEPRINT_H
