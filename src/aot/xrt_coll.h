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
#include "../shared/xr_cell_abi.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_error_core.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_map_set_abi.h"
#include "../shared/xr_json_type.h"
#include "../shared/xr_typed_ops.h"
#include <errno.h>
#include <string.h>

static inline XrValue xrt_value_clone_for_coro(XrValue val);
XRT_COLD _Noreturn void xrt_type_no_index(const char *message);

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
} xrt_array_t;

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

/* Heap containers start with a bump-style header because the same init helpers
 * are shared by stack and heap constructors. Under the default deterministic
 * policy, heap containers become normal RC objects; under XRAY_AOT_ARENA=1 they
 * keep bump lifetime and release at process exit. */
static inline void xrt_coll_make_deterministic(XrObjHeader *h) {
    h->extra |= XR_OBJ_AOT_NATIVE;
    if (xrt_bump_enabled)
        return;
    h->extra &= (uint16_t) ~(uint16_t) XR_OBJ_STORAGE_BUMP;
    atomic_store_explicit(&h->refcount, 0, memory_order_relaxed);
}

static inline void xrt_array_init_header(xrt_array_t *a, int64_t cap, uint8_t etype,
                                         uint8_t elem_size) {
    xrt_bump_header_init(&a->hdr, XR_TARRAY);
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
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(total);
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "%s: out of memory\n", where);
        abort();
    }
    xrt_array_init_header(a, cap, etype, elem_size);
    xrt_coll_make_deterministic(&a->hdr);
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

static inline XrValue xrt_array_new_typed(int64_t len, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_ptr(len, etype), XR_TAG_ARRAY);
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
static inline XrValue xrt_json_new_named(int64_t field_count, const char *const *field_names);
static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val);
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
    XrValue exc = xrt_json_new_named(EXCEPTION_FIELD_COUNT, xr_exception_field_names());
    xrt_json_set_field(exc, EXCEPTION_FIELD_MESSAGE, xrt_error_message_value(message));
    xrt_json_set_field(exc, EXCEPTION_FIELD_STACK, xrt_array_new(0));
    xrt_json_set_field(exc, EXCEPTION_FIELD_CAUSE, XR_NULL_VAL);
    xrt_json_set_field(exc, EXCEPTION_FIELD_CODE, XR_FROM_INT(code));
    xrt_json_set_field(exc, EXCEPTION_FIELD_DATA, XR_NULL_VAL);
    return exc;
}

static inline void xrt_throw_error(int code, const char *message) {
    xrt_throw_exc(xrt_structured_error_value(code, message));
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
    XrAotEnumBox *ev = (XrAotEnumBox *) XRT_CALLOC(1, alloc_size);
    if (XR_UNLIKELY(!ev)) {
        fprintf(stderr, "xrt_enum_box_new_payloads: out of memory\n");
        abort();
    }
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

static inline XrAotEnumAggregate xrt_enum_aggregate_from_boxed(XrValue boxed) {
    if (boxed.tag != XR_TAG_ENUM || !boxed.ptr)
        return xrt_enum_aggregate_zero();
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
    const XrAotEnumBox *ev = hdr->type == XR_TENUM_CTOR ? NULL : (const XrAotEnumBox *) boxed.ptr;
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

enum {
    XRT_SPAN_FLAG_READONLY = 1u << 0,
};

typedef struct {
    void *data;
    int64_t length;
    void *guard;
    uint8_t elem_type;
    uint8_t elem_size;
    uint8_t elem_tid;
    uint8_t contains_refs;
    uint32_t flags;
} xr_span_t;

static inline xr_span_t xrt_span_empty(void) {
    return (xr_span_t) {NULL, 0, NULL, XR_ELEM_ANY, (uint8_t) sizeof(XrValue), 0, 0, 0};
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
        out.guard = NULL;
        out.elem_type = xr_native_type_to_elem_type(native_type);
        out.elem_size = elem_size ? elem_size : (uint8_t) sizeof(XrValue);
        out.elem_tid = 0;
        out.contains_refs = out.elem_type == XR_ELEM_ANY;
        out.flags = 0;
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
    out.guard = arr.ptr;
    out.elem_type = src->elem_type;
    out.elem_size = src->elem_size ? src->elem_size : (uint8_t) sizeof(XrValue);
    out.elem_tid = src->elem_tid;
    out.contains_refs = src->contains_refs;
    out.flags = 0;
    return out;
}

static inline xr_span_t xrt_span_from_span_slice(xr_span_t src, int64_t start, int64_t end) {
    xrt_array_normalize_slice(src.length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    xr_span_t out = src;
    out.data = (count > 0 && src.data)
                   ? (void *) ((uint8_t *) src.data + (size_t) start * (size_t) src.elem_size)
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
    out.guard = str.ptr;
    out.elem_type = XR_ELEM_U8;
    out.elem_size = 1;
    out.elem_tid = 0;
    out.contains_refs = 0;
    out.flags = XRT_SPAN_FLAG_READONLY;
    return out;
}

static inline bool xrt_span_is_readonly(xr_span_t span) {
    return (span.flags & XRT_SPAN_FLAG_READONLY) != 0;
}

static inline xr_span_t xrt_span_as_bytes_checked_raw(xr_span_t span) {
    if (span.elem_type == XR_ELEM_ANY || span.elem_type >= XR_ELEM_COUNT || span.elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.asBytes() requires POD Span element type");
    if (span.length < 0 ||
        (span.elem_size > 0 && span.length > INT64_MAX / (int64_t) span.elem_size))
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.asBytes() byte length overflow");
    xr_span_t out = span;
    out.length = span.length * (int64_t) span.elem_size;
    out.elem_type = XR_ELEM_U8;
    out.elem_size = 1;
    out.elem_tid = 0;
    out.contains_refs = 0;
    out.flags = span.flags & XRT_SPAN_FLAG_READONLY;
    return out;
}

static inline xr_span_t xrt_span_copy_checked_raw(xr_span_t dst, xr_span_t src) {
    if (dst.elem_type == XR_ELEM_ANY || dst.elem_type >= XR_ELEM_COUNT || dst.elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) receiver must be POD Span");
    if (src.elem_type == XR_ELEM_ANY || src.elem_type >= XR_ELEM_COUNT || src.elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) source must be POD Span");
    if (dst.elem_type != src.elem_type || dst.elem_size != src.elem_size)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) element type mismatch");
    if (xrt_span_is_readonly(dst))
        xrt_throw_error(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    if (dst.length < 0 || src.length < 0 || dst.length > INT64_MAX / (int64_t) dst.elem_size ||
        src.length > INT64_MAX / (int64_t) src.elem_size)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) byte length overflow");
    int64_t dst_bytes = dst.length * (int64_t) dst.elem_size;
    int64_t src_bytes = src.length * (int64_t) src.elem_size;
    if (src_bytes > dst_bytes)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) range out of bounds");
    if (src_bytes > 0) {
        if (!dst.data || !src.data)
            xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) range out of bounds");
        memmove(dst.data, src.data, (size_t) src_bytes);
    }
    return dst;
}

static inline int64_t xrt_span_compare_checked_raw(xr_span_t left, xr_span_t right) {
    if (left.elem_type == XR_ELEM_ANY || left.elem_type >= XR_ELEM_COUNT || left.elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.compare(other) receiver must be POD Span");
    if (right.elem_type == XR_ELEM_ANY || right.elem_type >= XR_ELEM_COUNT || right.elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.compare(other) operand must be POD Span");
    if (left.elem_type != right.elem_type || left.elem_size != right.elem_size)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.compare(other) element type mismatch");
    if (left.length < 0 || right.length < 0 || left.length > INT64_MAX / (int64_t) left.elem_size ||
        right.length > INT64_MAX / (int64_t) right.elem_size)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.compare(other) byte length overflow");
    int64_t left_bytes = left.length * (int64_t) left.elem_size;
    int64_t right_bytes = right.length * (int64_t) right.elem_size;
    int64_t n = left_bytes < right_bytes ? left_bytes : right_bytes;
    int cmp = 0;
    if (n > 0) {
        if (!left.data || !right.data)
            xrt_throw_error(XR_ERR_TYPE_MISMATCH, "Span.compare(other) span has no data");
        cmp = memcmp(left.data, right.data, (size_t) n);
    }
    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    if (left.length < right.length)
        return -1;
    if (left.length > right.length)
        return 1;
    return 0;
}

static inline xr_span_t xrt_span_reinterpret_checked_raw(xr_span_t span, uint8_t elem_type,
                                                         uint8_t elem_size, uint8_t elem_tid) {
    if (elem_type == XR_ELEM_ANY || elem_type >= XR_ELEM_COUNT || elem_size == 0)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG);
    if (XR_ELEM_SIZES[elem_type] != elem_size)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG);
    if (span.elem_type != XR_ELEM_U8 || span.elem_size != 1)
        xrt_throw_error(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_RECEIVER_MSG);
    if (span.length < 0)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG);
    if (span.length % (int64_t) elem_size != 0)
        xrt_throw_error(XR_ERR_INDEX_OUT_OF_BOUNDS,
                        XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG);
    xr_span_t out = span;
    out.length = span.length / (int64_t) elem_size;
    out.elem_type = elem_type;
    out.elem_size = elem_size;
    out.elem_tid = elem_tid;
    out.contains_refs = 0;
    out.flags = span.flags & XRT_SPAN_FLAG_READONLY;
    return out;
}

