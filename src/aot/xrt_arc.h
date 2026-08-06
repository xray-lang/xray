/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_arc.h - Execution-local ARC allocator for AOT-generated code
 *
 * KEY CONCEPT:
 *   Self-contained memory management for AOT-generated code.
 *   Objects carry an XrObjHeader before user data for type tracking.
 *   Normal reference counting reclaims acyclic objects immediately. Every
 *   remaining execution-local object is also registered with the current
 *   coroutine arena, which is the deterministic upper bound for cycles.
 */

#ifndef XRT_ARC_H
#define XRT_ARC_H

#include "xrt_value.h"
#include "../shared/xr_obj_header.h" /* unified XrObjHeader + storage/dtor flags */

/* =========================================================================
 * AOT allocator adapter
 *
 * In standalone AOT mode, maps to system allocator.
 * When building inside xray project, define XRT_USE_XR_MALLOC to route
 * through xr_malloc/xr_free (set by CMakeLists.txt: -DXRT_USE_XR_MALLOC).
 * ========================================================================= */

#ifndef XRT_MALLOC
#ifdef XRT_USE_XR_MALLOC
#include "../base/xmalloc.h"
#define XRT_MALLOC(sz) xr_malloc(sz)
#define XRT_CALLOC(n, sz) xr_calloc(n, sz)
#define XRT_REALLOC(p, sz) xr_realloc(p, sz)
#define XRT_FREE(p) xr_free(p)
#else
#define XRT_MALLOC(sz) malloc(sz)
#define XRT_CALLOC(n, sz) calloc(n, sz)
#define XRT_REALLOC(p, sz) realloc(p, sz)
#define XRT_FREE(p) free(p)
#endif
#endif

/* Alignment of array element storage (xrt_array_t.data). 32 bytes lets the
 * C compiler use full-width AVX loads/stores in vectorized loops; generated
 * _adN caches assert it via XR_ASSUME_ALIGNED(..., XRT_DATA_ALIGN). Growth
 * spill buffers with this contract MUST be allocated with XRT_ALLOC_ALIGNED and
 * released with XRT_FREE_ALIGNED — on Windows _aligned_malloc storage is not
 * addressable through plain free(), and realloc would silently drop the
 * alignment, so growth re-allocates + copies (see xrt_array_data_grow). */
#ifndef XRT_DATA_ALIGN
#define XRT_DATA_ALIGN 32
#endif

#ifndef XRT_ALLOC_ALIGNED
#ifdef XRT_USE_XR_MALLOC
#include "../base/xmalloc.h"
#define XRT_ALLOC_ALIGNED(sz) xr_malloc_aligned((sz), XRT_DATA_ALIGN)
#define XRT_FREE_ALIGNED(p) xr_free_aligned((p), XRT_DATA_ALIGN)
#elif defined(_WIN32)
#include <malloc.h>
#define XRT_ALLOC_ALIGNED(sz) _aligned_malloc((sz), XRT_DATA_ALIGN)
#define XRT_FREE_ALIGNED(p) _aligned_free(p)
#else
static inline void *xrt_alloc_aligned_impl(size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, XRT_DATA_ALIGN, size) != 0)
        return NULL;
    return p;
}
#define XRT_ALLOC_ALIGNED(sz) xrt_alloc_aligned_impl(sz)
#define XRT_FREE_ALIGNED(p) free(p)
#endif
#endif

/* =========================================================================
 * Object header — precedes every generic execution-arena allocation.
 *
 * Layout: [XrObjHeader][  user data  ]
 *          ^--- hdr pointer (via XRT_ARC_HDR macro)
 *
 * AOT objects carry the unified XrObjHeader (src/shared/xr_obj_header.h), the
 * same 16-byte header the VM/AOT runtime uses, so a value crossing the
 * coroutine boundary needs no re-shelling. `type` is always the canonical
 * XrObjType object-kind tag; `extra` carries execution/immortal storage flags
 * and XR_OBJ_HAS_DTOR; `refcount` is the 0-based RC (rc == N means N+1 live
 * refs). AOT-local class identity is encoded separately in `_rsv` and must
 * never be confused with the canonical object-kind namespace.
 * ========================================================================= */

#define XRT_ARC_HDR(p) ((XrObjHeader *) ((char *) (p) - sizeof(XrObjHeader)))

/* Type-specific destructor dispatch. Defined in xrt_class.h (which owns the
 * type table), forward-declared here because xrt_release (L1) runs before
 * the table type is visible (L5). Runs the object's destructor if its type
 * registered one; no-op otherwise. */
static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj);
static inline void xrt_dispatch_builtin_destructor(uint32_t kind, void *obj);

/* Container RC dispatch. Arrays/maps/sets carry an embedded-at-0 XrObjHeader and
 * per-type backing storage, so xrt_retain/xrt_release route container tags here
 * instead of through the generic prepended-header path. Defined in xrt_coll.h. */
static inline void xrt_coll_retain(XrValue v);
static inline void xrt_coll_release(XrValue v);

