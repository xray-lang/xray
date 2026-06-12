/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_arc.h - Bump allocator for AOT-generated code
 *
 * KEY CONCEPT:
 *   Self-contained memory management for AOT-generated code.
 *   Objects carry an XrtArcHdr before user data for type tracking.
 *   The bump allocator provides a fast allocation path; all objects
 *   are freed in bulk by xrt_bump_destroy() at program exit.
 */

#ifndef XRT_ARC_H
#define XRT_ARC_H

#include "xrt_value.h"

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
 * _adN caches assert it via XR_ASSUME_ALIGNED(..., XRT_DATA_ALIGN).
 * Buffers with this contract MUST be allocated with XRT_ALLOC_ALIGNED and
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
 * Object header — precedes every bump-allocated object.
 *
 * Layout: [XrtArcHdr][  user data  ]
 *          ^--- hdr pointer (via XRT_ARC_HDR macro)
 *
 * The `type` field records the class/struct type ID for runtime dispatch
 * (e.g. xrt_type_table lookup). The `flags` field carries allocation
 * metadata (bump vs heap).
 * ========================================================================= */

typedef struct {
    uint16_t flags;    // XRT_ARC_* flags
    uint16_t type;     // object type tag for type-table dispatch
    int32_t refcount;  // ARC reference count (0 = unmanaged, >0 = live)
    uint64_t _pad;     // keeps sizeof == 16 so user data stays 16-byte aligned
} XrtArcHdr;

#define XRT_ARC_HDR(p) ((XrtArcHdr *) ((char *) (p) - sizeof(XrtArcHdr)))
#define XRT_ARC_HAS_DEINIT (1u << 1)

/* Type-specific destructor dispatch. Defined in xrt_class.h (which owns the
 * type table), forward-declared here because xrt_release (L1) runs before
 * the table type is visible (L5). Runs the object's destructor if its type
 * registered one; no-op otherwise. */
static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj);

/* =========================================================================
 * Bump allocator
 *
 * Primary allocation path for AOT-generated code. Objects are never
 * individually freed — the entire arena is released at program exit
 * via xrt_bump_destroy(). Each object carries an XrtArcHdr for type
 * tracking. When xrt_bump_enabled is 0, falls back to calloc/free.
 * ========================================================================= */

#define XRT_BUMP_BLOCK_SIZE (2u * 1024u * 1024u)  // 2 MB per block
#define XRT_ARC_BUMP (1u << 2)                    // bump-allocated (skip individual free)

typedef struct XrtBumpBlock {
    struct XrtBumpBlock *next;
    uint64_t _pad;  // data starts at offset 16: malloc base is 16-aligned, so data is too
    char data[];
} XrtBumpBlock;

#ifdef XRT_IMPL
char *xrt_bump_cursor;
char *xrt_bump_end;
XrtBumpBlock *xrt_bump_blocks;
int xrt_bump_enabled = 0;  // 0 = calloc (safe default); 1 = bump (fast, no per-object free)
#else
extern char *xrt_bump_cursor;
extern char *xrt_bump_end;
extern XrtBumpBlock *xrt_bump_blocks;
extern int xrt_bump_enabled;
#endif

static void xrt_bump_new_block(size_t min_size) {
    size_t bsize = XRT_BUMP_BLOCK_SIZE;
    if (min_size > bsize)
        bsize = min_size;
    XrtBumpBlock *b = (XrtBumpBlock *) XRT_MALLOC(sizeof(XrtBumpBlock) + bsize);
    if (XR_UNLIKELY(!b)) {
        fprintf(stderr, "xrt_bump: out of memory\n");
        abort();
    }
    b->next = xrt_bump_blocks;
    xrt_bump_blocks = b;
    xrt_bump_cursor = b->data;
    xrt_bump_end = b->data + bsize;
}

static inline void *xrt_bump_alloc(size_t size) {
    if (XR_LIKELY(xrt_bump_cursor + size <= xrt_bump_end)) {
        void *p = xrt_bump_cursor;
        xrt_bump_cursor += size;
        return p;
    }
    xrt_bump_new_block(size);
    void *p = xrt_bump_cursor;
    xrt_bump_cursor += size;
    return p;
}

static void xrt_bump_destroy(void) {
    XrtBumpBlock *b = xrt_bump_blocks;
    while (b) {
        XrtBumpBlock *next = b->next;
        XRT_FREE(b);
        b = next;
    }
    xrt_bump_blocks = NULL;
    xrt_bump_cursor = NULL;
    xrt_bump_end = NULL;
}

