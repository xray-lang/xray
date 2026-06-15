/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen.h - Xm → ARM64 machine code generation
 *
 * KEY CONCEPT:
 *   Translates Xm SSA instructions into ARM64 machine code.
 *   register allocation with spill support.
 *   Single-pass emit with deferred branch patching.
 *
 * RELATED MODULES:
 *   - xm.h: Xm data structures
 *   - xm_arm64.h: ARM64 instruction encoding
 *   - xm_code_alloc.h: executable memory allocation
 */

#ifndef XM_CODEGEN_H
#define XM_CODEGEN_H

#include "xm.h"
#include "xm_arm64.h"
#include "xm_code_alloc.h"
#include "../base/xmalloc.h"
#include "../base/xdefs.h"

/* ========== Runtime Safepoint Table ========== */

typedef enum {
    XM_SAFEPOINT_DEOPT = 1,
    XM_SAFEPOINT_OSR = 2,
    XM_SAFEPOINT_SUSPEND = 3,
} XmSafepointKind;

// Location of a value at deopt time (physical register or spill slot)
typedef enum {
    DEOPT_LOC_REG,        // value in a GP register (phys_reg = A64Reg)
    DEOPT_LOC_FP_REG,     // value in an FP register (phys_reg = A64FReg)
    DEOPT_LOC_SPILL,      // value in spill slot (spill_offset from SP)
    DEOPT_LOC_CONST_I64,  // compile-time i64 constant
    DEOPT_LOC_CONST_F64,  // compile-time f64 constant
    DEOPT_LOC_CONST_PTR,  // compile-time pointer constant
} XmDeoptLocKind;

// Per-slot live value entry in runtime safepoint metadata.
typedef struct {
    int16_t bc_slot;   // bytecode register index R[bc_slot]
    uint8_t type;      // XrRep (I64/F64/PTR/TAGGED)
    uint8_t loc_kind;  // XmDeoptLocKind
    uint8_t xr_tag;    // XrValue tag (0-15), or 0xFF=unknown
    uint8_t _pad;
    uint16_t vreg_idx;  // source vreg index (for vreg_runtime_tags[] lookup)
    union {
        uint8_t phys_reg;      // for LOC_REG / LOC_FP_REG
        int16_t spill_offset;  // for LOC_SPILL (byte offset from frame base)
        int64_t const_i64;     // for LOC_CONST_I64
        double const_f64;      // for LOC_CONST_F64
        void *const_ptr;       // for LOC_CONST_PTR
    } loc;
} XmLiveSlot;

// Single runtime-visible safepoint metadata entry.
typedef struct {
    uint8_t kind;  // XmSafepointKind
    uint8_t _pad;
    uint16_t id;                // deopt_id or suspend_id; 0 for OSR
    uint32_t land_pc;           // bytecode pc / loop header bytecode pc
    uint32_t code_offset;       // machine-code byte offset (OSR entry or suspend continuation)
    uint32_t block_id;          // OSR loop block id; UINT32_MAX otherwise
    uint32_t smap_id;           // suspend stack map id; UINT32_MAX otherwise
    int16_t result_bc_slot;     // suspend await result bytecode slot; -1 otherwise
    int32_t result_tag_offset;  // suspend result tag offset; -1 otherwise
    uint16_t nslots;            // number of live slot entries
    XmLiveSlot *slots;          // owned by this safepoint; NULL when nslots == 0
} XmSafepoint;

/* ========== GC Stack Map (compile-time bitmap for precise GC root scanning) ========== */

// XrStackMapEntry / XrStackMapTable / XR_STACK_MAP_MAGIC / XM_MAX_STACK_MAP_ENTRIES
// live in runtime/gc/xstackmap.h so GC owns the contract; JIT codegen produces
// tables which GC consumes.
#include "../runtime/gc/xstackmap.h"

/* ========== Codegen Result ========== */

typedef struct {
    void *code;          // pointer to executable code
    uint32_t code_size;  // size in bytes
    bool success;
    const char *error;  // error message if !success
    // Fast entry offset: byte offset from code start to fast prologue
    // (skip param loading, for register-passing cross-function calls).
    // Both ARM64 and x64 codegens return byte offsets.
    uint32_t fast_entry_offset;
    // Unified runtime safepoint table. Ownership transfers to XrProto; each
    // deopt safepoint owns its slots array.
    XmSafepoint *safepoints;
    uint32_t nsafepoints;
    uint32_t safepoint_cap;
    // GC stack map table (heap-allocated, transferred to XrProto)
    XrStackMapTable *stack_map;
    // Resume entry offset: byte offset from code start (0 = none).
    // Non-zero when function contains XM_SUSPEND points.
    uint32_t resume_entry_offset;
} XmCodegenResult;