#define XRT_ARC_KIND_NONE 0u
#define XRT_ARC_KIND_CLOSURE 1u
#define XRT_ARC_KIND_CELL 2u
#define XRT_ARC_KIND_REGEX 3u
#define XRT_ARC_KIND_SYS_MUTEX 4u
#define XRT_ARC_KIND_SYS_RWLOCK 5u
#define XRT_ARC_KIND_SYS_CONDVAR 6u
#define XRT_ARC_KIND_SYS_BARRIER 7u
#define XRT_ARC_KIND_SYS_ONCE 8u
#define XRT_ARC_KIND_THREAD 9u
#define XRT_ARC_KIND_BUFFER 10u
#define XRT_ARC_KIND_NET_CONN 11u
#define XRT_ARC_KIND_NET_LISTENER 12u
#define XRT_ARC_KIND_JSON 13u

/* `_rsv` is an ABI-stable auxiliary word whose meaning is selected by the
 * object's storage/runtime domain. Generic prefix allocations use the small
 * XRT_ARC_KIND_* values for builtin destructor routing. AOT-native class
 * instances and registered value-struct boxes use a disjoint tagged encoding
 * for the compilation-local type table id. Keeping that id out of
 * XrObjHeader.type is what makes the object header's type field canonical
 * across VM and AOT. */
#define XRT_AOT_CLASS_TYPE_TAG 0x80000000u
#define XRT_AOT_CLASS_TYPE_TAG_MASK 0xFFFF0000u
#define XRT_AOT_CLASS_TYPE_ID_MASK 0x0000FFFFu

static inline uint16_t xrt_aot_class_type_id(const XrObjHeader *hdr) {
    if (!hdr || hdr->type != XR_TINSTANCE ||
        (hdr->_rsv & XRT_AOT_CLASS_TYPE_TAG_MASK) != XRT_AOT_CLASS_TYPE_TAG)
        return 0;
    return (uint16_t) (hdr->_rsv & XRT_AOT_CLASS_TYPE_ID_MASK);
}

static inline void xrt_aot_class_type_set(XrObjHeader *hdr, uint16_t type_id) {
    if (!hdr || type_id == 0) {
        fprintf(stderr, "xrt_aot_class_type_set: class type id must be non-zero\n");
        abort();
    }
    hdr->_rsv = XRT_AOT_CLASS_TYPE_TAG | (uint32_t) type_id;
}
#define XRT_ARC_KIND_STRBUF 14u
#define XRT_ARC_KIND_ITERATOR 15u
#define XRT_ARC_KIND_TUPLE 16u

typedef struct xrt_buffer_object {
    void *data;
    int64_t length;
    size_t align;
} xrt_buffer_object_t;

static inline void xrt_buffer_free_data(void *data, size_t align) {
    if (!data)
        return;
#if defined(XRT_USE_XR_MALLOC)
    if (align)
        xr_free_aligned(data, align);
    else
        XRT_FREE(data);
#elif defined(_WIN32)
    if (align)
        _aligned_free(data);
    else
        XRT_FREE(data);
#else
    (void) align;
    XRT_FREE(data);
#endif
}

static inline void xrt_buffer_destroy_builtin(void *obj) {
    xrt_buffer_object_t *buf = (xrt_buffer_object_t *) obj;
    if (!buf)
        return;
    xrt_buffer_free_data(buf->data, buf->align);
    buf->data = NULL;
    buf->length = 0;
    buf->align = 0;
}

/* =========================================================================
 * Execution-local arenas
 *
 * An arena is an ownership registry, not a bump allocator. Objects retain
 * normal RC and are individually reclaimed on their last release. The arena
 * owns whatever remains when its physical coroutine ends, including strong
 * reference cycles that pure RC cannot break. Publishing an object into the
 * shared or transferable storage domain unbinds it from this registry first.
 * ========================================================================= */

typedef struct XrtExecutionArena XrtExecutionArena;
typedef struct XrtExecutionAllocation XrtExecutionAllocation;
typedef void (*XrtExecutionFinalizer)(XrObjHeader *hdr);

struct XrtExecutionArena {
    XrtExecutionAllocation *head;
    uint64_t live_bytes;
    uint64_t live_objects;
    uint64_t finalizer_count;
    uint8_t initialized;
    uint8_t destroying;
    uint8_t heap_owned;
    uint8_t _pad[5];
};

/* The prefix is deliberately 16-byte sized/aligned as a whole: the following
 * XrObjHeader and the user payload therefore preserve xrt_arc_alloc's 16-byte
 * alignment contract on every supported 64-bit provider. */
struct XrtExecutionAllocation {
    XrtExecutionAllocation *prev;
    XrtExecutionAllocation *next;
    XrtExecutionArena *arena;
    XrtExecutionFinalizer finalizer;
    uint64_t object_bytes;
    uint64_t magic;
};

#define XRT_EXECUTION_ALLOCATION_MAGIC UINT64_C(0x585241594152454e)
_Static_assert((sizeof(XrtExecutionAllocation) & 15u) == 0,
               "execution allocation prefix must preserve 16-byte alignment");

static void xrt_execution_finalize_generic(XrObjHeader *hdr);
static void xrt_execution_finalize_array(XrObjHeader *hdr);