static inline XrValue xrt_span_to_owned_array(xr_span_t span) {
    int64_t len = span.length < 0 ? 0 : span.length;
    XrValue outv = xrt_array_new_typed(len, span.elem_type);
    xrt_array_t *out = (xrt_array_t *) outv.ptr;
    out->elem_tid = span.elem_tid;
    out->contains_refs = span.contains_refs;
    if (len <= 0 || !span.data || !out->data)
        return outv;
    if (span.elem_type == XR_ELEM_ANY) {
        XrValue *src_items = (XrValue *) span.data;
        XrValue *dst_items = (XrValue *) out->data;
        for (int64_t i = 0; i < len; i++)
            dst_items[i] = xrt_value_clone_for_coro(src_items[i]);
    } else {
        size_t bytes = (size_t) len * (size_t) span.elem_size;
        memcpy(out->data, span.data, bytes);
    }
    return outv;
}

static inline xr_span_t xrt_span_to_owned_span(xr_span_t span) {
    XrValue owner = xrt_span_to_owned_array(span);
    if (!XR_IS_ARRAY(owner) || !owner.ptr)
        return xrt_span_empty();
    xrt_array_t *arr = (xrt_array_t *) owner.ptr;
    return xrt_span_from_array_slice(owner, 0, arr->length);
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

    xrt_array_t *slice = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_slice_view: out of memory\n");
        abort();
    }
    xrt_array_init_header(slice, count, src->elem_type, src->elem_size);
    xrt_coll_make_deterministic(&slice->hdr);
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

static inline void xrt_array_stack_borrow_span_view_init(xrt_array_t *view, xr_span_t span) {
    if (XR_UNLIKELY(!view)) {
        fprintf(stderr, "xrt_array_stack_borrow_span_view_init: NULL view\n");
        abort();
    }
    uint8_t elem_size = span.elem_size ? span.elem_size : (uint8_t) sizeof(XrValue);
    int64_t len = span.length < 0 ? 0 : span.length;
    xrt_array_init_header(view, len, span.elem_type, elem_size);
    view->data = span.data;
    view->length = len;
    view->capacity = len;
    view->data_storage = XR_ARRAY_DATA_BORROWED;
    view->source = span.guard;
    view->storage = NULL;
    view->elem_type = span.elem_type;
    view->elem_size = elem_size;
    view->elem_tid = span.elem_tid;
    view->contains_refs = span.contains_refs;
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
    int64_t len;
    XrValue items[];
} xrt_tuple_t;

static inline XrValue xrt_tuple_new(int64_t len) {
    if (len < 0)
        len = 0;
    xrt_tuple_t *t =
        (xrt_tuple_t *) XRT_MALLOC(sizeof(xrt_tuple_t) + (size_t) len * sizeof(XrValue));
    if (XR_UNLIKELY(!t)) {
        fprintf(stderr, "xrt_tuple_new: out of memory\n");
        abort();
    }
    t->len = len;
    for (int64_t i = 0; i < len; i++)
        t->items[i] = XR_NULL_VAL;
    return xr_mkptr(t, XR_TAG_TUPLE);
}

