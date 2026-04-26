/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_code_alloc.h - Executable memory allocator for JIT code
 *
 * KEY CONCEPT:
 *   Allocates RWX memory pages for JIT-compiled machine code.
 *   Uses bump-pointer within pages, page-granularity free.
 *
 * WHY THIS DESIGN:
 *   - macOS Apple Silicon requires MAP_JIT + pthread_jit_write_protect_np()
 *   - Linux uses mmap + mprotect for W^X
 *   - Simple bump allocator is sufficient for JIT (code is rarely freed individually)
 */

#ifndef XIR_CODE_ALLOC_H
#define XIR_CODE_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

/* ========== Code Page ========== */

#define XIR_CODE_PAGE_SIZE (64 * 1024)             // 64KB per page
#define XIR_CODE_CACHE_DEFAULT (64 * 1024 * 1024)  // 64MB default budget
#define XIR_CODE_CACHE_MAX (256 * 1024 * 1024)     // 256MB hard ceiling

typedef struct XirCodePage {
    uint8_t *base;             // page base address (mmap'd)
    size_t size;               // total page size
    size_t used;               // bytes used (bump pointer offset)
    struct XirCodePage *next;  // linked list
} XirCodePage;

/* ========== Code Garbage (Epoch-based reclaim) ========== */

typedef struct XirCodeGarbage {
    void *code;             // retired code pointer
    size_t size;            // code size (for accounting)
    uint64_t retire_epoch;  // epoch when code was retired
    struct XirCodeGarbage *next;
} XirCodeGarbage;

/* ========== Code Allocator ========== */

typedef struct XirCodeAlloc {
    XirCodePage *pages;       // linked list of allocated pages
    XirCodePage *current;     // current page for bump allocation
    size_t total_allocated;   // total bytes mmap'd
    size_t total_used;        // total bytes of JIT code
    size_t budget;            // max bytes allowed (0 = unlimited)
    uint32_t n_pages;         // page count
    uint64_t epoch;           // monotonically increasing epoch (bumped at safepoints)
    XirCodeGarbage *garbage;  // retired code awaiting safe reclaim
} XirCodeAlloc;

/* ========== API ========== */

// Initialize the code allocator
XR_FUNC void xir_code_alloc_init(XirCodeAlloc *alloc);

// Destroy: unmap all pages
XR_FUNC void xir_code_alloc_destroy(XirCodeAlloc *alloc);

// Allocate executable memory for JIT code
// Returns writable pointer. Call xir_code_alloc_finalize() after writing.
// alignment: 0 or power of 2 (0 defaults to 16-byte alignment)
XR_FUNC void *xir_code_alloc(XirCodeAlloc *alloc, size_t size, size_t alignment);

// Switch page from writable to executable (W^X transition)
// On macOS: pthread_jit_write_protect_np(true)
// On Linux: mprotect(PROT_READ | PROT_EXEC)
XR_FUNC void xir_code_make_executable(void *ptr, size_t size);

// Switch page to writable for patching (e.g., deopt, IC update)
// On macOS: pthread_jit_write_protect_np(false)
// On Linux: mprotect(PROT_READ | PROT_WRITE)
XR_FUNC void xir_code_make_writable(void *ptr, size_t size);

// Flush instruction cache after writing code
// Required on ARM64 (unified cache not guaranteed)
XR_FUNC void xir_code_flush_icache(void *ptr, size_t size);

// Set the code cache budget in bytes (0 = unlimited, clamped to XIR_CODE_CACHE_MAX).
XR_FUNC void xir_code_alloc_set_budget(XirCodeAlloc *alloc, size_t budget_bytes);

// Returns true if total_allocated exceeds the budget.
static inline bool xir_code_alloc_over_budget(XirCodeAlloc *alloc) {
    return alloc->budget > 0 && alloc->total_allocated > alloc->budget;
}

// Bump the global epoch (call at GC safepoints).
static inline void xir_code_alloc_bump_epoch(XirCodeAlloc *alloc) {
    alloc->epoch++;
}

// Retire old code: adds to garbage list for later safe reclaim.
// The code is NOT freed immediately; it stays valid until all coroutines
// have passed the retire_epoch.
XR_FUNC void xir_code_alloc_retire(XirCodeAlloc *alloc, void *code, size_t size);

// Reclaim garbage entries whose retire_epoch < safe_epoch.
// safe_epoch = min(saved_epoch) across all active coroutines.
XR_FUNC void xir_code_alloc_reclaim(XirCodeAlloc *alloc, uint64_t safe_epoch);

/* ========== Statistics ========== */

static inline size_t xir_code_alloc_total(XirCodeAlloc *alloc) {
    return alloc->total_allocated;
}

static inline size_t xir_code_alloc_used(XirCodeAlloc *alloc) {
    return alloc->total_used;
}

#endif  // XIR_CODE_ALLOC_H
