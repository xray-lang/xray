/*
 * xstdlib_vm_fastpath.c - hosted VM adapters for AOT-generated stdlib code
 *
 * Generated code owns algorithms and calls only canonical .xr exports.  This
 * file owns the one VM CFunction ABI adapter and module overlay operation.
 */

#include "xstdlib_vm_fastpath.h"
#include "xstdlib_vm_fastpaths_generated.h"
#include "../module/xmodule.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xstring.h"
#include "../runtime/class/xclass_builder.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xenum.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xheap.h"
#include "../runtime/mem/xalloc_unified.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/xisolate_api.h"
#include "../coro/xaot_coro.h"
#include "../coro/xyieldable.h"
#include "../runtime/value/xvalue.h"
#include "../vm/xvm.h"
#include "../os/os_thread.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct XrStdlibVmFastpathEntry {
    const char *module;
    const char *member;
    const char *abi;
    const char *effect;
    const char *ownership;
    XrStdlibVmFastpathFn entry;
    XrYieldableCFunctionPtr yieldable_entry;
} XrStdlibVmFastpathEntry;

typedef enum XrStdlibVmMemberKind {
    XR_STDLIB_VM_MEMBER_CONSTRUCTOR = 1,
    XR_STDLIB_VM_MEMBER_METHOD = 2,
    XR_STDLIB_VM_MEMBER_STATIC = 3,
    XR_STDLIB_VM_MEMBER_GETTER = 4,
    XR_STDLIB_VM_MEMBER_SETTER = 5,
} XrStdlibVmMemberKind;

typedef XrValue (*XrStdlibVmFastpathMethodFn)(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                              int nargs);

typedef struct XrStdlibVmFastpathClassEntry {
    const char *module;
    const char *type_name;
} XrStdlibVmFastpathClassEntry;

typedef struct XrStdlibVmFastpathMethodEntry {
    const char *module;
    const char *type_name;
    const char *member;
    uint8_t kind;
    uint16_t parameter_count;
    XrStdlibVmFastpathMethodFn entry;
    XrYieldablePrimitiveMethodFn yieldable_entry;
} XrStdlibVmFastpathMethodEntry;

extern void xr_stdlib_vm_fastpath_release_native(XrValue value);
extern void xr_stdlib_vm_fastpath_retain_native(XrValue value);
extern const XrHostedFragmentHostOps xr_stdlib_vm_fastpath_host_ops;
extern const void *xr_stdlib_vm_fastpath_runtime_ops(void);
extern void *xr_stdlib_vm_fastpath_current_coroutine(XrVMRuntime *isolate);

static xr_once_t g_stdlib_vm_fastpath_init_once = XR_ONCE_INITIALIZER;
static XR_THREAD_LOCAL const XrHostedFragmentContext *g_stdlib_vm_fastpath_init_context;
static bool g_stdlib_vm_fastpath_init_ok;

static void stdlib_vm_fastpath_initialize_once(void) {
    g_stdlib_vm_fastpath_init_ok = xr_hosted_fragment_initialize(g_stdlib_vm_fastpath_init_context);
}

static bool stdlib_vm_fastpath_ensure_initialized(XrVMRuntime *isolate) {
    XrHostedFragmentSignal signal = {0};
    XrHostedFragmentContext context = {0};
    context.ops = &xr_stdlib_vm_fastpath_host_ops;
    context.host = isolate;
    context.coroutine = xr_stdlib_vm_fastpath_current_coroutine(isolate);
    context.runtime_ops = xr_stdlib_vm_fastpath_runtime_ops();
    context.signal = &signal;
    g_stdlib_vm_fastpath_init_context = &context;
    xr_once_call(&g_stdlib_vm_fastpath_init_once, stdlib_vm_fastpath_initialize_once);
    g_stdlib_vm_fastpath_init_context = NULL;
    return g_stdlib_vm_fastpath_init_ok;
}

typedef struct XrStdlibHostedProxyBody {
    XrValue native_value;
    const char *nominal_owner;
    const char *type_name;
} XrStdlibHostedProxyBody;