static inline XrValue xrt_tuple_make(int64_t len, const XrValue *items) {
    XrValue tuple = xrt_tuple_new(len);
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    for (int64_t i = 0; i < t->len; i++)
        t->items[i] = items ? items[i] : XR_NULL_VAL;
    return tuple;
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
        xrt_bump_header_init(&_a->hdr, XR_TARRAY);                                                 \
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

#ifndef xrt_array_stack_borrow_span_view
#define xrt_array_stack_borrow_span_view(span_expr)                                                \
    ({                                                                                             \
        xr_span_t _span = (span_expr);                                                             \
        xrt_array_t *_view = (xrt_array_t *) __builtin_alloca(sizeof(xrt_array_t));                \
        xrt_array_stack_borrow_span_view_init(_view, _span);                                       \
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
    xrt_strbuf_t *sb = (xrt_strbuf_t *) XRT_MALLOC(sizeof(xrt_strbuf_t));
    if (XR_UNLIKELY(!sb)) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
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

typedef struct {
    const char *a;
    const char *b;
    size_t alen;
    size_t blen;
    char scratch[64];
    uint8_t kind;
} xrt_strpart_t;

#define XRT_STRPART_SINGLE 0u
#define XRT_STRPART_ENUM 1u

static inline size_t xrt_format_uint64(char *buf, size_t cap, uint64_t value) {
    int n = snprintf(buf, cap, "%llu", (unsigned long long) value);
    if (n < 0)
        return 0;
    if ((size_t) n >= cap)
        return cap ? cap - 1u : 0u;
    return (size_t) n;
}

static inline XrValue xrt_uint64_to_string(uint64_t value) {
    char scratch[32];
    size_t len = xrt_format_uint64(scratch, sizeof(scratch), value);
    XrValue out = xrt_str_alloc(len);
    memcpy(xr_str_buf(out), scratch, len);
    return out;
}

static inline void xrt_strpart_init_u64(xrt_strpart_t *part, uint64_t value) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;
    part->alen = xrt_format_uint64(part->scratch, sizeof(part->scratch), value);
    part->a = part->scratch;
}

static inline void xrt_strpart_init(xrt_strpart_t *part, XrValue val) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;

    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        part->a = xr_str_data(val);
        part->alen = (size_t) xr_str_len(val);
    } else if (val.tag == XR_TAG_I64) {
        int n = snprintf(part->scratch, sizeof(part->scratch), "%lld", (long long) val.i);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_F64) {
        int n = xr_format_float(part->scratch, sizeof(part->scratch), val.f);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_BOOL) {
        part->a = val.i ? "true" : "false";
        part->alen = val.i ? 4u : 5u;
    } else if (val.tag == XR_TAG_RUNE) {
        uint32_t cp = XR_TO_RUNE(val);
        int n = 0;
        if (cp <= 0x7Fu) {
            part->scratch[n++] = (char) cp;
        } else if (cp <= 0x7FFu) {
            part->scratch[n++] = (char) (0xC0u | (cp >> 6));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
            part->scratch[n++] = (char) (0xE0u | (cp >> 12));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0x10FFFFu) {
            part->scratch[n++] = (char) (0xF0u | (cp >> 18));
            part->scratch[n++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        }
        part->a = part->scratch;
        part->alen = (size_t) n;
    } else if (val.tag == XR_TAG_NULL) {
        part->a = "null";
        part->alen = 4u;
    } else if (val.tag == XR_TAG_ENUM) {
        const char *enum_name = NULL;
        const char *member_name = NULL;
        if (xrt_enum_key_parts(val, &enum_name, &member_name, NULL, NULL) && enum_name &&
            member_name) {
            part->a = enum_name;
            part->b = member_name;
            part->alen = strlen(enum_name);
            part->blen = strlen(member_name);
            part->kind = XRT_STRPART_ENUM;
        } else {
            int n = snprintf(part->scratch, sizeof(part->scratch), "<enum@%p>", val.ptr);
            part->a = part->scratch;
            part->alen = (size_t) (n < 0 ? 0 : n);
        }
    } else {
        const char *s = xr_to_cstr(val, part->scratch, sizeof(part->scratch));
        part->a = s;
        part->alen = strlen(s);
    }
}

static inline size_t xrt_strpart_len(const xrt_strpart_t *part) {
    return part->kind == XRT_STRPART_ENUM ? part->alen + 1u + part->blen : part->alen;
}

static inline char *xrt_strpart_copy(char *dst, const xrt_strpart_t *part) {
    memcpy(dst, part->a, part->alen);
    dst += part->alen;
    if (part->kind == XRT_STRPART_ENUM) {
        *dst++ = '.';
        memcpy(dst, part->b, part->blen);
        dst += part->blen;
    }
    return dst;
}

static inline XrValue xrt_str_concat_parts(size_t count, xrt_strpart_t *parts) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = xrt_strpart_len(&parts[i]);
        if (XR_UNLIKELY(len > SIZE_MAX - total)) {
            fprintf(stderr, "xrt_str_concat_parts: string length overflow\n");
            abort();
        }
        total += len;
    }
    XrValue out = xrt_str_alloc(total);
    char *dst = xr_str_buf(out);
    for (size_t i = 0; i < count; i++)
        dst = xrt_strpart_copy(dst, &parts[i]);
    *dst = 0;
    return out;
}

static inline XrValue xrt_enum_box_new(uint32_t layout_id, const char *enum_name,
                                       const char *member_name, uint32_t member_index) {
    return xrt_enum_box_new_payloads(layout_id, enum_name, member_name, member_index, 0, NULL);
}

static inline const XrAotEnumBox *xrt_enum_box_view(XrValue obj) {
    if (obj.tag != XR_TAG_ENUM || !obj.ptr)
        return NULL;
    const XrObjHeader *hdr = (const XrObjHeader *) obj.ptr;
    return hdr->type == XR_TENUM_CTOR ? NULL : (const XrAotEnumBox *) obj.ptr;
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
    uint64_t bits;
    if (d == 0.0)
        d = 0.0; /* canonicalize -0.0: IEEE == treats them equal */
    memcpy(&bits, &d, sizeof(bits));
    return xr_hash_core_mix_u64(bits);
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
        case XR_TAG_NULL:
            return xr_hash_core_mix_u64(0x9e3779b97f4a7c15ull);
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
    xrt_bump_header_init(&m->hdr, XR_TMAP);
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

/* Candidate comparators for the shared Swiss probe (xr_{map,set}_lookup_slot):
 * type tag then canonical equality. xrt_eq is type-aware, so the tag pre-check
 * only short-circuits type-mismatched hash collisions. Return int (not bool) to
 * match the runtime's bool-free generated-C convention. */
static inline int xrt_map_key_eq(const XrMapEntry *e, XrValue key, uint8_t key_tt) {
    return e->key_tt == key_tt && xrt_eq(e->key, key) != 0;
}
static inline int xrt_set_value_eq(const XrSetEntry *e, XrValue value, uint8_t val_tt) {
    return e->val_tt == val_tt && xrt_eq(e->value, value) != 0;
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

static inline XrValue xrt_map_new_flags(int64_t cap, uint8_t flags) {
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (XR_UNLIKELY(!m)) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    xrt_map_init_header(m);
    xrt_coll_make_deterministic(&m->hdr);
    m->flags |= (uint8_t) (flags & XR_MAP_FLAG_WEAK);
    if (cap > 0)
        xrt_map_resize_tagged(m, (uint32_t) cap);
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline XrValue xrt_map_new(int64_t cap) {
    return xrt_map_new_flags(cap, 0);
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
    XrValue mv = xrt_map_new_flags(cap, 0);
    if (mv.ptr)
        ((xrt_map_t *) mv.ptr)->value_type = value_type;
    return mv;
}

static inline XrValue xrt_map_static_storage_init(xrt_map_t *m, uint8_t *ctrl, int32_t *indices,
                                                  XrMapEntry *entries, uint32_t indices_size,
                                                  uint32_t entries_cap, uint8_t value_type) {
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
    if (XR_UNLIKELY(m && (m->flags & XR_MAP_FLAG_STATIC_READONLY))) {
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
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        xrt_release(m->entries[eidx].key);
        xrt_release(m->entries[eidx].value);
    }
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
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        xrt_release(m->entries[eidx].key);
        xrt_release(m->entries[eidx].value);
    }
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
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        xrt_release(m->entries[eidx].key);
        xrt_release(m->entries[eidx].value);
    }
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
        if (!(m->flags & XR_MAP_FLAG_WEAK)) {
            if (XR_IS_ARRAY_REF(val))
                val = xrt_value_to_owned(val);
            xrt_release(key);
            xrt_release(m->entries[eidx].value);
        }
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        if (XR_IS_ARRAY_REF(key))
            key = xrt_value_to_owned(key);
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
    }
    eidx = (int32_t) m->nentries++;
    XrMapEntry *entry = &m->entries[eidx];
    entry->key = key;
    entry->value = val;
    entry->hash = hash;
    entry->key_tt = key_tt;
    m->count++;
    xr_swiss_indices_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

static inline XrValue xrt_path_from_slice(const char *data, size_t len) {
    XrValue raw = xrt_str_alloc(len);
    if (len > 0 && data)
        memcpy(xr_str_buf(raw), data, len);
    XrValue path = xrt_map_set_class_name(xrt_map_new(1), "Path");
    xrt_map_set((xrt_map_t *) path.ptr, xr_box_str("raw"), raw);
    return path;
}

static inline XrValue xrt_path_from_cstr(const char *s) {
    return s ? xrt_path_from_slice(s, strlen(s)) : XR_NULL_VAL;
}

static inline XrValue xrt_path_raw_value(XrValue path) {
    if (xrt_map_backed_class_exact(path, "Path"))
        return xrt_map_get((xrt_map_t *) path.ptr, xr_box_str("raw"));
    if (path.tag == XR_TAG_PTR && path.heap_type == XR_TINSTANCE && path.ptr) {
        XrObjHeader *hdr = XRT_ARC_HDR(path.ptr);
        const char *name = xrt_type_display_name(hdr->type);
        if (name && strcmp(name, "Path") == 0)
            return ((XrValue *) path.ptr)[0];
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
        if (!(m->flags & XR_MAP_FLAG_WEAK)) {
            if (XR_IS_ARRAY_REF(val))
                val = xrt_value_to_owned(val);
            xrt_release(key);
            xrt_release(m->entries[eidx].value);
        }
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        if (XR_IS_ARRAY_REF(key))
            key = xrt_value_to_owned(key);
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
    }
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
        if (!(m->flags & XR_MAP_FLAG_WEAK)) {
            if (XR_IS_ARRAY_REF(val))
                val = xrt_value_to_owned(val);
            xrt_release(key);
            xrt_release(m->entries[eidx].value);
        }
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        if (XR_IS_ARRAY_REF(key))
            key = xrt_value_to_owned(key);
        if (XR_IS_ARRAY_REF(val))
            val = xrt_value_to_owned(val);
    }
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
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_release(m->entries[i].key);
            xrt_release(m->entries[i].value);
            m->entries[i].key = XR_NULL_VAL;
            m->entries[i].value = XR_NULL_VAL;
            m->entries[i].key_tt = XR_MAP_ENTRY_NIL_KEY;
        }
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
    xrt_bump_header_init(&s->hdr, XR_TSET);
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
    xrt_set_t *s = (xrt_set_t *) XRT_MALLOC(sizeof(xrt_set_t));
    if (XR_UNLIKELY(!s)) {
        fprintf(stderr, "xrt_set_new: out of memory\n");
        abort();
    }
    xrt_set_init_header(s, elem_type);
    xrt_coll_make_deterministic(&s->hdr);
    if (elem_type == XR_ELEM_ANY) {
        if (cap > 0)
            xrt_set_resize_tagged(s, (uint32_t) cap);
    } else {
        xrt_set_alloc_slots(s, xrt_swiss_slots_for(cap));
    }
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_new_flags(int64_t cap, uint8_t flags) {
    XrValue v = xrt_set_new_typed(cap, XR_ELEM_ANY);
    ((xrt_set_t *) v.ptr)->flags |= (uint8_t) (flags & XR_SET_FLAG_WEAK);
    return v;
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
        XRT_FREE(a);
        return;
    }
    if (a->storage) {
        /* Storage owns the buffer and (for ANY) the element refs. Snapshot the
         * owner's length so an orphaned storage can release the right count. */
        XrArrayStorage *s = (XrArrayStorage *) a->storage;
        if (s->elem_is_any)
            s->elem_count = a->length;
        xrt_array_storage_release(a);
        XRT_FREE(a);
        return;
    }
    if (a->elem_type == XR_ELEM_ANY && a->data && a->length > 0) {
        XrValue *items = (XrValue *) a->data;
        for (int64_t i = 0; i < a->length; i++)
            xrt_release(items[i]);
    }
    if (a->data_storage == XR_ARRAY_DATA_HEAP && a->data)
        XRT_FREE_ALIGNED(a->data);
    XRT_FREE(a);
}

