/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_coll.h - Collection runtime: Array, Map, StringBuilder, Closure, index ops
 */

#ifndef XRT_COLL_H
#define XRT_COLL_H

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

#include "xrt_value.h"
#include "xrt_callable.h"
#include "xrt_arc.h"  // xrt_str_alloc used by xrt_strbuf_finish
#include "xrt_net.h"
#include "xrt_range.h"
#include "xrt_class.h"
#include "xrt_sys.h"
#include "../runtime/xerror_codes.h"
#include "../shared/xr_array_abi.h"
#include "../shared/xr_array_core.h"
#include "../shared/xr_builtin_schema.h"
#include "../shared/xr_byte_array_append_core.h"
#include "../shared/xr_byte_array_copy_core.h"
#include "../shared/xr_byte_array_repeat_core.h"
#include "../shared/xr_cell_access_core.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_error_core.h"
#include "../shared/xr_enum_metadata_core.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_map_set_abi.h"
#include "../shared/xr_json_type.h"
#include "../shared/xobject_shape.h"
#include "../shared/xr_pod_slice_core.h"
#include "../shared/xr_range_core.h"
#include "../shared/xr_typed_ops.h"
#include <errno.h>
#include <string.h>

#define xrt_byte_array_copy_semantics(kind, dst_data, dst_length, dst_elem_type, src_data,        \
                                      src_length, src_elem_type, src_offset, dst_offset, count)   \
    XR_BYTE_ARRAY_COPY_OWNER_APPLY(                                                              \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI,                                               \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO, XR_SEM_CONSUMER_AOT_HOSTED,                  \
        xr_byte_array_copy_core((kind), (dst_data), (dst_length), (dst_elem_type), (src_data),   \
                                (src_length), (src_elem_type), (src_offset), (dst_offset),       \
                                (count)))

#define xrt_byte_array_repeat_semantics(view, distance, count, reserve_fn, reserve_ctx)            \
    XR_BYTE_ARRAY_REPEAT_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_LO, XR_SEM_CONSUMER_AOT_HOSTED,                \
        xr_byte_array_repeat_tail_core((view), (distance), (count), (reserve_fn),                \
                                       (reserve_ctx)))

#define xrt_byte_array_append_semantics(view, src_data, src_length, src_elem_type, src_guard,     \
                                        reserve_fn, reserve_ctx)                                  \
    XR_BYTE_ARRAY_APPEND_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_LO, XR_SEM_CONSUMER_AOT_HOSTED,                \
        xr_byte_array_append_core((view), (src_data), (src_length), (src_elem_type),             \
                                  (src_guard), (reserve_fn), (reserve_ctx)))

#define xrt_pod_slice_copy_semantics(dst_data, dst_length, dst_elem_size, src_data, src_length,   \
                                     src_elem_size)                                               \
    XR_POD_SLICE_COPY_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO,     \
        XR_SEM_CONSUMER_AOT_HOSTED,                                                              \
        xr_pod_slice_copy_core((dst_data), (dst_length), (dst_elem_size), (src_data),            \
                               (src_length), (src_elem_size)))
#define xrt_pod_slice_fill_semantics(data, length, elem_size, kind, value)                        \
    XR_POD_SLICE_FILL_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO,     \
        XR_SEM_CONSUMER_AOT_HOSTED,                                                             \
        xr_pod_slice_fill_core((data), (length), (elem_size), (kind), (value)))
#define xrt_pod_slice_compare_semantics(left_data, left_length, left_elem_size, right_data,       \
                                        right_length, right_elem_size)                            \
    XR_POD_SLICE_COMPARE_OWNER_APPLY(                                                            \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI,                                             \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO, XR_SEM_CONSUMER_AOT_HOSTED,                \
        xr_pod_slice_compare_core((left_data), (left_length), (left_elem_size), (right_data),    \
                                  (right_length), (right_elem_size)))
#define xrt_pod_slice_view_semantics(kind, data, length, source_elem_size, source_has_layout,     \
                                     target_elem_size, target_expected_elem_size,                 \
                                     target_alignment, target_layout_valid, target_is_aggregate) \
    XR_POD_SLICE_VIEW_OWNER_APPLY(                                                               \
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO,     \
        XR_SEM_CONSUMER_AOT_HOSTED,                                                              \
        xr_pod_slice_view_core((kind), (data), (length), (source_elem_size),                    \
                               (source_has_layout), (target_elem_size),                          \
                               (target_expected_elem_size), (target_alignment),                 \
                               (target_layout_valid), (target_is_aggregate)))

#define xrt_byte_slice_common_prefix_semantics(left_data, left_length, right_data, right_length,   \
                                               ok)                                                \
    XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY(                                                      \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI,                                       \
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO, XR_SEM_CONSUMER_AOT_HOSTED,           \
        xr_byte_slice_common_prefix_core((left_data), (left_length), XR_ELEM_U8, (right_data),    \
                                         (right_length), XR_ELEM_U8, (ok)))

static inline XrValue xrt_value_clone_for_coro(XrValue val);
static inline XrValue xrt_value_set_storage_graph(XrValue value, uint8_t storage_mode);
XRT_COLD _Noreturn void xrt_type_no_index(const char *message);
XRT_COLD _Noreturn void xrt_index_oob(int64_t index, int64_t length);
static void xrt_execution_finalize_array(XrObjHeader *hdr);
static void xrt_execution_finalize_map(XrObjHeader *hdr);
static void xrt_execution_finalize_boolmap(XrObjHeader *hdr);
static void xrt_execution_finalize_set(XrObjHeader *hdr);
static void xrt_execution_finalize_struct_object(XrObjHeader *hdr);

/* =========================================================================
 * Array runtime
 * ========================================================================= */

/* Shared array fields (data/length/capacity/source/data_storage/elem_*) come
 * from XR_ARRAY_ABI_FIELDS so the AOT and VM array layouts stay in lockstep.
 *
 * Alignment contract: non-slice arrays are XRT_DATA_ALIGN-aligned (32 bytes,
 * AVX-width). Initial storage lives in the header block or stack frame at a
 * rounded-up address; growth spills through xrt_array_data_grow into aligned
 * heap storage. Generated _adN caches assert it via XR_ASSUME_ALIGNED. Slice
 * views alias a sub-range of another array's storage at an arbitrary element
 * offset — no alignment promise.
 *
 * `source` and `contains_refs`/`elem_tid` are part of the shared ABI; AOT slice
 * views retain `source` when they borrow another array's element storage, while
 * the GC-tracking fields remain VM-owned metadata. */
typedef struct {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrArray so
                      * the two layouts line up (C0 object-header unification) */
    XR_ARRAY_ABI_FIELDS;
    /* Keep the complete VM array object size/layout.  A hosted fragment may
     * return this object directly to the VM; the VM-only flag remains zero for
     * AOT-owned buffers but must occupy its canonical slot. */
    uint8_t data_on_region_heap;
    uint8_t _vm_abi_pad[2];
} xrt_array_t;

/* Portable hosted-fragment accessors.  They deliberately take boxed values so
 * generated MSVC C does not need GNU statement expressions for a temporary
 * array/span pointer and index.  The optimizer still sees every operation as
 * an inline load/store. */
static inline XrValue xrt_array_index_get_portable(xrt_array_t *array, int64_t index, int checked,
                                                   int owned) {
    if (!array || (checked && (index < 0 || index >= array->length))) {
        xrt_index_oob(index, array ? array->length : 0);
        return XR_NULL_VAL;
    }
    XrValue value = xr_typed_get(array->data, (int32_t) index, array->elem_type);
    return owned ? xrt_value_to_owned(value) : value;
}

static inline XrValue xrt_array_index_set_portable(xrt_array_t *array, int64_t index, XrValue value,
                                                   int checked) {
    if (!array || (checked && (index < 0 || index >= array->length))) {
        xrt_index_oob(index, array ? array->length : 0);
        return XR_NULL_VAL;
    }
    if (xr_typed_set(array->data, (int32_t) index, value, array->elem_type))
        XR_ARRAY_MARK_MUTATED(array);
    return XR_NULL_VAL;
}

#ifdef XRT_IMPL
atomic_flag xrt_array_storage_promotion_lock = ATOMIC_FLAG_INIT;
#else
extern atomic_flag xrt_array_storage_promotion_lock;
#endif

static inline void xrt_array_storage_promotion_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(&xrt_array_storage_promotion_lock,
                                             memory_order_acquire)) {
    }
}

static inline void xrt_array_storage_promotion_lock_release(void) {
    atomic_flag_clear_explicit(&xrt_array_storage_promotion_lock, memory_order_release);
}

static inline size_t xrt_array_data_bytes_or_abort(int64_t cap, uint8_t elem_size,
                                                   const char *where) {
    if (cap < 0)
        cap = 0;
    if (elem_size == 0)
        elem_size = 1;
    if ((uint64_t) cap > (uint64_t) SIZE_MAX / (uint64_t) elem_size) {
        fprintf(stderr, "%s: capacity overflow\n", where);
        abort();
    }
    return (size_t) cap * (size_t) elem_size;
}

static inline void xrt_coll_set_storage_header(XrObjHeader *h, uint8_t storage_mode) {
    if (!h)
        return;
    uint8_t previous_storage = XR_OBJ_GET_STORAGE(h);
    int32_t previous_refcount = atomic_load_explicit(&h->refcount, memory_order_relaxed);
    if (storage_mode != XR_OBJ_STORAGE_NORMAL)
        xrt_execution_unbind(h);
    XR_OBJ_SET_STORAGE(h, storage_mode);
    if (h->extra & XR_OBJ_IMMORTAL)
        return;

    /* Local and shared objects encode the same ownership count differently:
     * local rc=N means N+1 owners, while shared rc=-N means N owners. Storage
     * promotion happens after child values may already have been retained, so
     * resetting every promoted header to -1 drops those owners and lets the
     * first release destroy a still-referenced child. Preserve the count while
     * changing only its encoding. The graph is execution-local until this
     * promotion completes, so no shared observer can race this transition. */
    int32_t next_refcount = previous_refcount;
    if (storage_mode == XR_OBJ_STORAGE_SHARED && previous_storage != XR_OBJ_STORAGE_SHARED &&
        previous_refcount >= 0) {
        int64_t owners = (int64_t) previous_refcount + 1;
        next_refcount = owners >= -(int64_t) XR_RC_STICKY_BAND ? XR_RC_STICKY : (int32_t) -owners;
    } else if (storage_mode != XR_OBJ_STORAGE_SHARED && previous_storage == XR_OBJ_STORAGE_SHARED &&
               previous_refcount < 0 && previous_refcount > XR_RC_STICKY_BAND) {
        next_refcount = (int32_t) (-(int64_t) previous_refcount - 1);
    }
    atomic_store_explicit(&h->refcount, next_refcount, memory_order_relaxed);
}

static inline void xrt_array_init_header(xrt_array_t *a, int64_t cap, uint8_t etype,
                                         uint8_t elem_size) {
    a->length = 0;
    a->capacity = cap;
    a->source = NULL;
    a->storage = NULL;
    a->elem_type = etype;
    a->elem_size = elem_size;
    a->elem_tid = 0;
    a->contains_refs = 0;
    a->content_version = XR_ARRAY_CONTENT_VERSION_INIT;
    a->deferred_submit_version = 0;
    a->data_storage = XR_ARRAY_DATA_INLINE;
}

/* ===== Refcounted array storage block (task 143/144 M2), AOT side =====
 * Mirrors the VM logic in xarray.c: a sliced array's buffer becomes a
 * refcounted system-heap block shared by the array and its slices, so a later
 * grow forks the buffer (snapshot) instead of freeing it under live views.
 * Covers POD and ANY elements; for ANY the storage owns the XrValue refs and
 * releases them at refcount 0. Buffers stay XRT_DATA_ALIGN-aligned so generated
 * XR_ASSUME_ALIGNED caches remain valid. */
static inline XrArrayStorage *xrt_array_storage_alloc(void *data, int64_t bytes, uint8_t is_any) {
    XrArrayStorage *s = (XrArrayStorage *) XRT_MALLOC(sizeof(XrArrayStorage));
    if (XR_UNLIKELY(!s)) {
        fprintf(stderr, "xrt_array_storage_alloc: out of memory\n");
        abort();
    }
    atomic_store_explicit(&s->refcount, 1, memory_order_relaxed);
    s->data = data;
    s->byte_capacity = bytes;
    s->account_heap = NULL;
    s->accounted_bytes = 0;
    s->elem_count = 0;
    s->elem_is_any = is_any;
    return s;
}

static inline void xrt_array_storage_retain_block(XrArrayStorage *s) {
    if (s)
        atomic_fetch_add_explicit(&s->refcount, 1, memory_order_acq_rel);
}

static inline void xrt_array_storage_release_block(XrArrayStorage *s) {
    if (!s)
        return;
    if (atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel) == 1) {
        if (s->elem_is_any && s->data) {
            XrValue *items = (XrValue *) s->data;
            for (int64_t i = 0; i < s->elem_count; i++)
                xrt_release(items[i]);
        }
        if (s->data)
            XRT_FREE_ALIGNED(s->data);
        XRT_FREE(s);
    }
}

static inline void xrt_array_ensure_storage(xrt_array_t *a) {
    if (!a || a->storage || a->data_storage == XR_ARRAY_DATA_BORROWED)
        return;
    if (a->data_storage == XR_ARRAY_DATA_STACK)
        return;
    xrt_array_storage_promotion_lock_acquire();
    if (a->storage) {
        xrt_array_storage_promotion_lock_release();
        return;
    }
    uint8_t is_any = (a->elem_type == XR_ELEM_ANY) ? 1 : 0;
    int64_t bytes = (int64_t) a->elem_size * a->capacity;
    void *buf = NULL;
    if (bytes > 0) {
        buf = XRT_ALLOC_ALIGNED((size_t) bytes);
        if (XR_UNLIKELY(!buf)) {
            fprintf(stderr, "xrt_array_ensure_storage: out of memory\n");
            abort();
        }
        if (a->data)
            memcpy(buf, a->data, (size_t) bytes); /* ANY: moves the XrValue refs */
    }
    XrArrayStorage *s = xrt_array_storage_alloc(buf, bytes, is_any);
    s->elem_count = is_any ? a->length : 0;
    /* Free the old buffer only if it was a separately allocated heap buffer;
     * INLINE buffers live inside the header allocation and are reclaimed with it.
     * For ANY this frees the raw buffer only — the element refs moved to storage. */
    if (a->data && a->data_storage == XR_ARRAY_DATA_HEAP)
        XRT_FREE_ALIGNED(a->data);
    a->storage = s;
    a->data = buf;
    a->data_storage = XR_ARRAY_DATA_HEAP;
    xrt_array_storage_promotion_lock_release();
}

static inline void xrt_throw_error(int code, const char *message);

static inline void xrt_array_check_store_or_abort(const xrt_array_t *a, XrValue val,
                                                  const char *where) {
    (void) where;
    if (XR_UNLIKELY(a && !xr_typed_value_is_storable(val, a->elem_type))) {
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, a->elem_type == XR_ELEM_RUNE
                                                  ? "Array<char> element must be char"
                                                  : "typed array element must be numeric");
    }
}

static inline xrt_array_t *xrt_array_alloc_inline(int64_t cap, uint8_t etype, int zeroed,
                                                  const char *where) {
    if (etype >= XR_ELEM_COUNT)
        etype = XR_ELEM_ANY;
    uint8_t elem_size = XR_ELEM_SIZES[etype];
    size_t data_bytes = xrt_array_data_bytes_or_abort(cap, elem_size, where);
    size_t pad = data_bytes ? (XRT_DATA_ALIGN - 1) : 0;
    if (data_bytes > SIZE_MAX - sizeof(xrt_array_t) - pad) {
        fprintf(stderr, "%s: allocation size overflow\n", where);
        abort();
    }
    size_t total = sizeof(xrt_array_t) + data_bytes + pad;
    xrt_array_t *a =
        (xrt_array_t *) xrt_execution_alloc_embedded(total, xrt_execution_finalize_array);
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "%s: out of memory\n", where);
        abort();
    }
    xrt_heap_header_init(&a->hdr, XR_TARRAY);
    xrt_array_init_header(a, cap, etype, elem_size);
    if (data_bytes) {
        a->data =
            (void *) (((uintptr_t) ((char *) a + sizeof(xrt_array_t)) + (XRT_DATA_ALIGN - 1)) &
                      ~(uintptr_t) (XRT_DATA_ALIGN - 1));
        if (zeroed)
            memset(a->data, 0, data_bytes);
    } else {
        a->data = NULL;
    }
    return a;
}

/* Grow the element buffer to hold new_cap elements. Initial array storage lives
 * inline in the header allocation; the first growth spills to an aligned heap
 * buffer, and later growth frees the previous heap buffer. Stack arrays abort
 * instead of spilling so alloca-backed values cannot outlive their frame. */
static inline void xrt_array_data_grow(xrt_array_t *a, int64_t new_cap) {
    if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_STACK)) {
        fprintf(stderr, "xrt_array_data_grow: stack array capacity exceeded\n");
        abort();
    }
    size_t old_bytes =
        xrt_array_data_bytes_or_abort(a->capacity, a->elem_size, "xrt_array_data_grow");
    size_t new_bytes = xrt_array_data_bytes_or_abort(new_cap, a->elem_size, "xrt_array_data_grow");

    if (a->storage) {
        /* Sliced array: grow via the refcounted storage block. Fork a private
         * copy when shared with slices so existing views keep a stable snapshot. */
        XrArrayStorage *s = (XrArrayStorage *) a->storage;
        void *tmp = XRT_ALLOC_ALIGNED(new_bytes);
        if (XR_UNLIKELY(!tmp)) {
            fprintf(stderr, "xrt_array_data_grow: out of memory\n");
            abort();
        }
        if (a->data && old_bytes > 0)
            memcpy(tmp, a->data, old_bytes);
        if (new_bytes > old_bytes)
            memset((uint8_t *) tmp + old_bytes, 0, new_bytes - old_bytes);
        if (atomic_load_explicit(&s->refcount, memory_order_acquire) == 1) {
            XRT_FREE_ALIGNED(s->data);
            s->data = tmp;
            s->byte_capacity = (int64_t) new_bytes;
            if (s->elem_is_any)
                s->elem_count = a->length;
        } else {
            /* Fork: both buffers now reference the same ANY elements, so retain
             * each for the new copy and freeze the old storage's owning count. */
            XrArrayStorage *ns = xrt_array_storage_alloc(tmp, (int64_t) new_bytes, s->elem_is_any);
            if (s->elem_is_any) {
                XrValue *src = (XrValue *) a->data;
                XR_ASSUME(a->length == 0 || src != NULL);
                for (int64_t i = 0; i < a->length; i++)
                    xrt_retain(src[i]);
                ns->elem_count = a->length;
                s->elem_count = a->length;
            }
            atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel);
            /* This array leaves the shared (old) storage. */
            a->storage = ns;
        }
        a->data = tmp;
        a->capacity = new_cap;
        return;
    }
    void *tmp = XRT_ALLOC_ALIGNED(new_bytes);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_array_data_grow: out of memory\n");
        abort();
    }
    if (a->data && old_bytes > 0) {
        memcpy(tmp, a->data, old_bytes);
        if (a->data_storage == XR_ARRAY_DATA_HEAP) {
            XRT_FREE_ALIGNED(a->data);
        } else if (a->data_storage == XR_ARRAY_DATA_INLINE) {
            memset(a->data, 0, old_bytes);
        }
    }
    if (new_bytes > old_bytes)
        memset((uint8_t *) tmp + old_bytes, 0, new_bytes - old_bytes);
    a->data = tmp;
    a->data_storage = XR_ARRAY_DATA_HEAP;
    a->capacity = new_cap;
}

static inline int xrt_array_data_is_inline(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_INLINE;
}

static inline int xrt_array_data_is_heap(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_HEAP;
}

static inline int xrt_array_data_is_stack(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_STACK;
}

static inline int xrt_array_data_is_borrowed(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_BORROWED;
}

static inline XrValue xrt_array_new(int64_t len) {
    if (len < 0)
        len = 0;
    int64_t cap = len < 4 ? 4 : len;
    xrt_array_t *a = xrt_array_alloc_inline(cap, XR_ELEM_ANY, 1, "xrt_array_new");
    a->length = len;
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline XrValue xrt_array_with_capacity(int64_t cap) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = xrt_array_alloc_inline(cap, XR_ELEM_ANY, 1, "xrt_array_with_capacity");
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_ptr(int64_t len, uint8_t etype) {
    if (len < 0)
        len = 0;
    int64_t cap = len < 4 ? 4 : len;
    xrt_array_t *a = xrt_array_alloc_inline(cap, etype, 1, "xrt_array_new_typed");
    a->length = len;
    return a;
}

/* `etype` is the storage lane; `elem_tid` is the element's XrTypeId, 0 when the
 * creator has none to give. The two answer different questions: Array<Json> and
 * Array<string> share the tagged lane but not the element type, and the domain
 * test needs the latter to tell a Json array from any other tagged one. The VM
 * carries the same pair on its array-creation opcode. */
static inline XrValue xrt_array_new_typed(int64_t len, uint8_t etype, uint8_t elem_tid) {
    xrt_array_t *a = xrt_array_new_typed_ptr(len, etype);
    if (a)
        a->elem_tid = elem_tid;
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_copy(int64_t len, uint8_t etype,
                                                    const void *values) {
    xrt_array_t *array = xrt_array_new_typed_ptr(len, etype);
    if (len > 0 && values)
        memcpy(array->data, values, (size_t) len * (size_t) array->elem_size);
    return array;
}

static inline xrt_array_t *xrt_array_set_storage_ptr(xrt_array_t *array, uint8_t storage_mode) {
    if (array)
        xrt_coll_set_storage_header(&array->hdr, storage_mode);
    return array;
}

static inline XrValue xrt_array_set_storage(XrValue value, uint8_t storage_mode) {
    return xrt_value_set_storage_graph(value, storage_mode);
}

static inline XrValue xrt_array_with_capacity_typed(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = xrt_array_alloc_inline(cap, etype, 1, "xrt_array_with_capacity_typed");
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_uninit_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    return xrt_array_alloc_inline(cap, etype, 0, "xrt_array_new_typed_uninit");
}

static inline XrValue xrt_array_new_typed_uninit(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_uninit_ptr(cap, etype), XR_TAG_ARRAY);
}
static inline XrValue xrt_struct_object_new_named(int64_t field_count,
                                                  const char *const *field_names);
static inline void xrt_object_set_field(XrValue obj, int field_idx, XrValue val);
XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc);

static inline XrValue xrt_error_message_value(const char *message) {
    if (!message)
        return XR_NULL_VAL;
    size_t len = strlen(message);
    XrValue s = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(s), message, len);
    return s;
}

static inline XrValue xrt_structured_error_value(int code, const char *message) {
    XrValue exc = xrt_struct_object_new_named(EXCEPTION_FIELD_COUNT, xr_exception_field_names());
    xrt_object_set_field(exc, EXCEPTION_FIELD_MESSAGE, xrt_error_message_value(message));
    xrt_object_set_field(exc, EXCEPTION_FIELD_STACK, xrt_array_new(0));
    xrt_object_set_field(exc, EXCEPTION_FIELD_CAUSE, XR_NULL_VAL);
    xrt_object_set_field(exc, EXCEPTION_FIELD_CODE, XR_FROM_INT(code));
    xrt_object_set_field(exc, EXCEPTION_FIELD_DATA, XR_NULL_VAL);
    return exc;
}

static inline void xrt_throw_error(int code, const char *message) {
    xrt_throw_exc(xrt_structured_error_value(code, message));
}

static inline int64_t xrt_enum_metadata_access_variant_at(int64_t count, int64_t index) {
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,
        xr_enum_metadata_variant_at_core(count, index));
    if (result.status != XR_ENUM_METADATA_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_ENUM_VARIANT_INDEX_OOB_MSG);
    return result.value;
}

static inline int64_t xrt_enum_metadata_access_payload_at(uint64_t view, int64_t index) {
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_AOT_HOSTED,
        xr_enum_metadata_payload_at_core(view, index));
    if (result.status != XR_ENUM_METADATA_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_ENUM_PAYLOAD_INDEX_OOB_MSG);
    return result.value;
}

static inline int64_t xrt_numeric_float_to_int_or_throw(double source, uint8_t target_rep,
                                                        uint8_t pointer_bits) {
    int64_t result = 0;
    if (!xr_numeric_float_to_int(source, target_rep, pointer_bits, &result))
        xrt_throw_error(XR_ERR_OVERFLOW, XR_ERROR_CORE_NUMERIC_CONVERSION_RANGE_MSG);
    return result;
}

static inline void xrt_array_push(XrValue arr, XrValue val) {
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    xrt_array_check_store_or_abort(a, val, "xrt_array_push");
    if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_BORROWED)) {
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG);
    }
    if (XR_UNLIKELY(a->length >= a->capacity))
        xrt_array_data_grow(a, a->capacity == 0 ? 4 : a->capacity * 2);
    xr_typed_set(a->data, (int32_t) a->length, val, a->elem_type);
    a->length++;
    XR_ARRAY_MARK_MUTATED(a);
}

