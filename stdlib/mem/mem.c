/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.c - Raw-memory host leaves and Buffer ABI adapters
 *
 * KEY CONCEPT:
 *   Public policy lives in mem.xr. This file only binds allocator, page,
 *   overlap-move and machine-intrinsic leaves, plus the four native Buffer
 *   methods that the current runtime representation requires. Buffer storage,
 *   destruction and compiler materialization live in runtime/object/xbuffer.c
 *   so standard-library C is not a second semantic owner.
 *
 *   Moved out per the 151 surface convergence:
 *   - Cycle-collector control + memory statistics -> `runtime` module
 *     (stdlib/runtime/runtime.c, task 154).
 *   - Integer operations -> compiler-known receiver methods. Exact-width bit
 *     operations lower to stable xi.bit.* ops; arithmetic methods retain the
 *     shared xr_arith_core.h semantics.
 */

#include "../common.h"
#include "../../src/runtime/object/xbuffer.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/value/xstruct_layout.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/shared/xr_sync_core.h"
#include "../../src/os/os_mem.h"
#include "../../src/base/xplatform.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
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
 * Raw pointers are address-width ints in the VM (see OP_PTR_LOAD). Decode that
 * representation only at the retained host leaves and encode pointer results
 * at the same boundary.
 */
#define MEM_RAWPTR_ARG(value)                                                                      \
    ((void *) (uintptr_t) (intptr_t) (XR_IS_INT(value) ? XR_TO_INT(value) : 0))
#define MEM_PTR_RESULT(pointer) xr_int((int64_t) (intptr_t) (pointer))

static XrValue mem_move(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc >= 3 && XR_IS_INT(args[2]))
        memmove(MEM_RAWPTR_ARG(args[0]), MEM_RAWPTR_ARG(args[1]), (size_t) XR_TO_INT(args[2]));
    return xr_null();
}

static XrValue mem_cache_flush(XrVMRuntime *isolate, XrValue *args, int argc) {
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

static XrValue mem_alloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    int64_t n = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return xr_buffer_new(isolate, n, false, 0);
}

static XrValue mem_alloc_zeroed(XrVMRuntime *isolate, XrValue *args, int argc) {
    int64_t n = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return xr_buffer_new(isolate, n, true, 0);
}

static XrValue mem_alloc_aligned(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_buffer_new(isolate, 0, false, 0);
    int64_t n = XR_TO_INT(args[0]);
    size_t a = (size_t) XR_TO_INT(args[1]);
    return xr_buffer_new(isolate, n, false, a);
}

static XrValue mem_buffer_borrow_ptr(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return MEM_PTR_RESULT(xr_buffer_borrow_pointer(isolate, self));
}

static XrValue mem_buffer_as_bytes(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_buffer_byte_view(isolate, self, true);
}

static XrValue mem_buffer_as_mut_bytes(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int argc) {
    (void) args;
    (void) argc;
    return xr_buffer_byte_view(isolate, self, false);
}

static XrValue mem_buffer_resize(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_buffer_resize(isolate, self, XR_TO_INT(args[0])));
}

static XrValue mem_page_alloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return MEM_PTR_RESULT(NULL);
    int64_t bytes = XR_TO_INT(args[0]);
    int prot = (int) XR_TO_INT(args[1]);
    if (bytes <= 0)
        return MEM_PTR_RESULT(NULL);
    return MEM_PTR_RESULT(xr_mem_map((size_t) bytes, prot));
}

static XrValue mem_page_protect(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 3 || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return xr_bool(false);
    int64_t bytes = XR_TO_INT(args[1]);
    if (bytes <= 0)
        return xr_bool(false);
    return xr_bool(
        xr_mem_protect(MEM_RAWPTR_ARG(args[0]), (size_t) bytes, (int) XR_TO_INT(args[2])));
}

static XrValue mem_page_free(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[1]))
        return xr_bool(false);
    int64_t bytes = XR_TO_INT(args[1]);
    if (bytes <= 0)
        return xr_bool(false);
    return xr_bool(xr_mem_unmap(MEM_RAWPTR_ARG(args[0]), (size_t) bytes));
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
    void *p = MEM_RAWPTR_ARG(args[0]);
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
    void *p = MEM_RAWPTR_ARG(args[0]);
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

#define XR_STDLIB_VM_BIND_CLASS_BUFFER 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_BUFFER

#define XR_STDLIB_VM_BIND_MODULE_MEM 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MEM