#ifdef XRT_IMPL
XrtExecutionArena xrt_root_execution_arena;
_Thread_local XrtExecutionArena *xrt_current_execution_arena;
/* Depth-bounded recursive release (mirrors the VM's deferred_drops): when a
 * destructor cascade gets too deep, dead objects are queued here and drained
 * iteratively by the outermost xrt_release instead of recursing, so freeing a
 * deep data structure (10k+ node list/tree) cannot overflow the C stack.
 * These are thread-local: concurrent worker releases must not share the
 * recursive destructor queue.
 *
 * The queue is a side stack, not a list threaded through the queued objects:
 * a queued object has only reached rc == 0 and its destructor has NOT run yet,
 * so its first payload word is still a live field. */
_Thread_local int xrt_release_depth;
_Thread_local XrObjHeader **xrt_deferred_stack;
_Thread_local uint32_t xrt_deferred_count;
_Thread_local uint32_t xrt_deferred_cap;
#else
extern XrtExecutionArena xrt_root_execution_arena;
extern _Thread_local XrtExecutionArena *xrt_current_execution_arena;
extern _Thread_local int xrt_release_depth;
extern _Thread_local XrObjHeader **xrt_deferred_stack;
extern _Thread_local uint32_t xrt_deferred_count;
extern _Thread_local uint32_t xrt_deferred_cap;
#endif

/* Outermost release recurses at most this deep before queuing dead objects for
 * iterative draining (matches XR_DESTROY_DEPTH_LIMIT on the VM side). */
#define XRT_RELEASE_DEPTH_LIMIT 64

static inline void xrt_execution_arena_init(XrtExecutionArena *arena, int heap_owned) {
    if (!arena)
        return;
    memset(arena, 0, sizeof(*arena));
    arena->initialized = 1;
    arena->heap_owned = heap_owned ? 1 : 0;
}

static inline XrtExecutionArena *xrt_execution_root(void) {
    if (!xrt_root_execution_arena.initialized)
        xrt_execution_arena_init(&xrt_root_execution_arena, 0);
    return &xrt_root_execution_arena;
}

static inline XrtExecutionArena *xrt_execution_current(void) {
    if (!xrt_current_execution_arena)
        xrt_current_execution_arena = xrt_execution_root();
    return xrt_current_execution_arena;
}

static inline void *xrt_execution_arena_new(void) {
    XrtExecutionArena *arena = (XrtExecutionArena *) XRT_CALLOC(1, sizeof(*arena));
    if (XR_UNLIKELY(!arena))
        return NULL;
    xrt_execution_arena_init(arena, 1);
    return arena;
}

static inline void *xrt_execution_arena_enter(void *raw_arena) {
    XrtExecutionArena *previous = xrt_current_execution_arena;
    XrtExecutionArena *arena = (XrtExecutionArena *) raw_arena;
    if (!arena || !arena->initialized || arena->destroying) {
        fprintf(stderr, "xrt_execution_arena_enter: invalid arena\n");
        abort();
    }
    xrt_current_execution_arena = arena;
    return previous;
}

static inline void xrt_execution_arena_restore(void *raw_previous) {
    xrt_current_execution_arena = (XrtExecutionArena *) raw_previous;
}

static inline XrtExecutionAllocation *xrt_execution_node(XrObjHeader *hdr) {
    XrtExecutionAllocation *node =
        (XrtExecutionAllocation *) ((char *) hdr - sizeof(XrtExecutionAllocation));
    if (XR_UNLIKELY(node->magic != XRT_EXECUTION_ALLOCATION_MAGIC)) {
        fprintf(stderr, "xrt: corrupt execution allocation prefix\n");
        abort();
    }
    return node;
}

static inline void xrt_execution_unlink(XrObjHeader *hdr) {
    if (!hdr || !(hdr->extra & XR_OBJ_AOT_EXECUTION))
        return;
    XrtExecutionAllocation *node = xrt_execution_node(hdr);
    XrtExecutionArena *arena = node->arena;
    if (node->prev)
        node->prev->next = node->next;
    else if (arena)
        arena->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    if (arena) {
        arena->live_objects--;
        arena->live_bytes -= node->object_bytes;
    }
    node->prev = NULL;
    node->next = NULL;
    node->arena = NULL;
    hdr->extra &= (uint16_t) ~(uint16_t) XR_OBJ_AOT_EXECUTION;
}

static inline void xrt_execution_unbind(XrObjHeader *hdr) {
    xrt_execution_unlink(hdr);
}

static inline void *xrt_execution_alloc(size_t object_bytes, XrtExecutionFinalizer finalizer) {
    if (XR_UNLIKELY(object_bytes > SIZE_MAX - 15u)) {
        fprintf(stderr, "xrt_execution_alloc: allocation size overflow\n");
        abort();
    }
    object_bytes = (object_bytes + 15u) & ~(size_t) 15u;
    if (XR_UNLIKELY(object_bytes > SIZE_MAX - sizeof(XrtExecutionAllocation))) {
        fprintf(stderr, "xrt_execution_alloc: allocation size overflow\n");
        abort();
    }
    XrtExecutionArena *arena = xrt_execution_current();
    if (XR_UNLIKELY(arena->destroying)) {
        fprintf(stderr, "xrt_execution_alloc: allocation during arena teardown\n");
        abort();
    }
    XrtExecutionAllocation *node =
        (XrtExecutionAllocation *) XRT_CALLOC(1, sizeof(XrtExecutionAllocation) + object_bytes);
    if (XR_UNLIKELY(!node)) {
        fprintf(stderr, "xrt_execution_alloc: out of memory\n");
        abort();
    }
    node->arena = arena;
    node->finalizer = finalizer;
    node->object_bytes = object_bytes;
    node->magic = XRT_EXECUTION_ALLOCATION_MAGIC;
    node->next = arena->head;
    if (arena->head)
        arena->head->prev = node;
    arena->head = node;
    arena->live_objects++;
    arena->live_bytes += object_bytes;
    XrObjHeader *hdr = (XrObjHeader *) (node + 1);
    hdr->extra = XR_OBJ_AOT_ALLOCATION | XR_OBJ_AOT_EXECUTION;
    atomic_store_explicit(&hdr->refcount, XR_RC_INIT, memory_order_relaxed);
    return hdr;
}