static inline XrValue xrt_array_clear_value(XrValue arr) {
    if (!XR_IS_ARRAY(arr) || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    if (a->elem_type == XR_ELEM_ANY && a->data && a->length > 0 && !a->storage &&
        a->data_storage != XR_ARRAY_DATA_BORROWED) {
        XrValue *items = (XrValue *) a->data;
        for (int64_t i = 0; i < a->length; i++) {
            xrt_release(items[i]);
            items[i] = XR_NULL_VAL;
        }
    }
    a->length = 0;
    XR_ARRAY_MARK_MUTATED(a);
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_box_new_payloads(uint32_t layout_id, const char *enum_name,
                                                const char *member_name, uint32_t member_index,
                                                uint32_t payload_count, const XrValue *payloads) {
    size_t alloc_size = sizeof(XrAotEnumBox) + (size_t) payload_count * sizeof(XrValue);
    XrAotEnumBox *ev =
        (XrAotEnumBox *) xrt_execution_alloc_embedded(alloc_size, xrt_execution_finalize_generic);
    if (XR_UNLIKELY(!ev)) {
        fprintf(stderr, "xrt_enum_box_new_payloads: out of memory\n");
        abort();
    }
    XrObjHeader *hdr = (XrObjHeader *) ev;
    xrt_heap_header_init(hdr, XR_TINSTANCE);
    hdr->_rsv = XRT_ARC_KIND_ENUM_BOX;
    hdr->extra |= XR_OBJ_HAS_DTOR;
    ev->enum_name = enum_name;
    ev->member_name = member_name;
    ev->member_index = member_index;
    ev->payload_count = payload_count;
    ev->layout_id = layout_id;
    for (uint32_t i = 0; i < payload_count; i++)
        ev->payloads[i] = payloads ? payloads[i] : XR_NULL_VAL;

    XrValue out = {0};
    out.tag = XR_TAG_ENUM;
    out.ext = ev->member_index;
    out.ptr = ev;
    return out;
}

static inline XrValue xrt_enum_aggregate_box(XrAotEnumAggregate value) {
    return xrt_enum_box_new_payloads(value.layout_id, value.enum_name, value.member_name,
                                     (uint32_t) value.tag, value.payload_count, value.payloads);
}

/* Inline enum aggregates carry ownership in their active payload lanes even
 * though the aggregate itself has no heap header.  ARC emits these helpers for
 * aggregate RETAIN/RELEASE ops; inactive and scalar lanes are ordinary tagged
 * no-ops in xrt_retain/xrt_release. */
static inline void xrt_enum_aggregate_retain(XrAotEnumAggregate value) {
    uint32_t limit = value.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                         ? value.payload_count
                         : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        xrt_retain(value.payloads[i]);
}

static inline void xrt_enum_aggregate_release(XrAotEnumAggregate value) {
    uint32_t limit = value.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                         ? value.payload_count
                         : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        xrt_release(value.payloads[i]);
}

/* Box a borrowed inline aggregate.  The box owns its payload lanes, while the
 * source aggregate keeps its owners until its normal ARC cleanup. */
static inline XrValue xrt_enum_aggregate_box_from_borrowed(XrAotEnumAggregate value) {
    uint32_t limit = value.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                         ? value.payload_count
                         : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        xrt_retain(value.payloads[i]);
    return xrt_enum_aggregate_box(value);
}

static inline XrAotEnumAggregate xrt_enum_aggregate_from_boxed(XrValue boxed) {
    if (boxed.tag != XR_TAG_ENUM || !boxed.ptr)
        return xrt_enum_aggregate_zero();
    const XrObjHeader *hdr = (const XrObjHeader *) boxed.ptr;
    if (hdr->type == XR_TENUM_SCALAR_LAYOUT) {
        XrAotEnumAggregate out = xrt_enum_aggregate_zero();
        uint32_t member_index = 0;
        (void) xrt_enum_key_parts(boxed, &out.enum_name, &out.member_name, &member_index,
                                  &out.layout_id);
        out.tag = member_index;
        return out;
    }
    const XrAotEnumBox *ev = (const XrAotEnumBox *) boxed.ptr;
    XrAotEnumAggregate out = xrt_enum_aggregate_zero();
    out.enum_name = ev->enum_name;
    out.member_name = ev->member_name;
    out.tag = ev->member_index;
    out.payload_count = ev->payload_count;
    out.layout_id = ev->layout_id;
    uint32_t limit = ev->payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? ev->payload_count
                                                                     : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        out.payloads[i] = ev->payloads[i];
    return out;
}

/* Consume an owned dynamic box and transfer its payload owners into the
 * returned inline aggregate. Static scalar sidecars and runtime enum metadata
 * are borrowed immutable views and therefore remain untouched. */
static inline XrAotEnumAggregate xrt_enum_aggregate_take_from_boxed(XrValue boxed) {
    XrAotEnumAggregate out = xrt_enum_aggregate_from_boxed(boxed);
    if (!xrt_arc_value_has_header(boxed))
        return out;
    XrAotEnumBox *ev = (XrAotEnumBox *) boxed.ptr;
    uint32_t limit = ev->payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? ev->payload_count
                                                                     : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        ev->payloads[i] = XR_NULL_VAL;
    xrt_release(boxed);
    return out;
}

static inline XrValue xrt_enum_aggregate_field(XrAotEnumAggregate value, int64_t index) {
    if (index == 0)
        return XR_FROM_INT(value.tag);
    if (index > 0 && (uint32_t) index <= value.payload_count &&
        (uint32_t) index <= XR_AOT_ENUM_AGG_PAYLOAD_CAP)
        return value.payloads[index - 1];
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_field_get(XrValue boxed, int64_t index) {
    if (boxed.tag != XR_TAG_ENUM || !boxed.ptr)
        return XR_NULL_VAL;
    uint32_t member_index = 0;
    if (index == 0 && xrt_enum_key_parts(boxed, NULL, NULL, &member_index, NULL))
        return XR_FROM_INT(member_index);
    const XrObjHeader *hdr = (const XrObjHeader *) boxed.ptr;
    const XrAotEnumBox *ev = hdr->type == XR_TENUM_CTOR || hdr->type == XR_TENUM_SCALAR_LAYOUT
                                 ? NULL
                                 : (const XrAotEnumBox *) boxed.ptr;
    if (ev && index > 0 && (uint32_t) index <= ev->payload_count)
        return ev->payloads[index - 1];
    return XR_NULL_VAL;
}

/* Splice every element of `src_val` onto the end of `dst_val` (array spread
 * `[...a]`).  Borrowed source: each copied element is retained because the
 * source array keeps its own references.  Typed primitive arrays bulk-copy
 * (no per-element refcount), boxed (ANY) arrays retain each value. */
static inline void xrt_array_extend(XrValue dst_val, XrValue src_val) {
    xrt_array_t *dst = (xrt_array_t *) dst_val.ptr;
    xrt_array_t *src = (xrt_array_t *) src_val.ptr;
    int64_t n = src->length;
    if (n <= 0)
        return;
    if (XR_UNLIKELY(dst->data_storage == XR_ARRAY_DATA_BORROWED)) {
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG);
    }
    if (dst->elem_type == src->elem_type && dst->elem_type != XR_ELEM_ANY) {
        /* Packed primitive storage: bulk memcpy, no refcount work. */
        int64_t need = dst->length + n;
        if (need > dst->capacity) {
            int64_t ncap = dst->capacity == 0 ? 4 : dst->capacity;
            while (ncap < need)
                ncap *= 2;
            xrt_array_data_grow(dst, ncap);
        }
        memcpy((uint8_t *) dst->data + (size_t) dst->length * dst->elem_size, src->data,
               (size_t) n * src->elem_size);
        dst->length = need;
        return;
    }
    for (int64_t j = 0; j < n; j++) {
        XrValue elem = xr_typed_get(src->data, (int32_t) j, src->elem_type);
        xrt_retain(elem);
        xrt_array_push(dst_val, elem);
    }
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->length;
}

static inline void xrt_array_normalize_slice(int64_t len, int64_t *start, int64_t *end);

#if defined(_MSC_VER)
#define XRT_SPAN_ALIGN __declspec(align(8))
#else
#define XRT_SPAN_ALIGN __attribute__((aligned(8)))
#endif
typedef struct XRT_SPAN_ALIGN {
    void *data;
#if UINTPTR_MAX == UINT32_MAX
    uint32_t _abi_padding;
#endif
    int64_t length;
} xr_span_t;
#undef XRT_SPAN_ALIGN

_Static_assert(sizeof(xr_span_t) == 16, "release Slice ABI must be data + length");
#if !defined(_MSC_VER)
_Static_assert(_Alignof(xr_span_t) == 8, "release Slice ABI must remain 8-byte aligned");
#endif

static inline XrValue xrt_array_from_values(int64_t count, const XrValue *values) {
    XrValue array = xrt_array_with_capacity(count);
    for (int64_t i = 0; i < count; i++)
        xrt_array_push(array, values[i]);
    return array;
}

static inline XrValue xrt_span_index_get_portable(xr_span_t span, int64_t index, uint8_t elem_type,
                                                  int checked, int owned) {
    if (checked && (index < 0 || index >= span.length)) {
        xrt_index_oob(index, span.length);
        return XR_NULL_VAL;
    }
    XrValue value = xr_typed_get(span.data, (int32_t) index, elem_type);
    return owned ? xrt_value_to_owned(value) : value;
}

static inline XrValue xrt_span_index_set_portable(xr_span_t span, int64_t index, XrValue value,
                                                  uint8_t elem_type, int checked) {
    if (checked && (index < 0 || index >= span.length)) {
        xrt_index_oob(index, span.length);
        return XR_NULL_VAL;
    }
    (void) xr_typed_set(span.data, (int32_t) index, value, elem_type);
    return XR_NULL_VAL;
}

/* Box a frame-local Slice descriptor for a typed dynamic-call boundary.  Safe Xray code cannot
 * retain a Slice, so the aggregate reference is valid for the duration of the call and does not
 * allocate or transfer ownership. */
static inline XrValue xrt_span_to_value_ref(xr_span_t *span) {
    XrValue out = {0};
    out.tag = XR_TAG_AGG_REF;
    out.heap_type = UINT16_MAX;
    out.ptr = span;
    return out;
}

static inline XrValue xrt_span_box_value(xr_span_t span) {
    static _Thread_local xr_span_t slots[8];
    static _Thread_local unsigned cursor;
    xr_span_t *slot = &slots[cursor++ & 7u];
    *slot = span;
    return xrt_span_to_value_ref(slot);
}

static inline xr_span_t xrt_span_empty(void) {
    return (xr_span_t) {.data = NULL, .length = 0};
}

static inline xr_span_t xrt_span_from_value_ref(XrValue value) {
    if (value.tag == XR_TAG_AGG_REF && value.ext == 0 && value.heap_type == UINT16_MAX && value.ptr)
        return *(const xr_span_t *) value.ptr;
    xrt_throw_error(XR_ERR_TYPE_MISMATCH, "expected Slice value");
    return xrt_span_empty();
}

static inline xr_span_t xrt_span_from_array_slice(XrValue arr, int64_t start, int64_t end) {
    if (XR_IS_ARRAY_REF(arr)) {
        uint8_t native_type = XR_ARRAY_REF_ELEM_TYPE(arr);
        int64_t len = XR_ARRAY_REF_ELEM_COUNT(arr);
        xrt_array_normalize_slice(len, &start, &end);
        int64_t count = end - start;
        if (count < 0)
            count = 0;
        uint8_t elem_size = (uint8_t) xrt_value_native_type_size(native_type);
        xr_span_t out = {0};
        out.data = (count > 0 && arr.ptr)
                       ? (void *) ((uint8_t *) arr.ptr + (size_t) start * (size_t) elem_size)
                       : arr.ptr;
        out.length = count;
        return out;
    }
    if (!XR_IS_ARRAY(arr) || !arr.ptr)
        return xrt_span_empty();
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    xrt_array_normalize_slice(src->length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    xr_span_t out = {0};
    out.data = (count > 0 && src->data)
                   ? (void *) ((uint8_t *) src->data + (size_t) start * (size_t) src->elem_size)
                   : src->data;
    out.length = count;
    return out;
}

static inline xr_span_t xrt_span_from_span_slice(xr_span_t src, int64_t start, int64_t end,
                                                 uint16_t elem_size) {
    xrt_array_normalize_slice(src.length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    xr_span_t out = src;
    out.data = (count > 0 && src.data)
                   ? (void *) ((uint8_t *) src.data + (size_t) start * (size_t) elem_size)
                   : src.data;
    out.length = count;
    return out;
}

static inline xr_span_t xrt_span_from_string_bytes(XrValue str) {
    if (!XR_IS_STR(str))
        return xrt_span_empty();
    xr_span_t out = {0};
    out.data = (void *) xr_str_data(str);
    out.length = xr_str_len(str);
    return out;
}

static inline xr_span_t xrt_pod_slice_view_checked_raw(
    xr_span_t span, XrPodSliceViewKind kind, uint16_t source_elem_size, bool source_has_layout,
    uint16_t target_elem_size, uint16_t target_expected_elem_size, uint16_t target_alignment,
    bool target_layout_valid, bool target_is_aggregate) {
    XrPodSliceViewResult result = xrt_pod_slice_view_semantics(
        kind, span.data, span.length, source_elem_size, source_has_layout, target_elem_size,
        target_expected_elem_size, target_alignment, target_layout_valid, target_is_aggregate);
    if (kind == XR_POD_SLICE_VIEW_AS_BYTES &&
        result.status == XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.asBytes() requires POD Slice element type");
    if (kind == XR_POD_SLICE_VIEW_AS_BYTES && result.status != XR_POD_SLICE_VIEW_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.asBytes() byte length overflow");
    if (result.status == XR_POD_SLICE_VIEW_INVALID_TARGET_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG);
    if (result.status == XR_POD_SLICE_VIEW_TARGET_SIZE_MISMATCH)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG);
    if (result.status == XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG);
    if (result.status == XR_POD_SLICE_VIEW_LENGTH_NOT_DIVISIBLE)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG);
    if (result.status != XR_POD_SLICE_VIEW_OK)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISALIGNED_MSG);
    xr_span_t out = {.data = result.data, .length = result.length};
    return out;
}

static inline xr_span_t xrt_span_copy_checked_raw(xr_span_t dst, xr_span_t src,
                                                  uint16_t elem_size) {
    XrPodSliceStatus status = xrt_pod_slice_copy_semantics(
        dst.data, dst.length, elem_size, src.data, src.length, elem_size);
    if (status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "Slice.copyFrom(src) requires static element layout");
    if (status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) byte length overflow");
    if (status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) range out of bounds");
    return dst;
}

static inline xr_span_t xrt_span_fill_checked_raw(xr_span_t span, uint16_t elem_size,
                                                  XrPodSliceFillKind kind,
                                                  XrPodSliceFillValue value) {
    XrPodSliceStatus status =
        xrt_pod_slice_fill_semantics(span.data, span.length, elem_size, kind, value);
    if (status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.fill(value) element layout mismatch");
    if (status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) byte length overflow");
    if (status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) range out of bounds");
    return span;
}

static inline int64_t xrt_span_compare_checked_raw(xr_span_t left, xr_span_t right,
                                                   uint16_t elem_size) {
    XrPodSliceCompareResult result = xrt_pod_slice_compare_semantics(
        left.data, left.length, elem_size, right.data, right.length, elem_size);
    if (result.status == XR_POD_SLICE_INVALID_LAYOUT)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        "Slice.compare(other) requires static element layout");
    if (result.status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.compare(other) byte length overflow");
    if (result.status != XR_POD_SLICE_OK)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) span has no data");
    return result.ordering;
}

static inline XrValue xrt_span_to_owned_array(xr_span_t span, uint8_t elem_type, uint16_t elem_size,
                                              uint8_t elem_tid, uint8_t contains_refs) {
    int64_t len = span.length < 0 ? 0 : span.length;
    XrValue outv = xrt_array_new_typed(len, elem_type, elem_tid);
    xrt_array_t *out = (xrt_array_t *) outv.ptr;
    out->elem_tid = elem_tid;
    out->contains_refs = contains_refs;
    if (len <= 0 || !span.data || !out->data)
        return outv;
    if (elem_type == XR_ELEM_ANY) {
        XrValue *src_items = (XrValue *) span.data;
        XrValue *dst_items = (XrValue *) out->data;
        for (int64_t i = 0; i < len; i++)
            dst_items[i] = xrt_value_clone_for_coro(src_items[i]);
    } else {
        size_t bytes = (size_t) len * (size_t) elem_size;
        memcpy(out->data, span.data, bytes);
    }
    return outv;
}

static inline void xrt_array_normalize_slice(int64_t len, int64_t *start, int64_t *end) {
    if (*start < 0)
        *start += len;
    if (*end < 0)
        *end += len;
    if (*start < 0)
        *start = 0;
    if (*end < 0)
        *end = 0;
    if (*start > len)
        *start = len;
    if (*end > len)
        *end = len;
    if (*start > *end)
        *start = *end;
}

/* Array expression slices (`a[b:c]`) are zero-copy views. Use copy(a[b:c])
 * when an independent array is required. */
static inline XrValue xrt_array_slice_view(XrValue arr, int64_t start, int64_t end) {
    if (!XR_IS_ARRAY(arr) || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    xrt_array_normalize_slice(src->length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;

    /* Promote the source buffer to a shared refcounted storage block first so
     * the slice survives a later source grow (may move src->data). */
    xrt_array_ensure_storage(src);

    xrt_array_t *slice = (xrt_array_t *) xrt_execution_alloc_embedded(sizeof(xrt_array_t),
                                                                      xrt_execution_finalize_array);
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_slice_view: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&slice->hdr, XR_TARRAY);
    xrt_array_init_header(slice, count, src->elem_type, src->elem_size);
    slice->data = (count > 0 && src->data)
                      ? (void *) ((uint8_t *) src->data + (size_t) start * (size_t) src->elem_size)
                      : src->data;
    slice->length = count;
    slice->capacity = count;
    slice->data_storage = XR_ARRAY_DATA_BORROWED;
    slice->source = src->source ? src->source : (void *) src;
    if (slice->source)
        xrt_retain(xr_mkptr(slice->source, XR_TAG_ARRAY));
    /* Share the refcounted storage block. */
    slice->storage = src->storage;
    if (src->storage)
        xrt_array_storage_retain_block((XrArrayStorage *) src->storage);
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->elem_tid = src->elem_tid;
    slice->contains_refs = src->contains_refs;
    return xr_mkptr(slice, XR_TAG_ARRAY);
}

static inline void xrt_array_stack_slice_view_init(xrt_array_t *slice, XrValue arr, int64_t start,
                                                   int64_t end) {
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_stack_slice_view_init: NULL slice\n");
        abort();
    }
    if (!XR_IS_ARRAY(arr) || !arr.ptr) {
        xrt_stack_header_init(&slice->hdr, XR_TARRAY);
        xrt_array_init_header(slice, 0, XR_ELEM_ANY, (uint8_t) sizeof(XrValue));
        slice->data = NULL;
        slice->data_storage = XR_ARRAY_DATA_BORROWED;
        return;
    }

    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    xrt_array_normalize_slice(src->length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;

    xrt_array_ensure_storage(src);
    xrt_stack_header_init(&slice->hdr, XR_TARRAY);
    xrt_array_init_header(slice, count, src->elem_type, src->elem_size);
    slice->data = (count > 0 && src->data)
                      ? (void *) ((uint8_t *) src->data + (size_t) start * (size_t) src->elem_size)
                      : src->data;
    slice->length = count;
    slice->capacity = count;
    slice->data_storage = XR_ARRAY_DATA_BORROWED;
    slice->source = NULL;
    slice->storage = src->storage;
    if (slice->storage)
        xrt_array_storage_retain_block((XrArrayStorage *) slice->storage);
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->elem_tid = src->elem_tid;
    slice->contains_refs = src->contains_refs;
}

static inline void xrt_array_stack_borrow_slice_view_init(xrt_array_t *slice, XrValue arr,
                                                          int64_t start, int64_t end) {
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_stack_borrow_slice_view_init: NULL slice\n");
        abort();
    }
    if (!XR_IS_ARRAY(arr) || !arr.ptr) {
        xrt_stack_header_init(&slice->hdr, XR_TARRAY);
        xrt_array_init_header(slice, 0, XR_ELEM_ANY, (uint8_t) sizeof(XrValue));
        slice->data = NULL;
        slice->data_storage = XR_ARRAY_DATA_BORROWED;
        slice->source = NULL;
        slice->storage = NULL;
        return;
    }

    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    xrt_array_normalize_slice(src->length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;

    xrt_stack_header_init(&slice->hdr, XR_TARRAY);
    xrt_array_init_header(slice, count, src->elem_type, src->elem_size);
    slice->data = (count > 0 && src->data)
                      ? (void *) ((uint8_t *) src->data + (size_t) start * (size_t) src->elem_size)
                      : src->data;
    slice->length = count;
    slice->capacity = count;
    slice->data_storage = XR_ARRAY_DATA_BORROWED;
    slice->source = NULL;
    slice->storage = NULL;
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->elem_tid = src->elem_tid;
    slice->contains_refs = src->contains_refs;
}

static inline void xrt_array_stack_borrow_span_view_init(xrt_array_t *view, xr_span_t span,
                                                         uint8_t elem_type, uint16_t elem_size,
                                                         uint8_t elem_tid, uint8_t contains_refs) {
    if (XR_UNLIKELY(!view)) {
        fprintf(stderr, "xrt_array_stack_borrow_span_view_init: NULL view\n");
        abort();
    }
    if (XR_UNLIKELY(elem_size == 0 || elem_size > UINT8_MAX)) {
        fprintf(stderr, "Slice element layout cannot be materialized as an Array view\n");
        abort();
    }
    int64_t len = span.length < 0 ? 0 : span.length;
    xrt_stack_header_init(&view->hdr, XR_TARRAY);
    xrt_array_init_header(view, len, elem_type, (uint8_t) elem_size);
    view->data = span.data;
    view->length = len;
    view->capacity = len;
    view->data_storage = XR_ARRAY_DATA_BORROWED;
    view->source = NULL;
    view->storage = NULL;
    view->elem_type = elem_type;
    view->elem_size = (uint8_t) elem_size;
    view->elem_tid = elem_tid;
    view->contains_refs = contains_refs;
}

static inline void xrt_array_stack_slice_view_release(XrValue view) {
    if (!XR_IS_ARRAY(view) || !view.ptr)
        return;
    xrt_array_t *slice = (xrt_array_t *) view.ptr;
    if (slice->storage) {
        xrt_array_storage_release_block((XrArrayStorage *) slice->storage);
        slice->storage = NULL;
    }
}

#include "xrt_byte_array.inc.c"

typedef struct {
    XrObjHeader hdr;
    int64_t len;
    XrValue items[];
} xrt_tuple_t;

static inline XrValue xrt_tuple_new(int64_t len) {
    if (len < 0)
        len = 0;
    xrt_tuple_t *t = (xrt_tuple_t *) xrt_execution_alloc_embedded(
        sizeof(xrt_tuple_t) + (size_t) len * sizeof(XrValue), xrt_execution_finalize_generic);
    if (XR_UNLIKELY(!t)) {
        fprintf(stderr, "xrt_tuple_new: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&t->hdr, XR_TINSTANCE);
    t->hdr._rsv = XRT_ARC_KIND_TUPLE;
    t->hdr.extra |= XR_OBJ_HAS_DTOR;
    t->len = len;
    for (int64_t i = 0; i < len; i++)
        t->items[i] = XR_NULL_VAL;
    return xr_mkptr(t, XR_TAG_TUPLE);
}

/* Construct a tuple by consuming one owner from every lane. */
static inline XrValue xrt_tuple_make_consuming(int64_t len, const XrValue *items) {
    XrValue tuple = xrt_tuple_new(len);
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    for (int64_t i = 0; i < t->len; i++)
        t->items[i] = items ? items[i] : XR_NULL_VAL;
    return tuple;
}

/* Construct a tuple from borrowed lanes.  Ordinary reference values acquire
 * an independent owner; an array-ref lane materializes because a borrowed
 * view cannot be stored. */
static inline XrValue xrt_tuple_make_from_borrowed(int64_t len, const XrValue *items) {
    XrValue tuple = xrt_tuple_new(len);
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    for (int64_t i = 0; i < t->len; i++) {
        if (!items)
            t->items[i] = XR_NULL_VAL;
        else if (XR_IS_ARRAY_REF(items[i]))
            t->items[i] = xrt_array_ref_to_owned(items[i]);
        else {
            t->items[i] = items[i];
            xrt_retain(t->items[i]);
        }
    }
    return tuple;
}

/* XI_TUPLE_NEW consumes every item.  Build the complete tuple first, then
 * publish the root and all reference lanes into the compiler-planned storage
 * domain.  This matches the VM NEWTUPLE contract and keeps nested values out
 * of the producer execution arena without a copy. */
static inline XrValue xrt_tuple_make_storage(int64_t len, const XrValue *items,
                                             uint8_t storage_mode) {
    XrValue tuple = xrt_tuple_make_consuming(len, items);
    return xrt_value_set_storage_graph(tuple, storage_mode);
}

XR_FUNC bool xr_aot_atomic_compare_exchange_i64(XrValue atomic_value, int64_t expected,
                                                int64_t desired, int64_t ordering,
                                                int64_t *out_previous);
XR_FUNC bool xr_aot_atomic_compare_exchange_f64(XrValue atomic_value, double expected,
                                                double desired, int64_t ordering,
                                                double *out_previous);
XR_FUNC bool xr_aot_atomic_compare_exchange_bool(XrValue atomic_value, bool expected, bool desired,
                                                 int64_t ordering, bool *out_previous);

static inline XrValue xrt_atomic_compare_exchange_i64_tuple(XrValue atomic_value, int64_t expected,
                                                            int64_t desired, int64_t ordering) {
    int64_t previous = 0;
    bool ok =
        xr_aot_atomic_compare_exchange_i64(atomic_value, expected, desired, ordering, &previous);
    XrValue items[2] = {XR_FROM_INT(previous), XR_FROM_BOOL(ok)};
    return xrt_tuple_make_consuming(2, items);
}

static inline XrValue xrt_atomic_compare_exchange_f64_tuple(XrValue atomic_value, double expected,
                                                            double desired, int64_t ordering) {
    double previous = 0.0;
    bool ok =
        xr_aot_atomic_compare_exchange_f64(atomic_value, expected, desired, ordering, &previous);
    XrValue items[2] = {XR_FROM_FLOAT(previous), XR_FROM_BOOL(ok)};
    return xrt_tuple_make_consuming(2, items);
}

static inline XrValue xrt_atomic_compare_exchange_bool_tuple(XrValue atomic_value, bool expected,
                                                             bool desired, int64_t ordering) {
    bool previous = false;
    bool ok =
        xr_aot_atomic_compare_exchange_bool(atomic_value, expected, desired, ordering, &previous);
    XrValue items[2] = {XR_FROM_BOOL(previous), XR_FROM_BOOL(ok)};
    return xrt_tuple_make_consuming(2, items);
}

static inline XrValue xrt_tuple_get(XrValue tuple, int64_t index) {
    if (tuple.tag != XR_TAG_TUPLE || !tuple.ptr)
        return XR_NULL_VAL;
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    if (index < 0 || index >= t->len)
        return XR_NULL_VAL;
    return t->items[index];
}

static inline XrValue xrt_slice(XrValue source, XrValue start_value, XrValue end_value) {
    if (!XR_IS_INT(start_value) || !XR_IS_INT(end_value))
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG);
    int64_t start = XR_TO_INT(start_value);
    int64_t end = XR_TO_INT(end_value);
    if (XR_IS_ARRAY(source))
        return xrt_array_slice_view(source, start, end);
    if (XR_IS_STR(source)) {
        const char *s = xr_str_data(source);
        int64_t len = xr_str_len(source);
        if (start < 0) {
            start += len;
            if (start < 0)
                start = 0;
        }
        if (end < 0) {
            end += len;
            if (end < 0)
                end = 0;
        }
        if (start > len)
            start = len;
        if (end > len)
            end = len;
        if (start >= end)
            return xrt_str_alloc(0);
        int64_t rlen = end - start;
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy(xr_str_buf(sv), s + start, (size_t) rlen);
        return sv;
    }
    return XR_NULL_VAL;
}

/* Stack-allocated array: header on stack, data buffer on stack via alloca.
 * Used for NO_ESCAPE arrays (escape analysis optimization).
 * The returned XrValue is valid only within the current function scope.
 * The data pointer is rounded up so the XRT_DATA_ALIGN contract holds for
 * stack storage too (alloca only guarantees max_align_t). */
#ifndef xrt_array_stack_new
#define xrt_array_stack_new(cap_expr)                                                              \
    ({                                                                                             \
        int64_t _cap = (cap_expr);                                                                 \
        if (_cap < 4)                                                                              \
            _cap = 4;                                                                              \
        xrt_array_t *_a = (xrt_array_t *) __builtin_alloca(                                        \
            sizeof(xrt_array_t) + (size_t) _cap * sizeof(XrValue) + (XRT_DATA_ALIGN - 1));         \
        xrt_stack_header_init(&_a->hdr, XR_TARRAY);                                                \
        _a->length = 0;                                                                            \
        _a->capacity = _cap;                                                                       \
        _a->source = NULL;                                                                         \
        _a->storage = NULL;                                                                        \
        _a->elem_type = XR_ELEM_ANY;                                                               \
        _a->elem_size = (uint8_t) sizeof(XrValue);                                                 \
        _a->elem_tid = 0;                                                                          \
        _a->contains_refs = 0;                                                                     \
        _a->content_version = XR_ARRAY_CONTENT_VERSION_INIT;                                       \
        _a->deferred_submit_version = 0;                                                           \
        _a->data_storage = XR_ARRAY_DATA_STACK;                                                    \
        _a->data =                                                                                 \
            (void *) (((uintptr_t) ((char *) _a + sizeof(xrt_array_t)) + (XRT_DATA_ALIGN - 1)) &   \
                      ~(uintptr_t) (XRT_DATA_ALIGN - 1));                                          \
        memset(_a->data, 0, (size_t) _cap * sizeof(XrValue));                                      \
        xr_mkptr(_a, XR_TAG_ARRAY);                                                                \
    })
#endif

#ifndef xrt_array_stack_slice_view
#define xrt_array_stack_slice_view(source_expr, start_expr, end_expr)                              \
    ({                                                                                             \
        XrValue _srcv = (source_expr);                                                             \
        int64_t _start = (start_expr);                                                             \
        int64_t _end = (end_expr);                                                                 \
        xrt_array_t *_slice = (xrt_array_t *) __builtin_alloca(sizeof(xrt_array_t));               \
        xrt_array_stack_slice_view_init(_slice, _srcv, _start, _end);                              \
        xr_mkptr(_slice, XR_TAG_ARRAY);                                                            \
    })
#endif

#ifndef xrt_array_stack_borrow_slice_view
#define xrt_array_stack_borrow_slice_view(source_expr, start_expr, end_expr)                       \
    ({                                                                                             \
        XrValue _srcv = (source_expr);                                                             \
        int64_t _start = (start_expr);                                                             \
        int64_t _end = (end_expr);                                                                 \
        xrt_array_t *_slice = (xrt_array_t *) __builtin_alloca(sizeof(xrt_array_t));               \
        xrt_array_stack_borrow_slice_view_init(_slice, _srcv, _start, _end);                       \
        xr_mkptr(_slice, XR_TAG_ARRAY);                                                            \
    })
#endif

#ifndef xrt_array_stack_borrow_span_view_typed
#define xrt_array_stack_borrow_span_view_typed(span_expr, elem_type_expr, elem_size_expr,          \
                                               elem_tid_expr, contains_refs_expr)                  \
    ({                                                                                             \
        xr_span_t _span = (span_expr);                                                             \
        xrt_array_t *_view = (xrt_array_t *) __builtin_alloca(sizeof(xrt_array_t));                \
        xrt_array_stack_borrow_span_view_init(                                                     \
            _view, _span, (uint8_t) (elem_type_expr), (uint16_t) (elem_size_expr),                 \
            (uint8_t) (elem_tid_expr), (uint8_t) (contains_refs_expr));                            \
        _view;                                                                                     \
    })
#endif

/* =========================================================================
 * StringBuilder runtime
 * ========================================================================= */

typedef struct {
    char *buf;
    int64_t len;
    int64_t cap;
} xrt_strbuf_t;

static inline XrValue xrt_strbuf_new(void) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) xrt_arc_alloc(sizeof(xrt_strbuf_t));
    xrt_arc_mark_builtin(sb, XRT_ARC_KIND_STRBUF);
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char *) XRT_MALLOC(64);
    if (XR_UNLIKELY(!sb->buf)) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
    sb->buf[0] = 0;
    return xr_mkptr(sb, XR_TAG_STRBUF);
}

static inline void xrt_strbuf_grow(xrt_strbuf_t *sb, int64_t need) {
    if (XR_LIKELY(sb->len + need < sb->cap))
        return;
    while (sb->len + need >= sb->cap)
        sb->cap *= 2;
    char *tmp = (char *) XRT_REALLOC(sb->buf, (size_t) sb->cap);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_strbuf_grow: out of memory\n");
        abort();
    }
    sb->buf = tmp;
}

static inline void xrt_strbuf_reserve_extra_exact(XrValue sbv, int64_t extra) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    if (XR_UNLIKELY(!sb || sb->len < 0 || sb->cap <= 0 || sb->len >= sb->cap || extra < 0 ||
                    extra > INT64_MAX - sb->len - 1)) {
        fprintf(stderr, "xrt_strbuf_reserve_extra_exact: invalid capacity request\n");
        abort();
    }
    int64_t required = sb->len + extra + 1;
    if (XR_LIKELY(required <= sb->cap))
        return;
    if (XR_UNLIKELY((uint64_t) required > (uint64_t) SIZE_MAX)) {
        fprintf(stderr, "xrt_strbuf_reserve_extra_exact: capacity overflow\n");
        abort();
    }
    char *tmp = (char *) XRT_REALLOC(sb->buf, (size_t) required);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_strbuf_reserve_extra_exact: out of memory\n");
        abort();
    }
    sb->buf = tmp;
    sb->cap = required;
}

static inline void xrt_strbuf_append_string_no_grow(XrValue sbv, XrValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    if (XR_UNLIKELY(!sb || (val.tag != XR_TAG_STR && val.tag != XR_TAG_STR_ARC))) {
        fprintf(stderr, "xrt_strbuf_append_string_no_grow: invalid proven append\n");
        abort();
    }
    const char *s = xr_str_data(val);
    int64_t slen = xr_str_len(val);
    if (XR_UNLIKELY(slen < 0 || sb->len < 0 || sb->cap <= 0 || sb->len >= sb->cap ||
                    slen > sb->cap - sb->len - 1)) {
        fprintf(stderr, "xrt_strbuf_append_string_no_grow: capacity proof violated\n");
        abort();
    }
    memcpy(sb->buf + sb->len, s, (size_t) slen);
    sb->len += slen;
    sb->buf[sb->len] = 0;
}

/* No-grow append for non-string scalar literals whose formatted UTF-8 byte
 * length is a compile-time constant (rune / bool / null). The exact capacity
 * has already been reserved once for the known-append chain, so this proves the
 * capacity instead of growing. Kept in sync with the rune/bool/null formatting
 * branches of xrt_strbuf_append and with the compile-time length computed in
 * xi_cgen (xicgen_stringbuilder_exact_append_len). */
static inline void xrt_strbuf_append_scalar_no_grow(XrValue sbv, XrValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    char tmp[4];
    const char *src;
    int64_t n;
    if (XR_UNLIKELY(!sb || sb->len < 0 || sb->cap <= 0 || sb->len >= sb->cap)) {
        fprintf(stderr, "xrt_strbuf_append_scalar_no_grow: invalid proven append\n");
        abort();
    }
    if (val.tag == XR_TAG_RUNE) {
        int enc = xrt_rune_utf8_encode(XR_TO_RUNE(val), tmp);
        if (XR_UNLIKELY(enc <= 0)) {
            fprintf(stderr, "xrt_strbuf_append_scalar_no_grow: invalid rune append\n");
            abort();
        }
        src = tmp;
        n = enc;
    } else if (val.tag == XR_TAG_BOOL) {
        src = val.i ? "true" : "false";
        n = val.i ? 4 : 5;
    } else if (val.tag == XR_TAG_NULL) {
        src = "null";
        n = 4;
    } else {
        fprintf(stderr, "xrt_strbuf_append_scalar_no_grow: unsupported no-grow scalar tag\n");
        abort();
    }
    if (XR_UNLIKELY(n > sb->cap - sb->len - 1)) {
        fprintf(stderr, "xrt_strbuf_append_scalar_no_grow: capacity proof violated\n");
        abort();
    }
    memcpy(sb->buf + sb->len, src, (size_t) n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}

static inline void xrt_strbuf_append(XrValue sbv, XrValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        const char *s = xr_str_data(val);
        int64_t slen = xr_str_len(val);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t) slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_I64) {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long) val.i);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_F64) {
        char tmp[64];
        int n = xr_format_float(tmp, sizeof(tmp), val.f);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_RUNE) {
        char tmp[4];
        int n = xrt_rune_utf8_encode(XR_TO_RUNE(val), tmp);
        if (n > 0) {
            xrt_strbuf_grow(sb, n);
            memcpy(sb->buf + sb->len, tmp, (size_t) n);
            sb->len += n;
            sb->buf[sb->len] = 0;
        }
    } else if (val.tag == XR_TAG_BOOL) {
        const char *bs = val.i ? "true" : "false";
        int blen = val.i ? 4 : 5;
        xrt_strbuf_grow(sb, blen);
        memcpy(sb->buf + sb->len, bs, (size_t) blen);
        sb->len += blen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_NULL) {
        xrt_strbuf_grow(sb, 4);
        memcpy(sb->buf + sb->len, "null", 4);
        sb->len += 4;
        sb->buf[sb->len] = 0;
    } else {
        char tmp[64];
        const char *s = xr_to_cstr(val, tmp, sizeof(tmp));
        int64_t slen = (int64_t) strlen(s);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t) slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    }
}

static inline void xrt_strbuf_clear(XrValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    sb->len = 0;
    sb->buf[0] = 0;
}

static inline XrValue xrt_strbuf_finish(XrValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    XrValue v = xrt_str_alloc((size_t) sb->len);
    memcpy(xr_str_buf(v), sb->buf, (size_t) (sb->len + 1));
    return v;
}

