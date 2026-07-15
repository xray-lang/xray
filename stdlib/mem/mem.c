/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.c - Managed buffer + raw-memory capability module
 *
 * KEY CONCEPT:
 *   `mem.alloc*` returns a managed Buffer handle whose native body owns the
 *   allocated byte block and releases it on drop. Low-level callers can cross
 *   into the raw pointer world through unsafe Buffer.ptr().
 *
 *   The module also carries raw-memory capabilities: memory fence, cache
 *   performance hints (prefetch/flush/invalidate/non-temporal store),
 *   anonymous pages (mmap/VirtualAlloc), the numeric address bridge
 *   (ptr/mutPtr/addr), bulk byte operations (copy/move/set/compare)
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
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
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

typedef struct XrMemBufferBody {
    void *data;
    int64_t length;
    XrSpanView span_cache;
} XrMemBufferBody;

int64_t xr_mem_buffer_length(XrValue value) {
    if (!XR_IS_INSTANCE(value))
        return -1;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_BUFFER)
        return -1;
    XrMemBufferBody *buf = (XrMemBufferBody *) xr_instance_native_body(inst);
    return buf ? buf->length : 0;
}

static void mem_buffer_body_init(XrInstance *inst, void *body) {
    (void) inst;
    XrMemBufferBody *buf = (XrMemBufferBody *) body;
    memset(buf, 0, sizeof(*buf));
}

static void mem_buffer_body_destroy(void *body) {
    XrMemBufferBody *buf = (XrMemBufferBody *) body;
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->length = 0;
}

static XrNativeBodyDesc g_mem_buffer_body_desc = {
    .body_size = sizeof(XrMemBufferBody),
    .body_align = 0,
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = mem_buffer_body_init,
    .destroy = mem_buffer_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static XrClass *mem_buffer_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->memBufferClass != NULL, "mem.Buffer class not registered");
    return core ? core->memBufferClass : NULL;
}

static XrMemBufferBody *mem_buffer_body(XrVMRuntime *isolate, XrValue value) {
    if (!XR_IS_INSTANCE(value))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(value);
    XrClass *klass = mem_buffer_class(isolate);
    if (!klass || !xr_class_instanceof(inst->klass, klass))
        return NULL;
    return (XrMemBufferBody *) xr_instance_native_body(inst);
}

static XrValue mem_buffer_new(XrVMRuntime *isolate, int64_t length, bool zeroed, size_t align) {
    if (length < 0)
        length = 0;
    XrInstance *inst = xr_instance_new(isolate, mem_buffer_class(isolate));
    XR_CHECK(inst != NULL, "mem.Buffer allocation failed");
    XrMemBufferBody *buf = (XrMemBufferBody *) xr_instance_native_body(inst);
    XR_CHECK(buf != NULL, "mem.Buffer native body missing");

    if (length > 0) {
        size_t n = (size_t) length;
        void *data = NULL;
        if (align > 0) {
            if (align < sizeof(void *) || (align & (align - 1)) != 0)
                XR_CHECK(false, "mem.allocAligned: align must be a power of two >= sizeof(void*)");
            if (posix_memalign(&data, align, n) != 0)
                data = NULL;
            if (data && zeroed)
                memset(data, 0, n);
        } else {
            data = zeroed ? calloc(1, n) : malloc(n);
        }
        XR_CHECK(data != NULL, "mem.alloc: out of memory");
        buf->data = data;
    }
    buf->length = length;
    return XR_FROM_PTR(inst);
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

static XrValue mem_size_of(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int(0);
}

static XrValue mem_align_of(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int(0);
}

static XrValue mem_offset_of(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_int(0);
}

/* Managed allocation face (mem.alloc/allocZeroed/allocAligned). */
static inline XrValue mem_ptr_result(void *p) {
    return xr_int((int64_t) (intptr_t) p);
}

static XrValue mem_alloc(XrVMRuntime *isolate, XrValue *args, int argc) {
    int64_t n = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return mem_buffer_new(isolate, n, false, 0);
}

static XrValue mem_alloc_zeroed(XrVMRuntime *isolate, XrValue *args, int argc) {
    int64_t n = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return mem_buffer_new(isolate, n, true, 0);
}

static XrValue mem_alloc_aligned(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return mem_buffer_new(isolate, 0, false, 0);
    int64_t n = XR_TO_INT(args[0]);
    size_t a = (size_t) XR_TO_INT(args[1]);
    return mem_buffer_new(isolate, n, false, a);
}

static XrValue mem_buffer_ptr(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    return mem_ptr_result(buf ? buf->data : NULL);
}

static XrValue mem_buffer_as_span(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    if (!buf)
        return xr_span_ref(NULL);
    buf->span_cache.data = buf->data;
    buf->span_cache.length = buf->length;
    buf->span_cache.elem_type = XR_ELEM_U8;
    buf->span_cache.elem_size = 1;
    buf->span_cache.elem_tid = 0;
    buf->span_cache.contains_refs = 0;
    buf->span_cache.reserved = 0;
    buf->span_cache.guard = XR_TO_PTR(self);
    return xr_span_ref(&buf->span_cache);
}

static XrValue mem_buffer_resize(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    if (!buf || argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    int64_t new_len = XR_TO_INT(args[0]);
    if (new_len < 0)
        return xr_bool(false);
    if (new_len == 0) {
        free(buf->data);
        buf->data = NULL;
        buf->length = 0;
        return xr_bool(true);
    }
    void *new_data = realloc(buf->data, (size_t) new_len);
    if (!new_data)
        return xr_bool(false);
    buf->data = new_data;
    buf->length = new_len;
    return xr_bool(true);
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
 * Address <-> pointer bridge (mem.ptr / mem.mutPtr / mem.addr). In the VM a
 * raw pointer already IS an address-width int (mem_rawptr_arg / OP_PTR_LOAD),
 * so both directions are identity re-tags here; the AOT helpers cast between
 * int64 and the native .ptr slot. ptr/mutPtr enable MMIO / physical-address
 * access (147 §7.2): constructing the pointer is safe, dereferencing it stays
 * unsafe-gated as usual.
 */
static XrValue mem_ptr(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    int64_t addr = (argc >= 1 && XR_IS_INT(args[0])) ? XR_TO_INT(args[0]) : 0;
    return xr_int(addr);
}

static XrValue mem_mut_ptr(XrVMRuntime *isolate, XrValue *args, int argc) {
    return mem_ptr(isolate, args, argc);
}

static XrValue mem_addr(XrVMRuntime *isolate, XrValue *args, int argc) {
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

#define XR_STDLIB_VM_BIND_CLASS_BUFFER 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_BUFFER

#define XR_STDLIB_VM_BIND_MODULE_MEM 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MEM

XR_FUNC XrModule *xr_load_module_mem(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_mem: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "mem");
    if (!module)
        return NULL;

    xr_stdlib_vm_register_buffer_class_generated(isolate);
    xr_stdlib_vm_bind_mem_generated(isolate, module);

    return module;
}
