/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_arc.h - ARC (Automatic Reference Counting) + bump allocator
 *
 * KEY CONCEPT:
 *   Self-contained memory management for AOT-generated code.
 *   Objects carry an XrtArcHdr before user data for retain/release.
 *   Bump allocator provides allocation-heavy fast path; objects are
 *   freed in bulk by xrt_bump_destroy().
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
    #define XRT_MALLOC(sz)       xr_malloc(sz)
    #define XRT_CALLOC(n, sz)    xr_calloc(n, sz)
    #define XRT_REALLOC(p, sz)   xr_realloc(p, sz)
    #define XRT_FREE(p)          xr_free(p)
  #else
    #define XRT_MALLOC(sz)       malloc(sz)
    #define XRT_CALLOC(n, sz)    calloc(n, sz)
    #define XRT_REALLOC(p, sz)   realloc(p, sz)
    #define XRT_FREE(p)          free(p)
  #endif
#endif

/* =========================================================================
 * ARC (Automatic Reference Counting)
 *
 * Object layout (bytes preceding user data):
 *   [XrtArcHdr][  user data  ]
 *    ^--- hdr pointer (via XRT_ARC_HDR macro)
 *
 * rc = 0 means freed. XRT_ARC_IMMORTAL prevents retain/release.
 * xrt_arc_deinit is called once when rc drops to 0 and HAS_DEINIT is set.
 * ========================================================================= */

typedef struct {
    uint32_t rc; // reference count (non-atomic, single-coroutine)
    uint16_t flags; // XRT_ARC_* flags
    uint16_t type; // object type tag for deinit dispatch
} XrtArcHdr;

#define XRT_ARC_HDR(p)       ((XrtArcHdr *)((char *)(p) - sizeof(XrtArcHdr)))
#define XRT_ARC_IMMORTAL     (1u << 0)
#define XRT_ARC_HAS_DEINIT   (1u << 1)

/* =========================================================================
 * Bump allocator for ARC objects
 *
 * Replaces calloc in xrt_arc_alloc for allocation-heavy workloads.
 * Objects are never individually freed — the entire arena is released
 * at program exit via xrt_bump_destroy().
 *
 * This is always compiled in; xrt_arc_alloc picks the bump path when
 * xrt_bump_enabled is set (default: on). Individual objects still carry
 * an XrtArcHdr so retain/release/deinit semantics are preserved;
 * the actual free() in xrt_arc_release becomes a no-op for bump objects.
 * ========================================================================= */

#define XRT_BUMP_BLOCK_SIZE  (2u * 1024u * 1024u) // 2 MB per block
#define XRT_ARC_BUMP         (1u << 2) // object was bump-allocated

typedef struct XrtBumpBlock {
    struct XrtBumpBlock *next;
    char data[];
} XrtBumpBlock;

#ifdef XRT_IMPL
  char         *xrt_bump_cursor;
  char         *xrt_bump_end;
  XrtBumpBlock *xrt_bump_blocks;
  int           xrt_bump_enabled = 0; // 0 = calloc (safe default); 1 = bump (fast, no per-object free)
#else
  extern char         *xrt_bump_cursor;
  extern char         *xrt_bump_end;
  extern XrtBumpBlock *xrt_bump_blocks;
  extern int           xrt_bump_enabled;
#endif

static void xrt_bump_new_block(size_t min_size) {
    size_t bsize = XRT_BUMP_BLOCK_SIZE;
    if (min_size > bsize) bsize = min_size;
    XrtBumpBlock *b = (XrtBumpBlock *)XRT_MALLOC(sizeof(XrtBumpBlock) + bsize);
    if (!b) { fprintf(stderr, "xrt_bump: out of memory\n"); abort(); }
    b->next = xrt_bump_blocks;
    xrt_bump_blocks = b;
    xrt_bump_cursor = b->data;
    xrt_bump_end    = b->data + bsize;
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
    xrt_bump_blocks  = NULL;
    xrt_bump_cursor  = NULL;
    xrt_bump_end     = NULL;
}

static inline void *xrt_arc_alloc(size_t obj_size) {
    obj_size = (obj_size + 7u) & ~(size_t)7u;
    size_t total = sizeof(XrtArcHdr) + obj_size;
    XrtArcHdr *hdr;
    if (__builtin_expect(xrt_bump_enabled, 1)) {
        hdr = (XrtArcHdr *)xrt_bump_alloc(total);
        memset(hdr, 0, total);
        hdr->flags = XRT_ARC_BUMP; // mark as bump-allocated
    } else {
        hdr = (XrtArcHdr *)XRT_CALLOC(1, total);
        if (!hdr) { fprintf(stderr, "xrt_arc_alloc: out of memory\n"); abort(); }
    }
    hdr->rc = 1;
    return (char *)hdr + sizeof(XrtArcHdr);
}

static inline void *xrt_arc_retain(void *p) {
    if (!p) return NULL;
    XrtArcHdr *h = XRT_ARC_HDR(p);
    if (__builtin_expect(!!(h->flags & XRT_ARC_IMMORTAL), 0)) return p;
    h->rc++;
    return p;
}

// Forward declaration — definition generated per-module by xcgen_emit_struct_deinits()
static void xrt_arc_deinit(void *p, uint16_t type);

static inline void xrt_arc_release(void *p) {
    if (!p) return;
    XrtArcHdr *h = XRT_ARC_HDR(p);
    if (__builtin_expect(!!(h->flags & XRT_ARC_IMMORTAL), 0)) return;
    if (--h->rc == 0) {
        if (h->flags & XRT_ARC_HAS_DEINIT)
            xrt_arc_deinit((char *)h + sizeof(XrtArcHdr), h->type);
        if (!(h->flags & XRT_ARC_BUMP))
            XRT_FREE(h); // bump-allocated objects are freed in bulk by xrt_bump_destroy
    }
}

static inline XrtValue xrt_arc_retain_val(XrtValue v) {
    if ((v.tag == XRT_TAG_PTR || v.tag == XRT_TAG_STR_ARC) && v.ptr)
        xrt_arc_retain(v.ptr);
    return v;
}

static inline void xrt_arc_release_val(XrtValue v) {
    if ((v.tag == XRT_TAG_PTR || v.tag == XRT_TAG_STR_ARC) && v.ptr)
        xrt_arc_release(v.ptr);
}

static inline void xrt_arc_init(void) {
    if (xrt_bump_enabled) xrt_bump_new_block(0);
}

// Allocate a heap string via ARC; xrt_arc_release will free it
static inline XrtValue xrt_str_alloc(size_t len) {
    char *p = (char *)xrt_arc_alloc(len + 1);
    return xrt_mkptr(p, XRT_TAG_STR_ARC);
}

static inline XrtValue xrt_str_concat(const char *sa, const char *sb) {
    size_t la = strlen(sa), lb = strlen(sb);
    XrtValue v = xrt_str_alloc(la + lb);
    char *r = (char *)v.ptr;
    memcpy(r, sa, la);
    memcpy(r + la, sb, lb + 1);
    return v;
}

#endif // XRT_ARC_H
