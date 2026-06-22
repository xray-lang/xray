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
 * object header (VM: XrObjHeader; AOT: none) plus side-specific extras
 * (VM: data_on_gc_heap; AOT: adt_*).
 */

#ifndef XR_ARRAY_ABI_H
#define XR_ARRAY_ABI_H

#include <stdint.h>

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
    void *data;           /* element storage; layout depends on elem_type */                       \
    int64_t length;       /* live element count */                                                 \
    int64_t capacity;     /* allocated element capacity (pure capacity, never a slice flag) */     \
    void *source;         /* optional owned backing for slices (see above) */                      \
    uint8_t data_storage; /* XR_ARRAY_DATA_*; BORROWED == slice view (cannot grow) */              \
    uint8_t elem_type;    /* XrArrayElemType storage layout (XR_ELEM_*) */                         \
    uint8_t elem_size;    /* cached bytes per element */                                           \
    uint8_t elem_tid;     /* XrTypeId for reified generics (0 = any) */                            \
    uint8_t has_gc_ptrs   /* monotonic: 1 once any GC pointer was stored */

typedef struct XrArrayCore {
    XR_ARRAY_ABI_FIELDS;
} XrArrayCore;

#endif  // XR_ARRAY_ABI_H