static void stdlib_hosted_proxy_body_init(XrInstance *instance, void *raw_body) {
    (void) instance;
    XrStdlibHostedProxyBody *body = (XrStdlibHostedProxyBody *) raw_body;
    body->native_value = xr_null();
    body->nominal_owner = NULL;
    body->type_name = NULL;
}

static void stdlib_hosted_proxy_body_destroy(void *raw_body) {
    XrStdlibHostedProxyBody *body = (XrStdlibHostedProxyBody *) raw_body;
    if (body && !XR_IS_NULL(body->native_value)) {
        xr_stdlib_vm_fastpath_release_native(body->native_value);
        body->native_value = xr_null();
    }
}

static bool stdlib_hosted_proxy_body_deep_copy(XrCopyContext *ctx, XrInstance *src,
                                               XrInstance *dst) {
    (void) ctx;
    XrStdlibHostedProxyBody *src_body = (XrStdlibHostedProxyBody *) xr_instance_native_body(src);
    XrStdlibHostedProxyBody *dst_body = (XrStdlibHostedProxyBody *) xr_instance_native_body(dst);
    if (!src_body || !dst_body || XR_IS_NULL(src_body->native_value) || !src_body->nominal_owner ||
        !src_body->type_name)
        return false;
    xr_stdlib_vm_fastpath_retain_native(src_body->native_value);
    dst_body->native_value = src_body->native_value;
    dst_body->nominal_owner = src_body->nominal_owner;
    dst_body->type_name = src_body->type_name;
    return true;
}

static XrNativeBodyDesc g_stdlib_hosted_proxy_body_desc = {
    .body_size = sizeof(XrStdlibHostedProxyBody),
    .body_align = (uint16_t) _Alignof(XrStdlibHostedProxyBody),
    .copy_policy = XR_NATIVE_BODY_COPY_DEEP,
    .init = stdlib_hosted_proxy_body_init,
    .destroy = stdlib_hosted_proxy_body_destroy,
    .deep_copy = stdlib_hosted_proxy_body_deep_copy,
};

static XrClassBuilder *stdlib_hosted_proxy_class_builder_new(XrVMRuntime *isolate,
                                                             const char *type_name) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    if (!isolate || !core || !core->objectClass || !type_name)
        return NULL;
    XrClassBuilder *builder = xr_class_builder_new(isolate, type_name, core->objectClass);
    if (!builder)
        return NULL;
    xr_class_builder_set_flags(builder, XR_CLASS_FINAL | XR_CLASS_HAS_NATIVE_BODY);
    xr_class_builder_set_native_body(builder, &g_stdlib_hosted_proxy_body_desc);
    return builder;
}

static bool stdlib_host_object_view(void *host, XrValue value, XrHostedFragmentObjectView *out) {
    (void) host;
    if (!out || !xr_value_is_instance(value))
        return false;
    XrInstance *instance = xr_value_to_instance(value);
    if (!instance || !instance->klass ||
        instance->klass->native_body != &g_stdlib_hosted_proxy_body_desc)
        return false;
    XrStdlibHostedProxyBody *body = (XrStdlibHostedProxyBody *) xr_instance_native_body(instance);
    if (!body || XR_IS_NULL(body->native_value) || !body->nominal_owner || !body->type_name)
        return false;
    out->nominal_owner = body->nominal_owner;
    out->type_name = body->type_name;
    out->native_value = body->native_value;
    return true;
}