/* Alignment contract: returned user pointers are 16-byte aligned.
 * Bump path: block data starts 16-aligned and every allocation advances the
 * cursor by a multiple of 16. Heap path: calloc returns max_align_t (>= 16
 * on 64-bit). XR_ASSUME_ALIGNED hints in generated code rely on this. */
static inline void *xrt_arc_alloc(size_t obj_size) {
    obj_size = (obj_size + 15u) & ~(size_t) 15u;
    size_t total = sizeof(XrtArcHdr) + obj_size;
    XrtArcHdr *hdr;
    if (XR_LIKELY(xrt_bump_enabled)) {
        hdr = (XrtArcHdr *) xrt_bump_alloc(total);
        memset(hdr, 0, total);
        hdr->flags = XRT_ARC_BUMP;  // mark as bump-allocated
    } else {
        hdr = (XrtArcHdr *) XRT_CALLOC(1, total);
        if (XR_UNLIKELY(!hdr)) {
            fprintf(stderr, "xrt_arc_alloc: out of memory\n");
            abort();
        }
    }
    return (char *) hdr + sizeof(XrtArcHdr);
}

static inline int xrt_arc_value_has_header(XrValue v) {
    if (!v.ptr)
        return 0;
    return v.tag == XR_TAG_STR_ARC || v.tag == XR_TAG_CLOSURE || v.tag == XR_TAG_CELL ||
           v.tag == XR_TAG_STRUCT_REF;
}

/* ARC retain: increment refcount.
 * Called by generated code for values with escape > NO_ESCAPE.
 * No-op for values that do not carry an XrtArcHdr. */
static inline void xrt_retain(XrValue v) {
    if (!xrt_arc_value_has_header(v))
        return;
    XrtArcHdr *hdr = XRT_ARC_HDR(v.ptr);
    if (hdr->flags & XRT_ARC_BUMP)
        return; /* bump objects: freed in bulk */
    hdr->refcount++;
}

/* ARC release: decrement refcount, free on zero.
 * No-op for values that do not carry an XrtArcHdr. */
static inline void xrt_release(XrValue v) {
    if (!xrt_arc_value_has_header(v))
        return;
    XrtArcHdr *hdr = XRT_ARC_HDR(v.ptr);
    if (hdr->flags & XRT_ARC_BUMP)
        return;
    if (--hdr->refcount <= 0) {
        /* Run the type's destructor (closes resources / frees side buffers)
         * before releasing the block. No-op if the type has no destructor. */
        if (hdr->flags & XRT_ARC_HAS_DEINIT)
            xrt_dispatch_destructor(hdr->type, v.ptr);
        XRT_FREE(hdr);
    }
}

/* Drop-Reuse: drop an object and return its memory for immediate reuse
 * if it was the last reference. Returns raw pointer (reuse token) or NULL. */
static inline void *xrt_drop_reuse(XrValue v) {
    if (!xrt_arc_value_has_header(v))
        return NULL;
    XrtArcHdr *hdr = XRT_ARC_HDR(v.ptr);
    if (hdr->flags & XRT_ARC_BUMP)
        return NULL;
    if (--hdr->refcount <= 0) {
        if (hdr->flags & XRT_ARC_HAS_DEINIT)
            xrt_dispatch_destructor(hdr->type, v.ptr);
        /* Return the header for reuse instead of freeing. */
        return (void *) hdr;
    }
    return NULL; /* still alive */
}

/* Allocate using a reuse token if non-NULL, else fall back to fresh alloc.
 * gc_type and size are compile-time constants from the reuse pass. */
static inline void *xrt_alloc_at(void *token, unsigned gc_type, unsigned size) {
    (void) gc_type;
    if (token) {
        /* Reinitialize the header for the new object. */
        XrtArcHdr *hdr = (XrtArcHdr *) token;
        hdr->refcount = 1;
        hdr->flags = 0;
        hdr->type = (uint16_t) gc_type;
        return (void *) (hdr + 1);
    }
    return xrt_arc_alloc(size);
}

static inline void xrt_arc_init(void) {
    if (xrt_bump_enabled)
        xrt_bump_new_block(0);
}

/* =========================================================================
 * String constructors — header + bytes in one bump block.
 * Layout: [XrtArcHdr][xrt_str_t][len+1 bytes]; data points at the tail.
 * Callers fill the bytes via xr_str_buf() after xrt_str_alloc().
 * ========================================================================= */

static inline XrValue xrt_str_alloc(size_t len) {
    xrt_str_t *h = (xrt_str_t *) xrt_arc_alloc(sizeof(xrt_str_t) + len + 1);
    h->len = (int64_t) len;
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