static inline void *xrt_execution_alloc_embedded(size_t object_bytes,
                                                 XrtExecutionFinalizer finalizer) {
    XrObjHeader *hdr = (XrObjHeader *) xrt_execution_alloc(object_bytes, finalizer);
    hdr->extra |= XR_OBJ_AOT_NATIVE;
    return hdr;
}

/* Alignment contract: the allocation prefix and unified header are both
 * multiples of 16 bytes, so every returned user pointer is 16-byte aligned. */
static inline void *xrt_arc_alloc(size_t obj_size) {
    if (XR_UNLIKELY(obj_size > SIZE_MAX - sizeof(XrObjHeader) - 15u)) {
        fprintf(stderr, "xrt_arc_alloc: allocation size overflow\n");
        abort();
    }
    size_t object_bytes = sizeof(XrObjHeader) + ((obj_size + 15u) & ~(size_t) 15u);
    XrObjHeader *hdr =
        (XrObjHeader *) xrt_execution_alloc(object_bytes, xrt_execution_finalize_generic);
    return (char *) hdr + sizeof(XrObjHeader);
}

static inline XrValue xrt_enum_descriptor_box_new(uint32_t layout_id, uint8_t metadata_kind,
                                                  int64_t scalar) {
    XrAotErasedEnumDescriptor *box = (XrAotErasedEnumDescriptor *) xrt_arc_alloc(sizeof(*box));
    XrObjHeader *hdr = XRT_ARC_HDR(box);
    hdr->type = XR_TENUM_DESCRIPTOR;
    box->layout_id = layout_id;
    box->metadata_kind = metadata_kind;
    box->_reserved[0] = box->_reserved[1] = box->_reserved[2] = 0;
    box->scalar = scalar;
    XrValue value = {0};
    value.tag = XR_TAG_PTR;
    value.heap_type = XR_TENUM_DESCRIPTOR;
    value.ptr = box;
    return value;
}

static inline int xrt_enum_descriptor_matches(XrValue value, uint32_t layout_id,
                                              uint8_t metadata_kind) {
    if (value.tag != XR_TAG_PTR || value.heap_type != XR_TENUM_DESCRIPTOR || !value.ptr)
        return 0;
    const XrAotErasedEnumDescriptor *box = (const XrAotErasedEnumDescriptor *) value.ptr;
    return box->layout_id == layout_id && box->metadata_kind == metadata_kind;
}

static inline int64_t xrt_enum_descriptor_unbox(XrValue value) {
    if (value.tag != XR_TAG_PTR || value.heap_type != XR_TENUM_DESCRIPTOR || !value.ptr) {
        fprintf(stderr, "xray: value is not an erased enum descriptor\n");
        abort();
    }
    return ((const XrAotErasedEnumDescriptor *) value.ptr)->scalar;
}

static inline void xrt_arc_mark_builtin(void *obj, uint32_t kind) {
    XrObjHeader *hdr = XRT_ARC_HDR(obj);
    hdr->_rsv = kind;
    hdr->extra |= XR_OBJ_HAS_DTOR;
}

/* Static headers describe compiler-emitted process-lifetime metadata. They do
 * not belong to an execution arena and are intentionally immortal. */
static inline void xrt_static_header_init(XrObjHeader *h, uint16_t type) {
    h->type = type;
    h->extra = XR_OBJ_IMMORTAL;
    atomic_store_explicit(&h->refcount, XR_RC_STICKY, memory_order_relaxed);
    h->objsize = 0;
    h->_rsv = 0;
}

static inline void xrt_heap_header_init(XrObjHeader *h, uint16_t type) {
    uint16_t allocation_flags =
        h->extra & (XR_OBJ_AOT_ALLOCATION | XR_OBJ_AOT_EXECUTION | XR_OBJ_AOT_NATIVE);
    h->type = type;
    h->extra = allocation_flags;
    atomic_store_explicit(&h->refcount, XR_RC_INIT, memory_order_relaxed);
    h->objsize = 0;
    h->_rsv = 0;
}

static inline void xrt_stack_header_init(XrObjHeader *h, uint16_t type) {
    h->type = type;
    h->extra = XR_OBJ_STORAGE_STACK | XR_OBJ_AOT_NATIVE;
    atomic_store_explicit(&h->refcount, XR_RC_INIT, memory_order_relaxed);
    h->objsize = 0;
    h->_rsv = 0;
}