static XrValue stdlib_host_object_new(void *host, const char *nominal_owner, const char *type_name,
                                      XrValue native_value) {
    XrVMRuntime *isolate = (XrVMRuntime *) host;
    if (!isolate || !nominal_owner || !type_name || XR_IS_NULL(native_value) ||
        native_value.tag != XR_TAG_PTR || !native_value.ptr)
        return xr_null();
    /* The AOT representation of a class is an opaque implementation detail at
     * this boundary.  A class may use a fixed native instance or the canonical
     * map-backed layout; both carry an AOT-owned XrObjHeader and are released
     * by the generated runtime.  Nominal identity lives on the VM proxy and is
     * checked again by object_view, never inferred from this heap subtype. */
    XrObjHeader *native_header = (XrObjHeader *) native_value.ptr;
    if ((native_header->extra & XR_OBJ_AOT_NATIVE) == 0)
        return xr_null();
    XrValue module_value = xr_module_import(isolate, nominal_owner);
    if (!xr_value_is_module(module_value))
        return xr_null();
    XrValue class_value =
        xr_module_get_export(isolate, xr_value_to_module(module_value), type_name);
    if (!xr_value_is_class(class_value))
        return xr_null();
    XrClass *proxy_class = xr_value_to_class(class_value);
    if (!proxy_class || proxy_class->native_body != &g_stdlib_hosted_proxy_body_desc)
        return xr_null();
    XrInstance *instance = xr_instance_new(isolate, proxy_class);
    if (!instance)
        return xr_null();
    XrStdlibHostedProxyBody *body = (XrStdlibHostedProxyBody *) xr_instance_native_body(instance);
    if (!body)
        return xr_null();
    body->native_value = native_value;
    body->nominal_owner = nominal_owner;
    body->type_name = type_name;
    return xr_value_from_instance(instance);
}

static bool stdlib_host_string_view(void *host, XrValue value, XrHostedFragmentStringView *out) {
    (void) host;
    if (!out || !XR_IS_STRING(value))
        return false;
    const XrString *string = XR_TO_STRING(value);
    out->data = string->data;
    out->byte_length = string->length;
    out->rune_length = string->rune_length;
    out->hash = string->hash;
    return true;
}

static XrValue stdlib_host_string_new_utf8(void *host, const char *data, size_t byte_length,
                                           size_t rune_length, uint32_t hash) {
    XrVMRuntime *isolate = (XrVMRuntime *) host;
    XrString *string =
        isolate ? xr_string_new_valid_utf8(isolate, data, byte_length, rune_length) : NULL;
    if (!string)
        return xr_null();
    if (hash != 0)
        string->hash = hash;
    return XR_FROM_PTR(string);
}

static XrValue stdlib_host_error_new_utf8(void *host, int32_t code, const char *message,
                                          size_t byte_length) {
    XrVMRuntime *isolate = (XrVMRuntime *) host;
    if (!isolate)
        return xr_null();
    int printable_length = byte_length > (size_t) INT_MAX ? INT_MAX : (int) byte_length;
    return xr_panic_info_newf(isolate, (XrErrorCode) code, "%.*s", printable_length,
                              message ? message : "");
}

static XrValue stdlib_host_enum_new(void *host, const char *module_name, const char *enum_name,
                                    const char *member_name,
                                    const XrHostedFragmentValueView *payload_views,
                                    uint32_t payload_count) {
    XrVMRuntime *isolate = (XrVMRuntime *) host;
    if (!isolate || !module_name || !enum_name || !member_name ||
        (payload_count != 0 && !payload_views) || payload_count > 16)
        return xr_null();
    XrValue module_value = xr_module_import(isolate, module_name);
    if (!xr_value_is_module(module_value))
        return xr_null();
    XrValue type_value = xr_module_get_export(isolate, xr_value_to_module(module_value), enum_name);
    if (!XR_IS_ENUM_TYPE(type_value))
        return xr_null();
    XrEnumType *type = XR_TO_ENUM_TYPE(type_value);
    uint32_t member_index = UINT32_MAX;
    for (uint32_t i = 0; i < type->member_count; i++) {
        const char *candidate = xr_enum_type_member_name(type, i);
        if (candidate && strcmp(candidate, member_name) == 0) {
            member_index = i;
            break;
        }
    }
    if (member_index == UINT32_MAX ||
        xr_enum_type_payload_count(type, member_index) != (int) payload_count)
        return xr_null();

    XrValue payloads[16];
    uint32_t materialized = 0;
    for (; materialized < payload_count; materialized++) {
        const XrHostedFragmentValueView *view = &payload_views[materialized];
        if (view->kind == XR_HOSTED_FRAGMENT_VALUE_IMMEDIATE &&
            (XR_IS_NULL(view->immediate) || XR_IS_BOOL(view->immediate) ||
             XR_IS_INT(view->immediate) || XR_IS_FLOAT(view->immediate))) {
            payloads[materialized] = view->immediate;
            continue;
        }
        if (view->kind == XR_HOSTED_FRAGMENT_VALUE_STRING_UTF8 &&
            (view->data || view->byte_length == 0)) {
            XrString *string =
                xr_string_new(isolate, view->data ? view->data : "", view->byte_length);
            if (string) {
                payloads[materialized] = xr_string_value(string);
                continue;
            }
        }
        break;
    }
    if (materialized != payload_count)
        return xr_null();
    XrEnumAggregateValue *value =
        xr_enum_adt_construct(isolate, type, member_index, payloads, (int) payload_count);
    return value ? XR_FROM_PTR(value) : xr_null();
}

