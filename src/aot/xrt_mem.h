/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_mem.h - Freestanding AOT wrappers for the mem module.
 *
 * These mirror the VM bindings in stdlib/mem/mem.c for memory-management and
 * raw-memory operations. Exact-width integer bit operations are compiler Xi
 * intrinsics and intentionally have no runtime entry points here.
 */

#ifndef XRT_MEM_H
#define XRT_MEM_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#endif

#include "xrt_value.h"
#include "xrt_arc.h"
#include "xrt_coll.h"
#include "../shared/xr_arith_core.h"
#include "../shared/xr_sync_core.h"

#define XRT_MEM_PROT_NONE 0
#define XRT_MEM_PROT_READ 1
#define XRT_MEM_PROT_WRITE 2
#define XRT_MEM_PROT_EXEC 4

static inline int64_t xrt_mem_int_arg(XrValue v) {
    return XR_IS_INT(v) ? XR_TO_INT(v) : 0;
}

static inline int xrt_buffer_is(XrValue v) {
    return v.tag == XR_TAG_BUFFER && v.ptr != NULL;
}

static inline xrt_buffer_object_t *xrt_buffer_obj_ptr(XrValue v) {
    return xrt_buffer_is(v) ? (xrt_buffer_object_t *) v.ptr : NULL;
}

static inline int64_t xrt_buffer_length(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    return buf ? buf->length : 0;
}

static inline XrValue xrt_buffer_box(xrt_buffer_object_t *buf) {
    return buf ? xr_mkptr(buf, XR_TAG_BUFFER) : XR_NULL_VAL;
}

static inline void *xrt_buffer_alloc_aligned(size_t size, size_t align) {
    if (align == 0)
        return XRT_MALLOC(size);
    if (align < sizeof(void *) || (align & (align - 1u)) != 0) {
        fprintf(stderr, "mem.allocAligned: align must be a power of two >= sizeof(void*)\n");
        abort();
    }
#if defined(XRT_USE_XR_MALLOC)
    return xr_malloc_aligned(size, align);
#elif defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0)
        return NULL;
    return p;
#endif
}

static inline XrValue xrt_buffer_new(int64_t length, int zeroed, size_t align) {
    if (length < 0)
        length = 0;
    xrt_buffer_object_t *buf = (xrt_buffer_object_t *) xrt_arc_alloc(sizeof(*buf));
    buf->data = NULL;
    buf->length = length;
    buf->align = align;
    if (length > 0) {
        size_t size = (size_t) length;
        buf->data = align ? xrt_buffer_alloc_aligned(size, align)
                          : (zeroed ? XRT_CALLOC(1, size) : XRT_MALLOC(size));
        if (!buf->data) {
            fprintf(stderr, "mem.alloc: out of memory\n");
            abort();
        }
        if (zeroed && align)
            memset(buf->data, 0, size);
    }
    xrt_arc_mark_builtin(buf, XRT_ARC_KIND_BUFFER);
    return xrt_buffer_box(buf);
}

static inline xr_span_t xrt_buffer_bytes_view(XrValue value, int readonly) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    if (!buf)
        return xrt_span_empty();
    xr_span_t out = {0};
    out.data = buf->data;
    out.length = buf->length;
    out.guard = buf;
    out.elem_type = XR_ELEM_U8;
    out.elem_size = 1;
    out.elem_tid = 0;
    out.contains_refs = 0;
    out.flags = readonly ? XRT_SLICE_FLAG_READONLY : 0;
    return out;
}

static inline xr_span_t xrt_buffer_as_bytes(XrValue value) {
    return xrt_buffer_bytes_view(value, 1);
}

static inline xr_span_t xrt_buffer_as_mut_bytes(XrValue value) {
    return xrt_buffer_bytes_view(value, 0);
}

static inline XrValue xrt_buffer_borrow_ptr(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    return xr_mkptr(buf ? buf->data : NULL, XR_TAG_PTR);
}

