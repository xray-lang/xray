/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstackmap.h - JIT frame stack map descriptor (consumed by precise GC)
 *
 * KEY CONCEPT:
 *   XrStackMapTable records, for each safepoint in JIT-compiled code,
 *   which registers and spill slots hold GC-managed pointers. It is the
 *   data contract between JIT codegen (producer) and GC (consumer).
 *
 *   The structure lives in runtime/gc so the GC never needs to reach up
 *   into jit/; JIT simply populates the table and stores it on XrProto.
 */

#ifndef XSTACKMAP_H
#define XSTACKMAP_H

#include <stdint.h>

#define XIR_MAX_STACK_MAP_ENTRIES 256

/* One safepoint entry: records which registers and spill slots hold GC pointers.
 * Generated at compile time, queried by GC at runtime via safepoint_id index. */
typedef struct {
    uint32_t pc_offset;     // byte offset of safepoint in JIT code
    uint32_t reg_bitmap;    // bit N = alloc_regs[N] holds a PTR-typed vreg
    uint32_t spill_bitmap;  // bit N = spill_slot[N] holds a PTR-typed vreg
} XrStackMapEntry;

// Per-function stack map table, attached to XrProto after compilation.
#define XR_STACK_MAP_MAGIC 0x534D4150  // "SMAP" — validates JIT frame during FP chain walk

typedef struct {
    uint32_t magic;             // XR_STACK_MAP_MAGIC for FP chain walk validation
    uint32_t count;             // number of safepoint entries
    uint32_t frame_size;        // JIT frame size (for locating spill slots)
    XrStackMapEntry entries[];  // flexible array, sorted by pc_offset
} XrStackMapTable;

#endif  // XSTACKMAP_H
