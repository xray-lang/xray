/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.c - Raw-memory capability module
 *
 * KEY CONCEPT:
 *   `mem` carries raw-memory capabilities only: memory fence, cache
 *   performance hints (prefetch/flush/invalidate/non-temporal store),
 *   explicit allocation (malloc/calloc/aligned/realloc/free), anonymous
 *   pages (mmap/VirtualAlloc), the numeric address bridge
 *   (fromAddress/addressOf), bulk byte operations (copy/move/set/compare)
 *   and volatile sized load/store for MMIO.
 *
 *   Moved out per the 151 surface convergence:
 *   - Cycle-collector control + memory statistics -> `runtime` module
 *     (stdlib/runtime/runtime.c, task 154).
 *   - Bit intrinsics + wrapping/overflow arithmetic -> `int` methods
 *     (src/runtime/value/xint_methods.h VM, xrt_method.h + cgen direct
 *     lowering AOT; semantics in src/shared/xr_bits_core.h and
 *     xr_arith_core.h, task 153).
 */

#include "mem.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/shared/xr_sync_core.h"
#include "../../src/os/os_mem.h"
#include "../../src/base/xplatform.h"
#include "../../src/base/xchecks.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ========== Module Loading ========== */

/*
 * Standalone memory fence (sys.fence / mem.fence). `ordering` mirrors the
 * prelude Ordering enum ordinals (0 Relaxed .. 4 SeqCst). Semantics live in
 * the shared core (src/shared/xr_sync_core.h); the AOT wrapper is
 * xrt_mem_fence in src/aot/xrt_mem.h. Returns unit.
 */
static XrValue mem_fence(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t ordering = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 4;
    xr_sync_core_fence(ordering);
    return xr_null();
}

/*
 * Prefetch (mem.prefetch): a pure performance hint. The VM is an interpreter
 * with no cache-locality guarantees, so prefetch is a no-op here — no
 * observable effect, semantically identical to the AOT path (xrt_mem_prefetch
 * -> __builtin_prefetch) which does the real prefetch.
 */
static XrValue mem_prefetch(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

/*
 * Bulk memory ops (mem.copy/move/set/compare). In the VM a raw pointer is an
 * address-width int (see OP_PTR_LOAD), so decode it back to a void*. libc
 * memcpy/memmove/memset/memcmp are the shared semantics; the AOT direct helpers
 * (xrt_mem_*) call the same libc, so both backends agree.
 */
static inline void *mem_rawptr_arg(XrValue v) {
    return (void *) (uintptr_t) (intptr_t) (XR_IS_INT(v) ? XR_TO_INT(v) : 0);
}

static XrValue mem_copy(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc >= 3 && XR_IS_INT(args[2]))
        memcpy(mem_rawptr_arg(args[0]), mem_rawptr_arg(args[1]), (size_t) XR_TO_INT(args[2]));
    return xr_null();
}

static XrValue mem_move(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc >= 3 && XR_IS_INT(args[2]))
        memmove(mem_rawptr_arg(args[0]), mem_rawptr_arg(args[1]), (size_t) XR_TO_INT(args[2]));
    return xr_null();
}

static XrValue mem_set(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc >= 3 && XR_IS_INT(args[1]) && XR_IS_INT(args[2]))
        memset(mem_rawptr_arg(args[0]), (int) XR_TO_INT(args[1]), (size_t) XR_TO_INT(args[2]));
    return xr_null();
}

static XrValue mem_compare(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 3 || !XR_IS_INT(args[2]))
        return xr_int(0);
    return xr_int(
        memcmp(mem_rawptr_arg(args[0]), mem_rawptr_arg(args[1]), (size_t) XR_TO_INT(args[2])));
}

static XrValue mem_cache_flush(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

static XrValue mem_cache_invalidate(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

static XrValue mem_cache_line_size(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int(XR_CACHE_LINE);
}

/*
 * Allocation face (mem.alloc/allocAligned/realloc/free). In the VM a raw
 * pointer is an address-width int (see mem_rawptr_arg / OP_PTR_LOAD), so return
 * the address as an int; the AOT helpers (xrt_mem_alloc etc.) box a native
 * pointer. Buffers are user-managed — pair alloc with free. NULL/0 on OOM.
 */
static inline XrValue mem_ptr_result(void *p) {
    return xr_int((int64_t) (intptr_t) p);
}

static XrValue mem_alloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    size_t n = (argc >= 1 && XR_IS_INT(args[0])) ? (size_t) XR_TO_INT(args[0]) : 0;
    return mem_ptr_result(malloc(n));
}

static XrValue mem_alloc_zeroed(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    size_t n = (argc >= 1 && XR_IS_INT(args[0])) ? (size_t) XR_TO_INT(args[0]) : 0;
    return mem_ptr_result(calloc(1, n));
}

static XrValue mem_alloc_aligned(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return mem_ptr_result(NULL);
    size_t n = (size_t) XR_TO_INT(args[0]);
    size_t a = (size_t) XR_TO_INT(args[1]);
    void *p = NULL;
    if (a >= sizeof(void *) && (a & (a - 1)) == 0) {
        if (posix_memalign(&p, a, n) != 0)
            p = NULL;
    }
    return mem_ptr_result(p);
}

static XrValue mem_realloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    size_t n = (argc >= 2 && XR_IS_INT(args[1])) ? (size_t) XR_TO_INT(args[1]) : 0;
    return mem_ptr_result(realloc(mem_rawptr_arg(args[0]), n));
}