static inline void xrt_strbuf_destroy_builtin(void *obj) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) obj;
    if (!sb)
        return;
    XRT_FREE(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* String concatenation parts (xrt_strpart_t and friends) live in xrt_arith.h:
 * a payload-bearing enum part renders through the shared value formatter
 * (xrt_format_value), which is defined there. */

static inline XrValue xrt_enum_box_new(uint32_t layout_id, const char *enum_name,
                                       const char *member_name, uint32_t member_index) {
    return xrt_enum_box_new_payloads(layout_id, enum_name, member_name, member_index, 0, NULL);
}

static inline const XrAotEnumBox *xrt_enum_box_view(XrValue obj) {
    if (obj.tag != XR_TAG_ENUM || !obj.ptr)
        return NULL;
    const XrObjHeader *hdr = (const XrObjHeader *) obj.ptr;
    return hdr->type == XR_TENUM_CTOR || hdr->type == XR_TENUM_SCALAR_LAYOUT
               ? NULL
               : (const XrAotEnumBox *) obj.ptr;
}

static inline XrValue xrt_enum_box_name(XrValue obj) {
    const char *member_name = NULL;
    return xrt_enum_key_parts(obj, NULL, &member_name, NULL, NULL) && member_name
               ? xr_box_str(member_name)
               : XR_NULL_VAL;
}

static inline XrValue xrt_enum_box_ordinal(XrValue obj) {
    uint32_t member_index = 0;
    return xrt_enum_key_parts(obj, NULL, NULL, &member_index, NULL) ? XR_FROM_INT(member_index)
                                                                    : XR_NULL_VAL;
}

/* =========================================================================
 * Swiss-table core — group-probed open addressing shared by Map and Set.
 *
 * Control bytes:  EMPTY=0xFF  DELETED=0x80  FULL=h2 (top bit clear, 7 hash
 * bits).  Slot count is a power of two; the ctrl array carries XRT_GROUP
 * mirrored tail bytes so an unaligned group load never reads out of bounds.
 * Probing is triangular by whole groups, which visits every group exactly
 * once for power-of-two capacities.  Group matching uses portable 8-byte
 * SWAR; a byte match may be a false positive, so callers always confirm
 * with a key comparison (empty detection is exact).
 * ========================================================================= */

#define XRT_GROUP 8
#define XRT_CTRL_EMPTY 0xFFu
#define XRT_CTRL_DELETED 0x80u
#define XRT_SWAR_LOW 0x0101010101010101ull
#define XRT_SWAR_HIGH 0x8080808080808080ull

static inline uint64_t xrt_group_load(const uint8_t *p) {
    uint64_t g;
    memcpy(&g, p, sizeof(g));
    return g;
}

/* High bit set in each byte that may equal b (false positives possible). */
static inline uint64_t xrt_group_match(uint64_t g, uint8_t b) {
    uint64_t x = g ^ (XRT_SWAR_LOW * (uint64_t) b);
    return (x - XRT_SWAR_LOW) & ~x & XRT_SWAR_HIGH;
}

/* Exact: EMPTY (0xFF) has bits 7 and 6 set; DELETED only bit 7; FULL neither. */
static inline uint64_t xrt_group_match_empty(uint64_t g) {
    return g & (g << 1) & XRT_SWAR_HIGH;
}

static inline uint64_t xrt_group_match_free(uint64_t g) {
    return g & XRT_SWAR_HIGH; /* EMPTY or DELETED */
}

static inline int xrt_swar_first(uint64_t bits) {
    int n = 0;
    while (!(bits & 0xFFu)) {
        bits >>= 8;
        n++;
    }
    return n;
}

/* xrt_hash_mix_u64 / xrt_hash_bytes live in xrt_value.h (shared with the
 * compile-time literal hashing in the C backend). */

static inline uint64_t xrt_hash_f64(double d) {
    /* Container keys hash the canonical equivalence bits: one NaN pattern for
     * the whole NaN space and +0.0 for both zeros, matching the typed-storage
     * path and xr_hash_core_key_eq_f64. */
    return xr_hash_core_mix_u64(xr_hash_core_f64_key_bits(d));
}

/* Hash for tagged values, consistent with xrt_eq: strings hash content
 * through the header cache (literals carry a precomputed hash). */
static inline uint64_t xrt_hash_value(XrValue v) {
    uint32_t tag = (v.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : v.tag;
    switch (tag) {
        case XR_TAG_I64:
        case XR_TAG_BOOL:
        case XR_TAG_RUNE:
            return xr_hash_core_mix_u64((uint64_t) v.i);
        case XR_TAG_F64:
            return xrt_hash_f64(v.f);
        case XR_TAG_STR:
            return xr_hash_core_mix_u64(xrt_str_hash(v));
        case XR_TAG_BIGINT:
            return xrt_bigint_hash_value(v);
        case XR_TAG_ENUM: {
            const char *enum_name = NULL;
            const char *member_name = NULL;
            uint32_t member_index = 0;
            uint32_t layout_id = 0;
            if (!xrt_enum_key_parts(v, &enum_name, &member_name, &member_index, &layout_id))
                return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
            if (layout_id != 0)
                return xr_hash_core_mix_u64(((uint64_t) layout_id << 32) | member_index);
            uint64_t enum_hash = enum_name ? xr_hash_core_bytes(enum_name, strlen(enum_name)) : 0;
            uint64_t member_hash =
                member_name ? xr_hash_core_bytes(member_name, strlen(member_name)) : 0;
            return xr_hash_core_mix_u64(enum_hash ^ (member_hash << 1) ^ member_index);
        }
        case XR_TAG_NULL:
            return xr_hash_core_mix_u64(0x9e3779b97f4a7c15ull);
        case XR_TAG_PTR: {
            /* A hand-written hash() keys the instance by value; the compiled
             * boxed method is recorded on the class. Instances without one fall
             * to identity below. */
            XrtUserHashFn hash_fn = xrt_instance_user_hash_fn(v);
            if (hash_fn)
                return (uint64_t) (uint32_t) hash_fn(NULL, v.ptr);
            return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
        }
        case XR_TAG_AGG_REF:
            if (XR_IS_ARRAY_REF(v)) {
                if (!v.ptr)
                    return xr_hash_core_mix_u64((uint64_t) v.ext);
                size_t size = xrt_value_native_type_size(XR_ARRAY_REF_ELEM_TYPE(v)) *
                              (size_t) XR_ARRAY_REF_ELEM_COUNT(v);
                return xr_hash_core_mix_u64(xr_hash_core_bytes((const char *) v.ptr, size) ^
                                            ((uint64_t) v.ext << 32));
            }
            return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
        default:
            return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
    }
}

static inline int64_t xrt_swiss_slots_for(int64_t want) {
    /* Smallest power of two whose 7/8 usable share covers `want` entries. */
    int64_t slots = XRT_GROUP;
    if (want < 0)
        want = 0;
    while (slots - slots / 8 < want)
        slots <<= 1;
    return slots;
}

static inline uint8_t *xrt_swiss_ctrl_alloc(int64_t slots) {
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC((size_t) slots + XRT_GROUP);
    if (!ctrl) {
        fprintf(stderr, "xrt swiss: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XRT_CTRL_EMPTY, (size_t) slots + XRT_GROUP);
    return ctrl;
}

static inline void xrt_swiss_ctrl_set(uint8_t *ctrl, int64_t slots, int64_t slot, uint8_t b) {
    ctrl[slot] = b;
    if (slot < XRT_GROUP)
        ctrl[slots + slot] = b; /* mirrored tail keeps group loads in bounds */
}

/* Find the slot holding `h2`-tagged keys; `eq_slot(ctx, slot)` confirms.
 * Returns the slot index or -1. */
typedef int (*xrt_swiss_eq_fn)(void *ctx, int64_t slot);

static inline int64_t xrt_swiss_find(const uint8_t *ctrl, int64_t slots, uint64_t hash,
                                     xrt_swiss_eq_fn eq, void *ctx) {
    uint64_t mask = (uint64_t) slots - 1;
    uint8_t h2 = (uint8_t) (hash & 0x7F);
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match(g, h2);
        while (m) {
            int off = xrt_swar_first(m);
            int64_t slot = (int64_t) ((pos + (uint64_t) off) & mask);
            if (eq(ctx, slot))
                return slot;
            m &= ~(0xFFull << ((unsigned) off * 8u)); /* clear matched byte */
        }
        if (xrt_group_match_empty(g))
            return -1;
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* First free (EMPTY or DELETED) slot along the probe sequence. */
static inline int64_t xrt_swiss_find_free(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1;
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match_free(g);
        if (m) {
            int off = xrt_swar_first(m);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* First EMPTY (never DELETED) slot along the probe sequence. Insertion uses this
 * so a tombstoned slot is never reused while a stale order[] entry still points
 * at it; tombstones are reclaimed only by rehash compaction. The caller must
 * ensure a free slot exists (growth_left > 0). */
static inline int64_t xrt_swiss_find_empty(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1;
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match_empty(g);
        if (m) {
            int off = xrt_swar_first(m);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* =========================================================================
 * Map runtime.
 *
 * Tagged maps use the shared hybrid-C ABI: dense insertion-order entries plus
 * a Swiss ctrl/index layer. Typed scalar maps temporarily keep their packed
 * SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_map_t {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrMap (C0
                      * object-header unification) */
    XR_MAP_ABI_FIELDS;

    /* Typed scalar storage (key_type/value_type != XR_ELEM_ANY). */
    int64_t len;
    int64_t cap;
    int64_t growth_left;
    void *keys;
    void *values;
    int64_t *order;
    int64_t order_len;
    int64_t order_cap;
    uint8_t key_type;
    uint8_t value_type;
    uint8_t key_size;
    uint8_t value_size;
    const char *class_name;
} xrt_map_t;

static inline uint8_t xrt_value_type_tag(XrValue v) {
    return (uint8_t) (((v.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : v.tag) + 1u);
}

static inline uint32_t xrt_hash32_value(XrValue v) {
    return (uint32_t) xrt_hash_value(v);
}

static inline uint32_t xrt_ordered_indices_size_for(uint32_t needed, uint32_t max_hbits) {
    uint32_t size = XR_SWISS_GROUP;
    while ((uint64_t) size * 2 / 3 < needed) {
        if (size >= (1u << max_hbits))
            return 1u << max_hbits;
        size <<= 1;
    }
    return size;
}

static inline void xrt_map_init_header(xrt_map_t *m) {
    m->count = 0;
    m->nentries = 0;
    m->entries_cap = 0;
    m->indices_size = 0;
    m->ctrl = NULL;
    m->indices = NULL;
    m->entries = NULL;
    m->flags = XR_MAP_FLAG_DUMMY;
    m->key_tid = 0;
    m->value_tid = 0;
    m->len = 0;
    m->cap = 0;
    m->growth_left = 0;
    m->keys = NULL;
    m->values = NULL;
    m->order = NULL;
    m->order_len = 0;
    m->order_cap = 0;
    m->key_type = XR_ELEM_ANY;
    m->value_type = XR_ELEM_ANY;
    m->key_size = (uint8_t) sizeof(XrValue);
    m->value_size = (uint8_t) sizeof(XrValue);
    m->class_name = NULL;
}

/* Instance equality that honors a hand-written operator ==. Both keys are the
 * same class (their equal hashes put them in one bucket), so the query's eq_fn
 * governs; falls through to xrt_eq (reference identity) when the class has none.
 * Pointers are borrowed, matching the specialized-plan convention. */
static inline int xrt_value_key_eq(XrValue stored, XrValue query) {
    /* Map and Set key identity is reflexive even for NaN. The hash path already
     * canonicalizes every NaN payload; the equality path must make the same
     * choice or a stored NaN can neither be overwritten nor looked up. */
    if (stored.tag == XR_TAG_F64 && query.tag == XR_TAG_F64)
        return stored.f == query.f || (isnan(stored.f) && isnan(query.f));
    if (query.tag == XR_TAG_PTR && query.heap_type == XR_TINSTANCE && query.ptr && stored.ptr &&
        stored.tag == XR_TAG_PTR && stored.heap_type == XR_TINSTANCE) {
        XrtUserEqFn eq_fn = xrt_instance_user_eq_fn(query);
        if (eq_fn)
            return eq_fn(NULL, stored.ptr, query.ptr) != 0;
    }
    /* Key identity is an equivalence relation, not IEEE `==`: every NaN is one
     * key and both zeros are the same key, so a stored key always finds
     * itself. Value-level `==` (xrt_eq) keeps IEEE semantics untouched. */
    if (stored.tag == XR_TAG_F64 && query.tag == XR_TAG_F64)
        return xr_hash_core_key_eq_f64(stored.f, query.f);
    return xrt_eq(stored, query) != 0;
}

/* Candidate comparators for the shared Swiss probe (xr_{map,set}_lookup_slot):
 * type tag then canonical equality. xrt_value_key_eq is type-aware, so the tag
 * pre-check only short-circuits type-mismatched hash collisions. Return int (not
 * bool) to match the runtime's bool-free generated-C convention. */
static inline int xrt_map_key_eq(const XrMapEntry *e, XrValue key, uint8_t key_tt) {
    return e->key_tt == key_tt && xrt_value_key_eq(e->key, key);
}
static inline int xrt_set_value_eq(const XrSetEntry *e, XrValue value, uint8_t val_tt) {
    return e->val_tt == val_tt && xrt_value_key_eq(e->value, value);
}

typedef struct xrt_closure xrt_closure_t;

typedef int64_t (*xrt_user_hash_fn_t)(xrt_closure_t *closure, void *value);
typedef uint8_t (*xrt_user_eq_fn_t)(xrt_closure_t *closure, void *a, void *b);

static inline int xrt_map_backed_class_exact(XrValue value, const char *expected_class_name) {
    if (!expected_class_name || value.tag != XR_TAG_PTR || value.heap_type != XR_TMAP || !value.ptr)
        return 0;
    const xrt_map_t *map = (const xrt_map_t *) value.ptr;
    return map->class_name && strcmp(map->class_name, expected_class_name) == 0;
}

static inline int xrt_user_hash_eq_exact(XrValue value, uint16_t expected_type_id,
                                         const char *expected_class_name) {
    return xrt_instance_exact_type(value, expected_type_id) ||
           xrt_map_backed_class_exact(value, expected_class_name);
}

static inline uint32_t xrt_user_hash_value(XrValue value, uint16_t expected_type_id,
                                           const char *expected_class_name,
                                           xrt_user_hash_fn_t hash_fn) {
    if (hash_fn && xrt_user_hash_eq_exact(value, expected_type_id, expected_class_name))
        return (uint32_t) hash_fn(NULL, value.ptr);
    return xrt_hash32_value(value);
}

static inline int xrt_user_eq_value(XrValue a, XrValue b, uint16_t expected_type_id,
                                    const char *expected_class_name, xrt_user_eq_fn_t eq_fn) {
    if (a.tag != b.tag)
        return 0;
    int a_exact = xrt_user_hash_eq_exact(a, expected_type_id, expected_class_name);
    int b_exact = xrt_user_hash_eq_exact(b, expected_type_id, expected_class_name);
    if (a_exact && b_exact)
        return eq_fn ? eq_fn(NULL, a.ptr, b.ptr) != 0 : xrt_eq(a, b) != 0;
    if (a_exact || b_exact)
        return 0;
    return xrt_eq(a, b) != 0;
}

static inline uint32_t xrt_map_find_entry_slot(xrt_map_t *m, XrValue key, uint32_t hash,
                                               uint8_t key_tt, int32_t *out_eidx) {
    return xr_map_lookup_slot(m->ctrl, m->indices, m->entries, m->indices_size, key, hash, key_tt,
                              xrt_map_key_eq, out_eidx);
}

static inline int32_t xrt_map_find_entry(xrt_map_t *m, XrValue key, uint32_t hash, uint8_t key_tt) {
    int32_t eidx = -1;
    return xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline uint32_t xrt_map_find_entry_slot_user_eq(xrt_map_t *m, XrValue key, uint32_t hash,
                                                       uint8_t key_tt, uint16_t expected_type_id,
                                                       const char *expected_class_name,
                                                       xrt_user_eq_fn_t eq, int32_t *out_eidx) {
    if (!m || !eq || m->indices_size == 0)
        return UINT32_MAX;
    uint32_t mask = m->indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (uint32_t) ((hash >> 7u) & mask);
    uint32_t stride = 0;
    for (;;) {
        uint64_t group = xr_swiss_group_load(m->ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t ix = m->indices[slot];
            if (ix >= 0) {
                XrMapEntry *e = &m->entries[ix];
                if (e->hash == hash && e->key_tt == key_tt &&
                    xrt_user_eq_value(e->key, key, expected_type_id, expected_class_name, eq)) {
                    if (out_eidx)
                        *out_eidx = ix;
                    return slot;
                }
            }
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return UINT32_MAX;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline int32_t xrt_map_find_entry_user_eq(xrt_map_t *m, XrValue key, uint32_t hash,
                                                 uint16_t expected_type_id,
                                                 const char *expected_class_name,
                                                 xrt_user_eq_fn_t eq) {
    int32_t eidx = -1;
    return xrt_map_find_entry_slot_user_eq(m, key, hash, xrt_value_type_tag(key), expected_type_id,
                                           expected_class_name, eq, &eidx) == UINT32_MAX
               ? -1
               : eidx;
}

static inline void xrt_map_resize_tagged(xrt_map_t *m, uint32_t min_needed) {
    if (XR_UNLIKELY(m->flags & XR_MAP_FLAG_NODES_ON_STACK)) {
        fprintf(stderr, "xrt_map_stack_new: capacity exceeded\n");
        abort();
    }

    XrMapEntry *old_entries = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->entries;
    uint8_t *old_ctrl = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->ctrl;
    int32_t *old_indices = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->indices;
    uint32_t old_nentries = (m->flags & XR_MAP_FLAG_DUMMY) ? 0 : m->nentries;

    uint32_t needed = m->count > min_needed ? m->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t indices_size = xrt_ordered_indices_size_for(needed, XR_MAP_MAXHBITS);
    uint32_t entries_cap = (uint32_t) ((uint64_t) indices_size * 2 / 3);
    if (entries_cap < needed)
        entries_cap = needed;

    size_t cbytes = (size_t) indices_size + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) indices_size;
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC(cbytes);
    int32_t *indices = (int32_t *) XRT_MALLOC(ibytes);
    XrMapEntry *entries = (XrMapEntry *) XRT_CALLOC((size_t) entries_cap, sizeof(XrMapEntry));
    if (XR_UNLIKELY(!ctrl || !indices || !entries)) {
        fprintf(stderr, "xrt_map_resize_tagged: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_MAP_IX_EMPTY;

    uint32_t w =
        xr_map_rehash_into(entries, ctrl, indices, indices_size, old_entries, old_nentries);

    XRT_FREE(old_ctrl);
    XRT_FREE(old_indices);
    XRT_FREE(old_entries);

    m->ctrl = ctrl;
    m->indices = indices;
    m->entries = entries;
    m->indices_size = indices_size;
    m->entries_cap = entries_cap;
    m->nentries = w;
    m->flags &= (uint8_t) ~XR_MAP_FLAG_DUMMY;
}

static inline void xrt_map_order_reserve(xrt_map_t *m, int64_t need) {
    if (need <= m->order_cap)
        return;
    int64_t new_cap = m->order_cap > 0 ? m->order_cap : 4;
    while (new_cap < need)
        new_cap *= 2;
    int64_t *tmp = (int64_t *) XRT_REALLOC(m->order, (size_t) new_cap * sizeof(int64_t));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_map_order_reserve: out of memory\n");
        abort();
    }
    m->order = tmp;
    m->order_cap = new_cap;
}

static inline void xrt_map_order_append(xrt_map_t *m, int64_t slot) {
    xrt_map_order_reserve(m, m->order_len + 1);
    m->order[m->order_len++] = slot;
}

static inline int64_t xrt_map_growth_budget(int64_t slots) {
    return slots - slots / 8; /* 7/8 max load factor */
}

static inline void xrt_map_alloc_slots(xrt_map_t *m, int64_t slots) {
    m->cap = slots;
    m->growth_left = xrt_map_growth_budget(slots);
    m->ctrl = xrt_swiss_ctrl_alloc(slots);
    m->keys = XRT_CALLOC((size_t) slots, (size_t) m->key_size);
    m->values = XRT_CALLOC((size_t) slots, (size_t) m->value_size);
    if (XR_UNLIKELY(!m->keys || !m->values)) {
        fprintf(stderr, "xrt_map: out of memory\n");
        abort();
    }
}

static inline XrValue xrt_map_new(int64_t cap) {
    xrt_map_t *m =
        (xrt_map_t *) xrt_execution_alloc_embedded(sizeof(xrt_map_t), xrt_execution_finalize_map);
    if (XR_UNLIKELY(!m)) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&m->hdr, XR_TMAP);
    xrt_map_init_header(m);
    if (cap > 0)
        xrt_map_resize_tagged(m, (uint32_t) cap);
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline XrValue xrt_map_set_storage(XrValue value, uint8_t storage_mode) {
    return xrt_value_set_storage_graph(value, storage_mode);
}

static inline XrValue xrt_map_set_class_name(XrValue map_value, const char *class_name) {
    if (XR_IS_MAP(map_value))
        ((xrt_map_t *) map_value.ptr)->class_name = class_name;
    return map_value;
}

/* Untyped-storage map that still records its declared value element type
 * (e.g. Map<string, int>: string keys force tagged entry storage, but the
 * static value type is scalar). values() uses the recorded type so its
 * result array lanes match the Array<V> layout consumers were planned
 * with. key_type stays XR_ELEM_ANY, so xrt_map_is_typed remains false. */
static inline XrValue xrt_map_new_vt(int64_t cap, uint8_t value_type) {
    XrValue mv = xrt_map_new(cap);
    if (mv.ptr)
        ((xrt_map_t *) mv.ptr)->value_type = value_type;
    return mv;
}

/* Untyped-storage map that still records whichever of its declared element
 * types is a scalar (e.g. Map<string, int> or Map<int, string>: the non-scalar
 * side forces tagged entry storage). keys() and values() use the recorded types
 * so their result lanes match the Array<K> / Array<V> layout consumers were
 * planned with. Typed storage needs both sides scalar, so at least one stays
 * XR_ELEM_ANY here and xrt_map_is_typed remains false. */
static inline XrValue xrt_map_new_declared(int64_t cap, uint8_t key_type, uint8_t value_type) {
    XrValue mv = xrt_map_new(cap);
    if (mv.ptr) {
        ((xrt_map_t *) mv.ptr)->key_type = key_type;
        ((xrt_map_t *) mv.ptr)->value_type = value_type;
    }
    return mv;
}

static inline XrValue xrt_map_static_storage_init(xrt_map_t *m, uint8_t *ctrl, int32_t *indices,
                                                  XrMapEntry *entries, uint32_t indices_size,
                                                  uint32_t entries_cap, uint8_t key_type,
                                                  uint8_t value_type) {
    xrt_static_header_init(&m->hdr, XR_TMAP);
    xrt_map_init_header(m);
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) indices_size + XR_SWISS_GROUP);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_MAP_IX_EMPTY;
    memset(entries, 0, sizeof(XrMapEntry) * (size_t) entries_cap);
    m->ctrl = ctrl;
    m->indices = indices;
    m->entries = entries;
    m->indices_size = indices_size;
    m->entries_cap = entries_cap;
    m->flags = XR_MAP_FLAG_NODES_ON_STACK;
    m->key_type = key_type;
    m->value_type = value_type;
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline XrValue xrt_map_static_storage_freeze(xrt_map_t *m) {
    m->flags |= XR_MAP_FLAG_STATIC_READONLY;
    return xr_mkptr(m, XR_TAG_MAP);
}

#ifndef xrt_map_stack_new
#define xrt_map_stack_new(cap_expr)                                                                \
    ({                                                                                             \
        int64_t _need64 = (cap_expr);                                                              \
        if (_need64 < 1)                                                                           \
            _need64 = 1;                                                                           \
        uint32_t _need = (uint32_t) _need64;                                                       \
        uint32_t _slots = xrt_ordered_indices_size_for(_need, XR_MAP_MAXHBITS);                    \
        uint32_t _ecap = (uint32_t) ((uint64_t) _slots * 2u / 3u);                                 \
        if (_ecap < _need)                                                                         \
            _ecap = _need;                                                                         \
        xrt_map_t *_m = (xrt_map_t *) __builtin_alloca(sizeof(xrt_map_t));                         \
        uint8_t *_ctrl = (uint8_t *) __builtin_alloca((size_t) _slots + XR_SWISS_GROUP);           \
        int32_t *_indices = (int32_t *) __builtin_alloca(sizeof(int32_t) * (size_t) _slots);       \
        XrMapEntry *_entries =                                                                     \
            (XrMapEntry *) __builtin_alloca(sizeof(XrMapEntry) * (size_t) _ecap);                  \
        xrt_stack_header_init(&_m->hdr, XR_TMAP);                                                  \
        xrt_map_init_header(_m);                                                                   \
        memset(_ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) _slots + XR_SWISS_GROUP);                \
        for (uint32_t _i = 0; _i < _slots; _i++)                                                   \
            _indices[_i] = XR_MAP_IX_EMPTY;                                                        \
        memset(_entries, 0, sizeof(XrMapEntry) * (size_t) _ecap);                                  \
        _m->ctrl = _ctrl;                                                                          \
        _m->indices = _indices;                                                                    \
        _m->entries = _entries;                                                                    \
        _m->indices_size = _slots;                                                                 \
        _m->entries_cap = _ecap;                                                                   \
        _m->flags = XR_MAP_FLAG_NODES_ON_STACK;                                                    \
        xr_mkptr(_m, XR_TAG_MAP);                                                                  \
    })
#endif

#include "xrt_boolmap.inc.c"
#include "xrt_map_typed.inc.c"

static inline int64_t xrt_map_len(const xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_len((const xrt_boolmap_t *) m);
    return xrt_map_is_typed(m) ? m->len : (int64_t) m->count;
}

static inline int xrt_map_slot_is_full(const xrt_map_t *m, int64_t slot) {
    if (xrt_map_is_typed(m))
        return (m->ctrl[slot] & 0x80u) == 0;
    return slot >= 0 && (uint32_t) slot < m->nentries &&
           m->entries[slot].key_tt != XR_MAP_ENTRY_NIL_KEY;
}

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_get_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_get_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline XrValue xrt_map_get_prehashed(xrt_map_t *m, XrValue key, uint32_t hash) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_get(m, key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, xrt_value_type_tag(key));
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline XrValue xrt_map_get_user_hash_eq(xrt_map_t *m, XrValue key, uint16_t expected_type_id,
                                               const char *expected_class_name,
                                               xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    if (!hash_fn || !eq_fn || xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_get(m, key);
    uint32_t hash = xrt_user_hash_value(key, expected_type_id, expected_class_name, hash_fn);
    int32_t eidx =
        xrt_map_find_entry_user_eq(m, key, hash, expected_type_id, expected_class_name, eq_fn);
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline XrValue xrt_map_key_not_found(void) {
    xrt_throw_error(XR_ERR_KEY_NOT_FOUND, "Map key not found");
    return XR_NULL_VAL;
}

static inline XrValue xrt_boolmap_index_get_v(xrt_boolmap_t *b, XrValue key) {
    int i = 0;
    if (!xrt_boolmap_key_index_v(key, &i) || ((b->present >> i) & 1) == 0)
        return xrt_map_key_not_found();
    return xrt_boolmap_box_value(b, i);
}

static inline XrValue xrt_map_index_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_index_get_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m)) {
        int64_t slot = xrt_map_find_typed(m, key);
        return slot >= 0 ? xrt_map_slot_value(m, slot) : xrt_map_key_not_found();
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    return eidx >= 0 ? m->entries[eidx].value : xrt_map_key_not_found();
}

static inline XrValue xrt_map_index_get_prehashed(xrt_map_t *m, XrValue key, uint32_t hash) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_index_get(m, key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, xrt_value_type_tag(key));
    return eidx >= 0 ? m->entries[eidx].value : xrt_map_key_not_found();
}

static inline XrValue xrt_map_index_get_user_hash_eq(xrt_map_t *m, XrValue key,
                                                     uint16_t expected_type_id,
                                                     const char *expected_class_name,
                                                     xrt_user_hash_fn_t hash_fn,
                                                     xrt_user_eq_fn_t eq_fn) {
    if (!hash_fn || !eq_fn || xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_index_get(m, key);
    uint32_t hash = xrt_user_hash_value(key, expected_type_id, expected_class_name, hash_fn);
    int32_t eidx =
        xrt_map_find_entry_user_eq(m, key, hash, expected_type_id, expected_class_name, eq_fn);
    return eidx >= 0 ? m->entries[eidx].value : xrt_map_key_not_found();
}

static inline XrValue xrt_map_get_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_get(m, key));
}

static inline XrValue xrt_map_get_prehashed_owned(xrt_map_t *m, XrValue key, uint32_t hash) {
    return xrt_value_to_owned(xrt_map_get_prehashed(m, key, hash));
}

static inline XrValue xrt_map_get_user_hash_eq_owned(xrt_map_t *m, XrValue key,
                                                     uint16_t expected_type_id,
                                                     const char *expected_class_name,
                                                     xrt_user_hash_fn_t hash_fn,
                                                     xrt_user_eq_fn_t eq_fn) {
    return xrt_value_to_owned(
        xrt_map_get_user_hash_eq(m, key, expected_type_id, expected_class_name, hash_fn, eq_fn));
}

static inline XrValue xrt_map_index_get_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_index_get(m, key));
}

static inline XrValue xrt_map_index_get_prehashed_owned(xrt_map_t *m, XrValue key, uint32_t hash) {
    return xrt_value_to_owned(xrt_map_index_get_prehashed(m, key, hash));
}

static inline XrValue xrt_map_index_get_user_hash_eq_owned(xrt_map_t *m, XrValue key,
                                                           uint16_t expected_type_id,
                                                           const char *expected_class_name,
                                                           xrt_user_hash_fn_t hash_fn,
                                                           xrt_user_eq_fn_t eq_fn) {
    return xrt_value_to_owned(xrt_map_index_get_user_hash_eq(m, key, expected_type_id,
                                                             expected_class_name, hash_fn, eq_fn));
}

static inline int32_t xrt_map_find_entry_small(xrt_map_t *m, XrValue key) {
    if (!m || xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return -1;
    if (m->flags & XR_MAP_FLAG_DUMMY)
        return -1;
    uint8_t key_tt = xrt_value_type_tag(key);
    for (uint32_t i = 0; i < m->nentries; i++) {
        XrMapEntry *entry = &m->entries[i];
        if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        if (xrt_map_key_eq(entry, key, key_tt))
            return (int32_t) i;
    }
    return -1;
}

static inline XrValue xrt_map_get_small(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_get(m, key);
    int32_t eidx = xrt_map_find_entry_small(m, key);
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline XrValue xrt_map_get_small_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_get_small(m, key));
}

static inline XrValue xrt_map_index_get_small(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_index_get(m, key);
    int32_t eidx = xrt_map_find_entry_small(m, key);
    return eidx >= 0 ? m->entries[eidx].value : xrt_map_key_not_found();
}

static inline XrValue xrt_map_index_get_small_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_index_get_small(m, key));
}

static inline void xrt_map_require_mutable(const xrt_map_t *m, const char *operation) {
    /* A boolmap (xrt_boolmap_t) is a smaller heap object that stops well before
     * the tagged-map ABI `flags` byte, so probing m->flags on one reads past the
     * allocation. Only generic tagged maps ever carry XR_MAP_FLAG_STATIC_READONLY
     * and boolmaps are always runtime-mutable, so skip the flags read for them.
     * hdr.type (offset 0, checked by xrt_map_is_boolmap) is in bounds for every
     * map variant. */
    if (XR_UNLIKELY(m && !xrt_map_is_boolmap(m) && (m->flags & XR_MAP_FLAG_STATIC_READONLY))) {
        fprintf(stderr, "%s: readonly static Map cannot be mutated\n",
                operation ? operation : "Map mutation");
        abort();
    }
}

static inline int64_t xrt_map_find_dense_i64_slot(xrt_map_t *m, XrValue key) {
    if (!m || !XR_IS_INT(key) || key.i < 0)
        return -1;
    int64_t ordinal = key.i;
    if (xrt_map_is_typed(m)) {
        if (m->key_type == XR_ELEM_F32 || m->key_type == XR_ELEM_F64)
            return -1;
        if (ordinal >= m->order_len)
            return -1;
        int64_t slot = m->order[ordinal];
        if (slot < 0 || slot >= m->cap || !xrt_map_slot_is_full(m, slot))
            return -1;
        if (xrt_map_key_bits_i64(xrt_map_slot_key_raw(m, slot), m->key_type) !=
            xrt_map_key_bits_i64(key.i, m->key_type))
            return -1;
        return slot;
    }
    if (m->flags & XR_MAP_FLAG_DUMMY)
        return -1;
    if ((uint64_t) ordinal >= (uint64_t) m->nentries)
        return -1;
    XrMapEntry *entry = &m->entries[ordinal];
    if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
        return -1;
    return xrt_map_key_eq(entry, key, xrt_value_type_tag(key)) ? ordinal : -1;
}

static inline XrValue xrt_map_get_dense_i64(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_i64_slot(m, key);
    return slot >= 0 ? xrt_map_slot_value(m, slot) : xrt_map_get(m, key);
}

static inline XrValue xrt_map_get_dense_i64_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_get_dense_i64(m, key));
}

static inline XrValue xrt_map_index_get_dense_i64(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_i64_slot(m, key);
    return slot >= 0 ? xrt_map_slot_value(m, slot) : xrt_map_index_get(m, key);
}

static inline XrValue xrt_map_index_get_dense_i64_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_index_get_dense_i64(m, key));
}

static inline int64_t xrt_map_find_dense_enum_slot(xrt_map_t *m, XrValue key) {
    uint32_t ordinal = 0;
    if (!m || xrt_map_is_boolmap(m) || xrt_map_is_typed(m) || (m->flags & XR_MAP_FLAG_DUMMY) != 0 ||
        !xrt_enum_key_parts(key, NULL, NULL, &ordinal, NULL) || ordinal >= m->nentries)
        return -1;
    XrMapEntry *entry = &m->entries[ordinal];
    if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
        return -1;
    return xrt_map_key_eq(entry, key, xrt_value_type_tag(key)) ? (int64_t) ordinal : -1;
}

static inline XrValue xrt_map_get_dense_enum(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_enum_slot(m, key);
    return slot >= 0 ? xrt_map_slot_value(m, slot) : xrt_map_get(m, key);
}

static inline XrValue xrt_map_get_dense_enum_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_get_dense_enum(m, key));
}

static inline XrValue xrt_map_index_get_dense_enum(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_enum_slot(m, key);
    return slot >= 0 ? xrt_map_slot_value(m, slot) : xrt_map_index_get(m, key);
}

static inline XrValue xrt_map_index_get_dense_enum_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_index_get_dense_enum(m, key));
}

static inline int xrt_map_has(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_has_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_has_typed(m, key);
    return xrt_map_find_entry(m, key, xrt_hash32_value(key), xrt_value_type_tag(key)) >= 0;
}

static inline int xrt_map_has_value(xrt_map_t *m, XrValue value) {
    if (!m)
        return 0;
    if (xrt_map_is_boolmap(m)) {
        XrValue keys[2] = {XR_FROM_BOOL(0), XR_FROM_BOOL(1)};
        for (int i = 0; i < 2; i++) {
            if (xrt_map_has(m, keys[i]) && xrt_eq(xrt_map_get(m, keys[i]), value))
                return 1;
        }
        return 0;
    }
    if (xrt_map_is_typed(m)) {
        for (int64_t i = 0; i < m->order_len; i++) {
            int64_t slot = m->order[i];
            if (slot >= 0 && slot < m->cap && xrt_map_slot_is_full(m, slot) &&
                xrt_eq(xrt_map_slot_value(m, slot), value))
                return 1;
        }
        return 0;
    }
    if (m->flags & XR_MAP_FLAG_DUMMY)
        return 0;
    for (uint32_t i = 0; i < m->nentries; i++) {
        XrMapEntry *entry = &m->entries[i];
        if (entry->key_tt != XR_MAP_ENTRY_NIL_KEY && xrt_eq(entry->value, value))
            return 1;
    }
    return 0;
}

static inline int xrt_map_has_prehashed(xrt_map_t *m, XrValue key, uint32_t hash) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_has(m, key);
    return xrt_map_find_entry(m, key, hash, xrt_value_type_tag(key)) >= 0;
}

