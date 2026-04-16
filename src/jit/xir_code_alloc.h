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

#define XIR_CODE_PAGE_SIZE (64 * 1024)  // 64KB per page

typedef struct XirCodePage {
    uint8_t *base;              // page base address (mmap'd)
    size_t size;                // total page size
    size_t used;                // bytes used (bump pointer offset)
    struct XirCodePage *next;   // linked list
} XirCodePage;

/* ========== Code Allocator ========== */

typedef struct XirCodeAlloc {
    XirCodePage *pages;         // linked list of allocated pages
    XirCodePage *current;       // current page for bump allocation
    size_t total_allocated;     // total bytes mmap'd
    size_t total_used;          // total bytes of JIT code
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

/* ========== Statistics ========== */

static inline size_t xir_code_alloc_total(XirCodeAlloc *alloc) {
    return alloc->total_allocated;
}

static inline size_t xir_code_alloc_used(XirCodeAlloc *alloc) {
    return alloc->total_used;
}

#endif // XIR_CODE_ALLOC_H
