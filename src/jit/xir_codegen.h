/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen.h - XIR → ARM64 machine code generation
 *
 * KEY CONCEPT:
 *   Translates XIR SSA instructions into ARM64 machine code.
 *   register allocation with spill support.
 *   Single-pass emit with deferred branch patching.
 *
 * RELATED MODULES:
 *   - xir.h: XIR data structures
 *   - xir_arm64.h: ARM64 instruction encoding
 *   - xir_code_alloc.h: executable memory allocation
 */

#ifndef XIR_CODEGEN_H
#define XIR_CODEGEN_H

#include "xir.h"
#include "xir_arm64.h"
#include "xir_code_alloc.h"
#include "../base/xdefs.h"

/* ========== OSR Entry Point ========== */

#define XIR_MAX_OSR_ENTRIES  8
#define XIR_MAX_OSR_SLOTS   32

typedef struct {
    uint32_t block_id;           // loop header block index
    uint32_t bc_offset;          // bytecode PC of loop header (for VM matching)
    uint32_t entry_offset;       // byte offset of OSR entry stub from code start
    uint16_t nslots;             // number of live register slots
    struct {
        int16_t bc_slot;         // bytecode register slot (-1 = unmapped)
        uint8_t phys_reg;        // A64Reg physical register to load into
        uint8_t type;            // XIR type (I64 or F64)
    } slots[XIR_MAX_OSR_SLOTS];
} XirOsrEntry;

/* ========== Runtime Deopt Table ========== */

// Location of a value at deopt time (physical register or spill slot)
typedef enum {
    DEOPT_LOC_REG,       // value in a GP register (phys_reg = A64Reg)
    DEOPT_LOC_FP_REG,    // value in an FP register (phys_reg = A64FReg)
    DEOPT_LOC_SPILL,     // value in spill slot (spill_offset from SP)
    DEOPT_LOC_CONST_I64, // compile-time i64 constant
    DEOPT_LOC_CONST_F64, // compile-time f64 constant
    DEOPT_LOC_CONST_PTR, // compile-time pointer constant
} XirDeoptLocKind;

// Per-slot entry in runtime deopt table
typedef struct {
    int16_t  bc_slot;      // bytecode register index R[bc_slot]
    uint8_t  type;         // XrRep (I64/F64/PTR/TAGGED)
    uint8_t  loc_kind;     // XirDeoptLocKind
    uint8_t  xr_tag;       // XrValue tag (0-15), or 0xFF=unknown
    uint8_t  _pad[3];
    union {
        uint8_t  phys_reg;     // for LOC_REG / LOC_FP_REG
        int16_t  spill_offset; // for LOC_SPILL (byte offset from frame base)
        int64_t  const_i64;    // for LOC_CONST_I64
        double   const_f64;    // for LOC_CONST_F64
        void    *const_ptr;    // for LOC_CONST_PTR
    } loc;
} XirRtDeoptSlot;

#define XIR_MAX_DEOPT_SLOTS 32

// One deopt point in the runtime table
typedef struct {
    uint32_t       bc_pc;       // bytecode PC to resume interpreter at
    uint16_t       nslots;      // number of live slot entries
    uint16_t       deopt_id;    // index (matches codegen deopt stub id)
    XirRtDeoptSlot slots[XIR_MAX_DEOPT_SLOTS];
} XirRtDeoptEntry;

#define XIR_MAX_RT_DEOPT_ENTRIES 64

/* ========== GC Stack Map (compile-time bitmap for precise GC root scanning) ========== */

#define XIR_MAX_STACK_MAP_ENTRIES 256

/* One safepoint entry: records which registers and spill slots hold GC pointers.
 * Generated at compile time, queried by GC at runtime via safepoint_id index. */
typedef struct {
    uint32_t pc_offset;      // byte offset of safepoint in JIT code
    uint32_t reg_bitmap;     // bit N = alloc_regs[N] holds a PTR-typed vreg
    uint32_t spill_bitmap;   // bit N = spill_slot[N] holds a PTR-typed vreg
} XrStackMapEntry;

// Per-function stack map table, attached to XrProto after compilation.
#define XR_STACK_MAP_MAGIC 0x534D4150 // "SMAP" — validates JIT frame during FP chain walk

typedef struct {
    uint32_t magic;          // XR_STACK_MAP_MAGIC for FP chain walk validation
    uint32_t count;          // number of safepoint entries
    uint32_t frame_size;     // JIT frame size (for locating spill slots)
    XrStackMapEntry entries[];  // flexible array, sorted by pc_offset
} XrStackMapTable;

/* ========== Codegen Result ========== */

typedef struct {
    void    *code;       // pointer to executable code
    uint32_t code_size;  // size in bytes
    bool     success;
    const char *error;   // error message if !success
    // Fast entry offset: instruction count from code start to fast prologue
    // (skip param loading, for register-passing cross-function calls)
    uint32_t fast_entry_offset;
    // OSR entry points for loop headers
    XirOsrEntry osr_entries[XIR_MAX_OSR_ENTRIES];
    uint32_t    nosr;
    // Runtime deopt table (copied to XrProto after compilation)
    XirRtDeoptEntry deopt_entries[XIR_MAX_RT_DEOPT_ENTRIES];
    uint32_t        ndeopt;
    // GC stack map table (heap-allocated, transferred to XrProto)
    XrStackMapTable *stack_map;
    // Resume entry offset (instruction count from code start)
    // Non-zero when function contains XIR_SUSPEND points.
    uint32_t resume_entry_offset;
} XirCodegenResult;

/* ========== API ========== */

// Generate ARM64 machine code from XIR function
// Uses the provided code allocator for executable memory
XR_FUNC XirCodegenResult xir_codegen_arm64(XirFunc *func, XirCodeAlloc *alloc);

#endif // XIR_CODEGEN_H