static inline int xrt_map_has_user_hash_eq(xrt_map_t *m, XrValue key, uint16_t expected_type_id,
                                           const char *expected_class_name,
                                           xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    if (!hash_fn || !eq_fn || xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_has(m, key);
    return xrt_map_find_entry_user_eq(
               m, key, xrt_user_hash_value(key, expected_type_id, expected_class_name, hash_fn),
               expected_type_id, expected_class_name, eq_fn) >= 0;
}

static inline int xrt_map_has_small(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_has(m, key);
    return xrt_map_find_entry_small(m, key) >= 0;
}

static inline int xrt_map_has_dense_i64(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_i64_slot(m, key);
    return slot >= 0 ? 1 : xrt_map_has(m, key);
}

static inline int xrt_map_has_dense_enum(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_dense_enum_slot(m, key);
    return slot >= 0 ? 1 : xrt_map_has(m, key);
}

static inline int xrt_map_delete(xrt_map_t *m, XrValue key) {
    xrt_map_require_mutable(m, "Map.delete");
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_delete_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_delete_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = -1;
    uint32_t slot = xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    m->entries[eidx].key_tt = XR_MAP_ENTRY_NIL_KEY;
    xrt_release(m->entries[eidx].key);
    xrt_release(m->entries[eidx].value);
    m->entries[eidx].key = XR_NULL_VAL;
    m->entries[eidx].value = XR_NULL_VAL;
    m->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(m->ctrl, m->indices_size, slot, XR_SWISS_CTRL_DELETED);
    m->count--;
    return 1;
}

static inline int xrt_map_delete_prehashed(xrt_map_t *m, XrValue key, uint32_t hash) {
    xrt_map_require_mutable(m, "Map.delete");
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_delete(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    int32_t eidx = -1;
    uint32_t slot = xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    m->entries[eidx].key_tt = XR_MAP_ENTRY_NIL_KEY;
    xrt_release(m->entries[eidx].key);
    xrt_release(m->entries[eidx].value);
    m->entries[eidx].key = XR_NULL_VAL;
    m->entries[eidx].value = XR_NULL_VAL;
    m->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(m->ctrl, m->indices_size, slot, XR_SWISS_CTRL_DELETED);
    m->count--;
    return 1;
}

static inline int xrt_map_delete_user_hash_eq(xrt_map_t *m, XrValue key, uint16_t expected_type_id,
                                              const char *expected_class_name,
                                              xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    xrt_map_require_mutable(m, "Map.delete");
    if (!hash_fn || !eq_fn || xrt_map_is_boolmap(m) || xrt_map_is_typed(m))
        return xrt_map_delete(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    int32_t eidx = -1;
    uint32_t hash = xrt_user_hash_value(key, expected_type_id, expected_class_name, hash_fn);
    uint32_t slot = xrt_map_find_entry_slot_user_eq(m, key, hash, key_tt, expected_type_id,
                                                    expected_class_name, eq_fn, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    m->entries[eidx].key_tt = XR_MAP_ENTRY_NIL_KEY;
    xrt_release(m->entries[eidx].key);
    xrt_release(m->entries[eidx].value);
    m->entries[eidx].key = XR_NULL_VAL;
    m->entries[eidx].value = XR_NULL_VAL;
    m->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(m->ctrl, m->indices_size, slot, XR_SWISS_CTRL_DELETED);
    m->count--;
    return 1;
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
    xrt_map_require_mutable(m, "Map.set");
    if (xrt_map_is_boolmap(m)) {
        xrt_boolmap_set_v((xrt_boolmap_t *) m, key, val);
        return;
    }
    if (xrt_map_is_typed(m)) {
        xrt_map_set_typed(m, key, val);
        return;
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    if (eidx >= 0) {
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
        xrt_release(key);
        xrt_release(m->entries[eidx].value);
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (XR_IS_ARRAY_REF(key))
        key = xrt_value_to_owned(key);
    if (XR_IS_ARRAY_REF(val))
        val = xrt_value_to_owned(val);
    eidx = (int32_t) m->nentries++;
    XrMapEntry *entry = &m->entries[eidx];
    entry->key = key;
    entry->value = val;
    entry->hash = hash;
    entry->key_tt = key_tt;
    m->count++;
    xr_swiss_indices_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

static inline XrValue xrt_path_raw_value(XrValue path) {
    if (xrt_map_backed_class_exact(path, "Path"))
        return xrt_map_get((xrt_map_t *) path.ptr, xr_box_str("raw"));
    if (path.tag == XR_TAG_PTR && path.heap_type == XR_TINSTANCE && path.ptr) {
        /* Callers reach this helper only through a statically typed Path
         * boundary. Release profiles may strip class display names, so the
         * native layout must not depend on optional reflection metadata. */
        typedef struct {
            XrObjHeader hdr;
            XrValue raw;
        } XrtPathInstanceLayout;
        return ((const XrtPathInstanceLayout *) path.ptr)->raw;
    }
    return XR_NULL_VAL;
}

static inline const char *xrt_path_data(XrValue path) {
    XrValue raw = xrt_path_raw_value(path);
    return XR_IS_STR(raw) ? xr_str_data(raw) : NULL;
}

static inline int64_t xrt_path_len(XrValue path) {
    XrValue raw = xrt_path_raw_value(path);
    return XR_IS_STR(raw) ? xr_str_len(raw) : 0;
}

static inline void xrt_map_set_prehashed(xrt_map_t *m, XrValue key, XrValue val, uint32_t hash) {
    xrt_map_require_mutable(m, "Map.set");
    if (xrt_map_is_boolmap(m) || xrt_map_is_typed(m)) {
        xrt_map_set(m, key, val);
        return;
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    if (eidx >= 0) {
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
        xrt_release(key);
        xrt_release(m->entries[eidx].value);
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (XR_IS_ARRAY_REF(key))
        key = xrt_value_to_owned(key);
    if (XR_IS_ARRAY_REF(val))
        val = xrt_value_to_owned(val);
    eidx = (int32_t) m->nentries++;
    XrMapEntry *entry = &m->entries[eidx];
    entry->key = key;
    entry->value = val;
    entry->hash = hash;
    entry->key_tt = key_tt;
    m->count++;
    xr_swiss_indices_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

static inline void xrt_map_set_user_hash_eq(xrt_map_t *m, XrValue key, XrValue val,
                                            uint16_t expected_type_id,
                                            const char *expected_class_name,
                                            xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    xrt_map_require_mutable(m, "Map.set");
    if (!hash_fn || !eq_fn || xrt_map_is_boolmap(m) || xrt_map_is_typed(m)) {
        xrt_map_set(m, key, val);
        return;
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_user_hash_value(key, expected_type_id, expected_class_name, hash_fn);
    int32_t eidx =
        xrt_map_find_entry_user_eq(m, key, hash, expected_type_id, expected_class_name, eq_fn);
    if (eidx >= 0) {
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
        xrt_release(key);
        xrt_release(m->entries[eidx].value);
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (XR_IS_ARRAY_REF(key))
        key = xrt_value_to_owned(key);
    if (XR_IS_ARRAY_REF(val))
        val = xrt_value_to_owned(val);
    eidx = (int32_t) m->nentries++;
    XrMapEntry *entry = &m->entries[eidx];
    entry->key = key;
    entry->value = val;
    entry->hash = hash;
    entry->key_tt = key_tt;
    m->count++;
    xr_swiss_indices_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

static inline void xrt_map_clear(xrt_map_t *m) {
    xrt_map_require_mutable(m, "Map.clear");
    if (!m)
        return;
    if (xrt_map_is_boolmap(m)) {
        xrt_boolmap_clear((xrt_boolmap_t *) m);
        return;
    }
    if (xrt_map_is_typed(m)) {
        memset(m->ctrl, (int) XRT_CTRL_EMPTY, (size_t) m->cap + XRT_GROUP);
        m->growth_left = m->cap - m->cap / 8;
        m->len = 0;
        m->order_len = 0;
        return;
    }
    if (m->flags & XR_MAP_FLAG_DUMMY)
        return;
    for (uint32_t i = 0; i < m->nentries; i++) {
        if (m->entries[i].key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        xrt_release(m->entries[i].key);
        xrt_release(m->entries[i].value);
        m->entries[i].key = XR_NULL_VAL;
        m->entries[i].value = XR_NULL_VAL;
        m->entries[i].key_tt = XR_MAP_ENTRY_NIL_KEY;
    }
    memset(m->ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) m->indices_size + XR_SWISS_GROUP);
    for (uint32_t i = 0; i < m->indices_size; i++)
        m->indices[i] = XR_MAP_IX_EMPTY;
    m->nentries = 0;
    m->count = 0;
}

/* =========================================================================
 * Set runtime.
 *
 * Tagged sets use the shared hybrid-C ABI. Typed scalar sets temporarily keep
 * their packed SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_set_t {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrSet (C0
                      * object-header unification) */
    XR_SET_ABI_FIELDS;

    /* Typed scalar storage (elem_type != XR_ELEM_ANY). */
    int64_t len;
    int64_t cap;
    int64_t growth_left;
    void *items;
    int64_t *order;
    int64_t order_len;
    int64_t order_cap;
    uint8_t elem_type;
    uint8_t elem_size;
} xrt_set_t;

static inline void xrt_set_init_header(xrt_set_t *s, uint8_t elem_type) {
    s->count = 0;
    s->nentries = 0;
    s->entries_cap = 0;
    s->indices_size = 0;
    s->ctrl = NULL;
    s->indices = NULL;
    s->entries = NULL;
    s->flags = XR_SET_FLAG_DUMMY;
    s->elem_tid = 0;
    s->len = 0;
    s->cap = 0;
    s->growth_left = 0;
    s->items = NULL;
    s->order = NULL;
    s->order_len = 0;
    s->order_cap = 0;
    s->elem_type = elem_type;
    s->elem_size = elem_type == XR_ELEM_ANY ? (uint8_t) sizeof(XrValue) : XR_ELEM_SIZES[elem_type];
}

static inline uint32_t xrt_set_find_entry_slot(xrt_set_t *s, XrValue value, uint32_t hash,
                                               int32_t *out_eidx) {
    return xr_set_lookup_slot(s->ctrl, s->indices, s->entries, s->indices_size, value, hash,
                              xrt_value_type_tag(value), xrt_set_value_eq, out_eidx);
}

static inline int32_t xrt_set_find_entry(xrt_set_t *s, XrValue value, uint32_t hash) {
    int32_t eidx = -1;
    return xrt_set_find_entry_slot(s, value, hash, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline uint32_t xrt_set_find_entry_slot_user_eq(xrt_set_t *s, XrValue value, uint32_t hash,
                                                       uint8_t val_tt, uint16_t expected_type_id,
                                                       const char *expected_class_name,
                                                       xrt_user_eq_fn_t eq, int32_t *out_eidx) {
    if (!s || !eq || s->indices_size == 0)
        return UINT32_MAX;
    uint32_t mask = s->indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (uint32_t) ((hash >> 7u) & mask);
    uint32_t stride = 0;
    for (;;) {
        uint64_t group = xr_swiss_group_load(s->ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t ix = s->indices[slot];
            if (ix >= 0) {
                XrSetEntry *e = &s->entries[ix];
                if (e->hash == hash && e->val_tt == val_tt &&
                    xrt_user_eq_value(e->value, value, expected_type_id, expected_class_name, eq)) {
                    if (out_eidx)
                        *out_eidx = ix;
                    return slot;
                }
            }
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return UINT32_MAX;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline int32_t xrt_set_find_entry_user_eq(xrt_set_t *s, XrValue value, uint32_t hash,
                                                 uint16_t expected_type_id,
                                                 const char *expected_class_name,
                                                 xrt_user_eq_fn_t eq) {
    int32_t eidx = -1;
    return xrt_set_find_entry_slot_user_eq(s, value, hash, xrt_value_type_tag(value),
                                           expected_type_id, expected_class_name, eq,
                                           &eidx) == UINT32_MAX
               ? -1
               : eidx;
}

static inline void xrt_set_resize_tagged(xrt_set_t *s, uint32_t min_needed) {
    if (XR_UNLIKELY(s->flags & XR_SET_FLAG_NODES_ON_STACK)) {
        fprintf(stderr, "xrt_set_stack_new: capacity exceeded\n");
        abort();
    }

    XrSetEntry *old_entries = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->entries;
    uint8_t *old_ctrl = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->ctrl;
    int32_t *old_indices = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->indices;
    uint32_t old_nentries = (s->flags & XR_SET_FLAG_DUMMY) ? 0 : s->nentries;

    uint32_t needed = s->count > min_needed ? s->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t indices_size = xrt_ordered_indices_size_for(needed, XR_SET_MAXHBITS);
    uint32_t entries_cap = (uint32_t) ((uint64_t) indices_size * 2 / 3);
    if (entries_cap < needed)
        entries_cap = needed;

    size_t cbytes = (size_t) indices_size + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) indices_size;
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC(cbytes);
    int32_t *indices = (int32_t *) XRT_MALLOC(ibytes);
    XrSetEntry *entries = (XrSetEntry *) XRT_CALLOC((size_t) entries_cap, sizeof(XrSetEntry));
    if (XR_UNLIKELY(!ctrl || !indices || !entries)) {
        fprintf(stderr, "xrt_set_resize_tagged: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_SET_IX_EMPTY;

    uint32_t w =
        xr_set_rehash_into(entries, ctrl, indices, indices_size, old_entries, old_nentries);

    XRT_FREE(old_ctrl);
    XRT_FREE(old_indices);
    XRT_FREE(old_entries);

    s->ctrl = ctrl;
    s->indices = indices;
    s->entries = entries;
    s->indices_size = indices_size;
    s->entries_cap = entries_cap;
    s->nentries = w;
    s->flags &= (uint8_t) ~XR_SET_FLAG_DUMMY;
}

static inline void xrt_set_order_reserve(xrt_set_t *s, int64_t need) {
    if (need <= s->order_cap)
        return;
    int64_t new_cap = s->order_cap > 0 ? s->order_cap : 4;
    while (new_cap < need)
        new_cap *= 2;
    int64_t *tmp = (int64_t *) XRT_REALLOC(s->order, (size_t) new_cap * sizeof(int64_t));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_set_order_reserve: out of memory\n");
        abort();
    }
    s->order = tmp;
    s->order_cap = new_cap;
}

static inline void xrt_set_order_append(xrt_set_t *s, int64_t slot) {
    xrt_set_order_reserve(s, s->order_len + 1);
    s->order[s->order_len++] = slot;
}

static inline void xrt_set_alloc_slots(xrt_set_t *s, int64_t slots) {
    s->cap = slots;
    s->growth_left = slots - slots / 8;
    s->ctrl = xrt_swiss_ctrl_alloc(slots);
    s->items = XRT_CALLOC((size_t) slots, (size_t) s->elem_size);
    if (XR_UNLIKELY(!s->items)) {
        fprintf(stderr, "xrt_set: out of memory\n");
        abort();
    }
}

static inline XrValue xrt_set_new_typed(int64_t cap, uint8_t elem_type) {
    if (elem_type >= XR_ELEM_COUNT)
        elem_type = XR_ELEM_ANY;
    xrt_set_t *s =
        (xrt_set_t *) xrt_execution_alloc_embedded(sizeof(xrt_set_t), xrt_execution_finalize_set);
    if (XR_UNLIKELY(!s)) {
        fprintf(stderr, "xrt_set_new: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&s->hdr, XR_TSET);
    xrt_set_init_header(s, elem_type);
    if (elem_type == XR_ELEM_ANY) {
        if (cap > 0)
            xrt_set_resize_tagged(s, (uint32_t) cap);
    } else {
        xrt_set_alloc_slots(s, xrt_swiss_slots_for(cap));
    }
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_set_storage(XrValue value, uint8_t storage_mode) {
    return xrt_value_set_storage_graph(value, storage_mode);
}

static inline int xrt_set_is_typed(const xrt_set_t *s);

/* Drop one reference to a storage-backed array's buffer (AOT). Releases ANY
 * element refs (data[0..elem_count)) when the buffer's refcount reaches zero. */
static inline void xrt_array_storage_release(xrt_array_t *a) {
    XrArrayStorage *s = (XrArrayStorage *) a->storage;
    xrt_array_storage_release_block(s);
    a->storage = NULL;
}

static inline void xrt_array_destroy(xrt_array_t *a) {
    if (!a)
        return;
    if (a->data_storage == XR_ARRAY_DATA_BORROWED) {
        if (a->source) {
            xrt_release(xr_mkptr(a->source, XR_TAG_ARRAY));
            a->source = NULL;
        }
        /* Slice never freezes elem_count (the owner does); storage releases its
         * frozen ANY element refs at refcount 0. */
        if (a->storage)
            xrt_array_storage_release(a);
        xrt_execution_free_allocation(&a->hdr);
        return;
    }
    if (a->storage) {
        /* Storage owns the buffer and (for ANY) the element refs. Snapshot the
         * owner's length so an orphaned storage can release the right count. */
        XrArrayStorage *s = (XrArrayStorage *) a->storage;
        if (s->elem_is_any)
            s->elem_count = a->length;
        xrt_array_storage_release(a);
        xrt_execution_free_allocation(&a->hdr);
        return;
    }
    if (a->elem_type == XR_ELEM_ANY && a->data && a->length > 0) {
        XrValue *items = (XrValue *) a->data;
        for (int64_t i = 0; i < a->length; i++)
            xrt_release(items[i]);
    }
    if (a->data_storage == XR_ARRAY_DATA_HEAP && a->data)
        XRT_FREE_ALIGNED(a->data);
    xrt_execution_free_allocation(&a->hdr);
}

static inline void xrt_map_destroy(xrt_map_t *m) {
    if (!m)
        return;
    if (!xrt_map_is_typed(m) && !(m->flags & XR_MAP_FLAG_DUMMY) && m->entries) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_release(m->entries[i].key);
            xrt_release(m->entries[i].value);
        }
    }
    XRT_FREE(m->ctrl);
    XRT_FREE(m->indices);
    XRT_FREE(m->entries);
    XRT_FREE(m->keys);
    XRT_FREE(m->values);
    XRT_FREE(m->order);
    xrt_execution_free_allocation(&m->hdr);
}

static inline void xrt_set_destroy(xrt_set_t *s) {
    if (!s)
        return;
    if (!xrt_set_is_typed(s) && !(s->flags & XR_SET_FLAG_DUMMY) && s->entries) {
        for (uint32_t i = 0; i < s->nentries; i++) {
            if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
                continue;
            xrt_release(s->entries[i].value);
        }
    }
    XRT_FREE(s->ctrl);
    XRT_FREE(s->indices);
    XRT_FREE(s->entries);
    XRT_FREE(s->items);
    XRT_FREE(s->order);
    xrt_execution_free_allocation(&s->hdr);
}

static inline XrValue xrt_set_new(int64_t cap) {
    return xrt_set_new_typed(cap, XR_ELEM_ANY);
}

static inline XrValue xrt_set_static_storage_init(xrt_set_t *s, uint8_t *ctrl, int32_t *indices,
                                                  XrSetEntry *entries, uint32_t indices_size,
                                                  uint32_t entries_cap) {
    xrt_static_header_init(&s->hdr, XR_TSET);
    xrt_set_init_header(s, XR_ELEM_ANY);
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) indices_size + XR_SWISS_GROUP);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_SET_IX_EMPTY;
    memset(entries, 0, sizeof(XrSetEntry) * (size_t) entries_cap);
    s->ctrl = ctrl;
    s->indices = indices;
    s->entries = entries;
    s->indices_size = indices_size;
    s->entries_cap = entries_cap;
    s->flags = XR_SET_FLAG_NODES_ON_STACK;
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_static_storage_freeze(xrt_set_t *s) {
    s->flags |= XR_SET_FLAG_STATIC_READONLY;
    return xr_mkptr(s, XR_TAG_SET);
}

#ifndef xrt_set_stack_new
#define xrt_set_stack_new(cap_expr)                                                                \
    ({                                                                                             \
        int64_t _need64 = (cap_expr);                                                              \
        if (_need64 < 1)                                                                           \
            _need64 = 1;                                                                           \
        uint32_t _need = (uint32_t) _need64;                                                       \
        uint32_t _slots = xrt_ordered_indices_size_for(_need, XR_SET_MAXHBITS);                    \
        uint32_t _ecap = (uint32_t) ((uint64_t) _slots * 2u / 3u);                                 \
        if (_ecap < _need)                                                                         \
            _ecap = _need;                                                                         \
        xrt_set_t *_s = (xrt_set_t *) __builtin_alloca(sizeof(xrt_set_t));                         \
        uint8_t *_ctrl = (uint8_t *) __builtin_alloca((size_t) _slots + XR_SWISS_GROUP);           \
        int32_t *_indices = (int32_t *) __builtin_alloca(sizeof(int32_t) * (size_t) _slots);       \
        XrSetEntry *_entries =                                                                     \
            (XrSetEntry *) __builtin_alloca(sizeof(XrSetEntry) * (size_t) _ecap);                  \
        xrt_stack_header_init(&_s->hdr, XR_TSET);                                                  \
        xrt_set_init_header(_s, XR_ELEM_ANY);                                                      \
        memset(_ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) _slots + XR_SWISS_GROUP);                \
        for (uint32_t _i = 0; _i < _slots; _i++)                                                   \
            _indices[_i] = XR_SET_IX_EMPTY;                                                        \
        memset(_entries, 0, sizeof(XrSetEntry) * (size_t) _ecap);                                  \
        _s->ctrl = _ctrl;                                                                          \
        _s->indices = _indices;                                                                    \
        _s->entries = _entries;                                                                    \
        _s->indices_size = _slots;                                                                 \
        _s->entries_cap = _ecap;                                                                   \
        _s->flags = XR_SET_FLAG_NODES_ON_STACK;                                                    \
        xr_mkptr(_s, XR_TAG_SET);                                                                  \
    })
#endif

static inline int xrt_set_is_typed(const xrt_set_t *s) {
    return s && s->elem_type != XR_ELEM_ANY;
}

static inline int64_t xrt_set_len(const xrt_set_t *s) {
    return xrt_set_is_typed(s) ? s->len : (int64_t) s->count;
}

static inline int xrt_set_slot_is_full(const xrt_set_t *s, int64_t slot) {
    if (xrt_set_is_typed(s))
        return (s->ctrl[slot] & 0x80u) == 0;
    return slot >= 0 && (uint32_t) slot < s->nentries &&
           s->entries[slot].val_tt != XR_SET_ENTRY_NIL;
}

/* Slot accessor — valid only for FULL slots. */
static inline XrValue xrt_set_slot_item(xrt_set_t *s, int64_t slot) {
    if (!xrt_set_is_typed(s))
        return s->entries[slot].value;
    return xr_typed_get(s->items, (int32_t) slot, s->elem_type);
}

#include "xrt_set_direct.inc.c"

static inline int xrt_set_has(xrt_set_t *s, XrValue value) {
    if (!xrt_set_is_typed(s))
        return xrt_set_find_entry(s, value, xrt_hash32_value(value)) >= 0;
    return xrt_set_find_value(s, value) >= 0;
}

static inline int xrt_set_has_prehashed(xrt_set_t *s, XrValue value, uint32_t hash) {
    if (!xrt_set_is_typed(s))
        return xrt_set_find_entry(s, value, hash) >= 0;
    return xrt_set_has(s, value);
}

static inline int xrt_set_has_user_hash_eq(xrt_set_t *s, XrValue value, uint16_t expected_type_id,
                                           const char *expected_class_name,
                                           xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    if (!hash_fn || !eq_fn || xrt_set_is_typed(s))
        return xrt_set_has(s, value);
    return xrt_set_find_entry_user_eq(
               s, value, xrt_user_hash_value(value, expected_type_id, expected_class_name, hash_fn),
               expected_type_id, expected_class_name, eq_fn) >= 0;
}

static inline int32_t xrt_set_find_entry_small(xrt_set_t *s, XrValue value) {
    if (!s || xrt_set_is_typed(s))
        return -1;
    if (s->flags & XR_SET_FLAG_DUMMY)
        return -1;
    uint8_t val_tt = xrt_value_type_tag(value);
    for (uint32_t i = 0; i < s->nentries; i++) {
        XrSetEntry *entry = &s->entries[i];
        if (entry->val_tt == XR_SET_ENTRY_NIL)
            continue;
        if (xrt_set_value_eq(entry, value, val_tt))
            return (int32_t) i;
    }
    return -1;
}

static inline int xrt_set_has_small(xrt_set_t *s, XrValue value) {
    if (xrt_set_is_typed(s))
        return xrt_set_has(s, value);
    return xrt_set_find_entry_small(s, value) >= 0;
}

static inline void xrt_set_require_mutable(const xrt_set_t *s, const char *operation) {
    if (XR_UNLIKELY(s && (s->flags & XR_SET_FLAG_STATIC_READONLY))) {
        fprintf(stderr, "%s: readonly static Set cannot be mutated\n",
                operation ? operation : "Set mutation");
        abort();
    }
}

static inline int64_t xrt_set_find_dense_i64_slot(xrt_set_t *s, XrValue value) {
    if (!s || !XR_IS_INT(value) || value.i < 0)
        return -1;
    int64_t ordinal = value.i;
    if (xrt_set_is_typed(s)) {
        if (s->elem_type == XR_ELEM_F32 || s->elem_type == XR_ELEM_F64)
            return -1;
        if (ordinal >= s->order_len)
            return -1;
        int64_t slot = s->order[ordinal];
        if (slot < 0 || slot >= s->cap || !xrt_set_slot_is_full(s, slot))
            return -1;
        if (xrt_set_item_bits_i64(xrt_set_slot_raw_i64(s, slot), s->elem_type) !=
            xrt_set_item_bits_i64(value.i, s->elem_type))
            return -1;
        return slot;
    }
    if (s->flags & XR_SET_FLAG_DUMMY)
        return -1;
    if ((uint64_t) ordinal >= (uint64_t) s->nentries)
        return -1;
    XrSetEntry *entry = &s->entries[ordinal];
    if (entry->val_tt == XR_SET_ENTRY_NIL)
        return -1;
    return xrt_set_value_eq(entry, value, xrt_value_type_tag(value)) ? ordinal : -1;
}

static inline int xrt_set_has_dense_i64(xrt_set_t *s, XrValue value) {
    int64_t slot = xrt_set_find_dense_i64_slot(s, value);
    return slot >= 0 ? 1 : xrt_set_has(s, value);
}

static inline int64_t xrt_set_find_dense_enum_slot(xrt_set_t *s, XrValue value) {
    uint32_t ordinal = 0;
    if (!s || xrt_set_is_typed(s) || (s->flags & XR_SET_FLAG_DUMMY) != 0 ||
        !xrt_enum_key_parts(value, NULL, NULL, &ordinal, NULL) || ordinal >= s->nentries)
        return -1;
    XrSetEntry *entry = &s->entries[ordinal];
    if (entry->val_tt == XR_SET_ENTRY_NIL)
        return -1;
    return xrt_set_value_eq(entry, value, xrt_value_type_tag(value)) ? (int64_t) ordinal : -1;
}

static inline int xrt_set_has_dense_enum(xrt_set_t *s, XrValue value) {
    int64_t slot = xrt_set_find_dense_enum_slot(s, value);
    return slot >= 0 ? 1 : xrt_set_has(s, value);
}

static inline int xrt_set_add(xrt_set_t *s, XrValue value) {
    xrt_set_require_mutable(s, "Set.add");
    if (!xrt_set_is_typed(s)) {
        uint32_t hash = xrt_hash32_value(value);
        if (xrt_set_find_entry(s, value, hash) >= 0) {
            xrt_release(value);
            return 0;
        }
        if (s->nentries >= s->entries_cap)
            xrt_set_resize_tagged(s, s->count + 1);
        if (XR_IS_ARRAY_REF(value))
            value = xrt_value_to_owned(value);
        int32_t eidx = (int32_t) s->nentries++;
        XrSetEntry *entry = &s->entries[eidx];
        entry->value = value;
        entry->hash = hash;
        entry->val_tt = xrt_value_type_tag(value);
        s->count++;
        xr_swiss_indices_put(s->ctrl, s->indices, s->indices_size, hash, eidx);
        return 1;
    }
    if (xrt_set_find_value(s, value) >= 0)
        return 0;
    int64_t slot = xrt_set_insert_slot(s, xrt_set_hash_value(s, value));
    (void) xr_typed_set(s->items, (int32_t) slot, value, s->elem_type);
    return 1;
}

static inline int xrt_set_add_prehashed(xrt_set_t *s, XrValue value, uint32_t hash) {
    xrt_set_require_mutable(s, "Set.add");
    if (xrt_set_is_typed(s))
        return xrt_set_add(s, value);
    if (xrt_set_find_entry(s, value, hash) >= 0) {
        xrt_release(value);
        return 0;
    }
    if (s->nentries >= s->entries_cap)
        xrt_set_resize_tagged(s, s->count + 1);
    if (XR_IS_ARRAY_REF(value))
        value = xrt_value_to_owned(value);
    int32_t eidx = (int32_t) s->nentries++;
    XrSetEntry *entry = &s->entries[eidx];
    entry->value = value;
    entry->hash = hash;
    entry->val_tt = xrt_value_type_tag(value);
    s->count++;
    xr_swiss_indices_put(s->ctrl, s->indices, s->indices_size, hash, eidx);
    return 1;
}

static inline int xrt_set_add_user_hash_eq(xrt_set_t *s, XrValue value, uint16_t expected_type_id,
                                           const char *expected_class_name,
                                           xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    xrt_set_require_mutable(s, "Set.add");
    if (!hash_fn || !eq_fn || xrt_set_is_typed(s))
        return xrt_set_add(s, value);
    uint32_t hash = xrt_user_hash_value(value, expected_type_id, expected_class_name, hash_fn);
    if (xrt_set_find_entry_user_eq(s, value, hash, expected_type_id, expected_class_name, eq_fn) >=
        0) {
        xrt_release(value);
        return 0;
    }
    if (s->nentries >= s->entries_cap)
        xrt_set_resize_tagged(s, s->count + 1);
    if (XR_IS_ARRAY_REF(value))
        value = xrt_value_to_owned(value);
    int32_t eidx = (int32_t) s->nentries++;
    XrSetEntry *entry = &s->entries[eidx];
    entry->value = value;
    entry->hash = hash;
    entry->val_tt = xrt_value_type_tag(value);
    s->count++;
    xr_swiss_indices_put(s->ctrl, s->indices, s->indices_size, hash, eidx);
    return 1;
}

static inline int xrt_set_delete(xrt_set_t *s, XrValue value) {
    xrt_set_require_mutable(s, "Set.delete");
    if (!xrt_set_is_typed(s)) {
        uint32_t hash = xrt_hash32_value(value);
        int32_t eidx = -1;
        uint32_t slot = xrt_set_find_entry_slot(s, value, hash, &eidx);
        if (slot == UINT32_MAX)
            return 0;
        xrt_release(s->entries[eidx].value);
        s->entries[eidx].value = XR_NULL_VAL;
        s->entries[eidx].val_tt = XR_SET_ENTRY_NIL;
        s->indices[slot] = XR_SET_IX_EMPTY;
        xr_swiss_ctrl_set(s->ctrl, s->indices_size, slot, XR_SWISS_CTRL_DELETED);
        s->count--;
        return 1;
    }
    int64_t slot = xrt_set_find_value(s, value);
    if (slot < 0)
        return 0;
    xrt_set_erase_slot(s, slot);
    return 1;
}

static inline int xrt_set_delete_prehashed(xrt_set_t *s, XrValue value, uint32_t hash) {
    xrt_set_require_mutable(s, "Set.delete");
    if (xrt_set_is_typed(s))
        return xrt_set_delete(s, value);
    int32_t eidx = -1;
    uint32_t slot = xrt_set_find_entry_slot(s, value, hash, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    xrt_release(s->entries[eidx].value);
    s->entries[eidx].value = XR_NULL_VAL;
    s->entries[eidx].val_tt = XR_SET_ENTRY_NIL;
    s->indices[slot] = XR_SET_IX_EMPTY;
    xr_swiss_ctrl_set(s->ctrl, s->indices_size, slot, XR_SWISS_CTRL_DELETED);
    s->count--;
    return 1;
}

static inline int xrt_set_delete_user_hash_eq(xrt_set_t *s, XrValue value,
                                              uint16_t expected_type_id,
                                              const char *expected_class_name,
                                              xrt_user_hash_fn_t hash_fn, xrt_user_eq_fn_t eq_fn) {
    xrt_set_require_mutable(s, "Set.delete");
    if (!hash_fn || !eq_fn || xrt_set_is_typed(s))
        return xrt_set_delete(s, value);
    int32_t eidx = -1;
    uint32_t hash = xrt_user_hash_value(value, expected_type_id, expected_class_name, hash_fn);
    uint32_t slot =
        xrt_set_find_entry_slot_user_eq(s, value, hash, xrt_value_type_tag(value), expected_type_id,
                                        expected_class_name, eq_fn, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    xrt_release(s->entries[eidx].value);
    s->entries[eidx].value = XR_NULL_VAL;
    s->entries[eidx].val_tt = XR_SET_ENTRY_NIL;
    s->indices[slot] = XR_SET_IX_EMPTY;
    xr_swiss_ctrl_set(s->ctrl, s->indices_size, slot, XR_SWISS_CTRL_DELETED);
    s->count--;
    return 1;
}

static inline int xrt_set_has_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_has(s, XR_FROM_INT(value));
    return xrt_set_find_i64_typed_slot(s, value, XR_ELEM_I64) >= 0;
}

static inline int xrt_set_add_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_add(s, XR_FROM_INT(value));
    return xrt_set_add_i64_typed(s, value, XR_ELEM_I64);
}

static inline int xrt_set_delete_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_delete(s, XR_FROM_INT(value));
    return xrt_set_delete_i64_typed(s, value, XR_ELEM_I64);
}

static inline void xrt_set_clear(xrt_set_t *s) {
    xrt_set_require_mutable(s, "Set.clear");
    if (!xrt_set_is_typed(s)) {
        if (s->flags & XR_SET_FLAG_DUMMY)
            return;
        for (uint32_t i = 0; i < s->nentries; i++) {
            if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
                continue;
            xrt_release(s->entries[i].value);
            s->entries[i].value = XR_NULL_VAL;
            s->entries[i].val_tt = XR_SET_ENTRY_NIL;
        }
        memset(s->ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) s->indices_size + XR_SWISS_GROUP);
        for (uint32_t i = 0; i < s->indices_size; i++)
            s->indices[i] = XR_SET_IX_EMPTY;
        s->nentries = 0;
        s->count = 0;
        return;
    }
    memset(s->ctrl, (int) XRT_CTRL_EMPTY, (size_t) s->cap + XRT_GROUP);
    s->growth_left = s->cap - s->cap / 8;
    s->len = 0;
    s->order_len = 0;
}

static inline XrValue xrt_set_values(xrt_set_t *s) {
    XrValue arr = s->elem_type == XR_ELEM_ANY
                      ? xrt_array_with_capacity(xrt_set_len(s))
                      : xrt_array_with_capacity_typed(xrt_set_len(s), s->elem_type);
    if (!xrt_set_is_typed(s)) {
        for (uint32_t i = 0; i < s->nentries; i++) {
            if (s->entries[i].val_tt != XR_SET_ENTRY_NIL)
                xrt_array_push(arr, xrt_value_to_owned(s->entries[i].value));
        }
        return arr;
    }
    for (int64_t oi = 0; oi < s->order_len; oi++) {
        int64_t slot = s->order[oi];
        if (xrt_set_slot_is_full(s, slot))
            xrt_array_push(arr, xrt_value_to_owned(xrt_set_slot_item(s, slot)));
    }
    return arr;
}

/* keys()/values() of a typed map return arrays whose lane type matches
 * the map's key/value element type: AOT consumers read the result with
 * the statically planned Array<K>/Array<V> lane layout, so returning a
 * tagged array here would be misread as scalar lanes. Untyped maps keep
 * tagged lanes (their static result types are tagged too). */
static inline XrValue xrt_map_keys(xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_keys((xrt_boolmap_t *) m);
    if (!xrt_map_is_typed(m)) {
        /* Untyped storage may still carry a declared scalar key type
         * (xrt_map_new_declared); honor it so the result lanes match the
         * consumer's static Array<K> layout. */
        XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->key_type), XR_TAG_ARRAY);
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY)
                xrt_array_push(arr, xrt_value_to_owned(m->entries[i].key));
        }
        return arr;
    }
    XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->key_type), XR_TAG_ARRAY);
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_value_to_owned(xrt_map_slot_key(m, slot)));
    }
    return arr;
}

static inline XrValue xrt_map_values(xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_values((xrt_boolmap_t *) m);
    if (!xrt_map_is_typed(m)) {
        /* Untyped storage may still carry a declared scalar value type
         * (xrt_map_new_vt); honor it so the result lanes match the
         * consumer's static Array<V> layout. */
        XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->value_type), XR_TAG_ARRAY);
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY)
                xrt_array_push(arr, xrt_value_to_owned(m->entries[i].value));
        }
        return arr;
    }
    XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->value_type), XR_TAG_ARRAY);
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_value_to_owned(xrt_map_slot_value(m, slot)));
    }
    return arr;
}

static inline XrValue xrt_map_entries(xrt_map_t *m) {
    XrValue arr = xrt_array_with_capacity(xrt_map_len(m));
    if (xrt_map_is_boolmap(m)) {
        const xrt_boolmap_t *b = (const xrt_boolmap_t *) m;
        for (int64_t cursor = 0; cursor < xrt_boolmap_len(b); cursor++) {
            XrValue items[2] = {xrt_boolmap_iter_key(b, cursor), xrt_boolmap_iter_value(b, cursor)};
            xrt_array_push(arr, xrt_tuple_make_from_borrowed(2, items));
        }
        return arr;
    }
    if (xrt_map_is_typed(m)) {
        for (int64_t cursor = 0; cursor < m->order_len; cursor++) {
            int64_t slot = m->order[cursor];
            if (!xrt_map_slot_is_full(m, slot))
                continue;
            XrValue items[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
            xrt_array_push(arr, xrt_tuple_make_from_borrowed(2, items));
        }
        return arr;
    }
    for (uint32_t slot = 0; slot < m->nentries; slot++) {
        if (!xrt_map_slot_is_full(m, slot))
            continue;
        XrValue items[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
        xrt_array_push(arr, xrt_tuple_make_from_borrowed(2, items));
    }
    return arr;
}

static inline XrValue xrt_set_value_at(xrt_set_t *s, int64_t index) {
    if (index < 0)
        return XR_NULL_VAL;
    if (xrt_set_is_typed(s)) {
        // order[] may hold tombstoned slots (lazy delete); walk live slots in
        // insertion order to the index-th one.
        int64_t out = 0;
        for (int64_t oi = 0; oi < s->order_len; oi++) {
            int64_t slot = s->order[oi];
            if (!xrt_set_slot_is_full(s, slot))
                continue;
            if (out == index)
                return xrt_set_slot_item(s, slot);
            out++;
        }
        return XR_NULL_VAL;
    }
    int64_t out = 0;
    for (uint32_t i = 0; i < s->nentries; i++) {
        if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
            continue;
        if (out == index)
            return s->entries[i].value;
        out++;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_set_value_at_owned(xrt_set_t *s, int64_t index) {
    return xrt_value_to_owned(xrt_set_value_at(s, index));
}

/* =========================================================================
 * Structural object runtime header (flat field array, O(1) indexed access)
 * ========================================================================= */

enum {
    XRT_OBJECT_STRUCT = XR_OBJECT_DOMAIN_STRUCT
};

typedef struct {
    XrObjHeader hdr;
    const XrtObjectShape *shape;
    XrValue fields[]; /* flexible array of field values */
} xrt_object_t;

static const XrtObjectShape xrt_empty_struct_object_shape = {
    UINT64_C(0x9ed8ff3338f1dc1a), 0, NULL, XRT_OBJECT_STRUCT, XR_OBJECT_SHAPE_STATIC, 0, 0};

static inline int64_t xrt_object_field_count(const xrt_object_t *object) {
    return object && object->shape ? object->shape->field_count : 0;
}

static inline uint8_t xrt_object_domain(const xrt_object_t *object) {
    return object && object->shape ? object->shape->object_domain : XRT_OBJECT_STRUCT;
}

static inline const XrtObjectShapeField *xrt_object_shape_field(const xrt_object_t *object,
                                                                int64_t ordinal) {
    if (!object || !object->shape || !object->shape->fields || ordinal < 0 ||
        ordinal >= object->shape->field_count)
        return NULL;
    return &object->shape->fields[ordinal];
}

static inline const char *xrt_object_field_name(const xrt_object_t *object, int64_t ordinal) {
    const XrtObjectShapeField *field = xrt_object_shape_field(object, ordinal);
    return field ? field->name : NULL;
}

/* =========================================================================
 * Iterator runtime — backs the iterator protocol over Array / Map / Set / string.
 * for-in over an array lowers to an index loop, but Array.iterator() and
 * Array.entriesIterator() are part of the public protocol (§14) and must pull
 * the same elements the VM does.
 * The iterator owns one reference to its source so an in-progress traversal
 * cannot outlive the collection or ARC string it walks. It releases that
 * reference from its builtin ARC destructor.
 * ========================================================================= */

#define XRT_ITER_KEYS 0      /* map: yield key */
#define XRT_ITER_VALUES 1    /* set: yield value; map: yield value */
#define XRT_ITER_PAIRS 2     /* map/string: yield (key, value) tuple */
#define XRT_ITER_GENERATOR 3 /* coroutine-backed generator: pull-driven via gen_drive */

typedef struct XrCoroutine XrCoroutine;

typedef struct {
    XrValue coll;     /* XR_TAG_ARRAY, XR_TAG_MAP, XR_TAG_SET, or string being iterated */
    int64_t cursor;   /* collection cursor, or generator phase: 0=idle 1=buffered 2=done */
    int64_t index;    /* logical iteration index; used by string pair iteration */
    uint8_t kind;     /* XRT_ITER_* projection */
    XrCoroutine *gen; /* XRT_ITER_GENERATOR only; owns the producer coroutine */
} xrt_iterator_t;

/* Generator iterator pull helpers live in xray_rt_coro. Freestanding AOT
 * collection users must not reference them or they would pull in the coroutine
 * runtime for plain Map/Set/String iteration. */
#ifdef XRT_ENABLE_GENERATORS
XR_FUNC int xrt_gen_iter_has_next(xrt_iterator_t *it);
XR_FUNC XrValue xrt_gen_iter_next(xrt_iterator_t *it);
XR_FUNC void xrt_gen_iter_destroy(xrt_iterator_t *it);
#endif

static inline XrValue xrt_iterator_new(XrValue coll, uint8_t kind) {
    xrt_iterator_t *it = (xrt_iterator_t *) xrt_arc_alloc(sizeof(xrt_iterator_t));
    xrt_arc_mark_builtin(it, XRT_ARC_KIND_ITERATOR);
    it->coll = coll;
    it->cursor = 0;
    it->index = 0;
    it->kind = kind;
    it->gen = NULL;
    xrt_retain(coll);
    return xr_mkptr(it, XR_TAG_ITERATOR);
}

static inline int xrt_iter_utf8_decode_scalar(const char *s, int64_t slen, int64_t *cursor,
                                              uint32_t *out_cp) {
    if (!s || !cursor || !out_cp || *cursor < 0 || *cursor >= slen)
        return 0;
    const unsigned char *p = (const unsigned char *) s + *cursor;
    const unsigned char *end = (const unsigned char *) s + slen;
    unsigned char b0 = p[0];
    uint32_t cp = 0;
    int size = 0;
    if (b0 < 0x80u) {
        cp = b0;
        size = 1;
    } else if ((b0 & 0xE0u) == 0xC0u) {
        if (p + 2 > end || (p[1] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x1Fu) << 6) | (uint32_t) (p[1] & 0x3Fu);
        if (cp < 0x80u)
            goto invalid;
        size = 2;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        if (p + 3 > end || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x0Fu) << 12) | ((uint32_t) (p[1] & 0x3Fu) << 6) |
             (uint32_t) (p[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            goto invalid;
        size = 3;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        if (p + 4 > end || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u ||
            (p[3] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x07u) << 18) | ((uint32_t) (p[1] & 0x3Fu) << 12) |
             ((uint32_t) (p[2] & 0x3Fu) << 6) | (uint32_t) (p[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu)
            goto invalid;
        size = 4;
    } else {
        goto invalid;
    }
    *cursor += size;
    *out_cp = cp;
    return 1;

invalid:
    (*cursor)++;
    *out_cp = 0;
    return 0;
}

static inline int xrt_is_struct_object_value(XrValue v) {
    return v.tag == XR_TAG_PTR && v.ptr && v.heap_type == 0;
}

/* Bounds of the range an iterator over XR_TAG_RANGE walks. Planned through the
 * runtime-neutral core so this and the VM's XR_ITERATOR_RANGE
 * (src/runtime/object/xiterator.c) agree on the element count and on every
 * element value. Ranges carry no ARC header, so the iterator's retain/release
 * of `coll` is a no-op and the bounds stay readable for the traversal. */
static inline XrRangeCore xrt_range_iter_core(const xrt_iterator_t *it) {
    const xrt_range_t *r = (const xrt_range_t *) it->coll.ptr;
    if (!r)
        return xr_range_core_make(0, 0, 1);
    return xr_range_core_make_with_bound(r->start, r->end, r->step, r->inclusive_end);
}

static inline int xrt_iterator_has_next(xrt_iterator_t *it) {
    if (it->kind == XRT_ITER_GENERATOR) {
#ifdef XRT_ENABLE_GENERATORS
        return xrt_gen_iter_has_next(it);
#else
        return 0;
#endif
    }
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        if (xrt_map_is_boolmap(m))
            return it->cursor < xrt_boolmap_len((const xrt_boolmap_t *) m);
        if (xrt_map_is_typed(m)) {
            while (it->cursor < m->order_len) {
                if (xrt_map_slot_is_full(m, m->order[it->cursor]))
                    return 1;
                it->cursor++;
            }
        } else {
            while ((uint32_t) it->cursor < m->nentries) {
                if (xrt_map_slot_is_full(m, it->cursor))
                    return 1;
                it->cursor++;
            }
        }
        return 0;
    }
    if (XR_IS_SET(it->coll)) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        if (xrt_set_is_typed(s)) {
            while (it->cursor < s->order_len) {
                if (xrt_set_slot_is_full(s, s->order[it->cursor]))
                    return 1;
                it->cursor++;
            }
        } else {
            while ((uint32_t) it->cursor < s->nentries) {
                if (xrt_set_slot_is_full(s, it->cursor))
                    return 1;
                it->cursor++;
            }
        }
        return 0;
    }
    /* Arrays reach an iterator only through the dynamic protocol — `for (x in
     * arr)` on a statically known array lowers to len()/index instead. A
     * nested generic body is that path (only file-scope generics are
     * monomorphized, so its type parameter survives into lowering), and so is
     * an explicit arr.iterator(). Mirrors XR_ITERATOR_ARRAY in the VM
     * (src/runtime/object/xiterator.c). */
    if (XR_IS_ARRAY(it->coll)) {
        const xrt_array_t *a = (const xrt_array_t *) it->coll.ptr;
        return a && it->cursor < a->length;
    }
    if (XR_IS_STR(it->coll))
        return it->cursor < xr_str_len(it->coll);
    if (it->coll.tag == XR_TAG_RANGE)
        return it->cursor < xr_range_core_length(xrt_range_iter_core(it));
    return 0;
}

static inline XrValue xrt_iterator_next(xrt_iterator_t *it) {
    if (it->kind == XRT_ITER_GENERATOR) {
#ifdef XRT_ENABLE_GENERATORS
        return xrt_gen_iter_next(it);
#else
        return XR_NULL_VAL;
#endif
    }
    /* Past the end there is no value to hand back: next() is typed T, not T?,
     * so the pull protocol is two-step (LANGUAGE_SPEC 5.3.6) and running off
     * the end is a contract violation, not a sentinel. */
    if (!xrt_iterator_has_next(it)) {
        xrt_throw_error(XR_ERR_ITERATOR_EXHAUSTED, XR_ERROR_CORE_ITERATOR_EXHAUSTED_NEXT_MSG);
        return XR_NULL_VAL;
    }
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        if (xrt_map_is_boolmap(m)) {
            xrt_boolmap_t *b = (xrt_boolmap_t *) m;
            int64_t cursor = it->cursor++;
            if (it->kind == XRT_ITER_PAIRS) {
                XrValue kv[2] = {xrt_boolmap_iter_key(b, cursor),
                                 xrt_boolmap_iter_value(b, cursor)};
                return xrt_tuple_make_from_borrowed(2, kv);
            }
            if (it->kind == XRT_ITER_VALUES)
                return xrt_value_to_owned(xrt_boolmap_iter_value(b, cursor));
            return xrt_value_to_owned(xrt_boolmap_iter_key(b, cursor));
        }
        int64_t slot = xrt_map_is_typed(m) ? m->order[it->cursor++] : it->cursor++;
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
            return xrt_tuple_make_from_borrowed(2, kv);
        }
        if (it->kind == XRT_ITER_VALUES)
            return xrt_value_to_owned(xrt_map_slot_value(m, slot));
        return xrt_value_to_owned(xrt_map_slot_key(m, slot));
    }
    if (XR_IS_SET(it->coll)) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        int64_t slot = xrt_set_is_typed(s) ? s->order[it->cursor++] : it->cursor++;
        return xrt_value_to_owned(xrt_set_slot_item(s, slot));
    }
    if (XR_IS_ARRAY(it->coll)) {
        xrt_array_t *a = (xrt_array_t *) it->coll.ptr;
        int64_t idx = it->cursor++;
        XrValue elem = xr_typed_get(a->data, (int32_t) idx, a->elem_type);
        if (it->kind == XRT_ITER_KEYS)
            return XR_FROM_INT(idx);
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {XR_FROM_INT(idx), elem};
            return xrt_tuple_make_from_borrowed(2, kv);
        }
        return xrt_value_to_owned(elem);
    }
    if (XR_IS_STR(it->coll)) {
        int64_t char_index = it->index++;
        uint32_t cp = 0;
        int ok = xrt_iter_utf8_decode_scalar(xr_str_data(it->coll), xr_str_len(it->coll),
                                             &it->cursor, &cp);
        XrValue ch = ok ? XR_FROM_RUNE(cp) : XR_NULL_VAL;
        if (it->kind == XRT_ITER_KEYS)
            return XR_FROM_INT(char_index);
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {XR_FROM_INT(char_index), ch};
            return xrt_tuple_make_from_borrowed(2, kv);
        }
        return ch;
    }
    if (it->coll.tag == XR_TAG_RANGE) {
        /* Range yields plain elements; there is no key/value projection to
         * pick, so KEYS and PAIRS never reach here (`for (k, v in r)` is
         * rejected by the analyzer). */
        int64_t idx = it->cursor++;
        return XR_FROM_INT(xr_range_core_value_at(xrt_range_iter_core(it), idx));
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_process_new(const char *file, int argc, char **argv, const char *dir) {
    if (argc < 0)
        argc = 0;
    XrValue args = xrt_array_with_capacity(argc);
    for (int i = 0; argv && i < argc; i++)
        xrt_array_push(args, xr_box_str(argv[i] ? argv[i] : ""));

    XrValue process = xrt_map_new(4);
    xrt_map_t *m = (xrt_map_t *) process.ptr;
    xrt_map_set(m, xr_box_str("file"), file ? xr_box_str(file) : XR_NULL_VAL);
    xrt_map_set(m, xr_box_str("args"), args);
    xrt_map_set(m, xr_box_str("dir"), dir ? xr_box_str(dir) : XR_NULL_VAL);
    return process;
}

static inline XrValue xrt_value_clone_for_coro(XrValue val);

static inline XrtObjectShape *xrt_object_shape_new_owned(int64_t field_count,
                                                         const char *const *field_names,
                                                         uint8_t object_domain) {
    if (field_count < 0 || field_count > UINT16_MAX) {
        fprintf(stderr, "xrt_object_shape_new_owned: invalid field count\n");
        abort();
    }
    size_t bytes = sizeof(XrtObjectShape) + (size_t) field_count * sizeof(XrtObjectShapeField);
    XrtObjectShape *shape = (XrtObjectShape *) XRT_MALLOC(bytes);
    if (XR_UNLIKELY(!shape)) {
        fprintf(stderr, "xrt_object_shape_new_owned: out of memory\n");
        abort();
    }
    XrtObjectShapeField *fields = field_count > 0 ? (XrtObjectShapeField *) (shape + 1) : NULL;
    for (int64_t i = 0; i < field_count; i++) {
        const char *name = field_names && field_names[i] ? field_names[i] : "?";
        fields[i].name = name;
        fields[i].stable_type_key = 0;
        fields[i].symbol_hash = xr_hash_bytes(name, strlen(name));
        fields[i].ordinal = (uint16_t) i;
        fields[i].flags = 0;
        fields[i].reserved = 0;
    }
    shape->stable_key = xr_object_shape_stable_key(object_domain, fields, field_count);
    shape->field_count = field_count;
    shape->fields = fields;
    shape->object_domain = object_domain;
    shape->storage = XR_OBJECT_SHAPE_OWNED;
    shape->reserved16 = 0;
    shape->reserved32 = 0;
    return shape;
}

static inline XrtObjectShape *xrt_object_shape_clone_owned(const XrtObjectShape *source,
                                                           uint8_t object_domain) {
    if (!source)
        return xrt_object_shape_new_owned(0, NULL, object_domain);
    XrtObjectShape *shape = xrt_object_shape_new_owned(source->field_count, NULL, object_domain);
    XrtObjectShapeField *fields = (XrtObjectShapeField *) shape->fields;
    for (int64_t i = 0; i < source->field_count; i++) {
        fields[i] = source->fields[i];
        fields[i].ordinal = (uint16_t) i;
        fields[i].reserved = 0;
    }
    shape->stable_key =
        xr_object_shape_stable_key(object_domain, shape->fields, shape->field_count);
    return shape;
}

static inline XrValue xrt_object_new_shape_slots(const XrtObjectShape *shape, int64_t field_count) {
    size_t object_bytes = sizeof(xrt_object_t) + (size_t) field_count * sizeof(XrValue);
    xrt_object_t *j = (xrt_object_t *) xrt_execution_alloc_embedded(
        object_bytes, xrt_execution_finalize_struct_object);
    if (XR_UNLIKELY(!j)) {
        fprintf(stderr, "xrt_object_new: out of memory\n");
        abort();
    }
    xrt_heap_header_init(&j->hdr, XR_TINSTANCE);
    j->hdr.extra |= XR_OBJ_HAS_DTOR;
    j->hdr._rsv = XRT_ARC_KIND_OBJECT;
    j->shape = shape;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = XR_NULL_VAL;
    XrValue value = xr_mkptr(j, XR_TAG_PTR);
    value.flags |= XR_VALUE_FLAG_HEADER_AT_PTR;
    return value;
}

static inline XrValue xrt_object_new_shape(const XrtObjectShape *shape) {
    if (!shape || shape->field_count < 0 || shape->field_count > UINT16_MAX) {
        fprintf(stderr, "xrt_object_new_shape: invalid shape descriptor\n");
        abort();
    }
    return xrt_object_new_shape_slots(shape, shape->field_count);
}

/* Generated file-static descriptors have already passed the CGen shape
 * verifier. Keeping the slot count as an immediate preserves native constant
 * propagation and loop unrolling without copying or revalidating metadata. */
static inline XrValue xrt_object_new_static_shape(const XrtObjectShape *shape,
                                                  int64_t field_count) {
    return xrt_object_new_shape_slots(shape, field_count);
}

static inline XrValue xrt_object_new_like(const xrt_object_t *source) {
    if (!source || !source->shape)
        return xrt_object_new_shape(&xrt_empty_struct_object_shape);
    if (source->shape->storage == XR_OBJECT_SHAPE_STATIC)
        return xrt_object_new_shape(source->shape);
    return xrt_object_new_shape(xrt_object_shape_clone_owned(source->shape, XRT_OBJECT_STRUCT));
}

static inline XrValue xrt_object_new_kind(int64_t field_count) {
    if (field_count == 0)
        return xrt_object_new_shape(&xrt_empty_struct_object_shape);
    return xrt_object_new_shape(xrt_object_shape_new_owned(field_count, NULL, XRT_OBJECT_STRUCT));
}

static inline XrValue xrt_object_set_storage(XrValue value, uint8_t storage_mode) {
    return xrt_value_set_storage_graph(value, storage_mode);
}

static inline XrValue xrt_value_set_storage(XrValue value, uint8_t storage_mode) {
    return xrt_value_set_storage_graph(value, storage_mode);
}

static inline XrValue xrt_object_new_named_kind(int64_t field_count, const char *const *field_names,
                                                uint8_t object_kind) {
    if (field_count == 0)
        return xrt_object_new_kind(0);
    return xrt_object_new_shape(xrt_object_shape_new_owned(field_count, field_names, object_kind));
}

static inline XrValue xrt_struct_object_new(int64_t field_count) {
    return xrt_object_new_kind(field_count);
}

static inline XrValue xrt_struct_object_new_named(int64_t field_count,
                                                  const char *const *field_names) {
    return xrt_object_new_named_kind(field_count, field_names, XRT_OBJECT_STRUCT);
}

static inline int64_t xrt_object_find_field(xrt_object_t *j, const char *name) {
    if (!j || !name || !j->shape || !j->shape->fields)
        return -1;
    uint32_t symbol_hash = xr_hash_bytes(name, strlen(name));
    for (int64_t i = 0; i < j->shape->field_count; i++) {
        const XrtObjectShapeField *field = &j->shape->fields[i];
        if (field->symbol_hash == symbol_hash && field->name && strcmp(field->name, name) == 0)
            return i;
    }
    return -1;
}

static inline XrValue xrt_object_get_field(XrValue obj, int field_idx) {
    xrt_object_t *j = (xrt_object_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < xrt_object_field_count(j))
        return j->fields[field_idx];
    return XR_NULL_VAL;
}

static inline void xrt_object_set_field(XrValue obj, int field_idx, XrValue val) {
    xrt_object_t *j = (xrt_object_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < xrt_object_field_count(j))
        j->fields[field_idx] = val;
}

static inline int xrt_object_shape_matches_key(XrValue obj, uint64_t stable_shape_key,
                                               uint8_t object_domain) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || stable_shape_key == 0)
        return 0;
    const xrt_object_t *object = (const xrt_object_t *) obj.ptr;
    return object->shape && object->shape->stable_key == stable_shape_key &&
           object->shape->object_domain == object_domain;
}