static bool stdlib_host_enum_view(void *host, XrValue value, XrHostedFragmentEnumView *out) {
    XrVMRuntime *isolate = (XrVMRuntime *) host;
    if (!isolate || !out || !xr_value_is_enum_aggregate(value))
        return false;
    XrEnumAggregateValue *aggregate = xr_value_to_enum_aggregate(value);
    XrEnumType *type = xr_enum_aggregate_type(aggregate);
    if (!aggregate || !type || !type->layout ||
        aggregate->payload_count > XR_HOSTED_FRAGMENT_MAX_ENUM_PAYLOADS)
        return false;
    memset(out, 0, sizeof(*out));
    out->nominal_owner = type->layout->nominal_owner;
    out->enum_name = type->name;
    out->member_name = xr_enum_aggregate_member_name(aggregate);
    out->member_index = aggregate->member_index;
    out->layout_id = type->layout->layout_id;
    out->payload_count = aggregate->payload_count;
    for (uint32_t i = 0; i < aggregate->payload_count; i++) {
        XrValue payload = xr_enum_aggregate_payload_get(aggregate, i);
        XrHostedFragmentValueView *view = &out->payloads[i];
        if (XR_IS_NULL(payload) || XR_IS_BOOL(payload) || XR_IS_INT(payload) ||
            XR_IS_FLOAT(payload)) {
            view->kind = XR_HOSTED_FRAGMENT_VALUE_IMMEDIATE;
            view->immediate = payload;
            continue;
        }
        if (XR_IS_STRING(payload)) {
            XrString *string = XR_TO_STRING(payload);
            view->kind = XR_HOSTED_FRAGMENT_VALUE_STRING_UTF8;
            view->data = string->data;
            view->byte_length = string->length;
            continue;
        }
        return false;
    }
    return out->nominal_owner && out->enum_name && out->member_name;
}

static bool stdlib_host_array_view(void *host, XrValue value, XrHostedFragmentArrayView *out) {
    (void) host;
    if (!out || !XR_IS_ARRAY(value) || !value.ptr)
        return false;
    const XrArray *array = (const XrArray *) value.ptr;
    if (array->length < 0 || array->elem_type >= XR_ELEM_COUNT)
        return false;
    memset(out, 0, sizeof(*out));
    out->length = (uint64_t) array->length;
    out->elem_type = array->elem_type;
    return true;
}

static bool stdlib_host_array_get(void *host, XrValue value, uint64_t index, XrValue *out) {
    (void) host;
    if (!out || !XR_IS_ARRAY(value) || !value.ptr || index > (uint64_t) INT_MAX)
        return false;
    XrArray *array = (XrArray *) value.ptr;
    if (index >= (uint64_t) array->length)
        return false;
    *out = xr_array_get(array, (int) index);
    return true;
}

static XrValue stdlib_host_array_new(void *host, uint64_t length, uint8_t elem_type) {
    (void) host;
    if (length > (uint64_t) INT_MAX || elem_type >= XR_ELEM_COUNT)
        return xr_null();
    XrArray *array = xr_array_with_capacity_typed(NULL, (int) length, (XrArrayElemType) elem_type);
    if (!array)
        return xr_null();
    array->length = (int32_t) length;
    return xr_value_from_array(array);
}

