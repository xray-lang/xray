/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_code_alloc.c - Executable memory allocator for JIT code
 *
 * KEY CONCEPT:
 *   Platform-specific W^X memory management for JIT-compiled code.
 *   macOS Apple Silicon: MAP_JIT + pthread_jit_write_protect_np()
 *   Linux: mmap PROT_READ|PROT_WRITE then mprotect to PROT_READ|PROT_EXEC
 */

#include "xir_code_alloc.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif // ========== Platform Helpers ==========

static size_t get_page_size(void) {
    static size_t ps = 0;
    if (ps == 0) {
        ps = (size_t)sysconf(_SC_PAGESIZE);
    }
    return ps;
}

// Round up to page boundary
static size_t page_align(size_t size) {
    size_t ps = get_page_size();
    return (size + ps - 1) & ~(ps - 1);
}

// Allocate a new code page via mmap
static XirCodePage *alloc_code_page(size_t min_size) {
    size_t size = min_size < XIR_CODE_PAGE_SIZE ? XIR_CODE_PAGE_SIZE : page_align(min_size);

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int prot;

#ifdef __APPLE__
    // macOS Apple Silicon: MAP_JIT requires RWX in mmap, then
    // pthread_jit_write_protect_np() toggles between W and X at runtime
    flags |= MAP_JIT;
    prot = PROT_READ | PROT_WRITE | PROT_EXEC;
#else
    prot = PROT_READ | PROT_WRITE;
#endif

    void *mem = mmap(NULL, size, prot, flags, -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }

    XirCodePage *page = (XirCodePage *)xr_malloc(sizeof(XirCodePage));
    if (!page) {
        munmap(mem, size);
        return NULL;
    }

    page->base = (uint8_t *)mem;
    page->size = size;
    page->used = 0;
    page->next = NULL;

    return page;
}

// Free a code page
static void free_code_page(XirCodePage *page) {
    if (page) {
        munmap(page->base, page->size);
        xr_free(page);
    }
}

/* ========== Public API ========== */

void xir_code_alloc_init(XirCodeAlloc *alloc) {
    XR_DCHECK(alloc != NULL, "code_alloc_init: NULL alloc");
    alloc->pages = NULL;
    alloc->current = NULL;
    alloc->total_allocated = 0;
    alloc->total_used = 0;
    alloc->budget = XIR_CODE_CACHE_DEFAULT;
    alloc->n_pages = 0;
    alloc->epoch = 0;
    alloc->garbage = NULL;
}

void xir_code_alloc_destroy(XirCodeAlloc *alloc) {
    XR_DCHECK(alloc != NULL, "code_alloc_destroy: NULL alloc");
    XirCodePage *page = alloc->pages;
    while (page) {
        XirCodePage *next = page->next;
        free_code_page(page);
        page = next;
    }
    alloc->pages = NULL;
    alloc->current = NULL;
    alloc->total_allocated = 0;
    alloc->total_used = 0;
    alloc->n_pages = 0;

    // Free remaining garbage entries (no epoch safety needed at shutdown)
    XirCodeGarbage *g = alloc->garbage;
    while (g) {
        XirCodeGarbage *next = g->next;
        xr_free(g);
        g = next;
    }
    alloc->garbage = NULL;
}

void *xir_code_alloc(XirCodeAlloc *alloc, size_t size, size_t alignment) {
    XR_DCHECK(alloc != NULL, "code_alloc: NULL alloc");
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 16;

    // Try current page first
    if (alloc->current) {
        size_t offset = alloc->current->used;
        // Align offset
        offset = (offset + alignment - 1) & ~(alignment - 1);
        if (offset + size <= alloc->current->size) {
            void *ptr = alloc->current->base + offset;
            alloc->current->used = offset + size;
            alloc->total_used += size;
            return ptr;
        }
    }

    // Need a new page
    size_t needed = size + alignment;  // worst case alignment waste
    XirCodePage *page = alloc_code_page(needed);
    if (!page) return NULL;

    // Budget check: refuse allocation if over budget
    if (alloc->budget > 0 &&
        alloc->total_allocated + page->size > alloc->budget) {
        free_code_page(page);
        return NULL;
    }

    // Link into page list
    page->next = alloc->pages;
    alloc->pages = page;
    alloc->current = page;
    alloc->total_allocated += page->size;
    alloc->n_pages++;

    // Allocate from new page (already aligned at 0)
    size_t offset = (0 + alignment - 1) & ~(alignment - 1);
    void *ptr = page->base + offset;
    page->used = offset + size;
    alloc->total_used += size;
    return ptr;
}