static inline int xrt_object_shape_field_matches_fingerprint(XrValue obj, uint16_t ordinal,
                                                             uint64_t stable_name_key,
                                                             uint32_t symbol_hash,
                                                             uint64_t stable_type_key,
                                                             uint8_t flags) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || stable_name_key == 0)
        return 0;
    const xrt_object_t *object = (const xrt_object_t *) obj.ptr;
    if (!object->shape || ordinal >= (uint16_t) object->shape->field_count)
        return 0;
    const XrtObjectShapeField *field = &object->shape->fields[ordinal];
    return field->name && xr_object_shape_stable_name_key(field->name) == stable_name_key &&
           field->symbol_hash == symbol_hash && field->stable_type_key == stable_type_key &&
           field->flags == flags;
}

static inline XrValue xrt_object_access_plan_miss_get(uint32_t object_access_id) {
    fprintf(stderr, "fatal: object access plan %u does not cover the runtime shape\n",
            (unsigned) object_access_id);
    abort();
}

static inline void xrt_object_access_plan_miss_set(uint32_t object_access_id) {
    fprintf(stderr, "fatal: object access plan %u does not cover the runtime shape\n",
            (unsigned) object_access_id);
    abort();
}

static inline XrValue xrt_object_get_name(XrValue obj, const char *name) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return XR_NULL_VAL;
    xrt_object_t *j = (xrt_object_t *) obj.ptr;
    int64_t idx = xrt_object_find_field(j, name);
    if (idx >= 0)
        return j->fields[idx];
    return XR_NULL_VAL;
}

static inline XrValue xrt_object_get_name_owned(XrValue obj, const char *name) {
    return xrt_value_to_owned(xrt_object_get_name(obj, name));
}

static inline int xrt_json_value_matches_kind(XrValue value, uint8_t encoded_kind) {
    if (XR_IS_NULL(value))
        return xr_json_value_kind_base(encoded_kind) == XR_JSON_VALUE_NULL ||
               xr_json_value_kind_base(encoded_kind) == XR_JSON_VALUE_JSON ||
               xr_json_value_kind_is_nullable(encoded_kind);
    switch ((XrJsonValueKind) xr_json_value_kind_base(encoded_kind)) {
        case XR_JSON_VALUE_BOOL:
            return XR_IS_BOOL(value);
        case XR_JSON_VALUE_INT:
            return XR_IS_INT(value);
        case XR_JSON_VALUE_FLOAT:
            return XR_IS_FLOAT(value);
        case XR_JSON_VALUE_STRING:
            return XR_IS_STR(value);
        case XR_JSON_VALUE_JSON:
            return XR_IS_BOOL(value) || XR_IS_INT(value) || XR_IS_FLOAT(value) ||
                   XR_IS_STR(value) || XR_IS_ARRAY(value) || XR_IS_MAP(value);
        case XR_JSON_VALUE_STRUCT_OBJECT:
            return value.tag == XR_TAG_PTR && value.ptr && value.heap_type == 0;
        case XR_JSON_VALUE_ARRAY:
            return XR_IS_ARRAY(value);
        case XR_JSON_VALUE_MAP:
            return XR_IS_MAP(value);
        case XR_JSON_VALUE_ENUM:
            return XR_IS_INT(value) || (value.tag == XR_TAG_ENUM && value.ptr);
        case XR_JSON_VALUE_CLASS_INSTANCE:
            return (value.tag == XR_TAG_PTR && value.ptr && value.heap_type == XR_TINSTANCE) ||
                   (value.tag == XR_TAG_AGG_REF && !XR_IS_ARRAY_REF(value) && value.ptr);
        case XR_JSON_VALUE_NULL:
        case XR_JSON_VALUE_ANY:
        default:
            return 0;
    }
}

static inline void xrt_json_decode_release_partial(XrValue *values, int64_t count) {
    for (int64_t i = 0; values && i < count; i++)
        xrt_release(values[i]);
}

static inline XrValue
xrt_json_decode_struct_object(XrValue data, const XrtObjectShape *target_shape, int64_t field_count,
                              const XrJsonDecodeFieldSpec *fields, int ignore_unknown_fields);

static inline XrValue xrt_json_decode_class_instance(XrValue data,
                                                     const XrJsonClassDecodeSpec *spec,
                                                     int ignore_unknown_fields);

static inline int xrt_json_decode_enum_value(XrValue source, const XrJsonEnumDecodeSpec *spec,
                                             XrValue *out) {
    if (!XR_IS_STR(source) || !spec || !spec->enum_name || !spec->member_names ||
        spec->member_count == 0 || !out)
        return 0;
    const char *name = xr_str_data(source);
    int64_t name_length = xr_str_len(source);
    for (uint16_t i = 0; i < spec->member_count; i++) {
        const char *candidate = spec->member_names[i];
        if (!candidate || (int64_t) strlen(candidate) != name_length ||
            memcmp(candidate, name, (size_t) name_length) != 0)
            continue;
        const XrAotEnumScalarLayout *layout = (const XrAotEnumScalarLayout *) spec->tagged_layout;
        /* The sidecar carries nominal identity and names without allocating.
         * Native class/struct storage extracts the compact ordinal below. */
        *out = layout ? xrt_enum_scalar_box(layout, (int64_t) i) : XR_FROM_INT((int64_t) i);
        return 1;
    }
    return 0;
}

static inline int xrt_json_decode_value(XrValue source, const XrJsonDecodeFieldSpec *spec,
                                        int ignore_unknown_fields, XrValue *out) {
    if (!spec || !out)
        return 0;
    if (XR_IS_NULL(source)) {
        if (xr_json_value_kind_base(spec->value_kind) != XR_JSON_VALUE_NULL &&
            xr_json_value_kind_base(spec->value_kind) != XR_JSON_VALUE_JSON &&
            !xr_json_value_kind_is_nullable(spec->value_kind))
            return 0;
        *out = XR_NULL_VAL;
        return 1;
    }
    switch ((XrJsonValueKind) xr_json_value_kind_base(spec->value_kind)) {
        case XR_JSON_VALUE_STRUCT_OBJECT: {
            if (!spec->target_shape || !spec->nested_fields || spec->nested_field_count == 0)
                return 0;
            XrValue decoded = xrt_json_decode_struct_object(
                source, (const XrtObjectShape *) spec->target_shape, spec->nested_field_count,
                spec->nested_fields, ignore_unknown_fields);
            if (XR_IS_NULL(decoded))
                return 0;
            *out = decoded;
            return 1;
        }
        case XR_JSON_VALUE_ARRAY: {
            if (!XR_IS_ARRAY(source) || !spec->nested_fields || spec->nested_field_count != 1)
                return 0;
            xrt_array_t *src = (xrt_array_t *) source.ptr;
            uint8_t storage = (uint8_t) (uintptr_t) spec->target_shape;
            if (storage >= XR_ELEM_COUNT)
                return 0;
            XrValue dst = xrt_array_new_typed_uninit(src->length, storage);
            for (int64_t i = 0; i < src->length; i++) {
                XrValue item = XR_NULL_VAL;
                XrValue borrowed = xr_typed_get(src->data, (int32_t) i, src->elem_type);
                if (!xrt_json_decode_value(borrowed, &spec->nested_fields[0], ignore_unknown_fields,
                                           &item)) {
                    xrt_release(dst);
                    return 0;
                }
                xrt_array_push(dst, item);
            }
            *out = dst;
            return 1;
        }
        case XR_JSON_VALUE_MAP: {
            if (!XR_IS_MAP(source) || !spec->nested_fields || spec->nested_field_count != 1)
                return 0;
            xrt_map_t *src = (xrt_map_t *) source.ptr;
            uint8_t storage = (uint8_t) (uintptr_t) spec->target_shape;
            if (storage >= XR_ELEM_COUNT)
                return 0;
            XrValue dst = xrt_map_new_declared(xrt_map_len(src), XR_ELEM_ANY, storage);
            xrt_map_t *map = (xrt_map_t *) dst.ptr;
            int64_t slots = xrt_map_is_typed(src) ? src->order_len : (int64_t) src->nentries;
            for (int64_t i = 0; i < slots; i++) {
                int64_t slot = xrt_map_is_typed(src) ? src->order[i] : i;
                if (!xrt_map_slot_is_full(src, slot))
                    continue;
                XrValue key = xrt_map_slot_key(src, slot);
                XrValue value = XR_NULL_VAL;
                if (!XR_IS_STR(key) ||
                    !xrt_json_decode_value(xrt_map_slot_value(src, slot), &spec->nested_fields[0],
                                           ignore_unknown_fields, &value)) {
                    xrt_release(dst);
                    return 0;
                }
                xrt_map_set(map, xrt_value_to_owned(key), value);
            }
            *out = dst;
            return 1;
        }
        case XR_JSON_VALUE_ENUM:
            return xrt_json_decode_enum_value(
                source, (const XrJsonEnumDecodeSpec *) spec->target_shape, out);
        case XR_JSON_VALUE_CLASS_INSTANCE: {
            XrValue decoded = xrt_json_decode_class_instance(
                source, (const XrJsonClassDecodeSpec *) spec->target_shape, ignore_unknown_fields);
            if (XR_IS_NULL(decoded))
                return 0;
            *out = decoded;
            return 1;
        }
        default:
            if (!xrt_json_value_matches_kind(source, spec->value_kind))
                return 0;
            *out = xrt_value_to_owned(source);
            return 1;
    }
}

static inline XrValue xrt_json_decode_typed_value(XrValue source, const XrJsonDecodeFieldSpec *spec,
                                                  int ignore_unknown_fields) {
    XrValue out = XR_NULL_VAL;
    return xrt_json_decode_value(source, spec, ignore_unknown_fields, &out) ? out : XR_NULL_VAL;
}

static inline XrValue xrt_json_decode_typed_value_or_throw(XrValue source,
                                                           const XrJsonDecodeFieldSpec *spec,
                                                           int ignore_unknown_fields) {
    XrValue out = XR_NULL_VAL;
    if (!xrt_json_decode_value(source, spec, ignore_unknown_fields, &out))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.require: value does not match the requested type");
    return out;
}

static inline XrValue
xrt_json_decode_struct_object(XrValue data, const XrtObjectShape *target_shape, int64_t field_count,
                              const XrJsonDecodeFieldSpec *fields, int ignore_unknown_fields) {
    if (field_count <= 0 || !fields || !target_shape || target_shape->field_count != field_count ||
        target_shape->object_domain != XRT_OBJECT_STRUCT)
        return XR_NULL_VAL;
    if (xrt_is_struct_object_value(data)) {
        const xrt_object_t *source = (const xrt_object_t *) data.ptr;
        const XrtObjectShape *actual = source ? source->shape : NULL;
        bool same = actual && actual->stable_key == target_shape->stable_key &&
                    actual->object_domain == target_shape->object_domain &&
                    actual->field_count == target_shape->field_count;
        for (int64_t i = 0; same && i < field_count; i++) {
            const XrtObjectShapeField *a = &actual->fields[i];
            const XrtObjectShapeField *b = &target_shape->fields[i];
            same = a->name && b->name && strcmp(a->name, b->name) == 0 &&
                   a->symbol_hash == b->symbol_hash && a->stable_type_key == b->stable_type_key &&
                   a->flags == b->flags;
        }
        return same ? xrt_value_to_owned(data) : XR_NULL_VAL;
    }
    if (!XR_IS_MAP(data))
        return XR_NULL_VAL;
    xrt_map_t *src = (xrt_map_t *) data.ptr;
    XrValue *decoded_values = (XrValue *) XRT_MALLOC((size_t) field_count * sizeof(XrValue));
    if (XR_UNLIKELY(!decoded_values)) {
        fprintf(stderr, "xrt_json_decode_struct_object: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < field_count; i++) {
        const XrJsonDecodeFieldSpec *field = &fields[i];
        const char *name = field->name;
        XrValue key = name ? xr_box_str(name) : XR_NULL_VAL;
        if (!name || !xrt_map_has(src, key)) {
            if (name && xr_json_value_kind_is_nullable(field->value_kind)) {
                decoded_values[i] = XR_NULL_VAL;
                continue;
            }
            xrt_json_decode_release_partial(decoded_values, i);
            XRT_FREE(decoded_values);
            return XR_NULL_VAL;
        }
        XrValue field_value = xrt_map_get(src, key);
        XrValue decoded = XR_NULL_VAL;
        if (!xrt_json_decode_value(field_value, field, ignore_unknown_fields, &decoded)) {
            xrt_json_decode_release_partial(decoded_values, i);
            XRT_FREE(decoded_values);
            return XR_NULL_VAL;
        }
        decoded_values[i] = decoded;
    }
    if (!ignore_unknown_fields) {
        int64_t slots = xrt_map_is_typed(src) ? src->order_len : (int64_t) src->nentries;
        for (int64_t slot_index = 0; slot_index < slots; slot_index++) {
            int64_t slot = xrt_map_is_typed(src) ? src->order[slot_index] : slot_index;
            if (!xrt_map_slot_is_full(src, slot))
                continue;
            XrValue key = xrt_map_slot_key(src, slot);
            int declared = 0;
            if (XR_IS_STR(key)) {
                const char *name = xr_str_data(key);
                for (int64_t i = 0; i < field_count; i++) {
                    if (fields[i].name && strcmp(fields[i].name, name) == 0) {
                        declared = 1;
                        break;
                    }
                }
            }
            if (!declared) {
                xrt_json_decode_release_partial(decoded_values, field_count);
                XRT_FREE(decoded_values);
                return XR_NULL_VAL;
            }
        }
    }
    XrValue dstv = xrt_object_new_shape(target_shape);
    if (XR_IS_NULL(dstv)) {
        xrt_json_decode_release_partial(decoded_values, field_count);
        XRT_FREE(decoded_values);
        return XR_NULL_VAL;
    }
    for (int64_t i = 0; i < field_count; i++)
        xrt_object_set_field(dstv, (int) i, decoded_values[i]);
    XRT_FREE(decoded_values);
    return dstv;
}

static inline int xrt_json_class_store_field(void *object, const XrJsonClassDecodeFieldSpec *field,
                                             XrValue value) {
    if (!object || !field)
        return 0;
    uint8_t *dst = (uint8_t *) object + field->offset;
    if (xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_ENUM &&
        !XR_IS_NULL(value)) {
        XrValue ordinal = xrt_enum_box_ordinal(value);
        if (!XR_IS_INT(ordinal))
            return 0;
        value = ordinal;
    }
#define XRT_JSON_STORE_NATIVE(type_, expression_)                                                  \
    do {                                                                                           \
        type_ native_value = (expression_);                                                        \
        memcpy(dst, &native_value, sizeof(native_value));                                          \
        return 1;                                                                                  \
    } while (0)
    switch (field->native_type) {
        case XR_NATIVE_I64:
            XRT_JSON_STORE_NATIVE(int64_t, XR_TO_INT(value));
        case XR_NATIVE_U64:
            XRT_JSON_STORE_NATIVE(uint64_t, (uint64_t) XR_TO_INT(value));
        case XR_NATIVE_ISIZE:
            XRT_JSON_STORE_NATIVE(ptrdiff_t, (ptrdiff_t) XR_TO_INT(value));
        case XR_NATIVE_USIZE:
            XRT_JSON_STORE_NATIVE(size_t, (size_t) XR_TO_INT(value));
        case XR_NATIVE_I32:
            XRT_JSON_STORE_NATIVE(int32_t, (int32_t) XR_TO_INT(value));
        case XR_NATIVE_U32:
            XRT_JSON_STORE_NATIVE(uint32_t, (uint32_t) XR_TO_INT(value));
        case XR_NATIVE_I16:
            XRT_JSON_STORE_NATIVE(int16_t, (int16_t) XR_TO_INT(value));
        case XR_NATIVE_U16:
            XRT_JSON_STORE_NATIVE(uint16_t, (uint16_t) XR_TO_INT(value));
        case XR_NATIVE_I8:
            XRT_JSON_STORE_NATIVE(int8_t, (int8_t) XR_TO_INT(value));
        case XR_NATIVE_U8:
            XRT_JSON_STORE_NATIVE(uint8_t, (uint8_t) XR_TO_INT(value));
        case XR_NATIVE_F64:
            XRT_JSON_STORE_NATIVE(double, XR_TO_FLOAT(value));
        case XR_NATIVE_F32:
            XRT_JSON_STORE_NATIVE(float, (float) XR_TO_FLOAT(value));
        case XR_NATIVE_BOOL:
            XRT_JSON_STORE_NATIVE(uint8_t, XR_TO_BOOL(value) ? 1u : 0u);
        case XR_NATIVE_STRING:
        case XR_NATIVE_VALUE:
            memcpy(dst, &value, sizeof(value));
            return 1;
        case XR_NATIVE_ARRAY_REF:
            if (!XR_IS_ARRAY(value))
                return 0;
            XRT_JSON_STORE_NATIVE(void *, value.ptr);
        case XR_NATIVE_MAP_REF:
            if (!XR_IS_MAP(value))
                return 0;
            XRT_JSON_STORE_NATIVE(void *, value.ptr);
        case XR_NATIVE_SET_REF:
            if (!XR_IS_SET(value))
                return 0;
            XRT_JSON_STORE_NATIVE(void *, value.ptr);
        case XR_NATIVE_NESTED_AGGREGATE: {
            const XrJsonClassDecodeSpec *nested =
                xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_CLASS_INSTANCE
                    ? (const XrJsonClassDecodeSpec *) field->value.target_shape
                    : NULL;
            if (!nested || nested->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT ||
                nested->instance_size == 0 || value.tag != XR_TAG_AGG_REF ||
                XR_IS_ARRAY_REF(value) || !value.ptr)
                return 0;
            memcpy(dst, value.ptr, nested->instance_size);
            memset(value.ptr, 0, nested->instance_size);
            xrt_release(value);
            return 1;
        }
        default:
            return 0;
    }
#undef XRT_JSON_STORE_NATIVE
}

static inline XrValue xrt_json_value_struct_alloc(const XrJsonClassDecodeSpec *spec) {
    if (!spec || spec->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT || spec->type_id == 0 ||
        spec->instance_size == 0 || spec->instance_size > UINT16_MAX)
        return XR_NULL_VAL;
    void *object = xrt_arc_alloc(spec->instance_size);
    XrObjHeader *header = XRT_ARC_HDR(object);
    xrt_heap_header_init(header, XR_TINSTANCE);
    xrt_aot_class_type_set(header, spec->type_id);
    header->extra |= XR_OBJ_HAS_DTOR;
    return xr_aggregate_ref(object, (uint16_t) spec->instance_size);
}

static inline XrValue xrt_json_class_from_values(const XrJsonClassDecodeSpec *spec,
                                                 XrValue *values) {
    if (!spec || !values || spec->type_id == 0 || spec->field_count == 0 || !spec->fields)
        return XR_NULL_VAL;
    const XrtTypeDeriveInfo *derive = xrt_type_derive_info(spec->type_id);
    if (!derive || (derive->derive_flags & XR_DERIVE_JSON) == 0)
        return XR_NULL_VAL;
    XrValue result = XR_NULL_VAL;
    void *object = NULL;
    if (spec->target_kind == XR_JSON_NOMINAL_TARGET_CLASS &&
        spec->instance_size >= sizeof(XrObjHeader)) {
        object = xrt_obj_alloc(spec->type_id, spec->instance_size);
        result = xrt_box_obj(object);
    } else if (spec->target_kind == XR_JSON_NOMINAL_TARGET_VALUE_STRUCT) {
        result = xrt_json_value_struct_alloc(spec);
        object = result.ptr;
    }
    if (!object)
        return XR_NULL_VAL;
    for (uint16_t i = 0; i < spec->field_count; i++) {
        if (xrt_json_class_store_field(object, &spec->fields[i], values[i])) {
            values[i] = XR_NULL_VAL;
            continue;
        }
        xrt_release(result);
        return XR_NULL_VAL;
    }
    return result;
}

static inline XrValue xrt_json_decode_class_instance(XrValue data,
                                                     const XrJsonClassDecodeSpec *spec,
                                                     int ignore_unknown_fields) {
    if (!spec || spec->field_count == 0 || !spec->fields || !XR_IS_MAP(data))
        return XR_NULL_VAL;
    xrt_map_t *source = (xrt_map_t *) data.ptr;
    XrValue *values = (XrValue *) XRT_CALLOC((size_t) spec->field_count, sizeof(XrValue));
    if (!values)
        return XR_NULL_VAL;
    for (uint16_t i = 0; i < spec->field_count; i++) {
        const XrJsonClassDecodeFieldSpec *field = &spec->fields[i];
        XrValue key = field->name ? xr_box_str(field->name) : XR_NULL_VAL;
        if (!field->name || !xrt_map_has(source, key)) {
            if (field->name && xr_json_value_kind_is_nullable(field->value.value_kind)) {
                values[i] = XR_NULL_VAL;
                continue;
            }
            xrt_json_decode_release_partial(values, i);
            XRT_FREE(values);
            return XR_NULL_VAL;
        }
        if (!xrt_json_decode_value(xrt_map_get(source, key), &field->value, ignore_unknown_fields,
                                   &values[i])) {
            xrt_json_decode_release_partial(values, i);
            XRT_FREE(values);
            return XR_NULL_VAL;
        }
    }
    if (!ignore_unknown_fields) {
        int64_t slots = xrt_map_is_typed(source) ? source->order_len : (int64_t) source->nentries;
        for (int64_t slot_index = 0; slot_index < slots; slot_index++) {
            int64_t slot = xrt_map_is_typed(source) ? source->order[slot_index] : slot_index;
            if (!xrt_map_slot_is_full(source, slot))
                continue;
            XrValue key = xrt_map_slot_key(source, slot);
            int declared = 0;
            if (XR_IS_STR(key)) {
                const char *name = xr_str_data(key);
                for (uint16_t i = 0; i < spec->field_count; i++) {
                    if (spec->fields[i].name && strcmp(spec->fields[i].name, name) == 0) {
                        declared = 1;
                        break;
                    }
                }
            }
            if (!declared) {
                xrt_json_decode_release_partial(values, spec->field_count);
                XRT_FREE(values);
                return XR_NULL_VAL;
            }
        }
    }
    XrValue result = xrt_json_class_from_values(spec, values);
    if (XR_IS_NULL(result))
        xrt_json_decode_release_partial(values, spec->field_count);
    XRT_FREE(values);
    return result;
}

static inline int64_t xrt_json_static_has(XrValue obj, XrValue key) {
    return XR_IS_MAP(obj) && XR_IS_STR(key) && xrt_map_has((xrt_map_t *) obj.ptr, key) ? 1 : 0;
}

static inline XrValue xrt_json_static_get(XrValue obj, XrValue key, XrValue fallback) {
    if (!XR_IS_MAP(obj) || !XR_IS_STR(key))
        return fallback;
    xrt_map_t *map = (xrt_map_t *) obj.ptr;
    if (!xrt_map_has(map, key))
        return fallback;
    return xrt_map_get_owned(map, key);
}

static inline int64_t xrt_json_static_size(XrValue obj) {
    return XR_IS_MAP(obj) ? xrt_map_len((const xrt_map_t *) obj.ptr) : 0;
}

static inline int64_t xrt_json_static_is_empty(XrValue obj) {
    return xrt_json_static_size(obj) == 0 ? 1 : 0;
}

enum {
    XRT_JSON_RUNTIME_INVALID = -1,
    XRT_JSON_RUNTIME_NULL,
    XRT_JSON_RUNTIME_BOOL,
    XRT_JSON_RUNTIME_INT,
    XRT_JSON_RUNTIME_FLOAT,
    XRT_JSON_RUNTIME_STRING,
    XRT_JSON_RUNTIME_ARRAY,
    XRT_JSON_RUNTIME_OBJECT,
};

static inline int xrt_json_runtime_kind(XrValue value) {
    switch (xrt_value_kind(value)) {
        case XR_TAG_NULL:
            return XRT_JSON_RUNTIME_NULL;
        case XR_TAG_BOOL:
            return XRT_JSON_RUNTIME_BOOL;
        case XR_TAG_I64:
            return XRT_JSON_RUNTIME_INT;
        case XR_TAG_F64:
            return XRT_JSON_RUNTIME_FLOAT;
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return XRT_JSON_RUNTIME_STRING;
        case XR_TAG_ARRAY:
            return XRT_JSON_RUNTIME_ARRAY;
        case XR_TAG_MAP:
            return XRT_JSON_RUNTIME_OBJECT;
        default:
            return XRT_JSON_RUNTIME_INVALID;
    }
}

static inline XrValue xrt_json_static_kind_of(XrValue value) {
    XRT_STR_LIT_DEF(xs_null, "null");
    XRT_STR_LIT_DEF(xs_bool, "bool");
    XRT_STR_LIT_DEF(xs_int, "int");
    XRT_STR_LIT_DEF(xs_float, "float");
    XRT_STR_LIT_DEF(xs_string, "string");
    XRT_STR_LIT_DEF(xs_array, "array");
    XRT_STR_LIT_DEF(xs_object, "object");
    switch (xrt_json_runtime_kind(value)) {
        case XRT_JSON_RUNTIME_NULL:
            return xr_str_lit(&xs_null);
        case XRT_JSON_RUNTIME_BOOL:
            return xr_str_lit(&xs_bool);
        case XRT_JSON_RUNTIME_INT:
            return xr_str_lit(&xs_int);
        case XRT_JSON_RUNTIME_FLOAT:
            return xr_str_lit(&xs_float);
        case XRT_JSON_RUNTIME_STRING:
            return xr_str_lit(&xs_string);
        case XRT_JSON_RUNTIME_ARRAY:
            return xr_str_lit(&xs_array);
        case XRT_JSON_RUNTIME_OBJECT:
            return xr_str_lit(&xs_object);
        default:
            return XR_NULL_VAL;
    }
}

static inline XrValue xrt_json_static_is_kind(XrValue value, int expected_kind) {
    return XR_FROM_BOOL(xrt_json_runtime_kind(value) == expected_kind);
}

static inline int xrt_json_path_read(XrValue root, XrValue path_value, XrValue *out) {
    if (!XR_IS_ARRAY(path_value) || !path_value.ptr || !out)
        return 0;
    xrt_array_t *path = (xrt_array_t *) path_value.ptr;
    XrValue current = root;
    for (int64_t i = 0; i < path->length; i++) {
        XrValue segment = xr_typed_get(path->data, (int32_t) i, path->elem_type);
        if (XR_IS_STR(segment) && XR_IS_MAP(current)) {
            xrt_map_t *map = (xrt_map_t *) current.ptr;
            if (!xrt_map_has(map, segment))
                return 0;
            current = xrt_map_get(map, segment);
            continue;
        }
        if (XR_IS_INT(segment) && XR_IS_ARRAY(current)) {
            xrt_array_t *array = (xrt_array_t *) current.ptr;
            int64_t index = XR_TO_INT(segment);
            if (!array || index < 0 || index >= array->length)
                return 0;
            current = xr_typed_get(array->data, (int32_t) index, array->elem_type);
            continue;
        }
        return 0;
    }
    *out = current;
    return 1;
}

static inline XrValue xrt_json_path_get(XrValue root, XrValue path) {
    XrValue out = XR_NULL_VAL;
    return xrt_json_path_read(root, path, &out) ? xrt_value_to_owned(out) : XR_NULL_VAL;
}

static inline XrValue xrt_json_path_require(XrValue root, XrValue path) {
    XrValue out = XR_NULL_VAL;
    if (!xrt_json_path_read(root, path, &out))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.require: path does not exist or crosses a wrong kind");
    return xrt_value_to_owned(out);
}

static inline int64_t xrt_json_path_contains(XrValue root, XrValue path) {
    XrValue out = XR_NULL_VAL;
    return xrt_json_path_read(root, path, &out) ? 1 : 0;
}

static inline int xrt_json_path_parent(XrValue root, XrValue path_value, int create_parents,
                                       XrValue *out_parent, XrValue *out_last) {
    if (!XR_IS_ARRAY(path_value) || !path_value.ptr || !out_parent || !out_last)
        return 0;
    xrt_array_t *path = (xrt_array_t *) path_value.ptr;
    if (path->length == 0)
        return 0;
    XrValue current = root;
    for (int64_t i = 0; i + 1 < path->length; i++) {
        XrValue segment = xr_typed_get(path->data, (int32_t) i, path->elem_type);
        XrValue next_segment = xr_typed_get(path->data, (int32_t) i + 1, path->elem_type);
        if (XR_IS_STR(segment) && XR_IS_MAP(current)) {
            xrt_map_t *map = (xrt_map_t *) current.ptr;
            if (!xrt_map_has(map, segment)) {
                if (!create_parents || !XR_IS_STR(next_segment))
                    return 0;
                XrValue child = xrt_map_new(4);
                xrt_map_set(map, xrt_value_to_owned(segment), child);
            }
            current = xrt_map_get(map, segment);
            continue;
        }
        if (XR_IS_INT(segment) && XR_IS_ARRAY(current)) {
            xrt_array_t *array = (xrt_array_t *) current.ptr;
            int64_t index = XR_TO_INT(segment);
            if (!array || index < 0 || index >= array->length)
                return 0;
            current = xr_typed_get(array->data, (int32_t) index, array->elem_type);
            continue;
        }
        return 0;
    }
    *out_parent = current;
    *out_last = xr_typed_get(path->data, (int32_t) path->length - 1, path->elem_type);
    return 1;
}

static inline XrValue xrt_json_path_set(XrValue root, XrValue path, XrValue value,
                                        int create_parents) {
    XrValue parent = XR_NULL_VAL;
    XrValue last = XR_NULL_VAL;
    if (xrt_json_path_parent(root, path, create_parents, &parent, &last)) {
        if (XR_IS_STR(last) && XR_IS_MAP(parent)) {
            xrt_map_set((xrt_map_t *) parent.ptr, xrt_value_to_owned(last),
                        xrt_value_to_owned(value));
            return XR_NULL_VAL;
        }
        if (XR_IS_INT(last) && XR_IS_ARRAY(parent)) {
            xrt_array_t *array = (xrt_array_t *) parent.ptr;
            int64_t index = XR_TO_INT(last);
            if (array && index >= 0 && index < array->length) {
                xrt_array_index_set_portable(array, index, value, 1);
                return XR_NULL_VAL;
            }
        }
    }
    xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.set: path does not exist or crosses a wrong kind");
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_path_remove(XrValue root, XrValue path) {
    XrValue parent = XR_NULL_VAL;
    XrValue last = XR_NULL_VAL;
    if (!xrt_json_path_parent(root, path, 0, &parent, &last))
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.remove: path crosses a wrong kind");
    if (XR_IS_STR(last) && XR_IS_MAP(parent))
        return XR_FROM_BOOL(xrt_map_delete((xrt_map_t *) parent.ptr, last));
    if (XR_IS_INT(last) && XR_IS_ARRAY(parent)) {
        xrt_array_t *array = (xrt_array_t *) parent.ptr;
        int64_t index = XR_TO_INT(last);
        if (!array || index < 0 || index >= array->length)
            return XR_FROM_BOOL(0);
        if (array->elem_type != XR_ELEM_ANY)
            xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.remove: array is not a JSON array");
        XrValue *items = (XrValue *) array->data;
        xrt_release(items[index]);
        for (int64_t i = index; i + 1 < array->length; i++)
            items[i] = items[i + 1];
        array->length--;
        items[array->length] = XR_NULL_VAL;
        XR_ARRAY_MARK_MUTATED(array);
        return XR_FROM_BOOL(1);
    }
    xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.remove: path crosses a wrong kind");
    return XR_FROM_BOOL(0);
}

typedef struct {
    const char *src;
    const char *end;
    const char *pos;
    int depth;
    int ignore_unknown_fields;
    xrt_map_t *top_level_rest;
} xrt_json_parser_t;

#define XRT_JSON_PARSE_MAX_DEPTH 256

static inline int xrt_json_parse_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline void xrt_json_parse_skip_ws(xrt_json_parser_t *p) {
    while (p->pos < p->end) {
        char c = *p->pos;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

static int xrt_json_parse_value(xrt_json_parser_t *p, XrValue *out);

static inline int xrt_json_parse_null(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "null", 4) == 0) {
        p->pos += 4;
        *out = XR_NULL_VAL;
        return 1;
    }
    return 0;
}

static inline int xrt_json_parse_bool(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "true", 4) == 0) {
        p->pos += 4;
        *out = XR_FROM_BOOL(1);
        return 1;
    }
    if (p->pos + 5 <= p->end && strncmp(p->pos, "false", 5) == 0) {
        p->pos += 5;
        *out = XR_FROM_BOOL(0);
        return 1;
    }
    return 0;
}