static bool stdlib_host_array_set(void *host, XrValue value, uint64_t index, XrValue element) {
    (void) host;
    if (!XR_IS_ARRAY(value) || !value.ptr || index > (uint64_t) INT_MAX)
        return false;
    XrArray *array = (XrArray *) value.ptr;
    if (index >= (uint64_t) array->length)
        return false;
    xr_array_set(array, (int) index, element);
    return true;
}

static bool stdlib_host_byte_span_view(void *host, XrValue value,
                                       XrHostedFragmentByteSpanView *out) {
    (void) host;
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (XR_IS_SLICE_REF(value)) {
        const XrSliceView *slice = XR_TO_SLICE_REF(value);
        if (!slice || slice->length < 0 || XR_SLICE_REF_ELEM_TYPE(value) != XR_ELEM_U8 ||
            XR_SLICE_REF_ELEM_SIZE(value) != 1 || (!slice->data && slice->length != 0))
            return false;
        out->data = (uint8_t *) slice->data;
        out->length = (uint64_t) slice->length;
        out->readonly = XR_SLICE_REF_IS_READONLY(value) ? 1u : 0u;
        return true;
    }
    if (XR_IS_ARRAY(value) && value.ptr) {
        const XrArray *array = (const XrArray *) value.ptr;
        if (array->length < 0 || array->elem_type != XR_ELEM_U8 || array->elem_size != 1 ||
            (!array->data && array->length != 0))
            return false;
        out->data = (uint8_t *) array->data;
        out->length = (uint64_t) array->length;
        return true;
    }
    return false;
}

static void stdlib_host_retain(void *host, XrValue value) {
    (void) host;
    xr_rc_retain_value(value);
}

static void stdlib_host_release(void *host, XrValue value) {
    (void) host;
    xr_rc_release_value(xr_current_coro_heap(), value);
}

const XrHostedFragmentHostOps xr_stdlib_vm_fastpath_host_ops = {
    .abi_version = XR_HOSTED_FRAGMENT_ABI_VERSION,
    .struct_size = sizeof(XrHostedFragmentHostOps),
    .string_view = stdlib_host_string_view,
    .string_new_utf8 = stdlib_host_string_new_utf8,
    .error_new_utf8 = stdlib_host_error_new_utf8,
    .enum_new = stdlib_host_enum_new,
    .enum_view = stdlib_host_enum_view,
    .array_view = stdlib_host_array_view,
    .array_get = stdlib_host_array_get,
    .array_new = stdlib_host_array_new,
    .array_set = stdlib_host_array_set,
    .byte_span_view = stdlib_host_byte_span_view,
    .object_view = stdlib_host_object_view,
    .object_new = stdlib_host_object_new,
    .retain = stdlib_host_retain,
    .release = stdlib_host_release,
};

static XrRuntimeCore *stdlib_aot_host_runtime_core(void *host) {
    return xr_isolate_get_runtime_core((XrVMRuntime *) host);
}

static XrRuntime *stdlib_aot_host_scheduler(void *host) {
    return xr_isolate_get_scheduler_runtime((XrVMRuntime *) host);
}

static XrCoroutine *stdlib_aot_host_current_coro(void *host) {
    return xr_current_coro((XrVMRuntime *) host);
}

static const XrAotVmHostOps g_stdlib_aot_vm_host_ops = {
    .runtime_core = stdlib_aot_host_runtime_core,
    .scheduler = stdlib_aot_host_scheduler,
    .current_coro = stdlib_aot_host_current_coro,
};

const void *xr_stdlib_vm_fastpath_runtime_ops(void) {
    return &g_stdlib_aot_vm_host_ops;
}

void *xr_stdlib_vm_fastpath_current_coroutine(XrVMRuntime *isolate) {
    return xr_current_coro(isolate);
}

XrValue xr_stdlib_vm_fastpath_handle_signal(XrVMRuntime *isolate, const char *symbol,
                                            const XrHostedFragmentSignal *signal) {
    if (signal && signal->status == XR_HOSTED_FRAGMENT_ERROR) {
        xr_vm_set_pending_error(isolate, signal->error);
        return xr_null();
    }
    char message[192];
    snprintf(message, sizeof(message),
             "%s hosted fragment rejected the call (status=%u, argument=%u)",
             symbol ? symbol : "stdlib", signal ? signal->status : UINT32_MAX,
             signal ? signal->argument_index : UINT32_MAX);
    xr_vm_set_pending_error(isolate, xr_panic_info_new(isolate, XR_ERR_TYPE_MISMATCH, message));
    return xr_null();
}