static inline XrValue xrt_buffer_resize(XrValue value, XrValue size_value) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(value);
    int64_t new_len = xrt_mem_int_arg(size_value);
    if (!buf || new_len < 0)
        return XR_FALSE_VAL;
    if (new_len == 0) {
        xrt_buffer_free_data(buf->data, buf->align);
        buf->data = NULL;
        buf->length = 0;
        buf->align = 0;
        return XR_TRUE_VAL;
    }
    if (buf->align) {
        void *new_data = xrt_buffer_alloc_aligned((size_t) new_len, buf->align);
        if (!new_data)
            return XR_FALSE_VAL;
        size_t copy = (size_t) ((buf->length < new_len) ? buf->length : new_len);
        if (copy > 0 && buf->data)
            memcpy(new_data, buf->data, copy);
        xrt_buffer_free_data(buf->data, buf->align);
        buf->data = new_data;
    } else {
        void *new_data = XRT_REALLOC(buf->data, (size_t) new_len);
        if (!new_data)
            return XR_FALSE_VAL;
        buf->data = new_data;
    }
    buf->length = new_len;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_buffer_method_0(XrValue recv, int sym) {
    xrt_buffer_object_t *buf = xrt_buffer_obj_ptr(recv);
    if (!buf)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(buf->length);
    if (sym == XRT_SYM_BORROW_PTR)
        return xrt_buffer_borrow_ptr(recv);
    return XR_NULL_VAL;
}

static inline XrValue xrt_buffer_method_1(XrValue recv, int sym, XrValue arg0) {
    if (sym == XRT_SYM_RESIZE)
        return xrt_buffer_resize(recv, arg0);
    return XR_NULL_VAL;
}

/* Exact-width bit operations lower through xi.bit.* and have no boxed mem.*
 * or method-dispatch wrappers. Arithmetic intrinsics use their own lowering. */

static inline XrValue xrt_mem_fence(XrValue ordering) {
    xr_sync_core_fence(xrt_mem_int_arg(ordering));
    return XR_NULL_VAL;
}

/* Prefetch a cache line at the given raw address into caches (performance hint,
 * no observable effect). `rw` != 0 requests write intent. AOT lowers to
 * __builtin_prefetch; the VM binding (mem.c) is a no-op — both are observably
 * identical, so no shared semantic core is needed. */
/* Bulk memory ops (mem.copy/move/set/compare). Raw pointers reach AOT native
 * code as real C pointers in the value's .ptr slot; libc provides the shared
 * semantics so VM (mem.c, address-int pointers) and AOT agree. */