static inline int xrt_json_parse_number(xrt_json_parser_t *p, XrValue *out) {
    const char *start = p->pos;
    if (p->pos < p->end && *p->pos == '-')
        p->pos++;
    if (p->pos >= p->end || !xrt_json_parse_is_digit(*p->pos)) {
        p->pos = start;
        return 0;
    }
    const char *digit_start = p->pos;
    while (p->pos < p->end && xrt_json_parse_is_digit(*p->pos))
        p->pos++;
    if (p->pos - digit_start > 1 && *digit_start == '0') {
        p->pos = start;
        return 0;
    }

    int is_float = 0;
    if (p->pos < p->end && *p->pos == '.') {
        is_float = 1;
        p->pos++;
        if (p->pos >= p->end || !xrt_json_parse_is_digit(*p->pos)) {
            p->pos = start;
            return 0;
        }
        while (p->pos < p->end && xrt_json_parse_is_digit(*p->pos))
            p->pos++;
    }
    if (p->pos < p->end && (*p->pos == 'e' || *p->pos == 'E')) {
        is_float = 1;
        p->pos++;
        if (p->pos < p->end && (*p->pos == '+' || *p->pos == '-'))
            p->pos++;
        if (p->pos >= p->end || !xrt_json_parse_is_digit(*p->pos)) {
            p->pos = start;
            return 0;
        }
        while (p->pos < p->end && xrt_json_parse_is_digit(*p->pos))
            p->pos++;
    }

    size_t tok_len = (size_t) (p->pos - start);
    char stack_buf[64];
    char *buf = stack_buf;
    if (tok_len + 1 > sizeof(stack_buf)) {
        buf = (char *) XRT_MALLOC(tok_len + 1);
        if (!buf) {
            p->pos = start;
            return 0;
        }
    }
    memcpy(buf, start, tok_len);
    buf[tok_len] = '\0';

    if (is_float) {
        *out = XR_FROM_FLOAT(strtod(buf, NULL));
    } else {
        errno = 0;
        int64_t ival = strtoll(buf, NULL, 10);
        *out = (errno == ERANGE) ? XR_FROM_FLOAT(strtod(buf, NULL)) : XR_FROM_INT(ival);
    }
    if (buf != stack_buf)
        XRT_FREE(buf);
    return 1;
}

static inline int xrt_json_parse_hex4(const char *s) {
    unsigned int val = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        val <<= 4;
        if (c >= '0' && c <= '9')
            val |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f')
            val |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val |= (unsigned) (c - 'A' + 10);
        else
            return -1;
    }
    return (int) val;
}

static int xrt_json_parse_string_value(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos >= p->end || *p->pos != '"')
        return 0;
    p->pos++;

    char stack_buf[256];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;
    XrValue str;

#define XRT_JSON_PARSE_STR_ENSURE(n)                                                               \
    do {                                                                                           \
        if (len + (n) >= cap) {                                                                    \
            size_t new_cap = cap * 2;                                                              \
            while (new_cap < len + (n) + 1)                                                        \
                new_cap *= 2;                                                                      \
            char *nb = (char *) XRT_MALLOC(new_cap);                                               \
            if (!nb) {                                                                             \
                if (buf != stack_buf)                                                              \
                    XRT_FREE(buf);                                                                 \
                return 0;                                                                          \
            }                                                                                      \
            memcpy(nb, buf, len);                                                                  \
            if (buf != stack_buf)                                                                  \
                XRT_FREE(buf);                                                                     \
            buf = nb;                                                                              \
            cap = new_cap;                                                                         \
        }                                                                                          \
    } while (0)

    while (p->pos < p->end && *p->pos != '"') {
        if (*p->pos == '\\') {
            p->pos++;
            if (p->pos >= p->end)
                goto bad_string;
            XRT_JSON_PARSE_STR_ENSURE(4);
            switch (*p->pos) {
                case '"':
                    buf[len++] = '"';
                    p->pos++;
                    break;
                case '\\':
                    buf[len++] = '\\';
                    p->pos++;
                    break;
                case '/':
                    buf[len++] = '/';
                    p->pos++;
                    break;
                case 'b':
                    buf[len++] = '\b';
                    p->pos++;
                    break;
                case 'f':
                    buf[len++] = '\f';
                    p->pos++;
                    break;
                case 'n':
                    buf[len++] = '\n';
                    p->pos++;
                    break;
                case 'r':
                    buf[len++] = '\r';
                    p->pos++;
                    break;
                case 't':
                    buf[len++] = '\t';
                    p->pos++;
                    break;
                case 'u': {
                    p->pos++;
                    if (p->pos + 4 > p->end)
                        goto bad_string;
                    int cp = xrt_json_parse_hex4(p->pos);
                    if (cp < 0)
                        goto bad_string;
                    p->pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p->pos + 6 > p->end || p->pos[0] != '\\' || p->pos[1] != 'u')
                            goto bad_string;
                        int low = xrt_json_parse_hex4(p->pos + 2);
                        if (low < 0xDC00 || low > 0xDFFF)
                            goto bad_string;
                        uint32_t full =
                            0x10000u + ((uint32_t) (cp - 0xD800) << 10) + (uint32_t) (low - 0xDC00);
                        int n = xrt_rune_utf8_encode(full, buf + len);
                        if (n <= 0)
                            goto bad_string;
                        len += (size_t) n;
                        p->pos += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        goto bad_string;
                    } else {
                        int n = xrt_rune_utf8_encode((uint32_t) cp, buf + len);
                        if (n <= 0)
                            goto bad_string;
                        len += (size_t) n;
                    }
                    break;
                }
                default:
                    goto bad_string;
            }
        } else {
            XRT_JSON_PARSE_STR_ENSURE(1);
            buf[len++] = *p->pos++;
        }
    }

    if (p->pos >= p->end || *p->pos != '"')
        goto bad_string;
    p->pos++;

    str = xrt_str_alloc(len);
    if (XR_IS_NULL(str))
        goto bad_string;
    memcpy(xr_str_buf(str), buf, len);
    if (buf != stack_buf)
        XRT_FREE(buf);
    *out = str;
    return 1;

bad_string:
    if (buf != stack_buf)
        XRT_FREE(buf);
    return 0;

#undef XRT_JSON_PARSE_STR_ENSURE
}

static int xrt_json_parse_array(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos >= p->end || *p->pos != '[')
        return 0;
    p->pos++;
    XrValue arr = xrt_array_with_capacity(0);
    xrt_json_parse_skip_ws(p);
    if (p->pos < p->end && *p->pos == ']') {
        p->pos++;
        *out = arr;
        return 1;
    }
    while (1) {
        XrValue elem = XR_NULL_VAL;
        xrt_json_parse_skip_ws(p);
        if (!xrt_json_parse_value(p, &elem)) {
            xrt_release(arr);
            return 0;
        }
        xrt_array_push(arr, elem);
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == ']') {
            p->pos++;
            *out = arr;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',') {
            xrt_release(arr);
            return 0;
        }
        p->pos++;
    }
}

static int xrt_json_parse_object(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos >= p->end || *p->pos != '{')
        return 0;
    p->pos++;
    XrValue obj = xrt_map_new(8);
    xrt_map_t *map = (xrt_map_t *) obj.ptr;
    xrt_json_parse_skip_ws(p);
    if (p->pos < p->end && *p->pos == '}') {
        p->pos++;
        *out = obj;
        return 1;
    }
    while (1) {
        XrValue key = XR_NULL_VAL;
        XrValue val = XR_NULL_VAL;
        xrt_json_parse_skip_ws(p);
        if (!xrt_json_parse_string_value(p, &key)) {
            xrt_release(obj);
            return 0;
        }
        xrt_json_parse_skip_ws(p);
        if (p->pos >= p->end || *p->pos != ':') {
            xrt_release(key);
            xrt_release(obj);
            return 0;
        }
        p->pos++;
        xrt_json_parse_skip_ws(p);
        if (!xrt_json_parse_value(p, &val)) {
            xrt_release(key);
            xrt_release(obj);
            return 0;
        }
        xrt_map_set(map, key, val);
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == '}') {
            p->pos++;
            *out = obj;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',') {
            xrt_release(obj);
            return 0;
        }
        p->pos++;
    }
}

static int xrt_json_parse_value(xrt_json_parser_t *p, XrValue *out) {
    xrt_json_parse_skip_ws(p);
    if (p->pos >= p->end || p->depth >= XRT_JSON_PARSE_MAX_DEPTH)
        return 0;
    p->depth++;
    int ok = 0;
    switch (*p->pos) {
        case 'n':
            ok = xrt_json_parse_null(p, out);
            break;
        case 't':
        case 'f':
            ok = xrt_json_parse_bool(p, out);
            break;
        case '"':
            ok = xrt_json_parse_string_value(p, out);
            break;
        case '[':
            ok = xrt_json_parse_array(p, out);
            break;
        case '{':
            ok = xrt_json_parse_object(p, out);
            break;
        default:
            if (*p->pos == '-' || xrt_json_parse_is_digit(*p->pos))
                ok = xrt_json_parse_number(p, out);
            break;
    }
    p->depth--;
    return ok;
}

typedef struct XrtJsonTypedParseError {
    char path[160];
    char expected[48];
    char actual[48];
} XrtJsonTypedParseError;

static inline const char *xrt_json_token_kind(const xrt_json_parser_t *p) {
    if (!p || p->pos >= p->end)
        return "end_of_input";
    switch (*p->pos) {
        case '{':
            return "object";
        case '[':
            return "array";
        case '"':
            return "string";
        case 'n':
            return "null";
        case 't':
        case 'f':
            return "bool";
        default:
            return *p->pos == '-' || xrt_json_parse_is_digit(*p->pos) ? "number" : "invalid";
    }
}

static inline void xrt_json_typed_error(xrt_json_parser_t *p, XrtJsonTypedParseError *error,
                                        const char *path, const char *expected,
                                        const char *actual) {
    if (!error || error->expected[0] != '\0')
        return;
    snprintf(error->path, sizeof(error->path), "%s", path ? path : "$");
    snprintf(error->expected, sizeof(error->expected), "%s", expected ? expected : "valid JSON");
    snprintf(error->actual, sizeof(error->actual), "%s", actual ? actual : xrt_json_token_kind(p));
}

static int xrt_json_skip_string(xrt_json_parser_t *p) {
    if (!p || p->pos >= p->end || *p->pos != '"')
        return 0;
    p->pos++;
    while (p->pos < p->end) {
        unsigned char c = (unsigned char) *p->pos++;
        if (c == '"')
            return 1;
        if (c < 0x20)
            return 0;
        if (c != '\\')
            continue;
        if (p->pos >= p->end)
            return 0;
        char escape = *p->pos++;
        if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
            escape == 'n' || escape == 'r' || escape == 't')
            continue;
        if (escape != 'u' || p->pos + 4 > p->end)
            return 0;
        int codepoint = xrt_json_parse_hex4(p->pos);
        if (codepoint < 0)
            return 0;
        p->pos += 4;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (p->pos + 6 > p->end || p->pos[0] != '\\' || p->pos[1] != 'u')
                return 0;
            int low = xrt_json_parse_hex4(p->pos + 2);
            if (low < 0xDC00 || low > 0xDFFF)
                return 0;
            p->pos += 6;
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return 0;
        }
    }
    return 0;
}

static int xrt_json_skip_value(xrt_json_parser_t *p);

static int xrt_json_skip_array(xrt_json_parser_t *p) {
    if (!p || p->pos >= p->end || *p->pos != '[')
        return 0;
    p->pos++;
    xrt_json_parse_skip_ws(p);
    if (p->pos < p->end && *p->pos == ']') {
        p->pos++;
        return 1;
    }
    while (1) {
        if (!xrt_json_skip_value(p))
            return 0;
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == ']') {
            p->pos++;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',')
            return 0;
        p->pos++;
        xrt_json_parse_skip_ws(p);
    }
}

static int xrt_json_skip_object(xrt_json_parser_t *p) {
    if (!p || p->pos >= p->end || *p->pos != '{')
        return 0;
    p->pos++;
    xrt_json_parse_skip_ws(p);
    if (p->pos < p->end && *p->pos == '}') {
        p->pos++;
        return 1;
    }
    while (1) {
        if (!xrt_json_skip_string(p))
            return 0;
        xrt_json_parse_skip_ws(p);
        if (p->pos >= p->end || *p->pos != ':')
            return 0;
        p->pos++;
        xrt_json_parse_skip_ws(p);
        if (!xrt_json_skip_value(p))
            return 0;
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == '}') {
            p->pos++;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',')
            return 0;
        p->pos++;
        xrt_json_parse_skip_ws(p);
    }
}

static int xrt_json_skip_value(xrt_json_parser_t *p) {
    if (!p)
        return 0;
    xrt_json_parse_skip_ws(p);
    if (p->pos >= p->end || p->depth >= XRT_JSON_PARSE_MAX_DEPTH)
        return 0;
    p->depth++;
    int ok = 0;
    XrValue ignored = XR_NULL_VAL;
    switch (*p->pos) {
        case 'n':
            ok = xrt_json_parse_null(p, &ignored);
            break;
        case 't':
        case 'f':
            ok = xrt_json_parse_bool(p, &ignored);
            break;
        case '"':
            ok = xrt_json_skip_string(p);
            break;
        case '[':
            ok = xrt_json_skip_array(p);
            break;
        case '{':
            ok = xrt_json_skip_object(p);
            break;
        default:
            if (*p->pos == '-' || xrt_json_parse_is_digit(*p->pos))
                ok = xrt_json_parse_number(p, &ignored);
            break;
    }
    p->depth--;
    return ok;
}

static inline const char *xrt_json_expected_kind(uint8_t encoded_kind) {
    switch ((XrJsonValueKind) xr_json_value_kind_base(encoded_kind)) {
        case XR_JSON_VALUE_NULL:
            return "null";
        case XR_JSON_VALUE_BOOL:
            return "bool";
        case XR_JSON_VALUE_INT:
            return "int";
        case XR_JSON_VALUE_FLOAT:
            return "float";
        case XR_JSON_VALUE_STRING:
            return "string";
        case XR_JSON_VALUE_JSON:
            return "JSON";
        case XR_JSON_VALUE_STRUCT_OBJECT:
            return "object";
        case XR_JSON_VALUE_ARRAY:
            return "array";
        case XR_JSON_VALUE_MAP:
            return "object";
        case XR_JSON_VALUE_ENUM:
            return "enum member string";
        case XR_JSON_VALUE_CLASS_INSTANCE:
            return "object";
        case XR_JSON_VALUE_ANY:
        default:
            return "supported JSON.Value";
    }
}

static int
xrt_json_parse_typed_object_value(xrt_json_parser_t *p, const XrtObjectShape *target_shape,
                                  const XrJsonClassDecodeSpec *class_spec, int64_t field_count,
                                  const XrJsonDecodeFieldSpec *fields, const char *path,
                                  XrValue *out, XrtJsonTypedParseError *error, void *native_target);

static int xrt_json_parse_typed_field(xrt_json_parser_t *p, const XrJsonDecodeFieldSpec *field,
                                      const char *path, XrValue *out,
                                      XrtJsonTypedParseError *error);

static int xrt_json_parse_typed_array_value(xrt_json_parser_t *p,
                                            const XrJsonDecodeFieldSpec *field, const char *path,
                                            XrValue *out, XrtJsonTypedParseError *error) {
    if (!p || !field || !field->nested_fields || field->nested_field_count != 1 ||
        p->pos >= p->end || *p->pos != '[' || p->depth >= XRT_JSON_PARSE_MAX_DEPTH)
        return 0;
    uint8_t storage = (uint8_t) (uintptr_t) field->target_shape;
    if (storage >= XR_ELEM_COUNT)
        return 0;
    p->pos++;
    p->depth++;
    XrValue array = xrt_array_new_typed_uninit(4, storage);
    int ok = !XR_IS_NULL(array);
    int64_t index = 0;
    xrt_json_parse_skip_ws(p);
    if (ok && p->pos < p->end && *p->pos == ']') {
        p->pos++;
    } else {
        while (ok) {
            XrValue item = XR_NULL_VAL;
            char item_path[160];
            snprintf(item_path, sizeof(item_path), "%s[%lld]", path, (long long) index);
            ok = xrt_json_parse_typed_field(p, &field->nested_fields[0], item_path, &item, error);
            if (!ok)
                break;
            xrt_array_push(array, item);
            index++;
            xrt_json_parse_skip_ws(p);
            if (p->pos < p->end && *p->pos == ']') {
                p->pos++;
                break;
            }
            if (p->pos >= p->end || *p->pos != ',') {
                xrt_json_typed_error(p, error, path, "',' or ']'", NULL);
                ok = 0;
                break;
            }
            p->pos++;
            xrt_json_parse_skip_ws(p);
        }
    }
    p->depth--;
    if (!ok) {
        xrt_release(array);
        return 0;
    }
    *out = array;
    return 1;
}

static int xrt_json_parse_typed_map_value(xrt_json_parser_t *p, const XrJsonDecodeFieldSpec *field,
                                          const char *path, XrValue *out,
                                          XrtJsonTypedParseError *error) {
    if (!p || !field || !field->nested_fields || field->nested_field_count != 1 ||
        p->pos >= p->end || *p->pos != '{' || p->depth >= XRT_JSON_PARSE_MAX_DEPTH)
        return 0;
    uint8_t storage = (uint8_t) (uintptr_t) field->target_shape;
    if (storage >= XR_ELEM_COUNT)
        return 0;
    p->pos++;
    p->depth++;
    XrValue map_value = xrt_map_new_declared(4, XR_ELEM_ANY, storage);
    xrt_map_t *map = (xrt_map_t *) map_value.ptr;
    int ok = map != NULL;
    xrt_json_parse_skip_ws(p);
    if (ok && p->pos < p->end && *p->pos == '}') {
        p->pos++;
    } else {
        while (ok) {
            XrValue key = XR_NULL_VAL;
            XrValue value = XR_NULL_VAL;
            if (!xrt_json_parse_string_value(p, &key)) {
                xrt_json_typed_error(p, error, path, "object field name", NULL);
                ok = 0;
                break;
            }
            xrt_json_parse_skip_ws(p);
            if (p->pos >= p->end || *p->pos != ':') {
                xrt_release(key);
                xrt_json_typed_error(p, error, path, "':'", NULL);
                ok = 0;
                break;
            }
            p->pos++;
            xrt_json_parse_skip_ws(p);
            char value_path[160];
            snprintf(value_path, sizeof(value_path), "%s[%s]", path, xr_str_data(key));
            ok = xrt_json_parse_typed_field(p, &field->nested_fields[0], value_path, &value, error);
            if (!ok) {
                xrt_release(key);
                break;
            }
            xrt_map_set(map, key, value);
            xrt_json_parse_skip_ws(p);
            if (p->pos < p->end && *p->pos == '}') {
                p->pos++;
                break;
            }
            if (p->pos >= p->end || *p->pos != ',') {
                xrt_json_typed_error(p, error, path, "',' or '}'", NULL);
                ok = 0;
                break;
            }
            p->pos++;
            xrt_json_parse_skip_ws(p);
        }
    }
    p->depth--;
    if (!ok) {
        xrt_release(map_value);
        return 0;
    }
    *out = map_value;
    return 1;
}

static int xrt_json_parse_typed_field(xrt_json_parser_t *p, const XrJsonDecodeFieldSpec *field,
                                      const char *path, XrValue *out,
                                      XrtJsonTypedParseError *error) {
    xrt_json_parse_skip_ws(p);
    if (xr_json_value_kind_is_nullable(field->value_kind) && p->pos + 4 <= p->end &&
        strncmp(p->pos, "null", 4) == 0)
        return xrt_json_parse_null(p, out);

    int ok = 0;
    switch ((XrJsonValueKind) xr_json_value_kind_base(field->value_kind)) {
        case XR_JSON_VALUE_NULL:
            ok = xrt_json_parse_null(p, out);
            break;
        case XR_JSON_VALUE_BOOL:
            ok = xrt_json_parse_bool(p, out);
            break;
        case XR_JSON_VALUE_INT:
            ok = xrt_json_parse_number(p, out) && XR_IS_INT(*out);
            break;
        case XR_JSON_VALUE_FLOAT:
            ok = xrt_json_parse_number(p, out) && XR_IS_FLOAT(*out);
            break;
        case XR_JSON_VALUE_STRING:
            ok = xrt_json_parse_string_value(p, out);
            break;
        case XR_JSON_VALUE_JSON:
            ok = xrt_json_parse_value(p, out);
            break;
        case XR_JSON_VALUE_STRUCT_OBJECT:
            ok = field->target_shape && field->nested_fields && field->nested_field_count > 0 &&
                 xrt_json_parse_typed_object_value(p, (const XrtObjectShape *) field->target_shape,
                                                   NULL, field->nested_field_count,
                                                   field->nested_fields, path, out, error, NULL);
            break;
        case XR_JSON_VALUE_ARRAY:
            ok = xrt_json_parse_typed_array_value(p, field, path, out, error);
            break;
        case XR_JSON_VALUE_MAP:
            ok = xrt_json_parse_typed_map_value(p, field, path, out, error);
            break;
        case XR_JSON_VALUE_ENUM: {
            XrValue parsed = XR_NULL_VAL;
            ok = xrt_json_parse_string_value(p, &parsed) &&
                 xrt_json_decode_enum_value(
                     parsed, (const XrJsonEnumDecodeSpec *) field->target_shape, out);
            xrt_release(parsed);
            break;
        }
        case XR_JSON_VALUE_CLASS_INSTANCE: {
            const XrJsonClassDecodeSpec *class_spec =
                (const XrJsonClassDecodeSpec *) field->target_shape;
            ok = class_spec && class_spec->field_count > 0 && class_spec->fields &&
                 xrt_json_parse_typed_object_value(p, NULL, class_spec, class_spec->field_count,
                                                   NULL, path, out, error, NULL);
            break;
        }
        case XR_JSON_VALUE_ANY:
        default:
            ok = 0;
            break;
    }
    if (!ok)
        xrt_json_typed_error(p, error, path, xrt_json_expected_kind(field->value_kind), NULL);
    return ok;
}

static int xrt_json_parse_typed_object_value(xrt_json_parser_t *p,
                                             const XrtObjectShape *target_shape,
                                             const XrJsonClassDecodeSpec *class_spec,
                                             int64_t field_count,
                                             const XrJsonDecodeFieldSpec *fields, const char *path,
                                             XrValue *out, XrtJsonTypedParseError *error,
                                             void *native_target) {
    int struct_target = target_shape && fields && target_shape->field_count == field_count &&
                        target_shape->object_domain == XRT_OBJECT_STRUCT;
    int class_target = class_spec && class_spec->fields && class_spec->field_count == field_count;
    if (!p || !out || field_count <= 0 || (!struct_target && !class_target) ||
        p->depth >= XRT_JSON_PARSE_MAX_DEPTH) {
        xrt_json_typed_error(p, error, path, "object", NULL);
        return 0;
    }
    xrt_json_parse_skip_ws(p);
    if (p->pos >= p->end || *p->pos != '{') {
        xrt_json_typed_error(p, error, path, "object", NULL);
        return 0;
    }
    p->pos++;
    p->depth++;
    xrt_map_t *rest = p->top_level_rest;
    int collect_rest = rest != NULL;
    if (collect_rest)
        p->top_level_rest = NULL;

    XrValue *values = (XrValue *) XRT_MALLOC((size_t) field_count * sizeof(XrValue));
    uint8_t *seen = (uint8_t *) XRT_CALLOC((size_t) field_count, sizeof(uint8_t));
    if (!values || !seen) {
        XRT_FREE(values);
        XRT_FREE(seen);
        if (collect_rest)
            p->top_level_rest = rest;
        p->depth--;
        xrt_json_typed_error(p, error, path, "available memory", "out_of_memory");
        return 0;
    }
    for (int64_t i = 0; i < field_count; i++)
        values[i] = XR_NULL_VAL;

    int ok = 1;
    xrt_json_parse_skip_ws(p);
    if (p->pos < p->end && *p->pos == '}') {
        p->pos++;
    } else {
        while (ok) {
            XrValue key = XR_NULL_VAL;
            if (!xrt_json_parse_string_value(p, &key)) {
                xrt_json_typed_error(p, error, path, "object field name", NULL);
                ok = 0;
                break;
            }
            const char *key_name = xr_str_data(key);
            xrt_json_parse_skip_ws(p);
            if (p->pos >= p->end || *p->pos != ':') {
                xrt_release(key);
                xrt_json_typed_error(p, error, path, "':'", NULL);
                ok = 0;
                break;
            }
            p->pos++;
            xrt_json_parse_skip_ws(p);

            int64_t field_index = -1;
            for (int64_t i = 0; i < field_count; i++) {
                const char *candidate = class_target ? class_spec->fields[i].name : fields[i].name;
                if (candidate && strcmp(candidate, key_name) == 0) {
                    field_index = i;
                    break;
                }
            }
            if (field_index < 0) {
                if (collect_rest) {
                    XrValue rest_value = XR_NULL_VAL;
                    ok = xrt_json_parse_value(p, &rest_value);
                    if (ok) {
                        xrt_retain(key);
                        xrt_map_set(rest, key, rest_value);
                    }
                } else if (p->ignore_unknown_fields) {
                    ok = xrt_json_skip_value(p);
                } else {
                    char field_path[160];
                    snprintf(field_path, sizeof(field_path), "%s.%s", path, key_name);
                    xrt_json_typed_error(p, error, field_path, "declared field", "unknown");
                    ok = 0;
                }
                if (!ok && (!error || error->expected[0] == '\0'))
                    xrt_json_typed_error(p, error, path, "valid JSON.Value", NULL);
            } else if (seen[field_index]) {
                char field_path[160];
                snprintf(field_path, sizeof(field_path), "%s.%s", path, key_name);
                xrt_json_typed_error(p, error, field_path, "one field occurrence", "duplicate");
                ok = 0;
            } else {
                char field_path[160];
                snprintf(field_path, sizeof(field_path), "%s.%s", path, key_name);
                const XrJsonDecodeFieldSpec *value_spec =
                    class_target ? &class_spec->fields[field_index].value : &fields[field_index];
                const XrJsonClassDecodeFieldSpec *native_field =
                    class_target && native_target ? &class_spec->fields[field_index] : NULL;
                const XrJsonClassDecodeSpec *nested_native =
                    value_spec && xr_json_value_kind_base(value_spec->value_kind) ==
                                      XR_JSON_VALUE_CLASS_INSTANCE
                        ? (const XrJsonClassDecodeSpec *) value_spec->target_shape
                        : NULL;
                if (native_field && native_field->native_type == XR_NATIVE_NESTED_AGGREGATE &&
                    nested_native &&
                    nested_native->target_kind == XR_JSON_NOMINAL_TARGET_VALUE_STRUCT &&
                    nested_native->instance_size > 0) {
                    void *nested_target = (uint8_t *) native_target + native_field->offset;
                    memset(nested_target, 0, nested_native->instance_size);
                    ok = xrt_json_parse_typed_object_value(
                        p, NULL, nested_native, nested_native->field_count, NULL, field_path,
                        &values[field_index], error, nested_target);
                } else {
                    ok = xrt_json_parse_typed_field(p, value_spec, field_path, &values[field_index],
                                                    error);
                }
                if (ok)
                    seen[field_index] = 1;
            }
            xrt_release(key);
            if (!ok)
                break;
            xrt_json_parse_skip_ws(p);
            if (p->pos < p->end && *p->pos == '}') {
                p->pos++;
                break;
            }
            if (p->pos >= p->end || *p->pos != ',') {
                xrt_json_typed_error(p, error, path, "',' or '}'", NULL);
                ok = 0;
                break;
            }
            p->pos++;
            xrt_json_parse_skip_ws(p);
        }
    }

    if (ok) {
        for (int64_t i = 0; i < field_count; i++) {
            if (seen[i])
                continue;
            const XrJsonDecodeFieldSpec *missing_spec =
                class_target ? &class_spec->fields[i].value : &fields[i];
            if (missing_spec && xr_json_value_kind_is_nullable(missing_spec->value_kind))
                continue;
            char missing_path[160];
            const char *missing_name = class_target ? class_spec->fields[i].name : fields[i].name;
            snprintf(missing_path, sizeof(missing_path), "%s.%s", path,
                     missing_name ? missing_name : "?");
            xrt_json_typed_error(p, error, missing_path, "present field", "missing");
            ok = 0;
            break;
        }
    }

    XrValue result = XR_NULL_VAL;
    if (ok) {
        if (class_target && native_target) {
            for (int64_t i = 0; i < field_count; i++) {
                const XrJsonClassDecodeFieldSpec *native_field = &class_spec->fields[i];
                const XrJsonClassDecodeSpec *nested_native =
                    xr_json_value_kind_base(native_field->value.value_kind) ==
                            XR_JSON_VALUE_CLASS_INSTANCE
                        ? (const XrJsonClassDecodeSpec *) native_field->value.target_shape
                        : NULL;
                if (native_field->native_type == XR_NATIVE_NESTED_AGGREGATE && nested_native &&
                    nested_native->target_kind == XR_JSON_NOMINAL_TARGET_VALUE_STRUCT)
                    continue;
                if (!xrt_json_class_store_field(native_target, native_field, values[i])) {
                    xrt_json_typed_error(p, error, path, "valid native field layout",
                                         "invalid_target");
                    ok = 0;
                    break;
                }
                values[i] = XR_NULL_VAL;
            }
        } else {
            result = class_target ? xrt_json_class_from_values(class_spec, values)
                                  : xrt_object_new_shape(target_shape);
        }
        if (ok && !native_target && XR_IS_NULL(result)) {
            xrt_json_typed_error(p, error, path, "available memory", "out_of_memory");
            ok = 0;
        } else if (ok && struct_target) {
            for (int64_t i = 0; i < field_count; i++)
                xrt_object_set_field(result, (int) i, values[i]);
        }
    }
    if (!ok)
        xrt_json_decode_release_partial(values, field_count);
    XRT_FREE(seen);
    XRT_FREE(values);
    if (collect_rest)
        p->top_level_rest = rest;
    p->depth--;
    if (!ok)
        return 0;
    *out = result;
    return 1;
}

static void xrt_json_native_struct_release(const XrJsonClassDecodeSpec *spec, void *target) {
    if (!spec || !target || spec->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT ||
        spec->field_count == 0 || !spec->fields)
        return;
    for (uint16_t i = 0; i < spec->field_count; i++) {
        const XrJsonClassDecodeFieldSpec *field = &spec->fields[i];
        uint8_t *slot = (uint8_t *) target + field->offset;
        switch (field->native_type) {
            case XR_NATIVE_STRING:
            case XR_NATIVE_VALUE: {
                XrValue value = XR_NULL_VAL;
                memcpy(&value, slot, sizeof(value));
                xrt_release(value);
                break;
            }
            case XR_NATIVE_ARRAY_REF: {
                void *ptr = NULL;
                memcpy(&ptr, slot, sizeof(ptr));
                if (ptr)
                    xrt_release(xr_mkptr(ptr, XR_TAG_ARRAY));
                break;
            }
            case XR_NATIVE_MAP_REF: {
                void *ptr = NULL;
                memcpy(&ptr, slot, sizeof(ptr));
                if (ptr)
                    xrt_release(xr_mkptr(ptr, XR_TAG_MAP));
                break;
            }
            case XR_NATIVE_SET_REF: {
                void *ptr = NULL;
                memcpy(&ptr, slot, sizeof(ptr));
                if (ptr)
                    xrt_release(xr_mkptr(ptr, XR_TAG_SET));
                break;
            }
            case XR_NATIVE_NESTED_AGGREGATE: {
                const XrJsonClassDecodeSpec *nested =
                    xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_CLASS_INSTANCE
                        ? (const XrJsonClassDecodeSpec *) field->value.target_shape
                        : NULL;
                if (nested && nested->target_kind == XR_JSON_NOMINAL_TARGET_VALUE_STRUCT)
                    xrt_json_native_struct_release(nested, slot);
                break;
            }
            default:
                break;
        }
    }
    memset(target, 0, spec->instance_size);
}