static inline int xrt_arc_value_has_header(XrValue v) {
    if (!v.ptr)
        return 0;
    if (v.tag == XR_TAG_PTR && (v.flags & XR_VALUE_FLAG_HEADER_AT_PTR) != 0)
        return 1;
    if (XR_IS_ARRAY_REF(v))
        return (v.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) != 0;
    if (v.tag == XR_TAG_PTR)
        return v.heap_type == XR_TINSTANCE || v.heap_type == XR_TENUM_DESCRIPTOR;
    return v.tag == XR_TAG_STR_ARC || v.tag == XR_TAG_STRBUF || v.tag == XR_TAG_CLOSURE ||
           v.tag == XR_TAG_CELL || v.tag == XR_TAG_ITERATOR || v.tag == XR_TAG_AGG_REF ||
           v.tag == XR_TAG_REGEX || v.tag == XR_TAG_SYS_MUTEX || v.tag == XR_TAG_SYS_RWLOCK ||
           v.tag == XR_TAG_SYS_CONDVAR || v.tag == XR_TAG_SYS_BARRIER || v.tag == XR_TAG_SYS_ONCE ||
           v.tag == XR_TAG_THREAD || v.tag == XR_TAG_BUFFER || v.tag == XR_TAG_NET_CONN ||
           v.tag == XR_TAG_NET_LISTENER || v.tag == XR_TAG_RANGE || v.tag == XR_TAG_TUPLE;
}

static inline XrObjHeader *xrt_arc_value_header(XrValue v) {
    if (v.tag == XR_TAG_PTR &&
        (v.heap_type == XR_TINSTANCE || (v.flags & XR_VALUE_FLAG_HEADER_AT_PTR) != 0))
        return (XrObjHeader *) v.ptr;
    return XRT_ARC_HDR(v.ptr);
}

/* ========== --rc-guard debug codegen (task 219 P4) ==========
 *
 * When compiled with -DXR_RC_GUARD (the `--rc-guard` build flag), a released
 * object is POISONED and quarantined instead of freed: its header stores a
 * poison marker. Any later retain/release — and any access point the codegen
 * wraps with xrt_rc_guard_check — aborts on first touch, converting a silent
 * use-after-release / double-free (the task-219 incident class that escapes
 * static verification via dynamic/FFI paths) into a deterministic first-crime
 * crash with the object address. Debug-only: quarantined objects are leaked, so
 * this is never enabled in release builds (zero overhead when the macro is off).
 * Combines with the 218 asan_focused lane. */
#ifdef XR_RC_GUARD
#define XRT_RC_GUARD_POISON 0xDEADF0FDu

static inline void xrt_rc_guard_poison(XrObjHeader *hdr) {
    if (hdr)
        hdr->_rsv = XRT_RC_GUARD_POISON;
}

static inline int xrt_rc_guard_is_poisoned(XrValue v) {
    if (!xrt_arc_value_has_header(v))
        return 0;
    XrObjHeader *hdr = xrt_arc_value_header(v);
    return hdr && hdr->_rsv == XRT_RC_GUARD_POISON;
}

static inline void xrt_rc_guard_fail(const char *site, void *ptr) {
    fprintf(stderr,
            "\n[rc-guard] use-after-release: '%s' touched released (poisoned) object %p\n"
            "  the object was freed by an earlier xrt_release; this access would read/free\n"
            "  memory that ARC already reclaimed. (task 219 --rc-guard)\n",
            site ? site : "<access>", ptr);
    abort();
}

/* Access-point guard: codegen may wrap any xrt_* object access with this in
 * guard mode. Aborts if the object was already released. */
static inline void xrt_rc_guard_check(XrValue v, const char *site) {
    if (xrt_rc_guard_is_poisoned(v))
        xrt_rc_guard_fail(site, v.ptr);
}
#else
#define xrt_rc_guard_poison(hdr) ((void) 0)
#define xrt_rc_guard_check(v, site) ((void) 0)
#endif

/* ARC retain: acquire a new owning reference (0-based: rc++ adds one ref).
 * Called by generated code for values with escape > NO_ESCAPE.
 * No-op for values that do not carry an XrObjHeader. */