void xir_code_make_executable(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
#ifdef __APPLE__
    // macOS: per-thread W^X switch (MAP_JIT pages)
    pthread_jit_write_protect_np(1);  // enable execute, disable write
#else
    // Linux: per-page mprotect
    size_t ps = get_page_size();
    void *page_start = (void *)((uintptr_t)ptr & ~(ps - 1));
    size_t total = ((uintptr_t)ptr + size) - (uintptr_t)page_start;
    total = page_align(total);
    mprotect(page_start, total, PROT_READ | PROT_EXEC);
#endif
}

void xir_code_make_writable(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
#ifdef __APPLE__
    // macOS: per-thread W^X switch (MAP_JIT pages)
    pthread_jit_write_protect_np(0);  // enable write, disable execute
#else
    // Linux: per-page mprotect
    size_t ps = get_page_size();
    void *page_start = (void *)((uintptr_t)ptr & ~(ps - 1));
    size_t total = ((uintptr_t)ptr + size) - (uintptr_t)page_start;
    total = page_align(total);
    mprotect(page_start, total, PROT_READ | PROT_WRITE);
#endif
}

void xir_code_alloc_set_budget(XirCodeAlloc *alloc, size_t budget_bytes) {
    XR_DCHECK(alloc != NULL, "code_alloc_set_budget: NULL alloc");
    if (budget_bytes > XIR_CODE_CACHE_MAX)
        budget_bytes = XIR_CODE_CACHE_MAX;
    alloc->budget = budget_bytes;
}

void xir_code_alloc_retire(XirCodeAlloc *alloc, void *code, size_t size) {
    XR_DCHECK(alloc != NULL, "code_alloc_retire: NULL alloc");
    if (!code || size == 0) return;

    XirCodeGarbage *g = (XirCodeGarbage *)xr_malloc(sizeof(XirCodeGarbage));
    if (!g) return;  // leak is acceptable under OOM

    g->code = code;
    g->size = size;
    g->retire_epoch = alloc->epoch;
    g->next = alloc->garbage;
    alloc->garbage = g;
}

void xir_code_alloc_reclaim(XirCodeAlloc *alloc, uint64_t safe_epoch) {
    XR_DCHECK(alloc != NULL, "code_alloc_reclaim: NULL alloc");

    XirCodeGarbage **prev = &alloc->garbage;
    XirCodeGarbage *g = alloc->garbage;
    while (g) {
        XirCodeGarbage *next = g->next;
        if (g->retire_epoch < safe_epoch) {
            // All coroutines have moved past this epoch; code is unreachable.
            // NOTE: bump allocator cannot release individual ranges, so we
            // only track the accounting.  The actual pages are freed when
            // the entire code allocator is destroyed.
            if (alloc->total_used >= g->size)
                alloc->total_used -= g->size;
            *prev = next;
            xr_free(g);
        } else {
            prev = &g->next;
        }
        g = next;
    }
}

void xir_code_flush_icache(void *ptr, size_t size) {
#ifdef __APPLE__
    // macOS: sys_icache_invalidate (ARM64 requires this)
    sys_icache_invalidate(ptr, size);
#elif defined(__aarch64__)
    // Linux ARM64: manual cache flush
    uint64_t start = (uint64_t)ptr;
    uint64_t end = start + size;
    // Clean data cache
    for (uint64_t addr = start & ~63ULL; addr < end; addr += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(addr));
    }
    __asm__ volatile("dsb ish");
    // Invalidate instruction cache
    for (uint64_t addr = start & ~63ULL; addr < end; addr += 64) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr));
    }
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
#else
    // x86_64: coherent i-cache, no flush needed
    (void)ptr;
    (void)size;
#endif
}