static inline void xrt_map_destroy(xrt_map_t *m) {
    if (!m)
        return;
    if (!xrt_map_is_typed(m) && !(m->flags & (XR_MAP_FLAG_DUMMY | XR_MAP_FLAG_WEAK)) &&
        m->entries) {
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
    XRT_FREE(m);
}

static inline void xrt_set_destroy(xrt_set_t *s) {
    if (!s)
        return;
    if (!xrt_set_is_typed(s) && !(s->flags & (XR_SET_FLAG_DUMMY | XR_SET_FLAG_WEAK)) &&
        s->entries) {
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
    XRT_FREE(s);
}

static inline void xrt_coll_retain(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!h || (h->extra & XR_OBJ_STORAGE_BUMP))
        return;
    atomic_fetch_add_explicit(&h->refcount, 1, memory_order_relaxed);
}

static inline void xrt_coll_release(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!xrt_rc_claim_release_last(h))
        return;
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

static inline XrValue xrt_set_new(int64_t cap) {
    return xrt_set_new_typed(cap, XR_ELEM_ANY);
}

static inline XrValue xrt_set_static_storage_init(xrt_set_t *s, uint8_t *ctrl, int32_t *indices,
                                                  XrSetEntry *entries, uint32_t indices_size,
                                                  uint32_t entries_cap) {
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
            if (!(s->flags & XR_SET_FLAG_WEAK))
                xrt_release(value);
            return 0;
        }
        if (s->nentries >= s->entries_cap)
            xrt_set_resize_tagged(s, s->count + 1);
        if (!(s->flags & XR_SET_FLAG_WEAK) && XR_IS_ARRAY_REF(value))
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
        if (!(s->flags & XR_SET_FLAG_WEAK))
            xrt_release(value);
        return 0;
    }
    if (s->nentries >= s->entries_cap)
        xrt_set_resize_tagged(s, s->count + 1);
    if (!(s->flags & XR_SET_FLAG_WEAK) && XR_IS_ARRAY_REF(value))
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
        if (!(s->flags & XR_SET_FLAG_WEAK))
            xrt_release(value);
        return 0;
    }
    if (s->nentries >= s->entries_cap)
        xrt_set_resize_tagged(s, s->count + 1);
    if (!(s->flags & XR_SET_FLAG_WEAK) && XR_IS_ARRAY_REF(value))
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
        if (!(s->flags & XR_SET_FLAG_WEAK))
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
    if (!(s->flags & XR_SET_FLAG_WEAK))
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
    if (!(s->flags & XR_SET_FLAG_WEAK))
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
        if (!(s->flags & XR_SET_FLAG_WEAK)) {
            for (uint32_t i = 0; i < s->nentries; i++) {
                if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
                    continue;
                xrt_release(s->entries[i].value);
                s->entries[i].value = XR_NULL_VAL;
                s->entries[i].val_tt = XR_SET_ENTRY_NIL;
            }
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
                xrt_array_push(arr, s->entries[i].value);
        }
        return arr;
    }
    for (int64_t oi = 0; oi < s->order_len; oi++) {
        int64_t slot = s->order[oi];
        if (xrt_set_slot_is_full(s, slot))
            xrt_array_push(arr, xrt_set_slot_item(s, slot));
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
        XrValue arr = xrt_array_with_capacity(xrt_map_len(m));
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY)
                xrt_array_push(arr, m->entries[i].key);
        }
        return arr;
    }
    XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->key_type), XR_TAG_ARRAY);
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_map_slot_key(m, slot));
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
                xrt_array_push(arr, m->entries[i].value);
        }
        return arr;
    }
    XrValue arr = xr_mkptr(xrt_array_new_typed_ptr(0, m->value_type), XR_TAG_ARRAY);
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_map_slot_value(m, slot));
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
 * Json object runtime header (flat field array, O(1) indexed access)
 * ========================================================================= */

enum {
    XRT_OBJECT_JSON = 0,
    XRT_OBJECT_RECORD = 1,
};

typedef struct {
    int64_t field_count;
    uint8_t object_kind;
    const char **field_names;
    xrt_map_t *dynamic_fields;
    XrValue fields[]; /* flexible array of field values */
} xrt_json_t;

/* =========================================================================
 * Iterator runtime — backs the for-in iterator protocol over Map / Set / Json / string.
 * The iterator borrows its source by value (no extra RC: AOT collections are
 * not individually reclaimed) and walks dense entries, typed order[], Json
 * field slots, or UTF-8 scalar boundaries.
 * ========================================================================= */

