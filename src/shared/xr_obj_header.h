/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_obj_header.h - Single source of truth for the object header layout shared
 * by the VM and AOT runtimes.
 *
 * Self-contained: depends only on <stdint.h>/<stdatomic.h> so the AOT runtime
 * prelude can adopt the same header without pulling in any runtime translation
 * unit (a pure AOT program keeps its zero-runtime link contract). The runtime's
 * xgc_header.h includes this and layers its object-type enum, type-tag
 * accessors, the remaining collector/storage flag bits, and the signed-RC
 * dup/drop helpers on top.
 *
 * MEMORY LAYOUT (16 bytes):
 *   [0-1]   type     (2B) - object type tag -> destructor dispatch
 *   [2-3]   extra    (2B) - flags word: storage/mmap + XR_OBJ_*
 *   [4-7]   refcount (4B) - 0-based sign-tagged RC (atomic; relaxed fast path)
 *   [8-11]  objsize  (4B) - allocation size (region sweep / munmap)
 *   [12-15] _rsv     (4B) - reserved (weak table slot / cycle-report id)
 */

#ifndef XR_OBJ_HEADER_H
#define XR_OBJ_HEADER_H

#include <stdint.h>
#include <stdatomic.h>

/* ========== Unified Object Header (16 bytes) ========== */

typedef struct XrGCHeader {
    uint16_t type;            /* [0-1] object type tag */
    uint16_t extra;           /* [2-3] flags word: storage/mmap + XR_OBJ_* */
    _Atomic int32_t refcount; /* [4-7] 0-based sign-tagged RC. Atomic so the
                               * thread-shared (rc<0) band and the thread-local
                               * fast path share one well-defined object: the
                               * hot path uses relaxed loads/stores (identical
                               * codegen to a plain int on x86/arm64) and the
                               * shared band uses stronger orders. */
    uint32_t objsize;         /* [8-11] allocation size */
    uint32_t _rsv;            /* [12-15] reserved (weak slot / cycle-report id) */
} XrGCHeader;

_Static_assert(sizeof(XrGCHeader) == 16, "XrGCHeader must be 16 bytes");

/* Unified alias: the RC memory model refers to the object header as
 * XrObjHeader. */
typedef struct XrGCHeader XrObjHeader;

/* ========== Object-Model Flags shared with the AOT runtime ==========
 *
 * Only the bits the AOT runtime references live here so xrt_arc.h can adopt the
 * unified header standalone. The remaining `extra` bits (storage/mmap, REGION/
 * ATOMIC/WEAKABLE, DEAD/MANAGED, cycle-collector color, TRANSIT) are layered on
 * in src/runtime/gc/xgc_header.h and share this same bit space.
 *
 *   bit 3  HAS_DTOR      - object's type has a destructor to run at rc==0.
 *   bit 11 STORAGE_BUMP  - AOT bump-allocated: RC dup/drop are no-ops (freed in
 *                          bulk with the bump arena) and the GC never scans it.
 *   bit 12 STORAGE_STACK - AOT stack-allocated object with a real destructor
 *                          but no heap block to free.
 */
#define XR_OBJ_HAS_DTOR 0x0008
#define XR_OBJ_STORAGE_BUMP 0x0800
#define XR_OBJ_STORAGE_STACK 0x1000

/* Shared signed-RC sentinels. Bump/immortal objects store XR_RC_STICKY so the
 * VM and AOT fast paths never mistake them for unique thread-local objects. */
#define XR_RC_STICKY ((int32_t) INT32_MIN)
#define XR_RC_STICKY_BAND ((int32_t) (INT32_MIN + 1024))
#define XR_RC_INIT ((int32_t) 0)

/* ========== Object Type (header.type) ==========
 *
 * The object-type tag stored in XrGCHeader.type. Shared so the AOT runtime and
 * the VM agree on the numeric ids: a header-bearing AOT object (array/map/set)
 * stores the same id the VM uses, which lets a boxed value carry heap_type and
 * be type-checked / dispatched identically on both sides. The numeric order is
 * load-bearing (header type ids, switch tables) — append new types at the end. */
typedef enum {
    XR_TNULL = 0,
    XR_TBOOL,
    XR_TINT,
    XR_TFLOAT,
    XR_TSTRING,
    XR_TFUNCTION,
    XR_TCFUNCTION,
    XR_TARRAY,
    XR_TSET,
    XR_TMAP,
    XR_TCLASS,
    XR_TINSTANCE,
    XR_TBOUND_METHOD,
    XR_TERROR,
    XR_TMODULE,
    XR_TCOROUTINE,
    XR_TCHANNEL,
    XR_TCOROPOOL,
    XR_TBLOB,        /* Raw byte buffer on Region heap (no traverse/destroy) */
    XR_TCELL,        /* Single-slot mutable capture cell (32B) */
    XR_TTASK,        /* Lightweight GC-managed coroutine handle */
    XR_TATOMIC,      /* Atomic<T> shared primitive wrapper (system heap) */
    XR_TWORKQUEUE,   /* WorkQueue<T> shared sharded queue (system heap) */
    XR_TRESULTGROUP, /* ResultGroup shared scalar reducer (system heap) */
} XrObjType;

#endif  // XR_OBJ_HEADER_H
