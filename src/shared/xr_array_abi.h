/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_abi.h - Shared dynamic-array ABI (fields + storage discriminator).
 *
 * Dependency: the including struct must be able to name int64_t/uint8_t (this
 * header pulls <stdint.h>). It keeps the array field set identical across the
 * VM runtime and the standalone AOT runtime without forcing either side to
 * include the other's headers. Each side wraps these fields with its own
 * object header plus side-specific extras (VM: data_on_region_heap).
 */

#ifndef XR_ARRAY_ABI_H
#define XR_ARRAY_ABI_H

#include <stdint.h>
#include <stdatomic.h>

/* Refcounted backing store for array buffers (task 143/144 M2).
 *
 * Problem it solves: a zero-copy slice used to cache `source->data + offset`
 * while only RC-retaining the source HANDLE. When the source grew, its buffer
 * was realloc'd/freed and the slice's cached pointer dangled (heap-use-after-
 * free; see xarray.c / xrt_coll.h grow paths). Making the BUFFER a separately
 * refcounted object — held by both the array and its slices — fixes that:
 * grow forks the storage when it is shared, and the old buffer survives as long
 * as any view references it.
 *
 * Scope: POD and ANY element types. For ANY buffers, elem_count tracks the
 * owned XrValue slots that must be released when the storage refcount reaches
 * zero.
 *
 * The block is a plain system-heap allocation with a manual refcount. Its
 * lifetime is driven entirely by the array/slice object destructors (which RC
 * already manages), so xi_arc needs no changes: when a view object is collected,
 * its destructor decrements this refcount and frees the buffer at zero. */
typedef struct XrArrayStorage {
    _Atomic int64_t refcount; /* number of arrays/slices referencing `data` */
    void *data;               /* the element buffer (system heap) */
    int64_t byte_capacity;    /* allocated bytes in `data` */
    void *account_heap;       /* VM only: XrCoroHeap* charged for external bytes */
    int64_t accounted_bytes;  /* VM only: bytes charged to account_heap */
    int64_t elem_count;       /* ANY only: # of owning XrValue refs in data[0..elem_count);
                               * snapshotted from the owner's length at fork and at owner
                               * destruction, then released when refcount hits zero. 0 for POD. */
    uint8_t elem_is_any;      /* 1 if `data` holds XrValue elements needing RC release */
} XrArrayStorage;

/* Storage discriminator: the single source of truth for how `data` is owned.
 * Replaces the old VM `capacity == 0 && source != NULL` slice sentinel and the
 * AOT `is_slice` byte — a derived state beats two flags that can disagree.
 *   is_slice  <=>  data_storage == XR_ARRAY_DATA_BORROWED. */
#define XR_ARRAY_DATA_HEAP 0     /* owned heap buffer (freed / RC-managed on drop) */
#define XR_ARRAY_DATA_INLINE 1   /* buffer lives inside the header allocation */
#define XR_ARRAY_DATA_STACK 2    /* buffer on a stack/alloca frame; never grow or free */
#define XR_ARRAY_DATA_BORROWED 3 /* slice view aliasing another array's storage */

/* Shared array fields. `source` is self-referential to the embedding struct
 * (VM XrArray* / AOT xrt_array_t*), so it is typed void* here and each side
 * casts at its use sites:
 *   source != NULL  <=>  slice retains a backing array the collector must trace
 *                        (VM heap slice; released on drop).
 *   source == NULL  <=>  no ownership (stack/arena borrow; AOT slices). */
#define XR_ARRAY_ABI_FIELDS                                                                        \
    void *data;            /* element storage; layout depends on elem_type */                      \
    int64_t length;        /* live element count */                                                \
    int64_t capacity;      /* allocated element capacity (pure capacity, never a slice flag) */    \
    void *source;          /* optional owned backing for slices (see above) */                     \
    void *storage;         /* XrArrayStorage* refcounted buffer owner; NULL for unsliced arrays */ \
    uint8_t data_storage;  /* XR_ARRAY_DATA_*; BORROWED == slice view (cannot grow) */             \
    uint8_t elem_type;     /* XrArrayElemType storage layout (XR_ELEM_*) */                        \
    uint8_t elem_size;     /* cached bytes per element */                                          \
    uint8_t elem_tid;      /* XrTypeId for reified generics (0 = any) */                           \
    uint8_t contains_refs; /* monotonic: 1 once any object reference was stored */                 \
    uint64_t content_version;         /* bumps when the visible element set changes */             \
    uint64_t deferred_submit_version; /* last content_version submitted as deferred task batch */  \
    const char *adt_enum_name;  /* AOT ADT enum lowering metadata, NULL for ordinary arrays */     \
    const char *adt_member_name /* AOT ADT enum lowering metadata, NULL for ordinary arrays */

#define XR_ARRAY_CONTENT_VERSION_INIT 1u

#define XR_ARRAY_MARK_MUTATED(arr)                                                                 \
    do {                                                                                           \
        uint64_t _xr_next_version = (uint64_t) ((arr)->content_version + 1u);                      \
        if (_xr_next_version == 0)                                                                 \
            _xr_next_version = XR_ARRAY_CONTENT_VERSION_INIT;                                      \
        (arr)->content_version = _xr_next_version;                                                 \
    } while (0)

#define XR_ARRAY_CAN_CACHE_DEFERRED_SUBMIT(arr)                                                    \
    ((arr) != NULL && (arr)->data_storage != XR_ARRAY_DATA_BORROWED)

#define XR_ARRAY_DEFERRED_SUBMIT_CURRENT(arr)                                                      \
    (XR_ARRAY_CAN_CACHE_DEFERRED_SUBMIT(arr) &&                                                    \
     (arr)->deferred_submit_version == (arr)->content_version)

#define XR_ARRAY_MARK_DEFERRED_SUBMITTED(arr, version)                                             \
    do {                                                                                           \
        if (XR_ARRAY_CAN_CACHE_DEFERRED_SUBMIT(arr) && (arr)->content_version == (version))        \
            (arr)->deferred_submit_version = (version);                                            \
    } while (0)

typedef struct XrArrayCore {
    XR_ARRAY_ABI_FIELDS;
} XrArrayCore;

#endif  // XR_ARRAY_ABI_H