static inline XrValue xrt_mem_copy(XrValue dst, XrValue src, XrValue n) {
    memcpy(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_move(XrValue dst, XrValue src, XrValue n) {
    memmove(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_set(XrValue dst, XrValue byte, XrValue n) {
    memset(dst.ptr, (int) xrt_mem_int_arg(byte), (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_compare(XrValue a, XrValue b, XrValue n) {
    return XR_FROM_INT(memcmp(a.ptr, b.ptr, (size_t) xrt_mem_int_arg(n)));
}

static inline void xrt_mem_cache_maintain_range(void *ptr, size_t n) {
    if (!ptr || n == 0)
        return;
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    uintptr_t addr = (uintptr_t) ptr;
    uintptr_t start = addr & ~(uintptr_t) 63u;
    uintptr_t end = addr + n;
    if (end < addr)
        end = UINTPTR_MAX;
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ __volatile__("clflush (%0)" : : "r"((const void *) p) : "memory");
        if (UINTPTR_MAX - p < 64u)
            break;
    }
#else
    (void) ptr;
    (void) n;
#endif
    xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_cache_flush(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_invalidate(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_line_size(void) {
    return XR_FROM_INT(64);
}

/* Managed allocation face (mem.alloc/allocZeroed/allocAligned). */
static inline XrValue xrt_mem_alloc(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, 0);
}

static inline XrValue xrt_mem_alloc_zeroed(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 1, 0);
}

static inline XrValue xrt_mem_alloc_aligned(XrValue n, XrValue align) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, (size_t) xrt_mem_int_arg(align));
}

#ifdef _WIN32
static inline DWORD xrt_mem_page_prot_to_win(int64_t prot) {
    bool r = (prot & XRT_MEM_PROT_READ) != 0;
    bool w = (prot & XRT_MEM_PROT_WRITE) != 0;
    bool x = (prot & XRT_MEM_PROT_EXEC) != 0;
    if (!r && !w && !x)
        return PAGE_NOACCESS;
    if (x && w)
        return PAGE_EXECUTE_READWRITE;
    if (x && r)
        return PAGE_EXECUTE_READ;
    if (x)
        return PAGE_EXECUTE;
    if (w)
        return PAGE_READWRITE;
    return PAGE_READONLY;
}
#else
static inline int xrt_mem_page_prot_to_posix(int64_t prot) {
    int out = 0;
    if (prot & XRT_MEM_PROT_READ)
        out |= PROT_READ;
    if (prot & XRT_MEM_PROT_WRITE)
        out |= PROT_WRITE;
    if (prot & XRT_MEM_PROT_EXEC)
        out |= PROT_EXEC;
    return out ? out : PROT_NONE;
}
#endif

static inline XrValue xrt_mem_page_alloc(XrValue bytes, XrValue prot) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (n <= 0)
        return xr_mkptr(NULL, XR_TAG_PTR);
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, (size_t) n, MEM_RESERVE | MEM_COMMIT,
                           xrt_mem_page_prot_to_win(xrt_mem_int_arg(prot)));
#else
    void *p = mmap(NULL, (size_t) n, xrt_mem_page_prot_to_posix(xrt_mem_int_arg(prot)),
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        p = NULL;
#endif
    return xr_mkptr(p, XR_TAG_PTR);
}

static inline XrValue xrt_mem_page_alloc_default(XrValue bytes) {
    return xrt_mem_page_alloc(bytes, XR_FROM_INT(XRT_MEM_PROT_READ | XRT_MEM_PROT_WRITE));
}

static inline XrValue xrt_mem_page_protect(XrValue ptr, XrValue bytes, XrValue prot) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (!ptr.ptr || n <= 0)
        return XR_FROM_BOOL(false);
#ifdef _WIN32
    DWORD old;
    return XR_FROM_BOOL(VirtualProtect(ptr.ptr, (size_t) n,
                                       xrt_mem_page_prot_to_win(xrt_mem_int_arg(prot)), &old) != 0);
#else
    return XR_FROM_BOOL(
        mprotect(ptr.ptr, (size_t) n, xrt_mem_page_prot_to_posix(xrt_mem_int_arg(prot))) == 0);
#endif
}

static inline XrValue xrt_mem_page_free(XrValue ptr, XrValue bytes) {
    int64_t n = xrt_mem_int_arg(bytes);
    if (!ptr.ptr || n <= 0)
        return XR_FROM_BOOL(false);
#ifdef _WIN32
    (void) n;
    return XR_FROM_BOOL(VirtualFree(ptr.ptr, 0, MEM_RELEASE) != 0);
#else
    return XR_FROM_BOOL(munmap(ptr.ptr, (size_t) n) == 0);
#endif
}

/* Address <-> pointer bridge (mem.ptr / mem.mutPtr / mem.addr). ptr/mutPtr
 * builds a raw pointer from a numeric address (MMIO / physical memory, 147
 * §7.2) — the cgen converts the tagged result to its RAWPTR rep exactly like
 * mem.alloc. addr is the inverse (alignment checks, diagnostics). The VM
 * side (mem.c) represents raw pointers as address ints, so both directions
 * agree byte-for-byte across backends. */
static inline XrValue xrt_mem_ptr(XrValue addr) {
    return xr_mkptr((void *) (uintptr_t) (int64_t) xrt_mem_int_arg(addr), XR_TAG_PTR);
}

static inline XrValue xrt_mem_mut_ptr(XrValue addr) {
    return xrt_mem_ptr(addr);
}

static inline XrValue xrt_mem_addr(XrValue ptr) {
    return XR_FROM_INT((int64_t) (intptr_t) ptr.ptr);
}

/* Volatile load/store (MMIO). `size` in {1,2,4,8} selects the access width; the
 * `volatile` qualifier forbids the compiler from eliding or reordering the
 * access. Native byte order (matches the VM's sized memcpy in mem.c). The
 * generic mem.volatileLoad<T> sugar is a follow-up (Ptr method + IR op). */
static inline XrValue xrt_mem_volatile_load(XrValue ptr, XrValue size) {
    void *p = ptr.ptr;
    uint64_t v = 0;
    switch (xrt_mem_int_arg(size)) {
        case 1:
            v = *(volatile uint8_t *) p;
            break;
        case 2:
            v = *(volatile uint16_t *) p;
            break;
        case 4:
            v = *(volatile uint32_t *) p;
            break;
        case 8:
            v = *(volatile uint64_t *) p;
            break;
        default:
            break;
    }
    return XR_FROM_INT((int64_t) v);
}

static inline XrValue xrt_mem_volatile_store(XrValue ptr, XrValue value, XrValue size) {
    void *p = ptr.ptr;
    int64_t v = xrt_mem_int_arg(value);
    switch (xrt_mem_int_arg(size)) {
        case 1:
            *(volatile uint8_t *) p = (uint8_t) v;
            break;
        case 2:
            *(volatile uint16_t *) p = (uint16_t) v;
            break;
        case 4:
            *(volatile uint32_t *) p = (uint32_t) v;
            break;
        case 8:
            *(volatile uint64_t *) p = (uint64_t) v;
            break;
        default:
            break;
    }
    return XR_NULL_VAL;
}

static inline void xrt_mem_nontemporal_store_raw(void *p, uint64_t value, int64_t size) {
    int streamed = 0;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    uintptr_t addr = (uintptr_t) p;
    if (size == 4 && (addr & 3u) == 0) {
        uint32_t v32 = (uint32_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *) p) : "r"(v32) : "memory");
        streamed = 1;
    }
#if defined(__x86_64__)
    else if (size == 8 && (addr & 7u) == 0) {
        uint64_t v64 = (uint64_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *) p) : "r"(v64) : "memory");
        streamed = 1;
    }
