/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_code_alloc.c - Executable memory allocator for JIT code
 *
 * KEY CONCEPT:
 *   Platform-specific W^X memory management for JIT-compiled code.
 *   macOS Apple Silicon: MAP_JIT + pthread_jit_write_protect_np()
 *   Linux: mmap PROT_READ|PROT_WRITE then mprotect to PROT_READ|PROT_EXEC
 */

#include "xm_code_alloc.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../os/os_codemem.h"
#include <stdlib.h>
#include <string.h>

// ========== Page helpers ==========

static size_t page_align(size_t size) {
    size_t ps = xr_os_page_size();
    return (size + ps - 1) & ~(ps - 1);
}

// Allocate a new code page via the OS shim (W^X-aware).
static XmCodePage *alloc_code_page(size_t min_size) {
    size_t size = min_size < XM_CODE_PAGE_SIZE ? XM_CODE_PAGE_SIZE : page_align(min_size);

    void *mem = xr_os_codemem_alloc(size);
    if (mem == NULL) {
        return NULL;
    }

    XmCodePage *page = (XmCodePage *) xr_malloc(sizeof(XmCodePage));
    if (!page) {
        xr_os_codemem_free(mem, size);
        return NULL;
    }

    page->base = (uint8_t *) mem;
    page->size = size;
    page->used = 0;
    page->next = NULL;

    return page;
}

static void free_code_page(XmCodePage *page) {
    if (page) {
        xr_os_codemem_free(page->base, page->size);
        xr_free(page);
    }
}

/* ========== Public API ========== */

void xm_code_alloc_init(XmCodeAlloc *alloc) {
    XR_DCHECK(alloc != NULL, "code_alloc_init: NULL alloc");
    alloc->pages = NULL;
    alloc->current = NULL;
    alloc->total_allocated = 0;
    alloc->total_used = 0;
    alloc->budget = XM_CODE_CACHE_DEFAULT;
    alloc->n_pages = 0;
    alloc->epoch = 0;
    alloc->garbage = NULL;
}

void xm_code_alloc_destroy(XmCodeAlloc *alloc) {
    XR_DCHECK(alloc != NULL, "code_alloc_destroy: NULL alloc");
    XmCodePage *page = alloc->pages;
    while (page) {
        XmCodePage *next = page->next;
        free_code_page(page);
        page = next;
    }
    alloc->pages = NULL;
    alloc->current = NULL;
    alloc->total_allocated = 0;
    alloc->total_used = 0;
    alloc->n_pages = 0;

    // Free remaining garbage entries (no epoch safety needed at shutdown)
    XmCodeGarbage *g = alloc->garbage;
    while (g) {
        XmCodeGarbage *next = g->next;
        xr_free(g);
        g = next;
    }
    alloc->garbage = NULL;
}

void *xm_code_alloc(XmCodeAlloc *alloc, size_t size, size_t alignment) {
    XR_DCHECK(alloc != NULL, "code_alloc: NULL alloc");
    if (size == 0)
        return NULL;
    if (alignment == 0)
        alignment = 16;

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
    XmCodePage *page = alloc_code_page(needed);
    if (!page)
        return NULL;

    // Budget check: refuse allocation if over budget
    if (alloc->budget > 0 && alloc->total_allocated + page->size > alloc->budget) {
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

void xm_code_make_executable(void *ptr, size_t size) {
    xr_os_codemem_make_executable(ptr, size);
}

void xm_code_make_writable(void *ptr, size_t size) {
    xr_os_codemem_make_writable(ptr, size);
}

void xm_code_alloc_set_budget(XmCodeAlloc *alloc, size_t budget_bytes) {
    XR_DCHECK(alloc != NULL, "code_alloc_set_budget: NULL alloc");
    if (budget_bytes > XM_CODE_CACHE_MAX)
        budget_bytes = XM_CODE_CACHE_MAX;
    alloc->budget = budget_bytes;
}

void xm_code_alloc_retire(XmCodeAlloc *alloc, void *code, size_t size) {
    XR_DCHECK(alloc != NULL, "code_alloc_retire: NULL alloc");
    if (!code || size == 0)
        return;

    XmCodeGarbage *g = (XmCodeGarbage *) xr_malloc(sizeof(XmCodeGarbage));
    if (!g)
        return;  // leak is acceptable under OOM

    g->code = code;
    g->size = size;
    g->retire_epoch = alloc->epoch;
    g->next = alloc->garbage;
    alloc->garbage = g;
}

void xm_code_alloc_reclaim(XmCodeAlloc *alloc, uint64_t safe_epoch) {
    XR_DCHECK(alloc != NULL, "code_alloc_reclaim: NULL alloc");

    XmCodeGarbage **prev = &alloc->garbage;
    XmCodeGarbage *g = alloc->garbage;
    while (g) {
        XmCodeGarbage *next = g->next;
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

void xm_code_flush_icache(void *ptr, size_t size) {
    xr_os_codemem_flush_icache(ptr, size);
}
