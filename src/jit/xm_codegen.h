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
#include "../base/xdefs.h"

/* ========== OSR Entry Point ========== */

#define XM_MAX_OSR_ENTRIES 8

// OSR entry metadata consumed at runtime: the loop-header match key (bc_offset)
// and the stub address (entry_offset). The bc_slot -> phys_reg restore is baked
// into the stub machine code, so no per-slot table is kept here.
typedef struct {
    uint32_t block_id;      // loop header block index
    uint32_t bc_offset;     // bytecode PC of loop header (for VM matching)
    uint32_t entry_offset;  // byte offset of OSR entry stub from code start
} XmOsrEntry;

typedef struct {
    uint32_t cont_offset;
    uint32_t smap_id;
    int16_t result_bc_slot;
    int32_t result_tag_offset;
} XmSuspendEntry;

/* ========== Runtime Deopt Table ========== */

// Location of a value at deopt time (physical register or spill slot)
typedef enum {
    DEOPT_LOC_REG,        // value in a GP register (phys_reg = A64Reg)
    DEOPT_LOC_FP_REG,     // value in an FP register (phys_reg = A64FReg)
    DEOPT_LOC_SPILL,      // value in spill slot (spill_offset from SP)
    DEOPT_LOC_CONST_I64,  // compile-time i64 constant
    DEOPT_LOC_CONST_F64,  // compile-time f64 constant
    DEOPT_LOC_CONST_PTR,  // compile-time pointer constant
} XmDeoptLocKind;

// Per-slot entry in runtime deopt table
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
} XmRtDeoptSlot;

// One deopt point in the runtime table
typedef struct {
    uint32_t bc_pc;     // bytecode PC to resume interpreter at
    uint16_t nslots;    // number of live slot entries
    uint16_t deopt_id;  // index (matches codegen deopt stub id)
    // Points into the single deopt-table block (the XmRtDeoptEntry array is
    // immediately followed by every entry's slots). The whole table frees with
    // one xr_free of the entry array; never free this pointer on its own.
    XmRtDeoptSlot *slots;
} XmRtDeoptEntry;

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
    // OSR entry points for loop headers
    XmOsrEntry osr_entries[XM_MAX_OSR_ENTRIES];
    uint32_t nosr;
    // Runtime deopt table: one heap block holding the XmRtDeoptEntry array
    // immediately followed by every entry's slots (each entry->slots points
    // into this block). Ownership transfers to XrProto.deopt_table; a single
    // xr_free releases the whole table. NULL when ndeopt == 0.
    XmRtDeoptEntry *deopt_entries;
    uint32_t ndeopt;
    // GC stack map table (heap-allocated, transferred to XrProto)
    XrStackMapTable *stack_map;
    XmSuspendEntry suspend_entries[XM_MAX_SUSPEND_ENTRIES];
    uint32_t nsuspend;
    // Resume entry offset: byte offset from code start (0 = none).
    // Non-zero when function contains XM_SUSPEND points.
    uint32_t resume_entry_offset;
} XmCodegenResult;

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
