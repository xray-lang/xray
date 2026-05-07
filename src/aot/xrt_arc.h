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
} XrtArcHdr;

#define XRT_ARC_HDR(p) ((XrtArcHdr *) ((char *) (p) - sizeof(XrtArcHdr)))
#define XRT_ARC_HAS_DEINIT (1u << 1)

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
    if (!b) {
        fprintf(stderr, "xrt_bump: out of memory\n");
        abort();
    }
    b->next = xrt_bump_blocks;
    xrt_bump_blocks = b;
    xrt_bump_cursor = b->data;
    xrt_bump_end = b->data + bsize;
}

static inline void *xrt_bump_alloc(size_t size) {
    if (__builtin_expect(xrt_bump_cursor + size <= xrt_bump_end, 1)) {
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

static inline void *xrt_arc_alloc(size_t obj_size) {
    obj_size = (obj_size + 7u) & ~(size_t) 7u;
    size_t total = sizeof(XrtArcHdr) + obj_size;
    XrtArcHdr *hdr;
    if (__builtin_expect(xrt_bump_enabled, 1)) {
        hdr = (XrtArcHdr *) xrt_bump_alloc(total);
        memset(hdr, 0, total);
        hdr->flags = XRT_ARC_BUMP;  // mark as bump-allocated
    } else {
        hdr = (XrtArcHdr *) XRT_CALLOC(1, total);
        if (!hdr) {
            fprintf(stderr, "xrt_arc_alloc: out of memory\n");
            abort();
        }
    }
    return (char *) hdr + sizeof(XrtArcHdr);
}

/* ARC retain: increment refcount.
 * Called by generated code for values with escape > NO_ESCAPE.
 * No-op for: NULL pointers, scalar tags, bump-allocated objects. */
static inline void xrt_retain(XrValue v) {
    if (v.tag == XR_TAG_I64 || v.tag == XR_TAG_F64 ||
        v.tag == XR_TAG_BOOL || v.tag == XR_TAG_NULL)
        return;  /* scalars have no header */
    if (!v.ptr) return;
    XrtArcHdr *hdr = XRT_ARC_HDR(v.ptr);
    if (hdr->flags & XRT_ARC_BUMP) return;  /* bump objects: freed in bulk */
    hdr->refcount++;
}

/* ARC release: decrement refcount, free on zero.
 * No-op for: NULL pointers, scalar tags, bump-allocated objects. */
static inline void xrt_release(XrValue v) {
    if (v.tag == XR_TAG_I64 || v.tag == XR_TAG_F64 ||
        v.tag == XR_TAG_BOOL || v.tag == XR_TAG_NULL)
        return;
    if (!v.ptr) return;
    XrtArcHdr *hdr = XRT_ARC_HDR(v.ptr);
    if (hdr->flags & XRT_ARC_BUMP) return;
    if (--hdr->refcount <= 0) {
        /* TODO: call type-specific destructor if XRT_ARC_HAS_DEINIT */
        XRT_FREE(hdr);
    }
}

static inline void xrt_arc_init(void) {
    if (xrt_bump_enabled)
        xrt_bump_new_block(0);
}

// Allocate a heap string via bump allocator
static inline XrValue xrt_str_alloc(size_t len) {
    char *p = (char *) xrt_arc_alloc(len + 1);
    return xr_mkptr(p, XR_TAG_STR_ARC);
}

static inline XrValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    XrValue v = xrt_str_alloc(la + lb);
    char *r = (char *) v.ptr;
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return v;
}

#endif  // XRT_ARC_H
