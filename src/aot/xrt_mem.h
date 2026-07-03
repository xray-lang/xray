/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_mem.h - Freestanding AOT wrappers for the mem.* bit intrinsics.
 *
 * These mirror the VM bindings in stdlib/mem/mem.c one-for-one and call
 * the same shared core (src/shared/xr_bits_core.h), so AOT-compiled code
 * and the VM produce identical results. AOT direct-call metadata for
 * these lives in stdlib/defs/core.def (aot: "xrt_mem_*", arg_spec).
 */

#ifndef XRT_MEM_H
#define XRT_MEM_H

#include <stdlib.h>
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
#include "../shared/xr_bits_core.h"
#include "../shared/xr_arith_core.h"
#include "../shared/xr_sync_core.h"

#define XRT_MEM_PROT_NONE 0
#define XRT_MEM_PROT_READ 1
#define XRT_MEM_PROT_WRITE 2
#define XRT_MEM_PROT_EXEC 4

static inline int64_t xrt_mem_int_arg(XrValue v) {
    return XR_IS_INT(v) ? XR_TO_INT(v) : 0;
}

static inline XrValue xrt_mem_popcount(XrValue x) {
    return XR_FROM_INT(xr_bits_core_popcount(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_leading_zeros(XrValue x) {
    return XR_FROM_INT(xr_bits_core_leading_zeros(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_trailing_zeros(XrValue x) {
    return XR_FROM_INT(xr_bits_core_trailing_zeros(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_byteswap(XrValue x) {
    return XR_FROM_INT(xr_bits_core_byteswap(xrt_mem_int_arg(x)));
}

static inline XrValue xrt_mem_rotate_left(XrValue x, XrValue n) {
    return XR_FROM_INT(xr_bits_core_rotate_left(xrt_mem_int_arg(x), xrt_mem_int_arg(n)));
}

static inline XrValue xrt_mem_rotate_right(XrValue x, XrValue n) {
    return XR_FROM_INT(xr_bits_core_rotate_right(xrt_mem_int_arg(x), xrt_mem_int_arg(n)));
}

static inline XrValue xrt_mem_add_wrapping(XrValue a, XrValue b) {
    return XR_FROM_INT(xr_arith_core_add_wrapping(xrt_mem_int_arg(a), xrt_mem_int_arg(b)));
}

static inline XrValue xrt_mem_sub_wrapping(XrValue a, XrValue b) {
    return XR_FROM_INT(xr_arith_core_sub_wrapping(xrt_mem_int_arg(a), xrt_mem_int_arg(b)));
}

static inline XrValue xrt_mem_mul_wrapping(XrValue a, XrValue b) {
    return XR_FROM_INT(xr_arith_core_mul_wrapping(xrt_mem_int_arg(a), xrt_mem_int_arg(b)));
}

static inline XrValue xrt_mem_add_overflows(XrValue a, XrValue b) {
    return XR_FROM_BOOL(xr_arith_core_add_overflows(xrt_mem_int_arg(a), xrt_mem_int_arg(b)) != 0);
}

static inline XrValue xrt_mem_sub_overflows(XrValue a, XrValue b) {
    return XR_FROM_BOOL(xr_arith_core_sub_overflows(xrt_mem_int_arg(a), xrt_mem_int_arg(b)) != 0);
}

static inline XrValue xrt_mem_mul_overflows(XrValue a, XrValue b) {
    return XR_FROM_BOOL(xr_arith_core_mul_overflows(xrt_mem_int_arg(a), xrt_mem_int_arg(b)) != 0);
}

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

static inline XrValue xrt_mem_cache_line_size(void) {
    return XR_FROM_INT(64);
}

/* Allocation face (mem.alloc/allocAligned/realloc/free). Returns a raw pointer
 * boxed as a tagged value; the cgen converts TAGGED->RAWPTR (extracting .ptr)
 * at the call's RawMut/RawPtr result rep. The VM (mem.c) returns the address as
 * an int (its raw-pointer representation). Buffers are user-managed: pair with
 * mem.free. NULL on OOM (isNull() testable). */
static inline XrValue xrt_mem_alloc(XrValue n) {
    return xr_mkptr(malloc((size_t) xrt_mem_int_arg(n)), XR_TAG_PTR);
}

static inline XrValue xrt_mem_alloc_zeroed(XrValue n) {
    return xr_mkptr(calloc(1, (size_t) xrt_mem_int_arg(n)), XR_TAG_PTR);
}

static inline XrValue xrt_mem_alloc_aligned(XrValue n, XrValue align) {
    size_t a = (size_t) xrt_mem_int_arg(align);
    size_t sz = (size_t) xrt_mem_int_arg(n);
    void *p = NULL;
    if (a >= sizeof(void *) && (a & (a - 1)) == 0) {
        if (posix_memalign(&p, a, sz) != 0)
            p = NULL;
    }
    return xr_mkptr(p, XR_TAG_PTR);
}

static inline XrValue xrt_mem_realloc(XrValue ptr, XrValue n) {
    return xr_mkptr(realloc(ptr.ptr, (size_t) xrt_mem_int_arg(n)), XR_TAG_PTR);
}

static inline XrValue xrt_mem_free(XrValue ptr) {
    free(ptr.ptr);
    return XR_NULL_VAL;
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

/* Address <-> pointer bridge (mem.fromAddress / mem.addressOf). fromAddress
 * builds a raw pointer from a numeric address (MMIO / physical memory, 147
 * §7.2) — the cgen converts the tagged result to its RAWPTR rep exactly like
 * mem.alloc. addressOf is the inverse (alignment checks, diagnostics). The VM
 * side (mem.c) represents raw pointers as address ints, so both directions
 * agree byte-for-byte across backends. */
static inline XrValue xrt_mem_from_address(XrValue addr) {
    return xr_mkptr((void *) (uintptr_t) (int64_t) xrt_mem_int_arg(addr), XR_TAG_PTR);
}

static inline XrValue xrt_mem_address_of(XrValue ptr) {
    return XR_FROM_INT((int64_t) (intptr_t) ptr.ptr);
}

/* Volatile load/store (MMIO). `size` in {1,2,4,8} selects the access width; the
 * `volatile` qualifier forbids the compiler from eliding or reordering the
 * access. Native byte order (matches the VM's sized memcpy in mem.c). The
 * generic mem.volatileLoad<T> sugar is a follow-up (RawPtr method + IR op). */
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