XrCFuncResult xr_stdlib_vm_fastpath_handle_yieldable_signal(XrVMRuntime *isolate,
                                                            const char *symbol,
                                                            const XrHostedFragmentSignal *signal,
                                                            XrContinuation continuation,
                                                            XrValue value, XrValue *result) {
    if (!isolate || !signal || !result)
        return XR_CFUNC_ERROR;
    switch ((XrHostedFragmentStatus) signal->status) {
        case XR_HOSTED_FRAGMENT_RETURN:
            *result = value;
            return XR_CFUNC_DONE;
        case XR_HOSTED_FRAGMENT_ERROR:
            xr_vm_set_pending_error(isolate, signal->error);
            return XR_CFUNC_ERROR;
        case XR_HOSTED_FRAGMENT_SUSPEND:
            if (!signal->continuation || !continuation ||
                (signal->suspend_kind != XR_HOSTED_FRAGMENT_SUSPEND_BLOCKED &&
                 signal->suspend_kind != XR_HOSTED_FRAGMENT_SUSPEND_YIELD) ||
                !xr_yield_set_continuation(isolate, continuation, signal->continuation)) {
                (void) xr_stdlib_vm_fastpath_handle_signal(isolate, symbol, signal);
                return XR_CFUNC_ERROR;
            }
            return signal->suspend_kind == XR_HOSTED_FRAGMENT_SUSPEND_BLOCKED ? XR_CFUNC_BLOCKED
                                                                              : XR_CFUNC_YIELD;
        case XR_HOSTED_FRAGMENT_INVALID_CALL:
        default:
            (void) xr_stdlib_vm_fastpath_handle_signal(isolate, symbol, signal);
            return XR_CFUNC_ERROR;
    }
}

#include "xstdlib_vm_fastpaths_generated.inc.c"

_Static_assert(XR_STDLIB_VM_FASTPATH_GENERATED_ABI_VERSION == XR_HOSTED_OBJECT_ABI_VERSION,
               "generated stdlib-native object ABI is stale");

#ifndef XTC_BUILD_HOST_TARGET
#error "generated stdlib-native fragments require an exact build-host target"
#endif

static bool stdlib_hosted_proxy_install_class(XrVMRuntime *isolate, XrModule *module,
                                              const char *module_name, const char *type_name) {
    XrClassBuilder *builder = stdlib_hosted_proxy_class_builder_new(isolate, type_name);
    if (!builder)
        return false;
    for (size_t i = 0; i < XR_STDLIB_VM_FASTPATH_GENERATED_METHOD_COUNT; i++) {
        const XrStdlibVmFastpathMethodEntry *entry = &g_stdlib_vm_fastpath_methods[i];
        if (strcmp(entry->module, module_name) != 0 || strcmp(entry->type_name, type_name) != 0)
            continue;
        int rc = -1;
        char accessor_name[256];
        switch (entry->kind) {
            case XR_STDLIB_VM_MEMBER_CONSTRUCTOR:
                rc = entry->yieldable_entry
                         ? xr_class_builder_add_yieldable_method(
                               builder, "call", entry->yieldable_entry, entry->parameter_count,
                               XMETHOD_FLAG_STATIC)
                         : xr_class_builder_add_static_method(builder, "call", entry->entry,
                                                              entry->parameter_count, 0);
                break;
            case XR_STDLIB_VM_MEMBER_METHOD:
                rc = entry->yieldable_entry
                         ? xr_class_builder_add_yieldable_method(builder, entry->member,
                                                                 entry->yieldable_entry,
                                                                 entry->parameter_count, 0)
                         : xr_class_builder_add_method(builder, entry->member, entry->entry,
                                                       entry->parameter_count, 0);
                break;
            case XR_STDLIB_VM_MEMBER_STATIC:
                rc = entry->yieldable_entry
                         ? xr_class_builder_add_yieldable_method(
                               builder, entry->member, entry->yieldable_entry,
                               entry->parameter_count, XMETHOD_FLAG_STATIC)
                         : xr_class_builder_add_static_method(builder, entry->member, entry->entry,
                                                              entry->parameter_count, 0);
                break;
            case XR_STDLIB_VM_MEMBER_GETTER:
            case XR_STDLIB_VM_MEMBER_SETTER: {
                const char *prefix = entry->kind == XR_STDLIB_VM_MEMBER_GETTER ? "get:" : "set:";
                int count =
                    snprintf(accessor_name, sizeof(accessor_name), "%s%s", prefix, entry->member);
                if (count <= 0 || (size_t) count >= sizeof(accessor_name))
                    break;
                rc = entry->yieldable_entry
                         ? xr_class_builder_add_yieldable_method(builder, accessor_name,
                                                                 entry->yieldable_entry,
                                                                 entry->parameter_count, 0)
                         : xr_class_builder_add_method(builder, accessor_name, entry->entry,
                                                       entry->parameter_count, 0);
                break;
            }
            default:
                break;
        }
        if (rc != 0) {
            xr_class_builder_destroy(builder);
            return false;
        }
    }
    XrClass *proxy = xr_class_builder_finalize(builder);
    if (!proxy) {
        xr_class_builder_destroy(builder);
        return false;
    }
    return xr_module_set_initializing_export(isolate, module, type_name, xr_value_from_class(proxy),
                                             true);
}