static inline void xm_safepoints_free(XmSafepoint *safepoints, uint32_t count) {
    if (!safepoints)
        return;
    for (uint32_t i = 0; i < count; i++) {
        xr_free(safepoints[i].slots);
        safepoints[i].slots = NULL;
    }
    xr_free(safepoints);
}

static inline bool xm_codegen_result_add_safepoint(XmCodegenResult *result,
                                                   const XmSafepoint *safepoint) {
    XR_DCHECK(result != NULL, "add safepoint: NULL result");
    XR_DCHECK(safepoint != NULL, "add safepoint: NULL safepoint");
    if (result->nsafepoints >= result->safepoint_cap) {
        uint32_t new_cap = result->safepoint_cap ? result->safepoint_cap * 2u : 8u;
        while (new_cap <= result->nsafepoints)
            new_cap *= 2u;
        XmSafepoint *items =
            (XmSafepoint *) xr_realloc(result->safepoints, (size_t) new_cap * sizeof(XmSafepoint));
        if (!items)
            return false;
        result->safepoints = items;
        result->safepoint_cap = new_cap;
    }
    result->safepoints[result->nsafepoints] = *safepoint;
    result->nsafepoints++;
    return true;
}

static inline bool xm_codegen_result_add_osr_safepoint(XmCodegenResult *result, uint32_t block_id,
                                                       uint32_t bc_offset, uint32_t entry_offset) {
    XmSafepoint sp = {
        .kind = XM_SAFEPOINT_OSR,
        .id = 0,
        .land_pc = bc_offset,
        .code_offset = entry_offset,
        .block_id = block_id,
        .smap_id = UINT32_MAX,
        .result_bc_slot = -1,
        .result_tag_offset = -1,
        .nslots = 0,
        .slots = NULL,
    };
    return xm_codegen_result_add_safepoint(result, &sp);
}

static inline bool xm_codegen_result_add_suspend_safepoint(XmCodegenResult *result,
                                                           uint16_t suspend_id,
                                                           uint32_t cont_offset, uint32_t smap_id,
                                                           int16_t result_bc_slot,
                                                           int32_t result_tag_offset) {
    XmSafepoint sp = {
        .kind = XM_SAFEPOINT_SUSPEND,
        .id = suspend_id,
        .land_pc = UINT32_MAX,
        .code_offset = cont_offset,
        .block_id = UINT32_MAX,
        .smap_id = smap_id,
        .result_bc_slot = result_bc_slot,
        .result_tag_offset = result_tag_offset,
        .nslots = 0,
        .slots = NULL,
    };
    return xm_codegen_result_add_safepoint(result, &sp);
}

static inline const XmSafepoint *xm_safepoint_find_deopt(const XmSafepoint *safepoints,
                                                         uint32_t count, uint32_t deopt_id) {
    for (uint32_t i = 0; i < count; i++) {
        const XmSafepoint *sp = &safepoints[i];
        if (sp->kind == XM_SAFEPOINT_DEOPT && sp->id == deopt_id)
            return sp;
    }
    return NULL;
}

static inline const XmSafepoint *xm_safepoint_find_osr(const XmSafepoint *safepoints,
                                                       uint32_t count, uint32_t bc_pc) {
    for (uint32_t i = 0; i < count; i++) {
        const XmSafepoint *sp = &safepoints[i];
        if (sp->kind == XM_SAFEPOINT_OSR && sp->land_pc == bc_pc)
            return sp;
    }
    return NULL;
}

static inline uint32_t xm_safepoint_count_kind(const XmSafepoint *safepoints, uint32_t count,
                                               XmSafepointKind kind) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (safepoints[i].kind == kind)
            n++;
    }
    return n;
}

/* ========== API ========== */

// Generate ARM64 machine code from Xm function
// Uses the provided code allocator for executable memory
XR_FUNC XmCodegenResult xm_codegen_arm64(XmFunc *func, XmCodeAlloc *alloc);

// Generate x86-64 machine code from Xm function
// Uses the provided code allocator for executable memory
XR_FUNC XmCodegenResult xm_codegen_x64(XmFunc *func, XmCodeAlloc *alloc);

// Generate RISC-V 64 machine code from Xm function
// Uses the provided code allocator for executable memory
XR_FUNC XmCodegenResult xm_codegen_riscv64(XmFunc *func, XmCodeAlloc *alloc);

#endif  // XM_CODEGEN_H