#define XRT_ITER_KEYS 0      /* map: yield key */
#define XRT_ITER_VALUES 1    /* set: yield value; map: yield value */
#define XRT_ITER_PAIRS 2     /* map/string: yield (key, value) tuple */
#define XRT_ITER_GENERATOR 3 /* coroutine-backed generator: pull-driven via gen_drive */

typedef struct XrCoroutine XrCoroutine;

typedef struct {
    XrValue coll;     /* XR_TAG_MAP, XR_TAG_SET, or string being iterated */
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
#endif

static inline XrValue xrt_iterator_new(XrValue coll, uint8_t kind) {
    xrt_iterator_t *it = (xrt_iterator_t *) XRT_MALLOC(sizeof(xrt_iterator_t));
    if (XR_UNLIKELY(!it)) {
        fprintf(stderr, "xrt_iterator_new: out of memory\n");
        abort();
    }
    it->coll = coll;
    it->cursor = 0;
    it->index = 0;
    it->kind = kind;
    it->gen = NULL;
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

static inline int xrt_is_json_object_value(XrValue v) {
    return v.tag == XR_TAG_PTR && v.ptr && v.heap_type == 0;
}

static inline int64_t xrt_json_dynamic_len(const xrt_json_t *j) {
    return (j && j->dynamic_fields) ? xrt_map_len(j->dynamic_fields) : 0;
}

static inline int64_t xrt_json_iter_count(const xrt_json_t *j) {
    return j ? j->field_count + xrt_json_dynamic_len(j) : 0;
}

static inline int xrt_json_dynamic_iter_has_next(xrt_map_t *m, int64_t *cursor) {
    if (!m || !cursor)
        return 0;
    while ((uint32_t) *cursor < m->nentries) {
        if (xrt_map_slot_is_full(m, *cursor))
            return 1;
        (*cursor)++;
    }
    return 0;
}

static inline int xrt_json_iterator_has_next(xrt_iterator_t *it) {
    xrt_json_t *j = (xrt_json_t *) it->coll.ptr;
    while (it->cursor < j->field_count) {
        if (!j->field_names || !j->field_names[it->cursor]) {
            it->cursor++;
            continue;
        }
        return 1;
    }
    if (!j->dynamic_fields)
        return 0;
    int64_t dyn_cursor = it->cursor - j->field_count;
    if (!xrt_json_dynamic_iter_has_next(j->dynamic_fields, &dyn_cursor))
        return 0;
    it->cursor = j->field_count + dyn_cursor;
    return 1;
}

static inline XrValue xrt_json_iterator_next(xrt_iterator_t *it) {
    xrt_json_t *j = (xrt_json_t *) it->coll.ptr;
    if (it->cursor < j->field_count) {
        int64_t idx = it->cursor++;
        XrValue key = xr_box_str(j->field_names && j->field_names[idx] ? j->field_names[idx] : "");
        XrValue value = j->fields[idx];
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {key, value};
            return xrt_tuple_make(2, kv);
        }
        if (it->kind == XRT_ITER_VALUES)
            return value;
        return key;
    }
    if (!j->dynamic_fields)
        return XR_NULL_VAL;
    int64_t dyn_cursor = it->cursor - j->field_count;
    if (!xrt_json_dynamic_iter_has_next(j->dynamic_fields, &dyn_cursor))
        return XR_NULL_VAL;
    int64_t slot = dyn_cursor;
    it->cursor = j->field_count + dyn_cursor + 1;
    XrValue key = xrt_map_slot_key(j->dynamic_fields, slot);
    XrValue value = xrt_map_slot_value(j->dynamic_fields, slot);
    if (it->kind == XRT_ITER_PAIRS) {
        XrValue kv[2] = {key, value};
        return xrt_tuple_make(2, kv);
    }
    if (it->kind == XRT_ITER_VALUES)
        return value;
    return key;
}

// Park cursor at the next live entry/order[] slot; return 1 if one exists.
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
    if (xrt_is_json_object_value(it->coll))
        return xrt_json_iterator_has_next(it);
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
    if (XR_IS_STR(it->coll))
        return it->cursor < xr_str_len(it->coll);
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
    if (!xrt_iterator_has_next(it))
        return XR_NULL_VAL;
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        if (xrt_map_is_boolmap(m)) {
            xrt_boolmap_t *b = (xrt_boolmap_t *) m;
            int64_t cursor = it->cursor++;
            if (it->kind == XRT_ITER_PAIRS) {
                XrValue kv[2] = {xrt_boolmap_iter_key(b, cursor),
                                 xrt_boolmap_iter_value(b, cursor)};
                return xrt_tuple_make(2, kv);
            }
            if (it->kind == XRT_ITER_VALUES)
                return xrt_boolmap_iter_value(b, cursor);
            return xrt_boolmap_iter_key(b, cursor);
        }
        int64_t slot = xrt_map_is_typed(m) ? m->order[it->cursor++] : it->cursor++;
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
            return xrt_tuple_make(2, kv);
        }
        if (it->kind == XRT_ITER_VALUES)
            return xrt_map_slot_value(m, slot);
        return xrt_map_slot_key(m, slot);
    }
    if (XR_IS_SET(it->coll)) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        int64_t slot = xrt_set_is_typed(s) ? s->order[it->cursor++] : it->cursor++;
        return xrt_set_slot_item(s, slot);
    }
    if (xrt_is_json_object_value(it->coll))
        return xrt_json_iterator_next(it);
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
            return xrt_tuple_make(2, kv);
        }
        return ch;
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

static inline XrValue xrt_object_new_kind(int64_t field_count, uint8_t object_kind) {
    xrt_json_t *j =
        (xrt_json_t *) XRT_MALLOC(sizeof(xrt_json_t) + (size_t) field_count * sizeof(XrValue));
    if (XR_UNLIKELY(!j)) {
        fprintf(stderr, "xrt_object_new: out of memory\n");
        abort();
    }
    j->field_count = field_count;
    j->object_kind = object_kind;
    j->field_names = NULL;
    j->dynamic_fields = NULL;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
}

static inline XrValue xrt_object_new_named_kind(int64_t field_count, const char *const *field_names,
                                                uint8_t object_kind) {
    XrValue obj = xrt_object_new_kind(field_count, object_kind);
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_count <= 0 || !field_names)
        return obj;
    j->field_names = (const char **) XRT_MALLOC((size_t) field_count * sizeof(const char *));
    if (XR_UNLIKELY(!j->field_names)) {
        fprintf(stderr, "xrt_json_new_named: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < field_count; i++)
        j->field_names[i] = field_names[i] ? field_names[i] : "?";
    return obj;
}

static inline XrValue xrt_json_new(int64_t field_count) {
    return xrt_object_new_kind(field_count, XRT_OBJECT_JSON);
}

static inline XrValue xrt_json_new_named(int64_t field_count, const char *const *field_names) {
    return xrt_object_new_named_kind(field_count, field_names, XRT_OBJECT_JSON);
}

static inline XrValue xrt_record_new(int64_t field_count) {
    return xrt_object_new_kind(field_count, XRT_OBJECT_RECORD);
}

static inline XrValue xrt_record_new_named(int64_t field_count, const char *const *field_names) {
    return xrt_object_new_named_kind(field_count, field_names, XRT_OBJECT_RECORD);
}

static inline int64_t xrt_json_find_field(xrt_json_t *j, const char *name) {
    if (!j || !name || !j->field_names)
        return -1;
    for (int64_t i = 0; i < j->field_count; i++) {
        if (j->field_names[i] && strcmp(j->field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static inline XrValue xrt_json_get_field(XrValue obj, int field_idx) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        return j->fields[field_idx];
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        j->fields[field_idx] = val;
}

static inline XrValue xrt_json_get_name(XrValue obj, const char *name) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return XR_NULL_VAL;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0)
        return j->fields[idx];
    return j->dynamic_fields ? xrt_map_get(j->dynamic_fields, xr_box_str(name)) : XR_NULL_VAL;
}

static inline XrValue xrt_json_get_name_owned(XrValue obj, const char *name) {
    return xrt_value_to_owned(xrt_json_get_name(obj, name));
}

static inline int xrt_json_shape_guard_matches(XrValue obj, int field_idx, const char *name) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name || field_idx < 0)
        return 0;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (j->object_kind != XRT_OBJECT_JSON && j->object_kind != XRT_OBJECT_RECORD)
        return 0;
    if (field_idx >= j->field_count || !j->field_names || !j->field_names[field_idx])
        return 0;
    return strcmp(j->field_names[field_idx], name) == 0;
}