size_t xr_stdlib_vm_fastpath_count(void) {
    return XR_STDLIB_VM_FASTPATH_GENERATED_COUNT;
}

XrStdlibVmFastpathFn xr_stdlib_vm_fastpath_lookup(const char *module_name,
                                                  const char *member_name) {
    if (!module_name || !member_name)
        return NULL;
    for (size_t i = 0; i < XR_STDLIB_VM_FASTPATH_GENERATED_COUNT; i++) {
        const XrStdlibVmFastpathEntry *entry = &g_stdlib_vm_fastpaths[i];
        if (strcmp(entry->module, module_name) == 0 && strcmp(entry->member, member_name) == 0)
            return entry->entry;
    }
    return NULL;
}

bool xr_stdlib_vm_fastpath_install(XrVMRuntime *isolate, XrModule *module,
                                   const char *module_name) {
    if (!isolate || !module || !module_name)
        return false;
    if (strcmp(XR_STDLIB_VM_FASTPATH_GENERATED_TARGET, XTC_BUILD_HOST_TARGET) != 0 ||
        XR_STDLIB_VM_FASTPATH_GENERATED_FINGERPRINT[0] == '\0')
        return false;
    if (!stdlib_vm_fastpath_ensure_initialized(isolate))
        return false;
    xr_runtime_core_set_aot_native_value_release(xr_isolate_get_runtime_core(isolate),
                                                 xr_stdlib_vm_fastpath_release_native);
    for (size_t i = 0; i < XR_STDLIB_VM_FASTPATH_GENERATED_CLASS_COUNT; i++) {
        const XrStdlibVmFastpathClassEntry *entry = &g_stdlib_vm_fastpath_classes[i];
        if (strcmp(entry->module, module_name) == 0 &&
            !stdlib_hosted_proxy_install_class(isolate, module, module_name, entry->type_name))
            return false;
    }
    for (size_t i = 0; i < XR_STDLIB_VM_FASTPATH_GENERATED_COUNT; i++) {
        const XrStdlibVmFastpathEntry *entry = &g_stdlib_vm_fastpaths[i];
        if (!entry->effect || !entry->effect[0] || !entry->ownership || !entry->ownership[0])
            return false;
        if (strcmp(entry->module, module_name) != 0)
            continue;
        XrCFunction *function =
            entry->yieldable_entry
                ? xr_vm_yieldable_cfunction_new(isolate, entry->yieldable_entry, entry->member)
                : xr_vm_cfunction_new(isolate, entry->entry, entry->member);
        if (!function)
            return false;
        if (!xr_module_set_initializing_export(isolate, module, entry->member,
                                               xr_value_from_cfunction(function), true))
            return false;
    }
    return true;
}
