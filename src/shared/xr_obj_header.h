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
 * Self-contained: depends only on <stdint.h>/xr_atomic_compat.h so the AOT runtime
 * prelude can adopt the same header without pulling in any runtime translation
 * unit (a pure AOT program keeps its zero-runtime link contract). The runtime's
 * xobj_header.h includes this and layers its object-type enum, type-tag
 * accessors, the remaining collector/storage flag bits, and the signed-RC
 * dup/drop helpers on top.
 *
 * MEMORY LAYOUT (16 bytes):
 *   [0-1]   type     (2B) - object type tag -> destructor dispatch
 *   [2-3]   extra    (2B) - flags word: storage/mmap + XR_OBJ_*
 *   [4-7]   refcount (4B) - 0-based sign-tagged RC (atomic; relaxed fast path)
 *   [8-11]  objsize  (4B) - allocation size (region sweep / munmap)
 *   [12-15] _rsv     (4B) - runtime-domain auxiliary discriminator/state;
 *                           never an object-kind tag
 */

#ifndef XR_OBJ_HEADER_H
#define XR_OBJ_HEADER_H

#include <stdint.h>
#include "xr_atomic_compat.h"

/* ========== Unified Object Header (16 bytes) ========== */

typedef struct XrObjHeader {
    uint16_t type;             /* [0-1] object type tag */
    uint16_t extra;            /* [2-3] flags word: storage/mmap + XR_OBJ_* */
    _Atomic(int32_t) refcount; /* [4-7] 0-based sign-tagged RC. Atomic so the
                                * thread-shared (rc<0) band and the thread-local
                                * fast path share one well-defined object: the
                                * hot path uses relaxed loads/stores (identical
                                * codegen to a plain int on x86/arm64) and the
                                * shared band uses stronger orders. */
    uint32_t objsize;          /* [8-11] allocation size */
    uint32_t _rsv;             /* [12-15] runtime-domain auxiliary word. VM uses
                                * it for weak/cycle state; AOT uses disjoint
                                * tagged domains for builtin destruction and
                                * compilation-local class identity. */
} XrObjHeader;

_Static_assert(sizeof(XrObjHeader) == 16, "XrObjHeader must be 16 bytes");

/* ========== Cross-execution storage mode ==========
 * Shared by VM and AOT because published values cross the runtime boundary
 * without re-shelling.  bit 0 denotes a shared root; bit 15 denotes a unique
 * transferable root. */
#define XR_OBJ_STORAGE_NORMAL 0
#define XR_OBJ_STORAGE_SHARED 1
#define XR_OBJ_STORAGE_TRANSFER 2
/* Bytecode allocation request only: resolve to the current constructor
 * receiver's storage domain before allocating.  This value must never be
 * written into XrObjHeader::extra. */
#define XR_OBJ_STORAGE_INHERIT 3

#define XR_OBJ_STORAGE_SHARED_BIT 0x0001u
#define XR_OBJ_STORAGE_TRANSFER_BIT 0x8000u
#define XR_OBJ_STORAGE_MODE_MASK (XR_OBJ_STORAGE_SHARED_BIT | XR_OBJ_STORAGE_TRANSFER_BIT)

#define XR_OBJ_GET_STORAGE(obj)                                                                    \
    (((obj)->extra & XR_OBJ_STORAGE_TRANSFER_BIT)                                                  \
         ? XR_OBJ_STORAGE_TRANSFER                                                                 \
         : (((obj)->extra & XR_OBJ_STORAGE_SHARED_BIT) ? XR_OBJ_STORAGE_SHARED                     \
                                                       : XR_OBJ_STORAGE_NORMAL))
#define XR_OBJ_SET_STORAGE(obj, m)                                                                 \
    do {                                                                                           \
        (obj)->extra = (uint16_t) ((obj)->extra & ~(uint16_t) XR_OBJ_STORAGE_MODE_MASK);           \
        if ((m) == XR_OBJ_STORAGE_SHARED)                                                          \
            (obj)->extra |= XR_OBJ_STORAGE_SHARED_BIT;                                             \
        else if ((m) == XR_OBJ_STORAGE_TRANSFER)                                                   \
            (obj)->extra |= XR_OBJ_STORAGE_TRANSFER_BIT;                                           \
    } while (0)
#define XR_OBJ_IS_SHARED(obj) (XR_OBJ_GET_STORAGE(obj) == XR_OBJ_STORAGE_SHARED)
#define XR_OBJ_IS_TRANSFER(obj) (XR_OBJ_GET_STORAGE(obj) == XR_OBJ_STORAGE_TRANSFER)