static inline void xrt_retain(XrValue v) {
    /* Compiler-interned string sidecars have static storage and are never ARC
     * objects.  Keep this fast rejection explicit before any container/header
     * routing so both optimized C and path-sensitive analyzers preserve that
     * lifetime fact when a literal key has travelled through a collection. */
    if (v.tag == XR_TAG_STR)
        return;
    if (XR_IS_ARRAY(v) || XR_IS_MAP(v) || XR_IS_SET(v) ||
        (v.tag == XR_TAG_PTR && v.heap_type == 0 && (v.flags & XR_VALUE_FLAG_HEADER_AT_PTR) != 0)) {
        xrt_coll_retain(v);
        return;
    }
    if (!xrt_arc_value_has_header(v))
        return;
    XrObjHeader *hdr = xrt_arc_value_header(v);
#ifdef XR_RC_GUARD
    if (hdr->_rsv == XRT_RC_GUARD_POISON)
        xrt_rc_guard_fail("xrt_retain", v.ptr); /* retain of a released object */
#endif
    if (hdr->extra & (XR_OBJ_IMMORTAL | XR_OBJ_STORAGE_STACK | XR_OBJ_AOT_SWEEP))
        return;
    if (XR_OBJ_IS_SHARED(hdr)) {
        atomic_fetch_sub_explicit(&hdr->refcount, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&hdr->refcount, 1, memory_order_relaxed);
}

static inline int xrt_rc_claim_release_last(XrObjHeader *hdr) {
    if (!hdr || (hdr->extra & (XR_OBJ_IMMORTAL | XR_OBJ_STORAGE_STACK | XR_OBJ_AOT_SWEEP)))
        return 0;
    if (XR_OBJ_IS_SHARED(hdr)) {
        int32_t old = atomic_fetch_add_explicit(&hdr->refcount, 1, memory_order_acq_rel);
        return old == -1;
    }
    for (;;) {
        int32_t rc = atomic_load_explicit(&hdr->refcount, memory_order_acquire);
        if (rc == XR_RC_STICKY)
            return 0;
        if (rc > 0) {
            int32_t next = rc - 1;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return 0;
            continue;
        }
        if (rc == 0) {
            int32_t next = XR_RC_STICKY;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return 1;
            continue;
        }
        return 0;
    }
}

static inline void xrt_array_ref_release_owned(XrValue v);

/* ARC release: release one owning reference, free on the LAST one.
 * 0-based RC (matching the VM/AOT unified header): rc == 0 means a single
 * owner, so a release at rc == 0 frees; otherwise it just decrements. The
 * old `--rc <= 0` form freed one reference too early for multi-owner objects
 * (rc == 1 means two owners, but `--rc == 0 <= 0` would have freed it).
 * No-op for values that do not carry an XrObjHeader. */
/* Finalize one dead object (run its destructor, free its block). The
 * destructor releases child references, which may recurse back into
 * xrt_release. */
static inline void xrt_finalize_payload(XrObjHeader *hdr) {
    void *obj =
        (hdr->extra & XR_OBJ_AOT_NATIVE) ? (void *) hdr : (char *) hdr + sizeof(XrObjHeader);
    if (hdr->extra & XR_OBJ_HAS_DTOR) {
        uint16_t class_type_id = xrt_aot_class_type_id(hdr);
        if (class_type_id != 0)
            xrt_dispatch_destructor(class_type_id, obj);
        else if (hdr->_rsv != XRT_ARC_KIND_NONE)
            xrt_dispatch_builtin_destructor(hdr->_rsv, obj);
        else
            xrt_dispatch_destructor(hdr->type, obj);
    }
}

static inline void xrt_execution_free_allocation(XrObjHeader *hdr) {
    if (!hdr)
        return;
    if (hdr->extra & XR_OBJ_AOT_SWEEP)
        return;
    if (hdr->extra & XR_OBJ_AOT_EXECUTION)
        xrt_execution_unlink(hdr);
    if (hdr->extra & XR_OBJ_AOT_ALLOCATION) {
        XrtExecutionAllocation *node = xrt_execution_node(hdr);
        node->magic = 0;
        XRT_FREE(node);
    } else {
        XRT_FREE(hdr);
    }
}

static inline void xrt_finalize_one(XrObjHeader *hdr) {
    xrt_finalize_payload(hdr);
#ifdef XR_RC_GUARD
    /* --rc-guard: quarantine instead of freeing so a later touch is caught.
     * The destructor above has already released children; stamp the poison
     * marker last (it overwrites _rsv, which the dtor dispatch just consumed). */
    if (!(hdr->extra & XR_OBJ_STORAGE_STACK)) {
        xrt_rc_guard_poison(hdr);
        return;
    }
#endif
    if (!(hdr->extra & XR_OBJ_STORAGE_STACK))
        xrt_execution_free_allocation(hdr);
}

static void xrt_execution_finalize_generic(XrObjHeader *hdr) {
    xrt_finalize_payload(hdr);
}

static inline int64_t xrt_execution_arena_live_bytes(const void *raw_arena) {
    const XrtExecutionArena *arena = (const XrtExecutionArena *) raw_arena;
    return arena ? (int64_t) arena->live_bytes : 0;
}

static inline int64_t xrt_execution_arena_live_objects(const void *raw_arena) {
    const XrtExecutionArena *arena = (const XrtExecutionArena *) raw_arena;
    return arena ? (int64_t) arena->live_objects : 0;
}

static inline int64_t xrt_execution_arena_finalizer_count(const void *raw_arena) {
    const XrtExecutionArena *arena = (const XrtExecutionArena *) raw_arena;
    return arena ? (int64_t) arena->finalizer_count : 0;
}

/* Mark every member before running any finalizer. A destructor that releases
 * another member of the same residual cycle then observes AOT_SWEEP and does
 * nothing; external published children still take their ordinary RC path.
 *
 * Teardown is deliberately split into finalization and deallocation passes.
 * Finalizers may inspect or release any same-arena peer, so every payload must
 * remain addressable until all finalizers have completed. */
static inline void xrt_execution_arena_dispose(XrtExecutionArena *arena) {
    if (!arena || !arena->initialized || arena->destroying)
        return;
    arena->destroying = 1;
    for (XrtExecutionAllocation *node = arena->head; node; node = node->next) {
        XrObjHeader *hdr = (XrObjHeader *) (node + 1);
        hdr->extra |= XR_OBJ_AOT_SWEEP;
    }
    for (XrtExecutionAllocation *node = arena->head; node; node = node->next) {
        XrObjHeader *hdr = (XrObjHeader *) (node + 1);
        if (node->finalizer)
            node->finalizer(hdr);
        arena->finalizer_count++;
    }
    XrtExecutionAllocation *node = arena->head;
    while (node) {
        XrtExecutionAllocation *next = node->next;
        node->magic = 0;
        XRT_FREE(node);
        node = next;
    }
    arena->head = NULL;
    arena->live_bytes = 0;
    arena->live_objects = 0;
    arena->destroying = 0;
}

static inline void xrt_execution_arena_destroy(void *raw_arena) {
    XrtExecutionArena *arena = (XrtExecutionArena *) raw_arena;
    if (!arena)
        return;
    xrt_execution_arena_dispose(arena);
    arena->initialized = 0;
    if (arena->heap_owned)
        XRT_FREE(arena);
}

/* Queue a dead object for iterative finalization. Returns 0 if the stack could
 * not grow, in which case the caller finalizes recursively instead. */
static int xrt_deferred_push(XrObjHeader *hdr) {
    if (xrt_deferred_count == xrt_deferred_cap) {
        uint32_t new_cap = xrt_deferred_cap ? xrt_deferred_cap * 2u : 64u;
        XrObjHeader **grown = (XrObjHeader **) XRT_REALLOC(
            xrt_deferred_stack, (size_t) new_cap * sizeof(XrObjHeader *));
        if (!grown)
            return 0;
        xrt_deferred_stack = grown;
        xrt_deferred_cap = new_cap;
    }
    xrt_deferred_stack[xrt_deferred_count++] = hdr;
    return 1;
}

static inline void xrt_release(XrValue v) {
    if (v.tag == XR_TAG_STR)
        return;
    if (XR_IS_ARRAY(v) || XR_IS_MAP(v) || XR_IS_SET(v) ||
        (v.tag == XR_TAG_PTR && v.heap_type == 0 && (v.flags & XR_VALUE_FLAG_HEADER_AT_PTR) != 0)) {
        xrt_coll_release(v);
        return;
    }
    if (XR_IS_ARRAY_REF(v)) {
        xrt_array_ref_release_owned(v);
        return;
    }
    if (!xrt_arc_value_has_header(v))
        return;
    XrObjHeader *hdr = xrt_arc_value_header(v);
#ifdef XR_RC_GUARD
    if (hdr->_rsv == XRT_RC_GUARD_POISON)
        xrt_rc_guard_fail("xrt_release", v.ptr); /* double release of a freed object */
#endif
    if (!xrt_rc_claim_release_last(hdr))
        return;

    /* Last owner (rc == 0). Depth-bound the destructor cascade: queue and
     * drain iteratively past the limit so deep graphs cannot blow the stack.
     * The object's destructor has not run yet, so its user data is still live
     * and cannot hold the queue link — xrt_deferred_push keeps it on the side.
     * A failed push (OOM) falls through to the recursive path. */
    if (xrt_release_depth >= XRT_RELEASE_DEPTH_LIMIT && xrt_deferred_push(hdr))
        return;

    xrt_release_depth++;
    xrt_finalize_one(hdr);
    xrt_release_depth--;

    /* NULL unless this cascade actually went deep, so the common release costs
     * one thread-local load and a branch, as it did with the intrusive list. */
    if (xrt_release_depth == 0 && xrt_deferred_stack) {
        /* Re-read the globals each turn: a destructor run here can queue and
         * drain again, which reallocates or empties the stack under us. */
        while (xrt_deferred_count > 0)
            xrt_finalize_one(xrt_deferred_stack[--xrt_deferred_count]);
        /* Deep cascades are rare and there is no thread-exit hook to reclaim
         * the buffer, so release it rather than hold it per thread. */
        XRT_FREE(xrt_deferred_stack);
        xrt_deferred_stack = NULL;
        xrt_deferred_cap = 0;
    }
}

static inline XrValue xrt_value_to_owned(XrValue v);

#if defined(__clang_analyzer__)
/* Clang's path-sensitive analyzer does not otherwise understand that an ARC
 * allocation embedded in the returned XrValue transfers ownership to the
 * caller.  This declaration is visible only to the analyzer: the unknown
 * external call models that escape without adding code or a link dependency
 * to ordinary generated-C builds. */
extern void xrt_clang_analyzer_escape_owned_pointer(void *ptr);
#endif

/* Materialize an independent fixed-array value.  This differs from
 * xrt_array_ref_to_owned(): an already-owned source must still get new outer
 * storage for value-copy semantics.  Reference-valued lanes retain their
 * elements rather than recursively cloning the referenced objects. */
static inline XrValue xrt_array_ref_clone_value(XrValue v) {
    if (!XR_IS_ARRAY_REF(v) || !v.ptr)
        return v;
    uint8_t elem_type = XR_ARRAY_REF_ELEM_TYPE(v);
    uint32_t elem_count = XR_ARRAY_REF_ELEM_COUNT(v);
    size_t elem_size = xrt_value_native_type_size(elem_type);
    size_t size = elem_size * (size_t) elem_count;
    void *dst = xrt_arc_alloc(size);
    if (elem_type == XR_NATIVE_VALUE) {
        XrValue *dst_values = (XrValue *) dst;
        const XrValue *src_values = (const XrValue *) v.ptr;
        for (uint32_t i = 0; i < elem_count; i++)
            dst_values[i] = xrt_value_to_owned(src_values[i]);
    } else {
        memcpy(dst, v.ptr, size);
    }
#if defined(__clang_analyzer__)
    xrt_clang_analyzer_escape_owned_pointer(dst);
#endif
    return xr_array_ref_owned(dst, elem_type, elem_count);
}

static inline XrValue xrt_array_ref_to_owned(XrValue v) {
    if (!XR_IS_ARRAY_REF(v) || !v.ptr)
        return v;
    if ((v.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) != 0) {
        xrt_retain(v);
        return v;
    }
    return xrt_array_ref_clone_value(v);
}

/* Preserve non-array and already-owned values while materializing a borrowed
 * array-ref.  Keeping this as an ordinary C helper lets generated code remain
 * portable to MSVC instead of relying on a GNU statement expression. */
static inline XrValue xrt_array_ref_ensure_owned(XrValue v) {
    if (XR_IS_ARRAY_REF(v) && (v.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) == 0)
        return xrt_array_ref_to_owned(v);
    return v;
}

static inline void xrt_array_ref_release_owned(XrValue v) {
    if (!XR_IS_ARRAY_REF(v) || (v.flags & XRT_VALUE_FLAG_ARRAY_REF_OWNED) == 0 || !v.ptr)
        return;
    XrObjHeader *hdr = XRT_ARC_HDR(v.ptr);
    if (!xrt_rc_claim_release_last(hdr))
        return;
    if (XR_ARRAY_REF_ELEM_TYPE(v) == XR_NATIVE_VALUE) {
        XrValue *values = (XrValue *) v.ptr;
        uint32_t count = XR_ARRAY_REF_ELEM_COUNT(v);
        for (uint32_t i = 0; i < count; i++)
            xrt_release(values[i]);
    }
    xrt_finalize_one(hdr);
}

static inline XrValue xrt_value_to_owned(XrValue v) {
    if (XR_IS_ARRAY_REF(v))
        return xrt_array_ref_to_owned(v);
    xrt_retain(v);
    return v;
}

static inline void xrt_arc_init(void) {
    XrtExecutionArena *root = xrt_execution_root();
    xrt_current_execution_arena = root;
}

static inline void xrt_arc_shutdown(void) {
    XrtExecutionArena *root = xrt_execution_root();
    xrt_execution_arena_dispose(root);
    xrt_current_execution_arena = NULL;
}

/* =========================================================================
 * String constructors — header + bytes in one execution allocation.
 * Layout: [XrObjHeader][xrt_str_t][len+1 bytes]; data points at the tail.
 * Callers fill the bytes via xr_str_buf() after xrt_str_alloc().
 * ========================================================================= */

static inline XrValue xrt_str_alloc(size_t len) {
    xrt_str_t *h = (xrt_str_t *) xrt_arc_alloc(sizeof(xrt_str_t) + len + 1);
    h->len = (int64_t) len;
    h->rune_len = -1;
    h->hash = 0;
    h->flags = 0;
    h->data = (char *) (h + 1);
    h->data[len] = 0;
    return xr_mkptr(h, XR_TAG_STR_ARC);
}

/* Wrap a NUL-terminated C string with static storage duration (literals,
 * argv, environment) without copying the bytes. */
static inline XrValue xr_box_str(const char *s) {
    xrt_str_t *h = (xrt_str_t *) xrt_arc_alloc(sizeof(xrt_str_t));
    h->len = (int64_t) strlen(s);
    h->rune_len = -1;
    h->hash = 0;
    h->flags = 0;
    h->data = (char *) s;
    return xr_mkptr(h, XR_TAG_STR_ARC);
}

/* Copy a transient C buffer (stack scratch, number formatting) into a fresh
 * heap string. */
static inline XrValue xrt_str_from_cstr(const char *s) {
    size_t len = strlen(s);
    XrValue v = xrt_str_alloc(len);
    memcpy(xr_str_buf(v), s, len);
    return v;
}

/* Copy a (data, len) slice into a fresh heap string. */
static inline XrValue xrt_str_from_slice(const char *data, size_t len) {
    XrValue v = xrt_str_alloc(len);
    if (len > 0 && data)
        memcpy(xr_str_buf(v), data, len);
    return v;
}

static inline XrValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    XrValue v = xrt_str_alloc(la + lb);
    char *r = xr_str_buf(v);
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return v;
}

/* Fast path for string + string: lengths come from the headers. */
static inline XrValue xrt_str_concat_value(XrValue a, XrValue b) {
    size_t la = (size_t) xr_str_len(a);
    size_t lb = (size_t) xr_str_len(b);
    XrValue v = xrt_str_alloc(la + lb);
    char *r = xr_str_buf(v);
    memcpy(r, xr_str_data(a), la);
    memcpy(r + la, xr_str_data(b), lb);
    return v;
}

#endif  // XRT_ARC_H
