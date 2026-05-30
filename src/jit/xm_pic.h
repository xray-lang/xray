/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pic.h - Polymorphic inline cache data structures and API
 *
 * PIC entries cache type-to-method mappings for polymorphic call
 * sites (> 1 receiver type but <= PIC_MAX_ENTRIES).  When the cache
 * is full, the site becomes megamorphic and falls back to vtable lookup.
 *
 * The JIT stub generator emits a PIC dispatch sequence:
 *   load receiver type → compare against cached entries → branch to
 *   target or fall through to slow-path vcall.
 *
 * Thread safety: PIC entries are updated atomically by the background
 * JIT thread; the main thread reads them with relaxed ordering since
 * a stale read is benign (just triggers the slow path).
 */

#ifndef XM_PIC_H
#define XM_PIC_H

#include "../base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct XrICMethod XrICMethod;

/* Maximum entries per PIC before degrading to megamorphic. */
#define XM_PIC_MAX_ENTRIES 8

/* A single cached type → method pointer mapping. */
typedef struct XmPicEntry {
    uint32_t type_id;       /* XrClass type ID (0 = empty slot) */
    uint32_t method_offset; /* method index in class vtable */
    void *code_ptr;         /* JIT compiled method entry, or NULL for interpreted */
} XmPicEntry;

/* PIC state for a single call site. */
typedef enum {
    XM_PIC_EMPTY,       /* no entries yet */
    XM_PIC_MONOMORPHIC, /* exactly 1 type seen */
    XM_PIC_POLYMORPHIC, /* 2..PIC_MAX_ENTRIES types */
    XM_PIC_MEGAMORPHIC, /* overflow — use vtable lookup */
} XmPicState;

/* PIC structure embedded in JIT metadata per call site. */
typedef struct XmPic {
    XmPicEntry entries[XM_PIC_MAX_ENTRIES];
    uint8_t nentries; /* current count (0..PIC_MAX_ENTRIES) */
    uint8_t state;    /* XmPicState */
    uint16_t _pad;
    uint32_t miss_count; /* slow-path fallback count */
    uint32_t hit_count;  /* fast-path hit count */
} XmPic;

/* Initialize a PIC to empty state. */
XR_FUNC void xm_pic_init(XmPic *pic);

/* Record a type hit.  Returns true if the entry was found in the cache
 * (fast path), false if it was a miss (added or megamorphic). */
XR_FUNC bool xm_pic_record(XmPic *pic, uint32_t type_id, uint32_t method_offset, void *code_ptr);

/* Look up a type in the PIC.  Returns the code pointer if found,
 * NULL if not cached (caller should use slow-path vtable lookup). */
XR_FUNC void *xm_pic_lookup(const XmPic *pic, uint32_t type_id);

/* Reset PIC to empty (e.g., on GC type invalidation). */
XR_FUNC void xm_pic_reset(XmPic *pic);

/* Populate a PIC snapshot from a VM method IC table entry. */
XR_FUNC void xm_pic_import_ic_method(XmPic *pic, const XrICMethod *ic);

#endif /* XM_PIC_H */
