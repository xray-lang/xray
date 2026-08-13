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
 *   allocated byte block and releases it on drop. Safe callers borrow byte
 *   views through Buffer.asBytes()/asMutBytes(); low-level callers cross into
 *   the raw pointer world through unsafe Buffer.borrowPtr().
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
 *   - Integer operations -> compiler-known receiver methods. Exact-width bit
 *     operations lower to stable xi.bit.* ops; arithmetic methods retain the
 *     shared xr_arith_core.h semantics.
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
    size_t align;
    XrSliceView readonly_span_cache;
    XrSliceView mutable_span_cache;
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
    if (buf->align > 0)
        xr_free_aligned(buf->data, buf->align);
    else
        free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->align = 0;
}

static XrNativeBodyDesc g_mem_buffer_body_desc = {
    .body_size = sizeof(XrMemBufferBody),
    .body_align = 0,
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = mem_buffer_body_init,
    .destroy = mem_buffer_body_destroy,
    .deep_copy = NULL,
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

static void *mem_buffer_alloc_data(size_t n, bool zeroed, size_t align) {
    void *data = NULL;
    if (align > 0) {
        if (align < sizeof(void *) || (align & (align - 1)) != 0)
            XR_CHECK(false, "mem.allocAligned: align must be a power of two >= sizeof(void*)");
        data = xr_malloc_aligned(n, align);
        if (data && zeroed)
            memset(data, 0, n);
    } else {
        data = zeroed ? calloc(1, n) : malloc(n);
    }
    return data;
}

static void mem_buffer_free_data(XrMemBufferBody *buf) {
    if (!buf || !buf->data)
        return;
    if (buf->align > 0)
        xr_free_aligned(buf->data, buf->align);
    else
        free(buf->data);
    buf->data = NULL;
}

static bool mem_copy_initialized_layout(uint8_t *dst, const uint8_t *src,
                                        const XrAggregateLayout *layout, unsigned depth) {
    if (!dst || !src || !layout || depth > 16)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->is_flexible || field->offset > layout->total_size ||
            field->size > layout->total_size - field->offset)
            return false;
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            if (!field->sub_layout || field->sub_layout->total_size != field->size ||
                !mem_copy_initialized_layout(dst + field->offset, src + field->offset,
                                             field->sub_layout, depth + 1))
                return false;
        } else {
            memcpy(dst + field->offset, src + field->offset, field->size);
        }
    }
    return true;
}

bool xr_mem_buffer_materialize(XrValue value, void *dst, size_t size, size_t align,
                               const XrAggregateLayout *layout) {
    if (!XR_IS_INSTANCE(value) || !dst || size == 0 || align == 0)
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_BUFFER)
        return false;
    XrMemBufferBody *buf = (XrMemBufferBody *) xr_instance_native_body(inst);
    if (!buf || buf->length != (int64_t) size || buf->align != align || !buf->data ||
        ((uintptr_t) buf->data % align) != 0)
        return false;

    memset(dst, 0, size);
    bool copied =
        layout
            ? (layout->total_size == size &&
               mem_copy_initialized_layout((uint8_t *) dst, (const uint8_t *) buf->data, layout, 0))
            : (memcpy(dst, buf->data, size), true);
    if (!copied)
        return false;

    mem_buffer_free_data(buf);
    buf->length = 0;
    buf->align = 0;
    return true;
}

static XrValue mem_buffer_new(XrVMRuntime *isolate, int64_t length, bool zeroed, size_t align) {
    if (length < 0)
        length = 0;
    XrInstance *inst = xr_instance_new(isolate, mem_buffer_class(isolate));
    XR_CHECK(inst != NULL, "mem.Buffer allocation failed");
    XrMemBufferBody *buf = (XrMemBufferBody *) xr_instance_native_body(inst);
    XR_CHECK(buf != NULL, "mem.Buffer native body missing");
    buf->align = align;

    if (length > 0) {
        size_t n = (size_t) length;
        void *data = mem_buffer_alloc_data(n, zeroed, align);
        XR_CHECK(data != NULL, "mem.alloc: out of memory");
        buf->data = data;
    }
    buf->length = length;
    return XR_FROM_PTR(inst);
}

bool xr_mem_buffer_bytes(XrValue value, const uint8_t **data, size_t *length) {
    if (!data || !length || !XR_IS_INSTANCE(value))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(value);
    if (!inst || !inst->klass || inst->klass->builtin_kind != XR_BK_BUFFER)
        return false;
    XrMemBufferBody *buf = (XrMemBufferBody *) xr_instance_native_body(inst);
    if (!buf || buf->length < 0 || (buf->length > 0 && !buf->data))
        return false;
    *data = (const uint8_t *) buf->data;
    *length = (size_t) buf->length;
    return true;
}