/* ========== Object-Model Flags shared with the AOT runtime ==========
 *
 * Only the bits the AOT runtime references live here so xrt_arc.h can adopt the
 * unified header standalone. The remaining `extra` bits (storage/mmap, REGION/
 * ATOMIC/WEAKABLE, DEAD/MANAGED, cycle-collector color, TRANSIT) are layered on
 * in src/runtime/mem/xobj_header.h and share this same bit space.
 *
 *   bit 3  HAS_DTOR      - object's type has a destructor to run at rc==0.
 *   bit 7  AOT_EXECUTION - AOT object owned by the current execution arena.
 *                          Normal RC still applies; the arena is the cycle/
 *                          abnormal-exit upper bound.
 *   bit 8  AOT_SWEEP     - arena teardown is finalizing this object. Child
 *                          releases back into the same arena are suppressed.
 *   bit 9  AOT_ALLOCATION- object has an execution-allocation prefix. This
 *                          remains set after publication so normal free can
 *                          recover the real allocation base.
 *   bit 11 IMMORTAL      - compiler-emitted process-lifetime metadata. RC
 *                          dup/drop are no-ops and no collector scans it.
 *   bit 12 STORAGE_STACK - AOT stack-allocated object with a real destructor
 *                          but no heap block to free.
 *   bit 14 AOT_NATIVE    - AOT-native header-bearing value. It boxes with the
 *                          same heap_type as VM containers for fast dispatch,
 *                          but must stay on AOT clone/release paths.
 */
#define XR_OBJ_HAS_DTOR 0x0008
#define XR_OBJ_AOT_EXECUTION 0x0080
#define XR_OBJ_AOT_SWEEP 0x0100
#define XR_OBJ_AOT_ALLOCATION 0x0200
#define XR_OBJ_IMMORTAL 0x0800
#define XR_OBJ_STORAGE_STACK 0x1000
#define XR_OBJ_AOT_NATIVE 0x4000

/* Shared signed-RC sentinels. Immortal objects store XR_RC_STICKY so the
 * VM and AOT fast paths never mistake them for unique thread-local objects. */
#define XR_RC_STICKY ((int32_t) INT32_MIN)
#define XR_RC_STICKY_BAND ((int32_t) (INT32_MIN + 1024))
#define XR_RC_INIT ((int32_t) 0)

/* ========== Object Type (header.type) ==========
 *
 * The object-type tag stored in XrObjHeader.type. Shared so the AOT runtime and
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
    XR_TBLOB,               /* Raw byte buffer on Region heap (no traverse/destroy) */
    XR_TCELL,               /* Single-slot mutable capture cell (32B) */
    XR_TTASK,               /* Lightweight GC-managed coroutine handle */
    XR_TATOMIC,             /* Atomic<T> shared primitive wrapper (system heap) */
    XR_TWORKQUEUE,          /* WorkQueue<T> shared sharded queue (system heap) */
    XR_TRESULTGROUP,        /* ResultGroup shared scalar reducer (system heap) */
    XR_TBOOLMAP,            /* AOT-only Map<bool,scalar> 2-slot direct store; boxes as
                             * XR_TAG_MAP (value heap_type stays XR_TMAP) and is
                             * discriminated from a generic map by this hdr.type */
    XR_TCOUNTDOWNLATCH,     /* CountdownLatch shared completion barrier (system heap) */
    XR_TSEMAPHORE,          /* Semaphore shared counting permit primitive (system heap) */
    XR_TEVENTCOUNT,         /* EventCount shared epoch/broadcast primitive (system heap) */
    XR_TTHREAD,             /* sys.Thread shared OS-thread handle (system heap) */
    XR_TENUM_TYPE,          /* Internal enum namespace/type metadata; not a user wrapper class */
    XR_TENUM_CTOR,          /* Internal payload variant constructor metadata */
    XR_TENUM_DESCRIPTOR,    /* Erased enum-domain descriptor {layout, kind, scalar}. */
    XR_TENUM_SCALAR_LAYOUT, /* Static unit-enum layout used by compact AOT scalar boxes. */
    XR_TWEAK_HANDLE,        /* Shared indirection cell for `weak` fields; holds a
                             * non-owning target pointer that is cleared when the
                             * target's last strong reference goes. */
} XrObjType;

#endif  // XR_OBJ_HEADER_H