static inline void xrt_json_parse_typed_native_or_throw(XrValue text,
                                                        const XrJsonClassDecodeSpec *spec,
                                                        void *target, int ignore_unknown_fields) {
    if (!spec || !target || spec->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT ||
        spec->instance_size == 0 || spec->field_count == 0 || !spec->fields)
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parse<T>: path $ expected valid native target, got invalid_target");
    memset(target, 0, spec->instance_size);
    if (!XR_IS_STR(text))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parse<T>: path $ expected string, got non_string");
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    xrt_json_parser_t parser = {
        .src = data,
        .end = data ? data + len : data,
        .pos = data,
        .depth = 0,
        .ignore_unknown_fields = ignore_unknown_fields != 0,
    };
    XrtJsonTypedParseError error = {{0}, {0}, {0}};
    XrValue ignored = XR_NULL_VAL;
    int ok = data && len > 0 &&
             xrt_json_parse_typed_object_value(&parser, NULL, spec, spec->field_count, NULL, "$",
                                               &ignored, &error, target);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_json_typed_error(&parser, &error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    if (!ok) {
        char message[320];
        xrt_json_native_struct_release(spec, target);
        snprintf(message, sizeof(message), "JSON.parse<T>: path %s expected %s, got %s",
                 error.path[0] ? error.path : "$",
                 error.expected[0] ? error.expected : "valid JSON",
                 error.actual[0] ? error.actual : "invalid");
        xrt_throw_error(XR_ERR_JSON_INVALID, message);
    }
}

static inline XrValue xrt_json_parse_typed_object_or_throw(XrValue text,
                                                           const XrtObjectShape *target_shape,
                                                           int64_t field_count,
                                                           const XrJsonDecodeFieldSpec *fields,
                                                           int ignore_unknown_fields) {
    if (!XR_IS_STR(text))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parse<T>: path $ expected string, got non_string");
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    xrt_json_parser_t parser = {
        .src = data,
        .end = data ? data + len : data,
        .pos = data,
        .depth = 0,
        .ignore_unknown_fields = ignore_unknown_fields != 0,
    };
    XrtJsonTypedParseError error = {{0}, {0}, {0}};
    XrValue out = XR_NULL_VAL;
    int ok = data && len > 0 &&
             xrt_json_parse_typed_object_value(&parser, target_shape, NULL, field_count, fields,
                                               "$", &out, &error, NULL);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_release(out);
            out = XR_NULL_VAL;
            xrt_json_typed_error(&parser, &error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    if (!ok) {
        char message[320];
        snprintf(message, sizeof(message), "JSON.parse<T>: path %s expected %s, got %s",
                 error.path[0] ? error.path : "$",
                 error.expected[0] ? error.expected : "valid JSON",
                 error.actual[0] ? error.actual : "invalid");
        xrt_throw_error(XR_ERR_JSON_INVALID, message);
    }
    return out;
}

static inline XrValue xrt_json_parse_typed_value_or_throw(XrValue text,
                                                          const XrJsonDecodeFieldSpec *spec,
                                                          int ignore_unknown_fields) {
    if (!XR_IS_STR(text))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parse<T>: path $ expected string, got non_string");
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    xrt_json_parser_t parser = {
        .src = data,
        .end = data ? data + len : data,
        .pos = data,
        .depth = 0,
        .ignore_unknown_fields = ignore_unknown_fields != 0,
    };
    XrtJsonTypedParseError error = {{0}, {0}, {0}};
    XrValue out = XR_NULL_VAL;
    int ok =
        data && len > 0 && spec && xrt_json_parse_typed_field(&parser, spec, "$", &out, &error);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_release(out);
            out = XR_NULL_VAL;
            xrt_json_typed_error(&parser, &error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    if (!ok) {
        char message[320];
        snprintf(message, sizeof(message), "JSON.parse<T>: path %s expected %s, got %s",
                 error.path[0] ? error.path : "$",
                 error.expected[0] ? error.expected : "valid JSON",
                 error.actual[0] ? error.actual : "invalid");
        xrt_throw_error(XR_ERR_JSON_INVALID, message);
    }
    return out;
}

static inline XrValue xrt_json_with_rest_object(const XrtObjectShape *wrapper_shape, XrValue rest,
                                                XrValue value) {
    XrValue wrapper = xrt_object_new_shape(wrapper_shape);
    if (XR_IS_NULL(wrapper)) {
        xrt_release(rest);
        xrt_release(value);
        return XR_NULL_VAL;
    }
    xrt_object_t *object = (xrt_object_t *) wrapper.ptr;
    int64_t rest_index = xrt_object_find_field(object, "rest");
    int64_t value_index = xrt_object_find_field(object, "value");
    if (rest_index < 0 || value_index < 0) {
        xrt_release(wrapper);
        xrt_release(rest);
        xrt_release(value);
        return XR_NULL_VAL;
    }
    xrt_object_set_field(wrapper, (int) rest_index, rest);
    xrt_object_set_field(wrapper, (int) value_index, value);
    return wrapper;
}

static inline XrValue xrt_json_parse_with_rest_object_or_throw(
    XrValue text, const XrtObjectShape *wrapper_shape, const XrtObjectShape *target_shape,
    int64_t field_count, const XrJsonDecodeFieldSpec *fields, int ignore_nested_unknown_fields) {
    if (!XR_IS_STR(text))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parseWithRest<T>: path $ expected string, got non_string");
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    XrValue rest = xrt_map_new_declared(4, XR_ELEM_ANY, XR_ELEM_ANY);
    xrt_json_parser_t parser = {
        .src = data,
        .end = data ? data + len : data,
        .pos = data,
        .depth = 0,
        .ignore_unknown_fields = ignore_nested_unknown_fields != 0,
        .top_level_rest = (xrt_map_t *) rest.ptr,
    };
    XrtJsonTypedParseError error = {{0}, {0}, {0}};
    XrValue value = XR_NULL_VAL;
    int ok = !XR_IS_NULL(rest) && data && len > 0 &&
             xrt_json_parse_typed_object_value(&parser, target_shape, NULL, field_count, fields,
                                               "$", &value, &error, NULL);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_release(value);
            value = XR_NULL_VAL;
            xrt_json_typed_error(&parser, &error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    XrValue wrapper = ok ? xrt_json_with_rest_object(wrapper_shape, rest, value) : XR_NULL_VAL;
    if (!ok || XR_IS_NULL(wrapper)) {
        if (!ok) {
            xrt_release(rest);
            xrt_release(value);
        }
        char message[320];
        snprintf(message, sizeof(message), "JSON.parseWithRest<T>: path %s expected %s, got %s",
                 error.path[0] ? error.path : "$",
                 error.expected[0] ? error.expected : "valid JSON",
                 error.actual[0] ? error.actual : "invalid");
        xrt_throw_error(XR_ERR_JSON_INVALID, message);
    }
    return wrapper;
}

static inline XrValue
xrt_json_parse_with_rest_class_or_throw(XrValue text, const XrtObjectShape *wrapper_shape,
                                        const XrJsonClassDecodeSpec *target_spec,
                                        int ignore_nested_unknown_fields) {
    if (!XR_IS_STR(text))
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.parseWithRest<T>: path $ expected string, got non_string");
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    XrValue rest = xrt_map_new_declared(4, XR_ELEM_ANY, XR_ELEM_ANY);
    xrt_json_parser_t parser = {
        .src = data,
        .end = data ? data + len : data,
        .pos = data,
        .depth = 0,
        .ignore_unknown_fields = ignore_nested_unknown_fields != 0,
        .top_level_rest = (xrt_map_t *) rest.ptr,
    };
    XrtJsonTypedParseError error = {{0}, {0}, {0}};
    XrValue value = XR_NULL_VAL;
    int ok = !XR_IS_NULL(rest) && data && len > 0 && target_spec &&
             xrt_json_parse_typed_object_value(&parser, NULL, target_spec, target_spec->field_count,
                                               NULL, "$", &value, &error, NULL);
    if (ok) {
        xrt_json_parse_skip_ws(&parser);
        if (parser.pos != parser.end) {
            xrt_release(value);
            value = XR_NULL_VAL;
            xrt_json_typed_error(&parser, &error, "$", "end_of_input", NULL);
            ok = 0;
        }
    }
    XrValue wrapper = ok ? xrt_json_with_rest_object(wrapper_shape, rest, value) : XR_NULL_VAL;
    if (!ok || XR_IS_NULL(wrapper)) {
        if (!ok) {
            xrt_release(rest);
            xrt_release(value);
        }
        char message[320];
        snprintf(message, sizeof(message), "JSON.parseWithRest<T>: path %s expected %s, got %s",
                 error.path[0] ? error.path : "$",
                 error.expected[0] ? error.expected : "valid JSON",
                 error.actual[0] ? error.actual : "invalid");
        xrt_throw_error(XR_ERR_JSON_INVALID, message);
    }
    return wrapper;
}

static inline int xrt_json_try_parse(XrValue text, XrValue *out) {
    if (out)
        *out = XR_NULL_VAL;
    if (!out)
        return 0;
    if (!XR_IS_STR(text))
        return 0;
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    if (!data || len <= 0)
        return 0;
    xrt_json_parser_t p = {.src = data, .end = data + len, .pos = data, .depth = 0};
    if (!xrt_json_parse_value(&p, out))
        return 0;
    xrt_json_parse_skip_ws(&p);
    if (p.pos == p.end)
        return 1;
    xrt_release(*out);
    *out = XR_NULL_VAL;
    return 0;
}

static inline XrValue xrt_json_parse_or_throw(XrValue text) {
    XrValue value = XR_NULL_VAL;
    if (!xrt_json_try_parse(text, &value))
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.parse: invalid JSON");
    return value;
}

static inline XrValue xrt_json_parse_object_or_throw(XrValue text) {
    XrValue value = XR_NULL_VAL;
    if (!xrt_json_try_parse(text, &value))
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.parseObject: invalid JSON");
    if (!XR_IS_MAP(value)) {
        xrt_release(value);
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.parseObject: root value must be an object");
    }
    return value;
}

static inline int64_t xrt_json_is_valid(XrValue text) {
    XrValue value = XR_NULL_VAL;
    if (!xrt_json_try_parse(text, &value))
        return 0;
    xrt_release(value);
    return 1;
}

static inline XrValue xrt_object_set_name(XrValue obj, const char *name, XrValue val) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return val;
    xrt_object_t *j = (xrt_object_t *) obj.ptr;
    int64_t idx = xrt_object_find_field(j, name);
    if (idx >= 0) {
        j->fields[idx] = val;
        return val;
    }
    xrt_type_no_index("structural object has no such field");
    return val;
}

static inline XrValue xrt_json_encode_value(XrValue val, int depth);

#define XRT_JSON_ENCODE_MAX_DEPTH 512

static inline void xrt_json_encode_abort(const char *msg, XrValue val) {
    fprintf(stderr, "JSON.value: %s (tag=%u, heap_type=%u)\n", msg ? msg : "unsupported value",
            (unsigned) val.tag, (unsigned) val.heap_type);
    abort();
}

static inline void xrt_json_put_string_key(XrValue obj, XrValue key, XrValue val) {
    if (!XR_IS_MAP(obj) || !XR_IS_STR(key))
        xrt_json_encode_abort("object key must be string", key);
    xrt_map_set((xrt_map_t *) obj.ptr, key, val);
}

static inline XrValue xrt_json_encode_map_key(XrValue key) {
    if (XR_IS_STR(key))
        return xrt_value_to_owned(key);
    xrt_json_encode_abort("Map keys must be string", key);
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_encode_array_value(xrt_array_t *src, int depth) {
    XrValue dst = xrt_array_with_capacity(src ? src->length : 0);
    if (!src)
        return dst;
    for (int64_t i = 0; i < src->length; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xrt_array_push(dst, xrt_json_encode_value(item, depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_object_value(xrt_object_t *src, int depth) {
    XrValue dstv = xrt_map_new(src ? xrt_object_field_count(src) : 0);
    if (!src)
        return dstv;
    for (int64_t i = 0; i < xrt_object_field_count(src); i++) {
        const char *name = xrt_object_field_name(src, i);
        if (name)
            xrt_json_put_string_key(dstv, xr_box_str(name),
                                    xrt_json_encode_value(src->fields[i], depth + 1));
    }
    return dstv;
}

/* A sealed structural object has a compiler-known field order. JSON.encode
 * still returns an independently mutable JSON.Value, but the corresponding
 * Json-domain shape can be emitted once as immutable generated data instead
 * of being rebuilt for every encoded value. */
static inline XrValue xrt_json_encode_static_object(XrValue value, const XrtObjectShape *json_shape,
                                                    int64_t field_count) {
    if (value.tag != XR_TAG_PTR || value.heap_type != 0 || !value.ptr || !json_shape ||
        json_shape->object_domain != XRT_OBJECT_STRUCT || json_shape->field_count != field_count)
        xrt_json_encode_abort("static object metadata is invalid", value);
    xrt_object_t *src = (xrt_object_t *) value.ptr;
    if (xrt_object_field_count(src) != field_count)
        xrt_json_encode_abort("static object shape does not match value", value);
    XrValue dstv = xrt_map_new(field_count);
    for (int64_t i = 0; i < field_count; i++) {
        const char *name = json_shape->fields[i].name;
        xrt_json_put_string_key(dstv, xr_box_str(name ? name : ""),
                                xrt_json_encode_value(src->fields[i], 1));
    }
    return dstv;
}

static inline XrValue xrt_json_encode_map_value(xrt_map_t *src, int depth) {
    XrValue dst = xrt_map_new(src ? xrt_map_len(src) : 0);
    if (!src)
        return dst;
    if (xrt_map_is_boolmap(src))
        xrt_json_encode_abort("Map keys must be string", XR_FROM_BOOL(1));
    if (!xrt_map_is_typed(src)) {
        for (uint32_t i = 0; i < src->nentries; i++) {
            XrMapEntry *entry = &src->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            XrValue key = xrt_json_encode_map_key(entry->key);
            xrt_json_put_string_key(dst, key, xrt_json_encode_value(entry->value, depth + 1));
        }
        return dst;
    }
    for (int64_t oi = 0; oi < src->order_len; oi++) {
        int64_t slot = src->order[oi];
        if (!xrt_map_slot_is_full(src, slot))
            continue;
        XrValue key = xrt_json_encode_map_key(xrt_map_slot_key(src, slot));
        xrt_json_put_string_key(dst, key,
                                xrt_json_encode_value(xrt_map_slot_value(src, slot), depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_set_value(xrt_set_t *src, int depth) {
    XrValue dst = xrt_array_with_capacity(src ? xrt_set_len(src) : 0);
    if (!src)
        return dst;
    if (!xrt_set_is_typed(src)) {
        for (uint32_t i = 0; i < src->nentries; i++) {
            if (src->entries[i].val_tt == XR_SET_ENTRY_NIL)
                continue;
            xrt_array_push(dst, xrt_json_encode_value(src->entries[i].value, depth + 1));
        }
        return dst;
    }
    for (int64_t oi = 0; oi < src->order_len; oi++) {
        int64_t slot = src->order[oi];
        if (xrt_set_slot_is_full(src, slot))
            xrt_array_push(dst, xrt_json_encode_value(xrt_set_slot_item(src, slot), depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_native_struct_field(const void *object,
                                                          const XrJsonClassDecodeFieldSpec *field,
                                                          int depth) {
    if (!object || !field)
        return XR_NULL_VAL;
    if (depth > XRT_JSON_ENCODE_MAX_DEPTH)
        xrt_json_encode_abort("value is too deeply nested", XR_NULL_VAL);
    const XrJsonClassDecodeSpec *nested =
        xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_CLASS_INSTANCE
            ? (const XrJsonClassDecodeSpec *) field->value.target_shape
            : NULL;
    if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
        if (!nested || nested->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT)
            xrt_json_encode_abort("nested value-struct metadata is missing", XR_NULL_VAL);
        const uint8_t *slot = (const uint8_t *) object + field->offset;
        XrValue dst = xrt_map_new(nested->field_count);
        for (uint16_t i = 0; i < nested->field_count; i++) {
            const XrJsonClassDecodeFieldSpec *child = &nested->fields[i];
            XrValue key = xr_box_str(child->name ? child->name : "");
            XrValue item = xrt_json_encode_native_struct_field(slot, child, depth + 1);
            xrt_json_put_string_key(dst, key, item);
        }
        return dst;
    }

    XrtInspectField inspect = {
        field->name, (uint16_t) field->offset, field->native_type, 0, 0, NULL,
    };
    XrValue item = xrt_inspect_field_value(object, &inspect);
    if (xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_ENUM) {
        if (XR_IS_NULL(item) && xr_json_value_kind_is_nullable(field->value.value_kind))
            return item;
        const XrJsonEnumDecodeSpec *enum_spec =
            (const XrJsonEnumDecodeSpec *) field->value.target_shape;
        int64_t ordinal = XR_IS_INT(item) ? XR_TO_INT(item) : -1;
        if (!enum_spec || !enum_spec->member_names || ordinal < 0 ||
            (uint64_t) ordinal >= enum_spec->member_count || !enum_spec->member_names[ordinal])
            xrt_json_encode_abort("enum field has invalid ordinal", item);
        return xr_box_str(enum_spec->member_names[ordinal]);
    }
    return xrt_json_encode_value(item, depth + 1);
}

static inline XrValue xrt_json_encode_native_struct(const void *object,
                                                    const XrJsonClassDecodeSpec *spec) {
    if (!object || !spec || spec->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT ||
        spec->field_count == 0 || !spec->fields)
        xrt_json_encode_abort("value-struct metadata is missing", XR_NULL_VAL);
    XrValue dst = xrt_map_new(spec->field_count);
    for (uint16_t i = 0; i < spec->field_count; i++) {
        const XrJsonClassDecodeFieldSpec *field = &spec->fields[i];
        XrValue key = xr_box_str(field->name ? field->name : "");
        XrValue item = xrt_json_encode_native_struct_field(object, field, 0);
        xrt_json_put_string_key(dst, key, item);
    }
    return dst;
}

static inline const XrtTypeDeriveInfo *xrt_json_instance_derive_info(XrValue val) {
    if (val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        return NULL;
    XrObjHeader *hdr = (XrObjHeader *) val.ptr;
    const XrtTypeInfo *ti = xrt_type_info(xrt_aot_class_type_id(hdr));
    return ti ? xrt_type_derive_info(ti->type_id) : NULL;
}

static inline XrValue xrt_json_encode_inspect_field_value(XrValue owner,
                                                          const XrtInspectField *field) {
    XrValue item = xrt_inspect_field_value(owner.ptr, field);
    if (!field || !field->json_enum_member_names || field->json_enum_member_count == 0)
        return item;
    if (XR_IS_NULL(item) && xr_json_value_kind_is_nullable(field->json_value_kind))
        return item;
    int64_t ordinal = -1;
    if (XR_IS_INT(item)) {
        ordinal = XR_TO_INT(item);
    } else if (item.tag == XR_TAG_ENUM) {
        uint32_t member_index = 0;
        if (xrt_enum_key_parts(item, NULL, NULL, &member_index, NULL))
            ordinal = (int64_t) member_index;
    }
    if (ordinal < 0 || (uint64_t) ordinal >= field->json_enum_member_count)
        xrt_json_encode_abort("enum field has invalid ordinal", owner);
    const char *member_name = field->json_enum_member_names[ordinal];
    if (!member_name)
        xrt_json_encode_abort("enum field metadata is missing", owner);
    return xr_box_str(member_name);
}

static inline XrValue xrt_json_encode_instance_value(XrValue val, int depth) {
    if (val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        xrt_json_encode_abort("cannot encode value to JSON", val);
    const XrtTypeDeriveInfo *di = xrt_json_instance_derive_info(val);
    if (!di || (di->derive_flags & XR_DERIVE_JSON) == 0)
        xrt_json_encode_abort("type does not derive JSON", val);
    if (di->inspect_field_count > 0 && !di->inspect_fields)
        xrt_json_encode_abort("derive Json metadata is missing", val);

    XrValue dst = xrt_map_new(di->inspect_field_count);
    for (uint16_t i = 0; i < di->inspect_field_count; i++) {
        const XrtInspectField *field = &di->inspect_fields[i];
        XrValue key = xr_box_str(field->name ? field->name : "");
        XrValue item = xrt_json_encode_inspect_field_value(val, field);
        xrt_json_put_string_key(dst, key, xrt_json_encode_value(item, depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_value(XrValue val, int depth) {
    if (depth > XRT_JSON_ENCODE_MAX_DEPTH)
        xrt_json_encode_abort("value is too deeply nested", val);
    switch (xrt_value_kind(val)) {
        case XR_TAG_NULL:
        case XR_TAG_BOOL:
        case XR_TAG_I64:
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            /* The encoded tree is an independent owner.  Container insertion
             * consumes this return value, so retaining borrowed source strings
             * prevents releasing the encoded tree from invalidating its input. */
            return xrt_value_to_owned(val);
        case XR_TAG_F64:
            return isfinite(val.f) ? val : XR_NULL_VAL;
        case XR_TAG_RUNE: {
            char buf[4];
            int n = xrt_rune_utf8_encode(XR_TO_RUNE(val), buf);
            if (n <= 0)
                return XR_NULL_VAL;
            XrValue out = xrt_str_alloc((size_t) n);
            memcpy(xr_str_buf(out), buf, (size_t) n);
            return out;
        }
        case XR_TAG_ENUM:
            return xrt_enum_box_name(val);
        case XR_TAG_ARRAY:
            return xrt_json_encode_array_value((xrt_array_t *) val.ptr, depth);
        case XR_TAG_MAP:
            return xrt_json_encode_map_value((xrt_map_t *) val.ptr, depth);
        case XR_TAG_SET:
            return xrt_json_encode_set_value((xrt_set_t *) val.ptr, depth);
        case XR_TAG_PTR:
            if (val.ptr && val.heap_type == XR_TINSTANCE)
                return xrt_json_encode_instance_value(val, depth);
            if (val.ptr && val.heap_type == 0)
                return xrt_json_encode_object_value((xrt_object_t *) val.ptr, depth);
            break;
        default:
            break;
    }
    xrt_json_encode_abort("cannot encode value to JSON", val);
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_encode(XrValue val) {
    return xrt_json_encode_value(val, 0);
}

static inline XrValue xrt_json_merge_with_rest(XrValue parts) {
    if (parts.tag != XR_TAG_PTR || !parts.ptr || parts.heap_type != 0)
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.merge: expected JSON.WithRest<T>");
    xrt_object_t *wrapper = (xrt_object_t *) parts.ptr;
    int64_t rest_index = xrt_object_find_field(wrapper, "rest");
    int64_t value_index = xrt_object_find_field(wrapper, "value");
    if (rest_index < 0 || value_index < 0)
        xrt_throw_error(XR_ERR_JSON_INVALID, "JSON.merge: expected JSON.WithRest<T>");
    XrValue rest_value = wrapper->fields[rest_index];
    XrValue encoded = xrt_json_encode(wrapper->fields[value_index]);
    if (!XR_IS_MAP(rest_value) || !XR_IS_MAP(encoded)) {
        xrt_release(encoded);
        xrt_throw_error(XR_ERR_JSON_INVALID,
                        "JSON.merge: typed value and rest must encode as objects");
    }
    xrt_map_t *dst = (xrt_map_t *) encoded.ptr;
    xrt_map_t *rest = (xrt_map_t *) rest_value.ptr;
    int64_t slots = xrt_map_is_typed(rest) ? rest->order_len : (int64_t) rest->nentries;
    for (int64_t slot_index = 0; slot_index < slots; slot_index++) {
        int64_t slot = xrt_map_is_typed(rest) ? rest->order[slot_index] : slot_index;
        if (!xrt_map_slot_is_full(rest, slot))
            continue;
        XrValue key = xrt_map_slot_key(rest, slot);
        if (!XR_IS_STR(key) || xrt_map_has(dst, key)) {
            xrt_release(encoded);
            xrt_throw_error(XR_ERR_JSON_INVALID,
                            XR_IS_STR(key)
                                ? "JSON.merge: rest conflicts with a declared top-level field"
                                : "JSON.merge: rest contains a non-string key");
        }
        xrt_map_set(dst, xrt_value_to_owned(key),
                    xrt_value_to_owned(xrt_map_slot_value(rest, slot)));
    }
    return encoded;
}

static void xrt_json_stringify_value(xrt_strbuf_t *sb, XrValue val, int depth);

#define XRT_JSON_STRINGIFY_MAX_DEPTH 512

static inline void xrt_json_sb_puts(xrt_strbuf_t *sb, const char *s, size_t n) {
    xrt_strbuf_grow(sb, (int64_t) n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += (int64_t) n;
    sb->buf[sb->len] = 0;
}

static inline void xrt_json_sb_cstr(xrt_strbuf_t *sb, const char *s) {
    xrt_json_sb_puts(sb, s, strlen(s));
}

static inline void xrt_json_sb_char(xrt_strbuf_t *sb, char c) {
    xrt_json_sb_puts(sb, &c, 1);
}

static inline void xrt_json_stringify_abort(const char *msg, XrValue val) {
    fprintf(stderr, "JSON.stringify: %s (tag=%u, heap_type=%u)\n", msg ? msg : "unsupported value",
            (unsigned) val.tag, (unsigned) val.heap_type);
    abort();
}

static void xrt_json_stringify_string(xrt_strbuf_t *sb, const char *s, size_t len) {
    xrt_json_sb_char(sb, '"');
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len) {
            unsigned char c = (unsigned char) s[i];
            if (c < 32 || c == '"' || c == '\\')
                break;
            i++;
        }
        if (i > start)
            xrt_json_sb_puts(sb, s + start, i - start);
        if (i >= len)
            break;
        unsigned char c = (unsigned char) s[i];
        switch (c) {
            case '"':
                xrt_json_sb_cstr(sb, "\\\"");
                break;
            case '\\':
                xrt_json_sb_cstr(sb, "\\\\");
                break;
            case '\n':
                xrt_json_sb_cstr(sb, "\\n");
                break;
            case '\r':
                xrt_json_sb_cstr(sb, "\\r");
                break;
            case '\t':
                xrt_json_sb_cstr(sb, "\\t");
                break;
            case '\b':
                xrt_json_sb_cstr(sb, "\\b");
                break;
            case '\f':
                xrt_json_sb_cstr(sb, "\\f");
                break;
            default: {
                char buf[8];
                int n = snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) c);
                xrt_json_sb_puts(sb, buf, (size_t) n);
                break;
            }
        }
        i++;
    }
    xrt_json_sb_char(sb, '"');
}

static void xrt_json_stringify_native_struct_fields(xrt_strbuf_t *sb, const void *object,
                                                    const XrJsonClassDecodeSpec *spec, int depth) {
    if (!object || !spec || spec->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT ||
        spec->field_count == 0 || !spec->fields)
        xrt_json_stringify_abort("value-struct metadata is missing", XR_NULL_VAL);
    if (depth > XRT_JSON_STRINGIFY_MAX_DEPTH)
        xrt_json_stringify_abort("value is too deeply nested", XR_NULL_VAL);

    xrt_json_sb_char(sb, '{');
    for (uint16_t i = 0; i < spec->field_count; i++) {
        const XrJsonClassDecodeFieldSpec *field = &spec->fields[i];
        if (i > 0)
            xrt_json_sb_char(sb, ',');
        xrt_json_stringify_string(sb, field->name ? field->name : "",
                                  strlen(field->name ? field->name : ""));
        xrt_json_sb_char(sb, ':');

        const XrJsonClassDecodeSpec *nested =
            xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_CLASS_INSTANCE
                ? (const XrJsonClassDecodeSpec *) field->value.target_shape
                : NULL;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!nested || nested->target_kind != XR_JSON_NOMINAL_TARGET_VALUE_STRUCT)
                xrt_json_stringify_abort("nested value-struct metadata is missing", XR_NULL_VAL);
            xrt_json_stringify_native_struct_fields(sb, (const uint8_t *) object + field->offset,
                                                    nested, depth + 1);
            continue;
        }

        XrtInspectField inspect = {
            field->name, (uint16_t) field->offset, field->native_type, 0, 0, NULL,
        };
        XrValue item = xrt_inspect_field_value(object, &inspect);
        if (xr_json_value_kind_base(field->value.value_kind) == XR_JSON_VALUE_ENUM) {
            if (XR_IS_NULL(item) && xr_json_value_kind_is_nullable(field->value.value_kind)) {
                xrt_json_sb_cstr(sb, "null");
                continue;
            }
            const XrJsonEnumDecodeSpec *enum_spec =
                (const XrJsonEnumDecodeSpec *) field->value.target_shape;
            int64_t ordinal = XR_IS_INT(item) ? XR_TO_INT(item) : -1;
            if (!enum_spec || !enum_spec->member_names || ordinal < 0 ||
                (uint64_t) ordinal >= enum_spec->member_count || !enum_spec->member_names[ordinal])
                xrt_json_stringify_abort("enum field has invalid ordinal", item);
            xrt_json_stringify_string(sb, enum_spec->member_names[ordinal],
                                      strlen(enum_spec->member_names[ordinal]));
            continue;
        }
        xrt_json_stringify_value(sb, item, depth + 1);
    }
    xrt_json_sb_char(sb, '}');
}

static void xrt_json_stringify_instance_fields(xrt_strbuf_t *sb, XrValue owner,
                                               const XrtTypeDeriveInfo *di, int depth) {
    if (!di || (di->derive_flags & XR_DERIVE_JSON) == 0 ||
        (di->inspect_field_count > 0 && !di->inspect_fields))
        xrt_json_stringify_abort("derive Json metadata is missing", owner);
    if (depth > XRT_JSON_STRINGIFY_MAX_DEPTH)
        xrt_json_stringify_abort("value is too deeply nested", owner);

    xrt_json_sb_char(sb, '{');
    for (uint16_t i = 0; i < di->inspect_field_count; i++) {
        const XrtInspectField *field = &di->inspect_fields[i];
        if (i > 0)
            xrt_json_sb_char(sb, ',');
        xrt_json_stringify_string(sb, field->name ? field->name : "",
                                  strlen(field->name ? field->name : ""));
        xrt_json_sb_char(sb, ':');

        XrValue item = xrt_inspect_field_value(owner.ptr, field);
        if (field->json_enum_member_names && field->json_enum_member_count > 0) {
            if (XR_IS_NULL(item) && xr_json_value_kind_is_nullable(field->json_value_kind)) {
                xrt_json_sb_cstr(sb, "null");
                continue;
            }
            int64_t ordinal = XR_IS_INT(item) ? XR_TO_INT(item) : -1;
            if (ordinal < 0 || (uint64_t) ordinal >= field->json_enum_member_count ||
                !field->json_enum_member_names[ordinal])
                xrt_json_stringify_abort("enum field has invalid ordinal", owner);
            xrt_json_stringify_string(sb, field->json_enum_member_names[ordinal],
                                      strlen(field->json_enum_member_names[ordinal]));
            continue;
        }
        xrt_json_stringify_value(sb, item, depth + 1);
    }
    xrt_json_sb_char(sb, '}');
}

static void xrt_json_stringify_array(xrt_strbuf_t *sb, xrt_array_t *arr, int depth) {
    xrt_json_sb_char(sb, '[');
    int64_t len = arr ? arr->length : 0;
    for (int64_t i = 0; i < len; i++) {
        if (i > 0)
            xrt_json_sb_char(sb, ',');
        xrt_json_stringify_value(sb, xr_typed_get(arr->data, (int32_t) i, arr->elem_type),
                                 depth + 1);
    }
    xrt_json_sb_char(sb, ']');
}

static void xrt_json_stringify_map_key(xrt_strbuf_t *sb, XrValue key) {
    if (XR_IS_STR(key)) {
        xrt_json_stringify_string(sb, xr_str_data(key), (size_t) xr_str_len(key));
        return;
    }
    if (key.tag == XR_TAG_I64) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lld", (long long) key.i);
        xrt_json_stringify_string(sb, buf, (size_t) n);
        return;
    }
    xrt_json_stringify_abort("object key must be string or int", key);
}

static void xrt_json_stringify_map(xrt_strbuf_t *sb, xrt_map_t *m, int depth) {
    xrt_json_sb_char(sb, '{');
    int64_t emitted = 0;
    int64_t n_slots = !m ? 0 : (xrt_map_is_typed(m) ? m->order_len : (int64_t) m->nentries);
    for (int64_t i = 0; i < n_slots; i++) {
        int64_t slot = xrt_map_is_typed(m) ? m->order[i] : i;
        if (!xrt_map_slot_is_full(m, slot))
            continue;
        if (emitted > 0)
            xrt_json_sb_char(sb, ',');
        xrt_json_stringify_map_key(sb, xrt_map_slot_key(m, slot));
        xrt_json_sb_char(sb, ':');
        xrt_json_stringify_value(sb, xrt_map_slot_value(m, slot), depth + 1);
        emitted++;
    }
    xrt_json_sb_char(sb, '}');
}

static void xrt_json_stringify_object_fields(xrt_strbuf_t *sb, xrt_object_t *j, int depth) {
    xrt_json_sb_char(sb, '{');
    int64_t emitted = 0;
    if (j) {
        for (int64_t i = 0; i < xrt_object_field_count(j); i++) {
            if (emitted > 0)
                xrt_json_sb_char(sb, ',');
            const char *name = xrt_object_field_name(j, i);
            if (!name)
                name = "";
            xrt_json_stringify_string(sb, name, strlen(name));
            xrt_json_sb_char(sb, ':');
            xrt_json_stringify_value(sb, j->fields[i], depth + 1);
            emitted++;
        }
    }
    xrt_json_sb_char(sb, '}');
}

static void xrt_json_stringify_value(xrt_strbuf_t *sb, XrValue val, int depth) {
    if (depth > XRT_JSON_STRINGIFY_MAX_DEPTH)
        xrt_json_stringify_abort("value is too deeply nested", val);
    switch (xrt_value_kind(val)) {
        case XR_TAG_NULL:
            xrt_json_sb_cstr(sb, "null");
            return;
        case XR_TAG_BOOL:
            xrt_json_sb_cstr(sb, val.i ? "true" : "false");
            return;
        case XR_TAG_I64: {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%lld", (long long) val.i);
            xrt_json_sb_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_F64: {
            if (!isfinite(val.f)) {
                xrt_json_sb_cstr(sb, "null");
                return;
            }
            char buf[64];
            int n = xr_format_float(buf, sizeof(buf), val.f);
            xrt_json_sb_puts(sb, buf, (size_t) n);
            return;
        }
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            xrt_json_stringify_string(sb, xr_str_data(val), (size_t) xr_str_len(val));
            return;
        case XR_TAG_RUNE: {
            char buf[4];
            int n = xrt_rune_utf8_encode(XR_TO_RUNE(val), buf);
            if (n > 0)
                xrt_json_stringify_string(sb, buf, (size_t) n);
            else
                xrt_json_sb_cstr(sb, "null");
            return;
        }
        case XR_TAG_ARRAY:
            xrt_json_stringify_array(sb, (xrt_array_t *) val.ptr, depth);
            return;
        case XR_TAG_MAP:
            xrt_json_stringify_map(sb, (xrt_map_t *) val.ptr, depth);
            return;
        case XR_TAG_ENUM: {
            const char *member_name = NULL;
            if (xrt_enum_key_parts(val, NULL, &member_name, NULL, NULL) && member_name)
                xrt_json_stringify_string(sb, member_name, strlen(member_name));
            else
                xrt_json_sb_cstr(sb, "null");
            return;
        }
        case XR_TAG_PTR:
            if (val.ptr && val.heap_type == 0) {
                xrt_json_stringify_object_fields(sb, (xrt_object_t *) val.ptr, depth);
                return;
            }
            if (val.ptr && val.heap_type == XR_TINSTANCE) {
                const XrtTypeDeriveInfo *di = xrt_json_instance_derive_info(val);
                if (di && (di->derive_flags & XR_DERIVE_JSON) != 0) {
                    xrt_json_stringify_instance_fields(sb, val, di, depth);
                    return;
                }
            }
            break;
        default:
            break;
    }
    xrt_json_stringify_abort("cannot serialize value to JSON", val);
}

static inline XrValue xrt_json_stringify(XrValue val) {
    XrValue sbv = xrt_strbuf_new();
    xrt_json_stringify_value((xrt_strbuf_t *) sbv.ptr, val, 0);
    return xrt_strbuf_finish(sbv);
}

static inline XrValue xrt_json_stringify_native_struct(const void *object,
                                                       const XrJsonClassDecodeSpec *spec) {
    XrValue sbv = xrt_strbuf_new();
    xrt_json_stringify_native_struct_fields((xrt_strbuf_t *) sbv.ptr, object, spec, 0);
    return xrt_strbuf_finish(sbv);
}

/* JSON namespace calls follow the ordinary Xi bodyless-call convention: every
 * explicit reference argument is transferred to the callee.  The core JSON
 * helpers above intentionally remain borrowing operations because they are
 * also composed internally.  AOT codegen targets only these adapters so its
 * specialized fast path has the same ownership contract as VM primitive
 * dispatch. */
static inline XrValue xrt_json_encode_consume(XrValue value) {
    XrValue out = xrt_json_encode(value);
    xrt_release(value);
    return out;
}

static inline XrValue xrt_json_encode_static_object_consume(XrValue value,
                                                            const XrtObjectShape *json_shape,
                                                            int64_t field_count) {
    XrValue out = xrt_json_encode_static_object(value, json_shape, field_count);
    xrt_release(value);
    return out;
}

static inline XrValue
xrt_json_encode_native_struct_boxed_consume(XrValue value, const XrJsonClassDecodeSpec *spec) {
    XrValue out = xrt_json_encode_native_struct(value.ptr, spec);
    xrt_release(value);
    return out;
}

static inline XrValue xrt_json_merge_with_rest_consume(XrValue parts) {
    XrValue out = xrt_json_merge_with_rest(parts);
    xrt_release(parts);
    return out;
}

static inline XrValue xrt_json_parse_or_throw_consume(XrValue text) {
    XrValue out = xrt_json_parse_or_throw(text);
    xrt_release(text);
    return out;
}

static inline XrValue xrt_json_parse_object_or_throw_consume(XrValue text) {
    XrValue out = xrt_json_parse_object_or_throw(text);
    xrt_release(text);
    return out;
}

static inline XrValue xrt_json_static_kind_of_consume(XrValue value) {
    XrValue out = xrt_json_static_kind_of(value);
    xrt_release(value);
    return out;
}

static inline XrValue xrt_json_static_is_kind_consume(XrValue value, int expected_kind) {
    XrValue out = xrt_json_static_is_kind(value, expected_kind);
    xrt_release(value);
    return out;
}

static inline XrValue xrt_json_as_kind_consume(XrValue value, int expected_kind) {
    if (xrt_json_runtime_kind(value) == expected_kind)
        return value;
    xrt_release(value);
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_path_get_consume(XrValue root, XrValue path) {
    XrValue out = xrt_json_path_get(root, path);
    xrt_release(root);
    xrt_release(path);
    return out;
}

static inline XrValue xrt_json_path_require_consume(XrValue root, XrValue path) {
    XrValue out = xrt_json_path_require(root, path);
    xrt_release(root);
    xrt_release(path);
    return out;
}

static inline int64_t xrt_json_path_contains_consume(XrValue root, XrValue path) {
    int64_t out = xrt_json_path_contains(root, path);
    xrt_release(root);
    xrt_release(path);
    return out;
}

static inline XrValue xrt_json_path_set_consume(XrValue root, XrValue path, XrValue value,
                                                int create_parents) {
    XrValue out = xrt_json_path_set(root, path, value, create_parents);
    xrt_release(root);
    xrt_release(path);
    xrt_release(value);
    return out;
}

static inline XrValue xrt_json_path_remove_consume(XrValue root, XrValue path) {
    XrValue out = xrt_json_path_remove(root, path);
    xrt_release(root);
    xrt_release(path);
    return out;
}

static inline int64_t xrt_json_is_valid_consume(XrValue text) {
    int64_t out = xrt_json_is_valid(text);
    xrt_release(text);
    return out;
}

static inline int64_t xrt_json_static_has_consume(XrValue object, XrValue key) {
    int64_t out = xrt_json_static_has(object, key);
    xrt_release(object);
    xrt_release(key);
    return out;
}

static inline XrValue xrt_json_stringify_consume(XrValue value) {
    XrValue out = xrt_json_stringify(value);
    xrt_release(value);
    return out;
}

static inline XrValue
xrt_json_stringify_native_struct_boxed_consume(XrValue value, const XrJsonClassDecodeSpec *spec) {
    XrValue out = xrt_json_stringify_native_struct(value.ptr, spec);
    xrt_release(value);
    return out;
}

static inline int64_t xrt_json_static_size_consume(XrValue object) {
    int64_t out = xrt_json_static_size(object);
    xrt_release(object);
    return out;
}

static inline int64_t xrt_json_static_is_empty_consume(XrValue object) {
    int64_t out = xrt_json_static_is_empty(object);
    xrt_release(object);
    return out;
}

static inline XrValue xrt_json_decode_typed_value_consume(XrValue source,
                                                          const XrJsonDecodeFieldSpec *spec,
                                                          int ignore_unknown_fields) {
    XrValue out = xrt_json_decode_typed_value(source, spec, ignore_unknown_fields);
    xrt_release(source);
    return out;
}

static inline XrValue
xrt_json_decode_typed_value_or_throw_consume(XrValue source, const XrJsonDecodeFieldSpec *spec,
                                             int ignore_unknown_fields) {
    XrValue out = xrt_json_decode_typed_value_or_throw(source, spec, ignore_unknown_fields);
    xrt_release(source);
    return out;
}

static inline XrValue xrt_json_decode_struct_object_consume(XrValue data,
                                                            const XrtObjectShape *target_shape,
                                                            int64_t field_count,
                                                            const XrJsonDecodeFieldSpec *fields,
                                                            int ignore_unknown_fields) {
    XrValue out = xrt_json_decode_struct_object(data, target_shape, field_count, fields,
                                                ignore_unknown_fields);
    xrt_release(data);
    return out;
}

static inline XrValue xrt_json_parse_typed_value_or_throw_consume(XrValue text,
                                                                  const XrJsonDecodeFieldSpec *spec,
                                                                  int ignore_unknown_fields) {
    XrValue out = xrt_json_parse_typed_value_or_throw(text, spec, ignore_unknown_fields);
    xrt_release(text);
    return out;
}

static inline XrValue xrt_json_parse_typed_object_or_throw_consume(
    XrValue text, const XrtObjectShape *target_shape, int64_t field_count,
    const XrJsonDecodeFieldSpec *fields, int ignore_unknown_fields) {
    XrValue out = xrt_json_parse_typed_object_or_throw(text, target_shape, field_count, fields,
                                                       ignore_unknown_fields);
    xrt_release(text);
    return out;
}

static inline void xrt_json_parse_typed_native_or_throw_consume(XrValue text,
                                                                const XrJsonClassDecodeSpec *spec,
                                                                void *target,
                                                                int ignore_unknown_fields) {
    xrt_json_parse_typed_native_or_throw(text, spec, target, ignore_unknown_fields);
    xrt_release(text);
}

static inline XrValue xrt_json_parse_with_rest_object_or_throw_consume(
    XrValue text, const XrtObjectShape *wrapper_shape, const XrtObjectShape *target_shape,
    int64_t field_count, const XrJsonDecodeFieldSpec *fields, int ignore_nested_unknown_fields) {
    XrValue out = xrt_json_parse_with_rest_object_or_throw(
        text, wrapper_shape, target_shape, field_count, fields, ignore_nested_unknown_fields);
    xrt_release(text);
    return out;
}

static inline XrValue
xrt_json_parse_with_rest_class_or_throw_consume(XrValue text, const XrtObjectShape *wrapper_shape,
                                                const XrJsonClassDecodeSpec *target_spec,
                                                int ignore_nested_unknown_fields) {
    XrValue out = xrt_json_parse_with_rest_class_or_throw(text, wrapper_shape, target_spec,
                                                          ignore_nested_unknown_fields);
    xrt_release(text);
    return out;
}

static inline XrValue xrt_getprop_key(XrValue obj, XrValue key) {
    if (!XR_IS_STR(key))
        return XR_NULL_VAL;
    const char *name = xr_str_data(key);
    if (obj.tag == XR_TAG_ENUM) {
        if (strcmp(name, "name") == 0)
            return xrt_enum_box_name(obj);
        if (strcmp(name, "ordinal") == 0)
            return xrt_enum_box_ordinal(obj);
    }
    if (XR_IS_MAP(obj))
        return xrt_map_get_owned((xrt_map_t *) obj.ptr, key);
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_object_get_name_owned(obj, name);
    return XR_NULL_VAL;
}

/* Name-keyed property store. Handles the two shapes that actually carry named
 * properties at run time: a Map, and a JSON/structural object.
 *
 * Fails closed on anything else. A store that cannot be performed has no
 * defensible "skip it" reading — the value the program assigned simply would
 * not be there — so returning `val` as if the write had happened turned a
 * codegen gap into a wrong answer with no crash and no diagnostic. That is how
 * `defer { this.n = ... }` silently did nothing in AOT builds for a
 * native-layout class instance: CGen fell out of its native field path, landed
 * here, and this function discarded the write.
 *
 * A null receiver reports the ordinary null-property error rather than the
 * shape error, so `x.f = v` on a null `x` reads the way the language spec
 * describes it. */
static inline XrValue xrt_setprop_key(XrValue obj, XrValue key, XrValue val) {
    if (!XR_IS_STR(key))
        xrt_throw_error(XR_ERR_INVALID_ARG_TYPE,
                        "property name must be a string in a name-keyed store");
    if (XR_IS_MAP(obj)) {
        xrt_map_set((xrt_map_t *) obj.ptr, key, val);
        return val;
    }
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_object_set_name(obj, xr_str_data(key), val);
    if (XR_IS_NULL(obj))
        xrt_throw_error(XR_ERR_NULL_PROPERTY, "property store on a null value");
    xrt_throw_error(XR_ERR_TYPE_NO_OPERATOR, "value has no name-keyed properties to store into");
    return val;
}

/* C/runtime callers may only have a NUL-terminated name.  Reads can borrow a
 * stack literal header for the duration of lookup.  Writes allocate an ARC
 * key because a newly inserted map entry takes ownership of it.  Generated
 * AOT code uses the key-valued forms above with module-static literal headers,
 * avoiding per-field-access allocation entirely. */
static inline XrValue xrt_getprop_name(XrValue obj, const char *name) {
    if (!name)
        return XR_NULL_VAL;
    xrt_str_t key_header = {
        .len = (int64_t) strlen(name),
        .rune_len = -1,
        .hash = 0,
        .flags = XRT_STR_LITERAL,
        .data = (char *) name,
    };
    return xrt_getprop_key(obj, xr_str_lit(&key_header));
}

static inline XrValue xrt_setprop_name(XrValue obj, const char *name, XrValue val) {
    if (!name)
        return val;
    if (XR_IS_MAP(obj))
        return xrt_setprop_key(obj, xr_box_str(name), val);
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_object_set_name(obj, name, val);
    return val;
}

static inline XrValue xrt_object_clone_for_coro(XrValue val) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return val;
    xrt_object_t *src = (xrt_object_t *) val.ptr;
    XrValue dstv = xrt_object_new_like(src);
    xrt_object_t *dst = (xrt_object_t *) dstv.ptr;
    for (int64_t i = 0; i < xrt_object_field_count(src); i++)
        dst->fields[i] = xrt_value_clone_for_coro(src->fields[i]);
    return dstv;
}

static inline void xrt_object_merge_copy_table(XrValue dst_val, XrValue src_val,
                                               int64_t copy_pair_count,
                                               const uint16_t *dst_src_ordinals) {
    if (copy_pair_count <= 0 || !dst_src_ordinals)
        return;
    if (dst_val.tag != XR_TAG_PTR || !dst_val.ptr)
        return;
    if (src_val.tag != XR_TAG_PTR || !src_val.ptr)
        return;
    xrt_object_t *dst = (xrt_object_t *) dst_val.ptr;
    xrt_object_t *src = (xrt_object_t *) src_val.ptr;
    if (xrt_object_domain(dst) != XRT_OBJECT_STRUCT || xrt_object_domain(src) != XRT_OBJECT_STRUCT)
        return;
    for (int64_t i = 0; i < copy_pair_count; i++) {
        uint16_t dst_idx = dst_src_ordinals[i * 2];
        uint16_t src_idx = dst_src_ordinals[i * 2 + 1];
        if ((int64_t) dst_idx < xrt_object_field_count(dst) &&
            (int64_t) src_idx < xrt_object_field_count(src))
            dst->fields[dst_idx] = src->fields[src_idx];
    }
}

static inline void xrt_struct_object_destroy(xrt_object_t *j) {
    if (!j)
        return;
    for (int64_t i = 0; i < xrt_object_field_count(j); i++)
        xrt_release(j->fields[i]);
    if (j->shape && j->shape->storage == XR_OBJECT_SHAPE_OWNED)
        XRT_FREE((void *) j->shape);
    j->shape = NULL;
}

static void xrt_execution_finalize_array(XrObjHeader *hdr) {
    xrt_array_destroy((xrt_array_t *) hdr);
}

static void xrt_execution_finalize_map(XrObjHeader *hdr) {
    xrt_map_destroy((xrt_map_t *) hdr);
}

static void xrt_execution_finalize_boolmap(XrObjHeader *hdr) {
    xrt_boolmap_destroy((xrt_boolmap_t *) hdr);
}

static void xrt_execution_finalize_set(XrObjHeader *hdr) {
    xrt_set_destroy((xrt_set_t *) hdr);
}

static void xrt_execution_finalize_struct_object(XrObjHeader *hdr) {
    xrt_struct_object_destroy((xrt_object_t *) hdr);
    xrt_execution_free_allocation(hdr);
}

static inline void xrt_coll_retain(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!h || (h->extra & (XR_OBJ_IMMORTAL | XR_OBJ_STORAGE_STACK | XR_OBJ_AOT_SWEEP)))
        return;
    if (XR_OBJ_IS_SHARED(h))
        atomic_fetch_sub_explicit(&h->refcount, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&h->refcount, 1, memory_order_relaxed);
}

static inline void xrt_coll_release(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!h || (h->extra & (XR_OBJ_IMMORTAL | XR_OBJ_STORAGE_STACK | XR_OBJ_AOT_SWEEP)))
        return;
    if (XR_OBJ_IS_SHARED(h)) {
        int32_t old = atomic_fetch_add_explicit(&h->refcount, 1, memory_order_acq_rel);
        if (old != -1)
            return;
    } else if (!xrt_rc_claim_release_last(h)) {
        return;
    }
    if (v.tag == XR_TAG_PTR && v.heap_type == 0 && (v.flags & XR_VALUE_FLAG_HEADER_AT_PTR) != 0) {
        xrt_struct_object_destroy((xrt_object_t *) v.ptr);
        xrt_execution_free_allocation(h);
        return;
    }
    switch (h->type) {
        case XR_TARRAY:
            xrt_array_destroy((xrt_array_t *) v.ptr);
            break;
        case XR_TMAP:
            xrt_map_destroy((xrt_map_t *) v.ptr);
            break;
        case XR_TBOOLMAP:
            xrt_boolmap_destroy((xrt_boolmap_t *) v.ptr);
            break;
        case XR_TSET:
            xrt_set_destroy((xrt_set_t *) v.ptr);
            break;
        default:
            break;
    }
}

#include "xrt_index_helpers.inc.c"

/* =========================================================================
 * Closure runtime
 * ========================================================================= */

struct xrt_closure {
    const XrAotCallableDesc *callable;  // canonical generated descriptor
    int nupvals;                        // number of captured upvalues
    XrValue upvals[];                   // captured values (flexible array)
};

static inline size_t xrt_closure_object_size(int nupvals) {
    if (nupvals < 0)
        nupvals = 0;
    if ((size_t) nupvals > (SIZE_MAX - sizeof(xrt_closure_t)) / sizeof(XrValue)) {
        fprintf(stderr, "xrt_closure_new: allocation size overflow\n");
        abort();
    }
    return sizeof(xrt_closure_t) + (size_t) nupvals * sizeof(XrValue);
}

static inline void xrt_closure_init(xrt_closure_t *c, const XrAotCallableDesc *callable,
                                    int nupvals) {
    if (nupvals < 0)
        nupvals = 0;
    c->callable = callable;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = XR_NULL_VAL;
}

static inline XrValue xrt_closure_new(const XrAotCallableDesc *callable, int nupvals) {
    xrt_closure_t *c = (xrt_closure_t *) xrt_arc_alloc(xrt_closure_object_size(nupvals));
    if (XR_UNLIKELY(!c)) {
        fprintf(stderr, "xrt_closure_new: out of memory\n");
        abort();
    }
    xrt_arc_mark_builtin(c, XRT_ARC_KIND_CLOSURE);
    xrt_closure_init(c, callable, nupvals);
    return xr_mkptr(c, XR_TAG_CLOSURE);
}

static inline XrValue xrt_closure_call0(XrValue callback) {
    if (callback.tag != XR_TAG_CLOSURE || !callback.ptr)
        return XR_NULL_VAL;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    typedef XrValue (*xrt_closure_fn0_t)(xrt_closure_t *);
    return ((xrt_closure_fn0_t) cl->callable->sync_entry)(cl);
}

#ifndef xrt_closure_stack_new
#define xrt_closure_stack_new(callable_expr, nupvals_expr)                                         \
    ({                                                                                             \
        int _nupvals = (nupvals_expr);                                                             \
        if (_nupvals < 0)                                                                          \
            _nupvals = 0;                                                                          \
        size_t _obj_size = xrt_closure_object_size(_nupvals);                                      \
        XrObjHeader *_hdr = (XrObjHeader *) __builtin_alloca(sizeof(XrObjHeader) + _obj_size);     \
        memset(_hdr, 0, sizeof(XrObjHeader) + _obj_size);                                          \
        _hdr->extra = XR_OBJ_STORAGE_STACK;                                                        \
        xrt_closure_t *_c = (xrt_closure_t *) ((char *) _hdr + sizeof(XrObjHeader));               \
        xrt_closure_init(_c, (callable_expr), _nupvals);                                           \
        xr_mkptr(_c, XR_TAG_CLOSURE);                                                              \
    })
#endif

/* Release what a stack closure captured, at the end of its scope.
 *
 * A stack closure's block lives in the frame, so refcounting does not govern
 * it: xrt_retain and xrt_rc_claim_release_last both no-op on
 * XR_OBJ_STORAGE_STACK, and xrt_release therefore returns before reaching
 * xrt_finalize_one. Nothing then released the upvals, so every captured cell
 * leaked once per call (2M iterations => 156 MiB of residue).
 *
 * The upvals still need releasing even though the block does not need freeing,
 * so codegen emits this where it would emit xrt_release for a heap closure --
 * it knows which allocation it produced. Idempotent by clearing as it goes: a
 * value that ARC released on two paths, or a frame reached twice, must not
 * release a captured reference twice. */
static inline void xrt_closure_stack_drop(XrValue v) {
    if (v.tag != XR_TAG_CLOSURE || !v.ptr)
        return;
    xrt_closure_t *c = (xrt_closure_t *) v.ptr;
    XrObjHeader *hdr = (XrObjHeader *) ((char *) c - sizeof(XrObjHeader));
    if (!(hdr->extra & XR_OBJ_STORAGE_STACK))
        return; /* heap closure: its own refcount owns the upvals */
    int n = c->nupvals;
    c->nupvals = 0;
    for (int i = 0; i < n; i++) {
        XrValue up = c->upvals[i];
        c->upvals[i] = XR_NULL_VAL;
        xrt_release(up);
    }
}

/* Post-header field set shared with the VM's XrCell (src/shared/xr_cell_abi.h)
 * so the AOT and VM cell layouts stay in lockstep. */
typedef struct xrt_cell {
    XR_CELL_ABI_FIELDS;
} xrt_cell_t;

#ifdef XRT_ENABLE_REGEX
static inline void xrt_regex_destroy_builtin(void *obj);
#endif

static inline XrValue xrt_cell_new(XrValue value) {
    xrt_cell_t *cell = (xrt_cell_t *) xrt_arc_alloc(sizeof(xrt_cell_t));
    if (XR_UNLIKELY(!cell)) {
        fprintf(stderr, "xrt_cell_new: out of memory\n");
        abort();
    }
    xrt_arc_mark_builtin(cell, XRT_ARC_KIND_CELL);
    cell->value = value;
    return xr_mkptr(cell, XR_TAG_CELL);
}

static inline XrValue xrt_cell_access_get(XrValue cell_value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return cell_value;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    return XR_CELL_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI, XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, xr_cell_access_load_core(&cell->value));
}

static inline void xrt_cell_access_set(XrValue cell_value, XrValue value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    XrValue old = XR_CELL_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI, XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, xr_cell_access_replace_core(&cell->value, value));
    xrt_release(old);
}

static inline void xrt_iterator_destroy_builtin(void *obj) {
    xrt_iterator_t *it = (xrt_iterator_t *) obj;
    if (!it)
        return;
    if (it->kind == XRT_ITER_GENERATOR) {
#ifdef XRT_ENABLE_GENERATORS
        xrt_gen_iter_destroy(it);
        return;
#endif
    }
    XrValue source = it->coll;
    it->coll = XR_NULL_VAL;
    xrt_release(source);
}

static inline void xrt_enum_box_destroy_builtin(void *obj) {
    XrAotEnumBox *box = (XrAotEnumBox *) obj;
    if (!box)
        return;
    for (uint32_t i = 0; i < box->payload_count; i++) {
        xrt_release(box->payloads[i]);
        box->payloads[i] = XR_NULL_VAL;
    }
}

static inline void xrt_dispatch_builtin_destructor(uint32_t kind, void *obj) {
    if (!obj)
        return;
    switch (kind) {
        case XRT_ARC_KIND_CLOSURE: {
            xrt_closure_t *c = (xrt_closure_t *) obj;
            for (int i = 0; i < c->nupvals; i++)
                xrt_release(c->upvals[i]);
            break;
        }
        case XRT_ARC_KIND_CELL:
            xrt_release(((xrt_cell_t *) obj)->value);
            break;
        case XRT_ARC_KIND_OBJECT:
            xrt_struct_object_destroy((xrt_object_t *) ((char *) obj - sizeof(XrObjHeader)));
            break;
        case XRT_ARC_KIND_STRBUF:
            xrt_strbuf_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_ITERATOR:
            xrt_iterator_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_TUPLE: {
            xrt_tuple_t *tuple = (xrt_tuple_t *) obj;
            for (int64_t i = 0; i < tuple->len; i++)
                xrt_release(tuple->items[i]);
            break;
        }
        case XRT_ARC_KIND_ENUM_BOX:
            xrt_enum_box_destroy_builtin(obj);
            break;
#ifdef XRT_ENABLE_REGEX
        case XRT_ARC_KIND_REGEX:
            xrt_regex_destroy_builtin(obj);
            break;
#endif
        case XRT_ARC_KIND_SYS_MUTEX:
            xrt_sys_mutex_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_SYS_RWLOCK:
            xrt_sys_rwlock_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_SYS_CONDVAR:
            xrt_sys_condvar_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_SYS_BARRIER:
            xrt_sys_barrier_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_SYS_ONCE:
            xrt_sys_once_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_BUFFER:
            xrt_buffer_destroy_builtin(obj);
            break;
        case XRT_ARC_KIND_NET_CONN:
        case XRT_ARC_KIND_NET_LISTENER:
            xrt_net_destroy_builtin(obj);
            break;
#ifdef XRT_ENABLE_SYS_THREAD
        /* Guarded like regex: xrt_thread_destroy_builtin calls the extern
         * xr_thread_detach, which only links when the coro runtime archive is
         * present. Thread handles can only exist when the program spawns
         * threads, which also sets this define (xaot_driver.c). */
        case XRT_ARC_KIND_THREAD:
            xrt_thread_destroy_builtin(obj);
            break;
#endif
        default:
            break;
    }
}

/* Publish an entire owned graph, not just its root. Clearing AOT_EXECUTION on
 * the root before visiting children is the cycle-safe visited mark: a back-edge
 * observes the requested storage mode and stops. This is what lets a value
 * survive its producer coroutine without turning the producer's whole arena
 * into process-lifetime storage. */
static inline XrValue xrt_value_set_storage_graph(XrValue value, uint8_t storage_mode) {
    if (value.tag == XR_TAG_STR_ARC) {
        XrString *string = (XrString *) value.ptr;
        if (!string || storage_mode == XR_OBJ_STORAGE_NORMAL)
            return value;
        /* Family-private lazy caches become immutable before publication. */
        (void) xr_str_rune_len(value);
        (void) xrt_str_hash(value);
        uint32_t domain_id = storage_mode == XR_OBJ_STORAGE_SHARED
                                 ? XR_RUNTIME_STRING_DOMAIN_CONST_SHARED
                                 : storage_mode == XR_OBJ_STORAGE_TRANSFER
                                       ? XR_RUNTIME_STRING_DOMAIN_TRANSFERABLE
                                       : XR_RUNTIME_STRING_DOMAIN_COUNT;
        if (XR_UNLIKELY(domain_id == XR_RUNTIME_STRING_DOMAIN_COUNT)) {
            fprintf(stderr, "xrt: invalid canonical string storage domain\n");
            abort();
        }
        XrtExecutionAllocation *node = xrt_execution_node(string);
        if (XR_UNLIKELY(
                node->object_format != XRT_EXECUTION_OBJECT_RUNTIME_STRING)) {
            fprintf(stderr, "xrt: canonical string allocation format mismatch\n");
            abort();
        }
        xrt_execution_unlink_object(string);
        string->header.domain_id = domain_id;
        atomic_fetch_and_explicit(
            &string->traits,
            (uint16_t) ~XR_RUNTIME_STRING_TRAIT_LOCAL,
            memory_order_relaxed);
        return value;
    }
    bool embedded = XR_IS_ARRAY(value) || XR_IS_MAP(value) || XR_IS_SET(value) ||
                    xrt_is_struct_object_value(value);
    if (storage_mode == XR_OBJ_STORAGE_NORMAL || (!embedded && !xrt_arc_value_has_header(value)))
        return value;

    XrObjHeader *hdr = NULL;
    if (embedded)
        hdr = (XrObjHeader *) value.ptr;
    else
        hdr = xrt_arc_value_header(value);
    if (!hdr || (hdr->extra & (XR_OBJ_IMMORTAL | XR_OBJ_STORAGE_STACK)))
        return value;
    if (!(hdr->extra & XR_OBJ_AOT_EXECUTION) && XR_OBJ_GET_STORAGE(hdr) == storage_mode)
        return value;

    xrt_coll_set_storage_header(hdr, storage_mode);

    if (XR_IS_ARRAY(value)) {
        xrt_array_t *array = (xrt_array_t *) value.ptr;
        if (array->source)
            (void) xrt_value_set_storage_graph(xr_mkptr(array->source, XR_TAG_ARRAY), storage_mode);
        if (array->elem_type == XR_ELEM_ANY && array->data) {
            XrValue *items = (XrValue *) array->data;
            for (int64_t i = 0; i < array->length; i++)
                (void) xrt_value_set_storage_graph(items[i], storage_mode);
        }
        return value;
    }
    if (XR_IS_MAP(value)) {
        xrt_map_t *map = (xrt_map_t *) value.ptr;
        if (xrt_map_is_boolmap(map))
            return value;
        if (!xrt_map_is_typed(map) && map->entries) {
            for (uint32_t i = 0; i < map->nentries; i++) {
                if (map->entries[i].key_tt == XR_MAP_ENTRY_NIL_KEY)
                    continue;
                (void) xrt_value_set_storage_graph(map->entries[i].key, storage_mode);
                (void) xrt_value_set_storage_graph(map->entries[i].value, storage_mode);
            }
        }
        return value;
    }
    if (XR_IS_SET(value)) {
        xrt_set_t *set = (xrt_set_t *) value.ptr;
        if (!xrt_set_is_typed(set) && set->entries) {
            for (uint32_t i = 0; i < set->nentries; i++) {
                if (set->entries[i].val_tt != XR_SET_ENTRY_NIL)
                    (void) xrt_value_set_storage_graph(set->entries[i].value, storage_mode);
            }
        }
        return value;
    }
    if (xrt_is_struct_object_value(value)) {
        xrt_object_t *object = (xrt_object_t *) value.ptr;
        for (int64_t i = 0; i < xrt_object_field_count(object); i++)
            (void) xrt_value_set_storage_graph(object->fields[i], storage_mode);
        return value;
    }
    if (XR_IS_ARRAY_REF(value) && XR_ARRAY_REF_ELEM_TYPE(value) == XR_NATIVE_VALUE) {
        XrValue *items = (XrValue *) value.ptr;
        uint32_t count = XR_ARRAY_REF_ELEM_COUNT(value);
        for (uint32_t i = 0; i < count; i++)
            (void) xrt_value_set_storage_graph(items[i], storage_mode);
        return value;
    }

    switch (value.tag) {
        case XR_TAG_TUPLE: {
            xrt_tuple_t *tuple = (xrt_tuple_t *) value.ptr;
            for (int64_t i = 0; i < tuple->len; i++)
                (void) xrt_value_set_storage_graph(tuple->items[i], storage_mode);
            break;
        }
        case XR_TAG_CELL:
            (void) xrt_value_set_storage_graph(((xrt_cell_t *) value.ptr)->value, storage_mode);
            break;
        case XR_TAG_CLOSURE: {
            xrt_closure_t *closure = (xrt_closure_t *) value.ptr;
            for (int i = 0; i < closure->nupvals; i++)
                (void) xrt_value_set_storage_graph(closure->upvals[i], storage_mode);
            break;
        }
        case XR_TAG_ITERATOR:
            (void) xrt_value_set_storage_graph(((xrt_iterator_t *) value.ptr)->coll, storage_mode);
            break;
        case XR_TAG_ENUM: {
            XrAotEnumBox *box = (XrAotEnumBox *) value.ptr;
            for (uint32_t i = 0; i < box->payload_count; i++)
                (void) xrt_value_set_storage_graph(box->payloads[i], storage_mode);
            break;
        }
        case XR_TAG_PTR: {
            uint16_t class_type_id = xrt_aot_class_type_id(hdr);
            if (value.heap_type == XR_TINSTANCE && class_type_id != 0 &&
                class_type_id < xrt_type_count) {
                XrtStoragePromoter promote = xrt_type_table[class_type_id].promote_storage;
                if (promote)
                    promote(value.ptr, storage_mode);
            }
            break;
        }
        case XR_TAG_AGG_REF: {
            uint16_t struct_type_id = xrt_aot_class_type_id(hdr);
            if (!XR_IS_ARRAY_REF(value) && struct_type_id != 0 && struct_type_id < xrt_type_count) {
                XrtStoragePromoter promote = xrt_type_table[struct_type_id].promote_storage;
                if (promote)
                    promote(value.ptr, storage_mode);
            }
            break;
        }
        default:
            break;
    }
    return value;
}

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (xrt_value_kind(val)) {
        case XR_TAG_STR:
            return val;
        case XR_TAG_STR_ARC: {
            XrString *src = (XrString *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_str_alloc((size_t) src->length);
            XrString *dst = (XrString *) dstv.ptr;
            dst->hash = src->hash;
            dst->rune_length = src->rune_length;
            memcpy(dst->data, src->data, (size_t) src->length);
            return dstv;
        }
        case XR_TAG_ARRAY: {
            xrt_array_t *src = (xrt_array_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_array_new_typed(src->capacity, src->elem_type, src->elem_tid);
            xrt_array_t *dst = (xrt_array_t *) dstv.ptr;
            dst->length = src->length;
            if (src->elem_type == XR_ELEM_ANY) {
                for (int64_t i = 0; i < src->length; i++) {
                    XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
                    xr_typed_set(dst->data, (int32_t) i, xrt_value_clone_for_coro(item),
                                 dst->elem_type);
                }
            } else {
                memcpy(dst->data, src->data, (size_t) src->length * (size_t) src->elem_size);
            }
            return dstv;
        }
        case XR_TAG_MAP: {
            xrt_map_t *src = (xrt_map_t *) val.ptr;
            if (!src)
                return val;
            if (xrt_map_is_boolmap(src))
                return xrt_boolmap_clone((const xrt_boolmap_t *) src);
            XrValue dstv = xrt_map_is_typed(src)
                               ? xrt_map_new_typed(src->len, src->key_type, src->value_type)
                               : xrt_map_new(xrt_map_len(src));
            xrt_map_t *dst = (xrt_map_t *) dstv.ptr;
            dst->class_name = src->class_name;
            if (xrt_map_is_typed(src) && dst->cap == src->cap) {
                /* Same geometry: clone the slot arrays and control bytes. */
                dst->len = src->len;
                dst->growth_left = src->growth_left;
                memcpy(dst->ctrl, src->ctrl, (size_t) src->cap + XRT_GROUP);
                memcpy(dst->keys, src->keys, (size_t) src->cap * (size_t) src->key_size);
                memcpy(dst->values, src->values, (size_t) src->cap * (size_t) src->value_size);
                xrt_map_order_reserve(dst, src->order_len);
                memcpy(dst->order, src->order, (size_t) src->order_len * sizeof(int64_t));
                dst->order_len = src->order_len;
            } else if (!xrt_map_is_typed(src)) {
                for (uint32_t i = 0; i < src->nentries; i++) {
                    XrMapEntry *entry = &src->entries[i];
                    if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                        continue;
                    XrValue cloned_key = xrt_value_clone_for_coro(entry->key);
                    XrValue cloned_val = xrt_value_clone_for_coro(entry->value);
                    xrt_map_set(dst, cloned_key, cloned_val);
                }
            } else {
                for (int64_t oi = 0; oi < src->order_len; oi++) {
                    int64_t slot = src->order[oi];
                    if (!xrt_map_slot_is_full(src, slot))
                        continue;
                    XrValue cloned_key = xrt_value_clone_for_coro(xrt_map_slot_key(src, slot));
                    XrValue cloned_val = xrt_value_clone_for_coro(xrt_map_slot_value(src, slot));
                    xrt_map_set(dst, cloned_key, cloned_val);
                }
            }
            return dstv;
        }
        case XR_TAG_SET: {
            xrt_set_t *src = (xrt_set_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_set_new_typed(xrt_set_len(src), src->elem_type);
            xrt_set_t *dst = (xrt_set_t *) dstv.ptr;
            if (src->elem_type != XR_ELEM_ANY && dst->cap == src->cap) {
                dst->len = src->len;
                dst->growth_left = src->growth_left;
                memcpy(dst->ctrl, src->ctrl, (size_t) src->cap + XRT_GROUP);
                memcpy(dst->items, src->items, (size_t) src->cap * (size_t) src->elem_size);
                xrt_set_order_reserve(dst, src->order_len);
                memcpy(dst->order, src->order, (size_t) src->order_len * sizeof(int64_t));
                dst->order_len = src->order_len;
            } else if (!xrt_set_is_typed(src)) {
                for (uint32_t i = 0; i < src->nentries; i++) {
                    if (src->entries[i].val_tt != XR_SET_ENTRY_NIL)
                        xrt_set_add(dst, xrt_value_clone_for_coro(src->entries[i].value));
                }
            } else {
                for (int64_t oi = 0; oi < src->order_len; oi++) {
                    int64_t slot = src->order[oi];
                    if (!xrt_set_slot_is_full(src, slot))
                        continue;
                    xrt_set_add(dst, xrt_value_clone_for_coro(xrt_set_slot_item(src, slot)));
                }
            }
            return dstv;
        }
        case XR_TAG_STRBUF: {
            xrt_strbuf_t *src = (xrt_strbuf_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_strbuf_new();
            xrt_strbuf_t *dst = (xrt_strbuf_t *) dstv.ptr;
            xrt_strbuf_grow(dst, src->len);
            memcpy(dst->buf, src->buf, (size_t) src->len + 1u);
            dst->len = src->len;
            return dstv;
        }
        case XR_TAG_ENUM: {
            if (!xrt_arc_value_has_header(val))
                return val;
            XrAotEnumAggregate aggregate = xrt_enum_aggregate_from_boxed(val);
            uint32_t limit = aggregate.payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP
                                 ? aggregate.payload_count
                                 : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
            for (uint32_t i = 0; i < limit; i++)
                aggregate.payloads[i] = xrt_value_clone_for_coro(aggregate.payloads[i]);
            return xrt_enum_aggregate_box(aggregate);
        }
        case XR_TAG_AGG_REF: {
            if (!val.ptr)
                return val;
            if (XR_IS_ARRAY_REF(val))
                return xrt_array_ref_clone_value(val);
            uint16_t storage_size = val.heap_type;
            uint32_t size = storage_size ? storage_size : *(uint32_t *) val.ptr;
            if (size == 0 || size > (16u * 1024u * 1024u))
                return val;
            /* Nominal boxed value-structs carry their type identity in the
             * aggregate allocation header.  Their registered clone owns the
             * field-by-field ARC operation; a raw memcpy would alias owning
             * string/container fields and the first release would invalidate
             * the clone. */
            XrObjHeader *header = XRT_ARC_HDR(val.ptr);
            if (header->type == XR_TINSTANCE) {
                uint16_t type_id = xrt_aot_class_type_id(header);
                if (type_id != 0 && type_id < xrt_type_count) {
                    XrtRuntimeClone clone_fn = xrt_type_table[type_id].runtime_clone;
                    if (clone_fn) {
                        void *cloned = clone_fn(val.ptr);
                        return cloned ? xr_aggregate_ref(cloned, storage_size) : XR_NULL_VAL;
                    }
                }
            }
            void *dst = xrt_arc_alloc(size);
            memcpy(dst, val.ptr, size);
            return storage_size ? xr_aggregate_ref(dst, storage_size)
                                : xr_mkptr(dst, XR_TAG_AGG_REF);
        }
        case XR_TAG_CELL: {
            xrt_cell_t *src = (xrt_cell_t *) val.ptr;
            if (!src)
                return val;
            XrValue cloned = xrt_value_clone_for_coro(src->value);
            return xrt_cell_new(cloned);
        }
        case XR_TAG_PTR: {
            if (!val.ptr)
                return val;
            if (val.heap_type == XR_TINSTANCE) {
                uint16_t type_id = xrt_aot_class_type_id((const XrObjHeader *) val.ptr);
                if (type_id != 0 && type_id < xrt_type_count) {
                    XrtRuntimeClone clone_fn = xrt_type_table[type_id].runtime_clone;
                    if (clone_fn) {
                        void *cloned = clone_fn(val.ptr);
                        return cloned ? xrt_box_obj(cloned) : XR_NULL_VAL;
                    }
                }
            }
            xrt_retain(val);
            return val;
        }
        case XR_TAG_REGEX:
        case XR_TAG_NET_CONN:
        case XR_TAG_NET_LISTENER:
            xrt_retain(val);
            return val;
        default:
            return val;
    }
}

#endif  // XRT_COLL_H