static XrValue mem_free(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc >= 1)
        free(mem_rawptr_arg(args[0]));
    return xr_null();
}

static int mem_page_default_prot(void) {
    return XR_MEM_PROT_READ | XR_MEM_PROT_WRITE;
}

static int mem_page_prot_arg(XrValue v) {
    return XR_IS_INT(v) ? (int) XR_TO_INT(v) : mem_page_default_prot();
}

static XrValue mem_page_alloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t bytes = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    int prot = argc >= 2 ? mem_page_prot_arg(args[1]) : mem_page_default_prot();
    if (bytes <= 0)
        return mem_ptr_result(NULL);
    return mem_ptr_result(xr_mem_map((size_t) bytes, prot));
}

static XrValue mem_page_protect(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 3 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    int64_t bytes = XR_TO_INT(args[1]);
    if (bytes <= 0)
        return xr_bool(false);
    return xr_bool(
        xr_mem_protect(mem_rawptr_arg(args[0]), (size_t) bytes, mem_page_prot_arg(args[2])));
}

static XrValue mem_page_free(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    int64_t bytes = XR_TO_INT(args[1]);
    if (bytes <= 0)
        return xr_bool(false);
    return xr_bool(xr_mem_unmap(mem_rawptr_arg(args[0]), (size_t) bytes));
}

/*
 * Address <-> pointer bridge (mem.fromAddress / mem.addressOf). In the VM a
 * raw pointer already IS an address-width int (mem_rawptr_arg / OP_PTR_LOAD),
 * so both directions are identity re-tags here; the AOT helpers cast between
 * int64 and the native .ptr slot. fromAddress enables MMIO / physical-address
 * access (147 §7.2): constructing the pointer is safe, dereferencing it stays
 * unsafe-gated as usual.
 */
static XrValue mem_from_address(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t addr = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return xr_int(addr);
}

static XrValue mem_address_of(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1)
        return xr_int(0);
    return xr_int((int64_t) (intptr_t) mem_rawptr_arg(args[0]));
}

/*
 * Volatile load/store (MMIO). The VM is an interpreter — every read re-fetches
 * from memory and nothing is reordered/elided, so "volatile" is satisfied by a
 * plain native-order sized access. `size` in {1,2,4,8}. This mirrors the AOT
 * *(volatile uintN_t*) path (xrt_mem_volatile_*) byte-for-byte.
 */
static XrValue mem_volatile_load(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[1]))
        return xr_int(0);
    void *p = mem_rawptr_arg(args[0]);
    int64_t size = XR_TO_INT(args[1]);
    uint64_t v = 0;
    if (size == 1 || size == 2 || size == 4 || size == 8)
        memcpy(&v, p, (size_t) size);
    return xr_int((int64_t) v);
}

static XrValue mem_volatile_store(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 3 || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return xr_null();
    void *p = mem_rawptr_arg(args[0]);
    uint64_t v = (uint64_t) XR_TO_INT(args[1]);
    int64_t size = XR_TO_INT(args[2]);
    if (size == 1 || size == 2 || size == 4 || size == 8)
        memcpy(p, &v, (size_t) size);
    return xr_null();
}

/*
 * Non-temporal store (mem.nontemporalStore): semantically a sized store. The VM
 * has no cache hierarchy contract, so it performs the same native-order write
 * as volatileStore; AOT may use target-specific streaming stores.
 */
static XrValue mem_nontemporal_store(XrVMRuntime *isolate, XrValue *args, int argc) {
    return mem_volatile_store(isolate, args, argc);
}

#define XR_STDLIB_VM_BIND_MODULE_MEM 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MEM

XR_FUNC XrModule *xr_load_module_mem(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_mem: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "mem");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_mem_generated(isolate, module);

    module->loaded = true;
    return module;
}
