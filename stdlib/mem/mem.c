/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.c - RC memory and cycle-collection introspection module
 *
 * KEY CONCEPT:
 *   Reclamation is per-coroutine reference counting; the only collection
 *   event is the Bacon-Rajan cycle collector. This module exposes a
 *   memory-model-native surface only:
 *   - mem.collectCycles()                 - run cycle collection + reclaim
 *   - mem.disableCycleCollection()        - pause automatic cycle collection
 *   - mem.enableCycleCollection()         - resume automatic cycle collection
 *   - mem.isCycleCollectionEnabled()      - automatic collector state
 *   - mem.liveBytes()                     - live memory bytes
 *   - mem.liveObjects()                   - live object count
 *   - mem.info()                          - introspection Map
 *
 *   Tracing-era knobs (step / debt / timems / state / fragmentation) are
 *   removed: they have no meaning under reference counting.
 */

#include "mem.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/shared/xr_bits_core.h"
#include "../../src/shared/xr_arith_core.h"
#include "../../src/shared/xr_sync_core.h"
#include "../../src/base/xplatform.h"
#include "../../src/base/xchecks.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ========== Helper ========== */

static XrCoroHeap *get_heap(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "get_heap: isolate must not be NULL");
    // Try current coroutine first (ensure heap exists via lazy init)
    XrCoroutine *coro = xr_current_coro(isolate);
    if (coro) {
        return xr_coro_ensure_heap(coro);
    }
    // Fallback to main coroutine
    coro = xr_isolate_get_main_coro(isolate);
    if (coro) {
        return xr_coro_ensure_heap(coro);
    }
    return NULL;
}

/* ========== mem.collectCycles() ========== */

// Run the cycle collector + whole-block reclaim on the current coroutine.
// Returns the cumulative cycle-collection count. Runs even when the
// automatic collector is disabled (explicit user request).
static XrValue mem_collect_cycles(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap) {
        xr_coro_heap_collect_cycles(heap);
        return xr_int((int64_t) heap->cycle_collect_count);
    }
    return xr_int(0);
}

/* ========== mem.liveBytes() ========== */

// Return live memory usage in bytes
static XrValue mem_live_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int(heap->totalbytes) : xr_int(0);
}

/* ========== mem.disableCycleCollection() / mem.enableCycleCollection() ========== */

// Pause the automatic cycle collector (increment cycle_collection_disabled, saturate at 255).
// Under RC the only thing that can be paused is the cycle-collector
// auto-trigger; xr_cycle_add_root honours cycle_collection_disabled.
static XrValue mem_disable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled < 255) {
        heap->cycle_collection_disabled++;
    }
    return xr_null();
}

// Resume the automatic cycle collector (decrement cycle_collection_disabled)
static XrValue mem_enable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled > 0) {
        heap->cycle_collection_disabled--;
    }
    return xr_null();
}

/* ========== mem.isCycleCollectionEnabled() ========== */

// Whether the automatic cycle collector is enabled
static XrValue mem_is_cycle_collection_enabled(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return xr_bool(heap && heap->cycle_collection_disabled == 0);
}

/* ========== mem.liveObjects() ========== */

// Return total live object count (O(1) via incremental counter)
static XrValue mem_live_objects(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int((int64_t) heap->object_count) : xr_int(0);
}

/* ========== mem.info() ========== */

// Map keys in mem.info() use camelCase for every field.
#define MAP_SET(map, key_str, val) xr_map_set((map), xrs_string_value_c(isolate, (key_str)), (val))

// Return memory-model-native info as a Map
static XrValue mem_info(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;

    XrCoroHeap *heap = get_heap(isolate);
    XrMap *map = xr_map_new(xr_current_coro(isolate));

    if (!heap) {
        MAP_SET(map, "error", xrs_string_value_c(isolate, "no memory heap"));
        return xr_value_from_map(map);
    }

    // Live memory + object stats
    MAP_SET(map, "liveBytes", xr_int(heap->totalbytes));
    MAP_SET(map, "liveKB", xr_float((double) heap->totalbytes / 1024.0));
    MAP_SET(map, "liveObjects", xr_int((int64_t) heap->object_count));

    // Automatic cycle collector state
    MAP_SET(map, "cycleCollectionEnabled", xr_bool(heap->cycle_collection_disabled == 0));
    MAP_SET(map, "cycleCollections", xr_int(heap->cycle_collect_count));
    MAP_SET(map, "finalizerCount", xr_int((int64_t) heap->finalizer_count));

    // Region block stats
    XrRegionStats istats;
    xr_region_get_stats(&heap->region, &istats);
    MAP_SET(map, "blocks", xr_int((int64_t) istats.total_blocks));
    MAP_SET(map, "freeBlocks", xr_int((int64_t) istats.free_blocks));
    MAP_SET(map, "fullBlocks", xr_int((int64_t) istats.full_blocks));

    return xr_value_from_map(map);
}

#undef MAP_SET

/* ========== Bit intrinsics (mem.popcount / leadingZeros / ...) ========== */

/*
 * Pure 64-bit bit-manipulation intrinsics. Semantics live in the shared
 * core (src/shared/xr_bits_core.h) so the VM bindings here and the AOT
 * wrappers in src/aot/xrt_mem.h stay bit-identical. No heap / coroutine
 * state is touched, so these are safe on any thread.
 */

static XrValue mem_popcount(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(0);
    return xr_int(xr_bits_core_popcount(XR_TO_INT(args[0])));
}

static XrValue mem_leading_zeros(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(64);
    return xr_int(xr_bits_core_leading_zeros(XR_TO_INT(args[0])));
}

static XrValue mem_trailing_zeros(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(64);
    return xr_int(xr_bits_core_trailing_zeros(XR_TO_INT(args[0])));
}

static XrValue mem_byteswap(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(0);
    return xr_int(xr_bits_core_byteswap(XR_TO_INT(args[0])));
}

static XrValue mem_rotate_left(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_bits_core_rotate_left(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

static XrValue mem_rotate_right(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_bits_core_rotate_right(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

/* ========== Wrapping / overflow-checked arithmetic (mem.addWrapping / ...) ========== */

/*
 * Fixed-width 64-bit signed arithmetic. Semantics live in the shared core
 * (src/shared/xr_arith_core.h) so these VM bindings and the AOT wrappers in
 * src/aot/xrt_mem.h stay bit-identical. Wrapping ops give two's-complement
 * wraparound; overflow predicates report signed overflow.
 */

static XrValue mem_add_wrapping(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_arith_core_add_wrapping(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

static XrValue mem_sub_wrapping(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_arith_core_sub_wrapping(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

static XrValue mem_mul_wrapping(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_arith_core_mul_wrapping(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

static XrValue mem_add_overflows(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_add_overflows(XR_TO_INT(args[0]), XR_TO_INT(args[1])) != 0);
}

static XrValue mem_sub_overflows(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_sub_overflows(XR_TO_INT(args[0]), XR_TO_INT(args[1])) != 0);
}

static XrValue mem_mul_overflows(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    return xr_bool(xr_arith_core_mul_overflows(XR_TO_INT(args[0]), XR_TO_INT(args[1])) != 0);
}

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