static inline XrValue xrt_json_get_shape_guard_owned(XrValue obj, int field_idx, const char *name) {
    if (xrt_json_shape_guard_matches(obj, field_idx, name))
        return xrt_value_to_owned(xrt_json_get_field(obj, field_idx));
    return xrt_json_get_name_owned(obj, name);
}

static inline XrValue xrt_json_get_computed_key_guard_owned(XrValue obj, XrValue key) {
    if (!XR_IS_STR(key))
        xrt_type_no_index("Json object only supports string keys");
    const char *name = xr_str_data(key);
    if (obj.tag == XR_TAG_PTR && obj.ptr) {
        xrt_json_t *j = (xrt_json_t *) obj.ptr;
        if ((j->object_kind == XRT_OBJECT_JSON || j->object_kind == XRT_OBJECT_RECORD) &&
            j->field_names) {
            int64_t idx = xrt_json_find_field(j, name);
            if (idx >= 0)
                return xrt_value_to_owned(xrt_json_get_field(obj, (int) idx));
        }
    }
    return xrt_json_get_name_owned(obj, name);
}

static inline int xrt_json_has_name(XrValue obj, const char *name) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return 0;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (xrt_json_find_field(j, name) >= 0)
        return 1;
    if (!j->dynamic_fields)
        return 0;
    XrValue key = xr_box_str(name);
    uint32_t hash = xrt_hash32_value(key);
    uint8_t key_tt = xrt_value_type_tag(key);
    return xrt_map_find_entry(j->dynamic_fields, key, hash, key_tt) >= 0;
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
                   XR_IS_STR(value) || XR_IS_ARRAY(value) ||
                   (value.tag == XR_TAG_PTR && value.ptr && value.heap_type == 0);
        case XR_JSON_VALUE_RECORD:
            return value.tag == XR_TAG_PTR && value.ptr && value.heap_type == 0;
        case XR_JSON_VALUE_ARRAY:
            return XR_IS_ARRAY(value);
        case XR_JSON_VALUE_NULL:
        case XR_JSON_VALUE_ANY:
        default:
            return 0;
    }
}

static inline XrValue xrt_json_decode_record(XrValue data, int64_t field_count,
                                             const XrJsonDecodeFieldSpec *fields) {
    if (field_count <= 0 || !fields)
        return XR_NULL_VAL;
    if (data.tag != XR_TAG_PTR || !data.ptr || data.heap_type != 0)
        return XR_NULL_VAL;
    xrt_json_t *src = (xrt_json_t *) data.ptr;
    if (src->object_kind != XRT_OBJECT_JSON && src->object_kind != XRT_OBJECT_RECORD)
        return XR_NULL_VAL;
    XrValue *decoded_values = (XrValue *) XRT_MALLOC((size_t) field_count * sizeof(XrValue));
    const char **field_names = (const char **) XRT_MALLOC((size_t) field_count * sizeof(char *));
    if (XR_UNLIKELY(!decoded_values || !field_names)) {
        fprintf(stderr, "xrt_json_decode_record: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < field_count; i++) {
        const XrJsonDecodeFieldSpec *field = &fields[i];
        const char *name = field->name;
        field_names[i] = name ? name : "?";
        if (!name || !xrt_json_has_name(data, name)) {
            XRT_FREE(decoded_values);
            XRT_FREE(field_names);
            return XR_NULL_VAL;
        }
        XrValue field_value = xrt_json_get_name(data, name);
        if (!xrt_json_value_matches_kind(field_value, field->value_kind)) {
            XRT_FREE(decoded_values);
            XRT_FREE(field_names);
            return XR_NULL_VAL;
        }
        if (xr_json_value_kind_base(field->value_kind) == XR_JSON_VALUE_RECORD &&
            !XR_IS_NULL(field_value)) {
            if (!field->nested_fields || field->nested_field_count == 0) {
                XRT_FREE(decoded_values);
                XRT_FREE(field_names);
                return XR_NULL_VAL;
            }
            XrValue nested = xrt_json_decode_record(field_value, field->nested_field_count,
                                                    field->nested_fields);
            if (XR_IS_NULL(nested)) {
                XRT_FREE(decoded_values);
                XRT_FREE(field_names);
                return XR_NULL_VAL;
            }
            field_value = nested;
        }
        decoded_values[i] = field_value;
    }
    XrValue dstv = xrt_record_new_named(field_count, field_names);
    XRT_FREE(field_names);
    for (int64_t i = 0; i < field_count; i++)
        xrt_json_set_field(dstv, (int) i, decoded_values[i]);
    XRT_FREE(decoded_values);
    return dstv;
}

static inline int64_t xrt_json_static_has(XrValue obj, XrValue key) {
    return XR_IS_STR(key) && xrt_json_has_name(obj, xr_str_data(key)) ? 1 : 0;
}

static inline XrValue xrt_json_static_get(XrValue obj, XrValue key, XrValue fallback) {
    if (!XR_IS_STR(key))
        return fallback;
    const char *name = xr_str_data(key);
    if (!xrt_json_has_name(obj, name))
        return fallback;
    return xrt_json_get_name_owned(obj, name);
}

static inline int64_t xrt_json_static_size(XrValue obj) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr)
        return 0;
    return xrt_json_iter_count((const xrt_json_t *) obj.ptr);
}

static inline int64_t xrt_json_static_is_empty(XrValue obj) {
    return xrt_json_static_size(obj) == 0 ? 1 : 0;
}

typedef struct {
    const char *src;
    const char *end;
    const char *pos;
    int depth;
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

    XrValue str = xrt_str_alloc(len);
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
        if (!xrt_json_parse_value(p, &elem))
            return 0;
        xrt_array_push(arr, elem);
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == ']') {
            p->pos++;
            *out = arr;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',')
            return 0;
        p->pos++;
    }
}