#endif
    else
#endif
    {
        switch (size) {
            case 1:
                *(volatile uint8_t *) p = (uint8_t) value;
                break;
            case 2:
                *(volatile uint16_t *) p = (uint16_t) value;
                break;
            case 4:
                *(volatile uint32_t *) p = (uint32_t) value;
                break;
            case 8:
                *(volatile uint64_t *) p = (uint64_t) value;
                break;
            default:
                break;
        }
    }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    if (streamed)
        __asm__ __volatile__("sfence" ::: "memory");
    else
#endif
        xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_nontemporal_store(XrValue ptr, XrValue value, XrValue size) {
    xrt_mem_nontemporal_store_raw(ptr.ptr, (uint64_t) xrt_mem_int_arg(value),
                                  xrt_mem_int_arg(size));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_prefetch(XrValue ptr, XrValue rw) {
#if defined(__GNUC__) || defined(__clang__)
    /* __builtin_prefetch requires compile-time-constant rw/locality args, so
     * branch on the runtime rw flag rather than passing it through. */
    if (xrt_mem_int_arg(rw) != 0)
        __builtin_prefetch(ptr.ptr, 1, 3);
    else
        __builtin_prefetch(ptr.ptr, 0, 3);
#else
    (void) ptr;
    (void) rw;
#endif
    return XR_NULL_VAL;
}

#endif  // XRT_MEM_H