XrValue xr_mem_buffer_copy_from_bytes(XrVMRuntime *isolate, const uint8_t *data, size_t length) {
    if (!isolate || length > (size_t) INT64_MAX || (length > 0 && !data))
        return XR_NULL_VAL;

    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    XrAllocationContext transfer_alloc;
    XrExecutionContext transfer_exec;
    xr_alloc_context_init(&transfer_alloc, core, XR_STORAGE_TRANSFERABLE);
    xr_exec_context_init(&transfer_exec, core, &transfer_alloc);
    XrExecutionContext *previous = xr_exec_context_enter(&transfer_exec);
    XrValue value = mem_buffer_new(isolate, (int64_t) length, false, 0);
    xr_exec_context_restore(previous);

    XrMemBufferBody *buf = mem_buffer_body(isolate, value);
    if (!buf)
        return XR_NULL_VAL;
    if (length > 0)
        memcpy(buf->data, data, length);
    return value;
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

static XrValue mem_buffer_borrow_ptr(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    return mem_ptr_result(buf ? buf->data : NULL);
}

static XrValue mem_buffer_bytes_view(XrVMRuntime *isolate, XrValue self, bool readonly) {
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    if (!buf)
        return xr_span_ref_typed(NULL, XR_ELEM_U8, 1, 0, 0, readonly ? XR_SLICE_VIEW_READONLY : 0);
    XrSliceView *span = readonly ? &buf->readonly_span_cache : &buf->mutable_span_cache;
    span->data = buf->data;
    span->length = buf->length;
    return xr_span_ref_typed(span, XR_ELEM_U8, 1, 0, 0, readonly ? XR_SLICE_VIEW_READONLY : 0);
}

static XrValue mem_buffer_as_bytes(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return mem_buffer_bytes_view(isolate, self, true);
}

static XrValue mem_buffer_as_mut_bytes(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int argc) {
    (void) args;
    (void) argc;
    return mem_buffer_bytes_view(isolate, self, false);
}

static XrValue mem_buffer_resize(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    XrMemBufferBody *buf = mem_buffer_body(isolate, self);
    if (!buf || argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    int64_t new_len = XR_TO_INT(args[0]);
    if (new_len < 0)
        return xr_bool(false);
    if (new_len == 0) {
        mem_buffer_free_data(buf);
        buf->length = 0;
        return xr_bool(true);
    }
    void *new_data = NULL;
    if (buf->align > 0) {
        new_data = mem_buffer_alloc_data((size_t) new_len, false, buf->align);
        if (new_data && buf->data) {
            size_t old_len = buf->length > 0 ? (size_t) buf->length : 0;
            memcpy(new_data, buf->data, old_len < (size_t) new_len ? old_len : (size_t) new_len);
        }
    } else {
        new_data = realloc(buf->data, (size_t) new_len);
    }
    if (!new_data)
        return xr_bool(false);
    if (buf->align > 0)
        mem_buffer_free_data(buf);
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

/* Direct mem.load/store<T> calls are compiler intrinsics: the selected T and
 * endian are encoded on XI_PTR_LOAD/STORE before bytecode or C emission. These
 * exports keep the module metadata complete but are never valid dynamic calls. */
static XrValue mem_load_intrinsic(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

static XrValue mem_store_intrinsic(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

/* mem.slice<T> is lowered with static element layout and owner evidence before
 * bytecode/AOT emission. Dynamic invocation cannot carry those proofs. */
static XrValue mem_slice_intrinsic(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

/* mem.assumeInitialized<T> is compiler-only.  The analyzer publishes the
 * exact layout plus a dominating complete-output proof and lowering consumes
 * the Buffer in one backend operation; a dynamic call cannot carry either. */
static XrValue mem_assume_initialized_intrinsic(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
}

/* mem.withSliceMut<T> is likewise compiler-only: lowering creates a writable
 * call-bound Slice place and invokes the proven noescape callback directly. */
static XrValue mem_with_slice_mut_intrinsic(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    return xr_null();
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

XR_FUNC XrModule *xr_native_module_create_mem(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_mem: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "mem");
    if (!module)
        return NULL;

    xr_stdlib_vm_register_buffer_class_generated(isolate);
    xr_stdlib_vm_bind_mem_generated(isolate, module);

    return module;
}