static int xrt_json_parse_object(xrt_json_parser_t *p, XrValue *out) {
    if (p->pos >= p->end || *p->pos != '{')
        return 0;
    p->pos++;
    XrValue obj = xrt_json_new(0);
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
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
        if (!xrt_json_parse_string_value(p, &key))
            return 0;
        xrt_json_parse_skip_ws(p);
        if (p->pos >= p->end || *p->pos != ':')
            return 0;
        p->pos++;
        xrt_json_parse_skip_ws(p);
        if (!xrt_json_parse_value(p, &val))
            return 0;
        if (!j->dynamic_fields) {
            XrValue dyn = xrt_map_new(8);
            j->dynamic_fields = (xrt_map_t *) dyn.ptr;
        }
        xrt_map_set(j->dynamic_fields, key, val);
        xrt_json_parse_skip_ws(p);
        if (p->pos < p->end && *p->pos == '}') {
            p->pos++;
            *out = obj;
            return 1;
        }
        if (p->pos >= p->end || *p->pos != ',')
            return 0;
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

static inline XrValue xrt_json_parse(XrValue text) {
    if (!XR_IS_STR(text))
        return XR_NULL_VAL;
    const char *data = xr_str_data(text);
    int64_t len = xr_str_len(text);
    if (!data || len <= 0)
        return XR_NULL_VAL;
    xrt_json_parser_t p = {.src = data, .end = data + len, .pos = data, .depth = 0};
    XrValue out = XR_NULL_VAL;
    if (!xrt_json_parse_value(&p, &out))
        return XR_NULL_VAL;
    xrt_json_parse_skip_ws(&p);
    return p.pos == p.end ? out : XR_NULL_VAL;
}

static inline XrValue xrt_json_set_name(XrValue obj, const char *name, XrValue val) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return val;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0) {
        j->fields[idx] = val;
        return val;
    }
    if (!j->dynamic_fields) {
        XrValue dyn = xrt_map_new(8);
        j->dynamic_fields = (xrt_map_t *) dyn.ptr;
    }
    xrt_map_set(j->dynamic_fields, xr_box_str(name), val);
    return val;
}

static inline XrValue xrt_json_set_shape_guard(XrValue obj, int field_idx, const char *name,
                                               XrValue val) {
    if (xrt_json_shape_guard_matches(obj, field_idx, name)) {
        xrt_json_set_field(obj, field_idx, val);
        return val;
    }
    return xrt_json_set_name(obj, name, val);
}

static inline XrValue xrt_json_set_computed_key_guard(XrValue obj, XrValue key, XrValue val) {
    if (!XR_IS_STR(key))
        xrt_type_no_index("Json object only supports string keys");
    const char *name = xr_str_data(key);
    if (obj.tag == XR_TAG_PTR && obj.ptr) {
        xrt_json_t *j = (xrt_json_t *) obj.ptr;
        if ((j->object_kind == XRT_OBJECT_JSON || j->object_kind == XRT_OBJECT_RECORD) &&
            j->field_names) {
            int64_t idx = xrt_json_find_field(j, name);
            if (idx >= 0) {
                xrt_json_set_field(obj, (int) idx, val);
                return val;
            }
        }
    }
    return xrt_json_set_name(obj, name, val);
}

static inline XrValue xrt_json_encode_value(XrValue val, int depth);

#define XRT_JSON_ENCODE_MAX_DEPTH 512

static inline void xrt_json_encode_abort(const char *msg, XrValue val) {
    fprintf(stderr, "Json.encode: %s (tag=%u, heap_type=%u)\n", msg ? msg : "unsupported value",
            (unsigned) val.tag, (unsigned) val.heap_type);
    abort();
}

static inline void xrt_json_put_string_key(XrValue obj, XrValue key, XrValue val) {
    if (!XR_IS_STR(key))
        xrt_json_encode_abort("object key must be string", key);
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    const char *name = xr_str_data(key);
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0) {
        j->fields[idx] = val;
        return;
    }
    if (!j->dynamic_fields) {
        XrValue dyn = xrt_map_new(8);
        j->dynamic_fields = (xrt_map_t *) dyn.ptr;
    }
    xrt_map_set(j->dynamic_fields, key, val);
}

static inline XrValue xrt_json_encode_map_key(XrValue key) {
    if (XR_IS_STR(key))
        return key;
    if (XR_IS_INT(key)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(key));
        return xrt_str_from_cstr(buf);
    }
    xrt_json_encode_abort("Map keys must be string or int", key);
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

static inline XrValue xrt_json_encode_object_value(xrt_json_t *src, int depth) {
    if (!src)
        return xrt_json_new(0);
    XrValue dstv = xrt_json_new_named(src->field_count, src->field_names);
    xrt_json_t *dst = (xrt_json_t *) dstv.ptr;
    for (int64_t i = 0; i < src->field_count; i++)
        dst->fields[i] = xrt_json_encode_value(src->fields[i], depth + 1);
    if (src->dynamic_fields) {
        xrt_map_t *m = src->dynamic_fields;
        for (uint32_t i = 0; i < m->nentries; i++) {
            XrMapEntry *entry = &m->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_json_put_string_key(dstv, entry->key,
                                    xrt_json_encode_value(entry->value, depth + 1));
        }
    }
    return dstv;
}

static inline XrValue xrt_json_encode_map_value(xrt_map_t *src, int depth) {
    XrValue dst = xrt_json_new(0);
    if (!src)
        return dst;
    if (xrt_map_is_boolmap(src))
        xrt_json_encode_abort("Map keys must be string or int", XR_FROM_BOOL(1));
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

static inline const XrtTypeDeriveInfo *xrt_json_instance_derive_info(XrValue val) {
    if (val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        return NULL;
    XrObjHeader *hdr = XRT_ARC_HDR(val.ptr);
    const XrtTypeInfo *ti = xrt_type_info(hdr ? hdr->type : 0);
    return ti ? xrt_type_derive_info(ti->type_id) : NULL;
}

static inline XrValue xrt_json_encode_instance_value(XrValue val, int depth) {
    if (val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        xrt_json_encode_abort("cannot encode value to JSON", val);
    const XrtTypeDeriveInfo *di = xrt_json_instance_derive_info(val);
    if (!di || (di->derive_flags & XR_DERIVE_JSON) == 0)
        xrt_json_encode_abort("type does not derive Json", val);
    if (di->inspect_field_count > 0 && !di->inspect_fields)
        xrt_json_encode_abort("derive Json metadata is missing", val);

    XrValue dst = xrt_json_new(0);
    for (uint16_t i = 0; i < di->inspect_field_count; i++) {
        const XrtInspectField *field = &di->inspect_fields[i];
        XrValue key = xr_box_str(field->name ? field->name : "");
        XrValue item = xrt_inspect_field_value(val.ptr, field);
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
            return val;
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
                return xrt_json_encode_object_value((xrt_json_t *) val.ptr, depth);
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
    fprintf(stderr, "Json.stringify: %s (tag=%u, heap_type=%u)\n", msg ? msg : "unsupported value",
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

static void xrt_json_stringify_object_fields(xrt_strbuf_t *sb, xrt_json_t *j, int depth) {
    xrt_json_sb_char(sb, '{');
    int64_t emitted = 0;
    if (j) {
        for (int64_t i = 0; i < j->field_count; i++) {
            if (emitted > 0)
                xrt_json_sb_char(sb, ',');
            const char *name = (j->field_names && j->field_names[i]) ? j->field_names[i] : "";
            xrt_json_stringify_string(sb, name, strlen(name));
            xrt_json_sb_char(sb, ':');
            xrt_json_stringify_value(sb, j->fields[i], depth + 1);
            emitted++;
        }
        if (j->dynamic_fields) {
            xrt_map_t *m = j->dynamic_fields;
            for (uint32_t i = 0; i < m->nentries; i++) {
                XrMapEntry *entry = &m->entries[i];
                if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                    continue;
                if (emitted > 0)
                    xrt_json_sb_char(sb, ',');
                xrt_json_stringify_map_key(sb, entry->key);
                xrt_json_sb_char(sb, ':');
                xrt_json_stringify_value(sb, entry->value, depth + 1);
                emitted++;
            }
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
                xrt_json_stringify_object_fields(sb, (xrt_json_t *) val.ptr, depth);
                return;
            }
            if (val.ptr && val.heap_type == XR_TINSTANCE) {
                const XrtTypeDeriveInfo *di = xrt_json_instance_derive_info(val);
                if (di && (di->derive_flags & XR_DERIVE_JSON) != 0) {
                    xrt_json_stringify_value(sb, xrt_json_encode_instance_value(val, depth + 1),
                                             depth + 1);
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

static inline XrValue xrt_getprop_name(XrValue obj, const char *name) {
    if (obj.tag == XR_TAG_ENUM) {
        if (!name)
            return XR_NULL_VAL;
        if (strcmp(name, "name") == 0)
            return xrt_enum_box_name(obj);
        if (strcmp(name, "ordinal") == 0)
            return xrt_enum_box_ordinal(obj);
    }
    if (XR_IS_MAP(obj)) {
        return xrt_map_get_owned((xrt_map_t *) obj.ptr, xr_box_str(name));
    }
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_json_get_name_owned(obj, name);
    return XR_NULL_VAL;
}

static inline XrValue xrt_setprop_name(XrValue obj, const char *name, XrValue val) {
    if (XR_IS_MAP(obj)) {
        xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str(name), val);
        return val;
    }
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_json_set_name(obj, name, val);
    return val;
}

static inline XrValue xrt_json_clone_for_coro(XrValue val) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return val;
    xrt_json_t *src = (xrt_json_t *) val.ptr;
    XrValue dstv = xrt_object_new_named_kind(src->field_count, src->field_names, src->object_kind);
    xrt_json_t *dst = (xrt_json_t *) dstv.ptr;
    for (int64_t i = 0; i < src->field_count; i++)
        dst->fields[i] = xrt_value_clone_for_coro(src->fields[i]);
    if (src->dynamic_fields) {
        XrValue dyn = xrt_map_new(xrt_map_len(src->dynamic_fields));
        xrt_map_t *dst_map = (xrt_map_t *) dyn.ptr;
        for (uint32_t i = 0; i < src->dynamic_fields->nentries; i++) {
            XrMapEntry *entry = &src->dynamic_fields->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_map_set(dst_map, xrt_value_clone_for_coro(entry->key),
                        xrt_value_clone_for_coro(entry->value));
        }
        dst->dynamic_fields = dst_map;
    }
    return dstv;
}

/* Object spread `{...src}`: copy every field of `src` into `dst`, overwriting
 * existing keys so later spread parts / literal fields win.  AOT json fields
 * are kept alive by escape analysis (the spread source is HEAP_ESCAPE), so no
 * per-value retain is needed — this mirrors how object-literal fields are
 * stored without an explicit dup. */
static inline void xrt_json_merge(XrValue dst_val, XrValue src_val) {
    if (dst_val.tag != XR_TAG_PTR || !dst_val.ptr)
        return;
    if (src_val.tag != XR_TAG_PTR || !src_val.ptr)
        return;
    xrt_json_t *src = (xrt_json_t *) src_val.ptr;
    for (int64_t i = 0; i < src->field_count; i++) {
        const char *name = src->field_names ? src->field_names[i] : NULL;
        if (name)
            xrt_json_set_name(dst_val, name, src->fields[i]);
    }
    if (src->dynamic_fields) {
        xrt_map_t *m = src->dynamic_fields;
        for (uint32_t i = 0; i < m->nentries; i++) {
            XrMapEntry *entry = &m->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            if (XR_IS_STR(entry->key))
                xrt_json_set_name(dst_val, xr_str_data(entry->key), entry->value);
        }
    }
}

/* Sealed Record spread: only overwrite fields that already exist in the target
 * Record shape. Unknown source fields are intentionally ignored here; a Record
 * merge that needs a dynamic source must be lowered through the explicit Json
 * bridge path instead of turning the result into an open Json object. */
static inline void xrt_record_merge(XrValue dst_val, XrValue src_val) {
    if (dst_val.tag != XR_TAG_PTR || !dst_val.ptr)
        return;
    if (src_val.tag != XR_TAG_PTR || !src_val.ptr)
        return;
    xrt_json_t *dst = (xrt_json_t *) dst_val.ptr;
    xrt_json_t *src = (xrt_json_t *) src_val.ptr;
    if (dst->object_kind != XRT_OBJECT_RECORD)
        return;
    for (int64_t i = 0; i < src->field_count; i++) {
        const char *name = src->field_names ? src->field_names[i] : NULL;
        int64_t dst_idx = name ? xrt_json_find_field(dst, name) : -1;
        if (dst_idx >= 0)
            dst->fields[dst_idx] = src->fields[i];
    }
}

static inline void xrt_record_merge_copy_table(XrValue dst_val, XrValue src_val,
                                               int64_t copy_pair_count,
                                               const uint16_t *dst_src_ordinals) {
    if (copy_pair_count <= 0 || !dst_src_ordinals)
        return;
    if (dst_val.tag != XR_TAG_PTR || !dst_val.ptr)
        return;
    if (src_val.tag != XR_TAG_PTR || !src_val.ptr)
        return;
    xrt_json_t *dst = (xrt_json_t *) dst_val.ptr;
    xrt_json_t *src = (xrt_json_t *) src_val.ptr;
    if (dst->object_kind != XRT_OBJECT_RECORD || src->object_kind != XRT_OBJECT_RECORD)
        return;
    for (int64_t i = 0; i < copy_pair_count; i++) {
        uint16_t dst_idx = dst_src_ordinals[i * 2];
        uint16_t src_idx = dst_src_ordinals[i * 2 + 1];
        if ((int64_t) dst_idx < dst->field_count && (int64_t) src_idx < src->field_count)
            dst->fields[dst_idx] = src->fields[src_idx];
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
    xrt_retain(value);
    return xr_mkptr(cell, XR_TAG_CELL);
}

static inline XrValue xrt_cell_get(XrValue cell_value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return cell_value;
    return ((xrt_cell_t *) cell_value.ptr)->value;
}

static inline void xrt_cell_set(XrValue cell_value, XrValue value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    XrValue old = cell->value;
    xrt_retain(value);
    cell->value = value;
    xrt_release(old);
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

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (xrt_value_kind(val)) {
        case XR_TAG_STR:
            return val;
        case XR_TAG_STR_ARC: {
            xrt_str_t *src = (xrt_str_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_str_alloc((size_t) src->len);
            xrt_str_t *dst = (xrt_str_t *) dstv.ptr;
            dst->hash = src->hash;
            dst->rune_len = src->rune_len;
            memcpy(xr_str_buf(dstv), src->data, (size_t) src->len);
            return dstv;
        }
        case XR_TAG_ARRAY: {
            xrt_array_t *src = (xrt_array_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_array_new_typed(src->capacity, src->elem_type);
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
            if (!xrt_map_is_typed(src) && (src->flags & XR_MAP_FLAG_WEAK))
                return xrt_map_new_flags(0, XR_MAP_FLAG_WEAK);
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
            if (!xrt_set_is_typed(src) && (src->flags & XR_SET_FLAG_WEAK))
                return xrt_set_new_flags(0, XR_SET_FLAG_WEAK);
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
        case XR_TAG_AGG_REF: {
            if (!val.ptr)
                return val;
            if (XR_IS_ARRAY_REF(val))
                return xrt_array_ref_to_owned(val);
            uint16_t storage_size = val.heap_type;
            uint32_t size = storage_size ? storage_size : *(uint32_t *) val.ptr;
            if (size == 0 || size > (16u * 1024u * 1024u))
                return val;
            void *dst = xrt_arc_alloc(size);
            memcpy(dst, val.ptr, size);
            return storage_size ? xr_aggregate_ref(dst, storage_size)
                                : xr_mkptr(dst, XR_TAG_AGG_REF);
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
