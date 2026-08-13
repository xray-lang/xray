/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_helpers.c - VM helper functions
 *
 * KEY CONCEPT:
 *   Upvalue, closure, error handling and VM initialization helpers.
 */

#include "xvm_internal.h"
#include "../base/xchecks.h"
#include "../coro/xworker.h"
#include "../runtime/mem/xheap.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/value/xstruct_layout.h"
#include "../base/xsource_cache.h"

#include <stddef.h>

/* ========== Struct Layout Registry ========== */

static bool xr_struct_layout_register_children(XrVMState *vm, XrAggregateLayout *layout) {
    if (!vm || !layout)
        return false;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        XrAggregateFieldLayout *field = &layout->fields[i];
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE && field->sub_layout) {
            field->sub_layout_id = xr_vm_struct_layout_register(vm, field->sub_layout);
            if (field->sub_layout_id == 0)
                return false;
        }
    }
    return true;
}

static bool xr_struct_layout_reserve(XrVMState *vm, uint16_t capacity) {
    if (!vm || capacity == 0)
        return false;
    XrAggregateLayout **layouts = (XrAggregateLayout **) xr_calloc(capacity, sizeof(*layouts));
    XrClass **classes = (XrClass **) xr_calloc(capacity, sizeof(*classes));
    bool *owned = (bool *) xr_calloc(capacity, sizeof(*owned));
    if (!layouts || !classes || !owned) {
        xr_free(layouts);
        xr_free(classes);
        xr_free(owned);
        return false;
    }
    if (vm->struct_layout_count > 0) {
        memcpy(layouts, vm->struct_layouts, (size_t) vm->struct_layout_count * sizeof(*layouts));
        memcpy(classes, vm->struct_layout_classes,
               (size_t) vm->struct_layout_count * sizeof(*classes));
        memcpy(owned, vm->struct_layout_owned, (size_t) vm->struct_layout_count * sizeof(*owned));
    }
    xr_free(vm->struct_layouts);
    xr_free(vm->struct_layout_classes);
    xr_free(vm->struct_layout_owned);
    vm->struct_layouts = layouts;
    vm->struct_layout_classes = classes;
    vm->struct_layout_owned = owned;
    vm->struct_layout_capacity = capacity;
    return true;
}

uint16_t xr_vm_struct_layout_register(XrVMState *vm, XrAggregateLayout *layout) {
    if (!vm || !layout)
        return 0;

    if (!xr_struct_layout_register_children(vm, layout))
        return 0;

    if (layout->layout_id != 0) {
        XrAggregateLayout *registered = xr_struct_layout_lookup_by_id(vm, layout->layout_id);
        if (registered == layout)
            return layout->layout_id;
    }

    for (uint16_t i = 1; i < vm->struct_layout_count; i++) {
        if (vm->struct_layouts && vm->struct_layouts[i] == layout) {
            layout->layout_id = i;
            return i;
        }
    }

    if (vm->struct_layout_count == UINT16_MAX)
        return 0;

    if (vm->struct_layout_capacity == 0) {
        uint16_t cap = 16;
        if (!xr_struct_layout_reserve(vm, cap))
            return 0;
        vm->struct_layout_count = 1;
    } else if (vm->struct_layout_count >= vm->struct_layout_capacity) {
        uint16_t old_cap = vm->struct_layout_capacity;
        uint16_t new_cap = (old_cap <= UINT16_MAX / 2) ? (uint16_t) (old_cap * 2) : UINT16_MAX;
        if (!xr_struct_layout_reserve(vm, new_cap))
            return 0;
    }

    uint16_t id = vm->struct_layout_count++;
    vm->struct_layouts[id] = layout;
    layout->layout_id = id;
    return id;
}

XrAggregateLayout *xr_struct_layout_intern_owned(XrVMState *vm, XrAggregateLayout *layout) {
    if (!vm || !layout)
        return NULL;
    uint64_t key = xr_aggregate_layout_stable_key(layout);
    for (uint16_t i = 1; i < vm->struct_layout_count; i++) {
        XrAggregateLayout *existing = vm->struct_layouts ? vm->struct_layouts[i] : NULL;
        if (existing && xr_aggregate_layout_stable_key(existing) == key &&
            xr_aggregate_layout_semantically_equal(existing, layout)) {
            xr_aggregate_layout_free_owned(layout);
            return existing;
        }
    }
    uint16_t id = xr_vm_struct_layout_register(vm, layout);
    if (id == 0)
        return NULL;
    vm->struct_layout_owned[id] = true;
    return layout;
}

XrAggregateLayout *xr_struct_layout_lookup_by_id(XrVMState *vm, uint16_t layout_id) {
    if (!vm || layout_id == 0 || layout_id >= vm->struct_layout_count || !vm->struct_layouts)
        return NULL;
    return vm->struct_layouts[layout_id];
}

bool xr_vm_struct_layout_bind_class(XrVMState *vm, XrAggregateLayout *layout, XrClass *cls) {
    if (!vm || !layout || !cls || !layout->nominal_name)
        return false;
    uint16_t layout_id = xr_vm_struct_layout_register(vm, layout);
    if (layout_id == 0 || !vm->struct_layout_classes)
        return false;
    XrClass *existing = vm->struct_layout_classes[layout_id];
    if (existing && existing != cls)
        return false;
    vm->struct_layout_classes[layout_id] = cls;
    return true;
}

XrClass *xr_vm_struct_layout_class(XrVMState *vm, uint16_t layout_id) {
    if (!vm || layout_id == 0 || layout_id >= vm->struct_layout_count || !vm->struct_layout_classes)
        return NULL;
    XrClass *direct = vm->struct_layout_classes[layout_id];
    if (direct)
        return direct;

    /* Class descriptors own arena-local layout copies.  A containing value
     * struct therefore need not point at the exact same descriptor address as
     * the nested struct's class, even though both carry one nominal layout.
     * Resolve that copied descriptor by semantic identity.  Ambiguous nominal
     * identities fail closed instead of choosing a method table by load order. */
    XrAggregateLayout *layout = vm->struct_layouts ? vm->struct_layouts[layout_id] : NULL;
    if (!layout || !layout->nominal_name)
        return NULL;
    XrClass *match = NULL;
    for (uint16_t i = 1; i < vm->struct_layout_count; i++) {
        XrClass *candidate = vm->struct_layout_classes[i];
        XrAggregateLayout *candidate_layout = vm->struct_layouts ? vm->struct_layouts[i] : NULL;
        if (!candidate || !candidate_layout ||
            !xr_aggregate_layout_semantically_equal(layout, candidate_layout))
            continue;
        if (match && match != candidate)
            return NULL;
        match = candidate;
    }
    return match;
}

XrAggregateLayout *xr_vm_struct_layout_lookup_stable_key(XrVMState *vm, uint64_t stable_key) {
    if (!vm || stable_key == 0 || !vm->struct_layouts)
        return NULL;
    for (uint16_t i = 1; i < vm->struct_layout_count; i++) {
        XrAggregateLayout *layout = vm->struct_layouts[i];
        if (layout && xr_aggregate_layout_stable_key(layout) == stable_key)
            return layout;
    }
    return NULL;
}

XrAggregateLayout *xr_vm_struct_ref_layout(XrVMRuntime *isolate, XrValue ref) {
    if (!XR_IS_AGG_REF(ref) || XR_IS_ARRAY_REF(ref) || XR_IS_SLICE_REF(ref) || !ref.ptr)
        return NULL;

    uint16_t layout_id = xr_aggregate_layout_id(ref);
    if (layout_id != 0 && isolate) {
        XrAggregateLayout *layout = xr_struct_layout_lookup_by_id(&isolate->vm, layout_id);
        if (layout)
            return layout;
    }

    XrClass *cls = *(XrClass **) ref.ptr;
    if (!cls || !cls->struct_layout)
        return NULL;
    if (isolate)
        xr_vm_struct_layout_register(&isolate->vm, cls->struct_layout);
    return cls->struct_layout;
}

uint8_t *xr_vm_struct_ref_payload(XrVMRuntime *isolate, XrValue ref,
                                  XrAggregateLayout **layout_out) {
    XrAggregateLayout *layout = xr_vm_struct_ref_layout(isolate, ref);
    if (layout_out)
        *layout_out = layout;
    if (!layout || !ref.ptr)
        return NULL;
    return (uint8_t *) ref.ptr + xr_aggregate_layout_header_size(layout);
}

int xr_vm_struct_layout_field_index(XrVMRuntime *isolate, const XrAggregateLayout *layout,
                                    int prop_symbol) {
    if (!isolate || !layout || !layout->field_names)
        return -1;
    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->core_rt->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name)
        return -1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (layout->field_names[i] && strcmp(layout->field_names[i], prop_name) == 0)
            return (int) i;
    }
    return -1;
}

static bool xr_struct_write_instance_bytes(XrVMRuntime *isolate, uint8_t *dst, XrInstance *inst);

static bool xr_struct_write_array_bytes(uint8_t *fp, const XrAggregateFieldLayout *field,
                                        XrValue src) {
    uint8_t es = xr_native_type_size(xr_target_data_layout_host(), field->elem_native_type);
    if (XR_IS_ARRAY(src)) {
        XrArray *arr = (XrArray *) src.ptr;
        int count = arr->length < field->elem_count ? arr->length : field->elem_count;
        for (int idx = 0; idx < count; idx++) {
            XrValue elem = xr_array_get(arr, idx);
            XrAggregateFieldLayout elem_field = {.native_type = field->elem_native_type};
            if (!xr_vm_struct_write_field_value(NULL, fp + idx * es, &elem_field, elem))
                return false;
        }
        if (count < field->elem_count)
            memset(fp + count * es, 0, (field->elem_count - count) * es);
        return true;
    }
    if (XR_IS_ARRAY_REF(src)) {
        memcpy(fp, src.ptr, field->size);
        return true;
    }
    return false;
}

XR_FUNC bool xr_vm_struct_read_field_value(XrVMRuntime *isolate, uint8_t *fp,
                                           XrAggregateFieldLayout *field, XrValue *out) {
    if (!fp || !field || !out)
        return false;
    if (field->is_flexible)
        return false;

    switch (field->native_type) {
        case XR_NATIVE_I64: {
            int64_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT(v);
            return true;
        }
        case XR_NATIVE_U64: {
            uint64_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_ISIZE: {
            ptrdiff_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_USIZE: {
            size_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_POINTER: {
            void *v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) (uintptr_t) v);
            return true;
        }
        case XR_NATIVE_F64: {
            double v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_FLOAT(v);
            return true;
        }
        case XR_NATIVE_BOOL:
            out->descriptor = 0;
            out->i = *(uint8_t *) fp ? 1 : 0;
            out->tag = XR_TAG_BOOL;
            return true;
        case XR_NATIVE_I32: {
            int32_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_U32: {
            uint32_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_I16: {
            int16_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_U16: {
            uint16_t v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_INT((int64_t) v);
            return true;
        }
        case XR_NATIVE_I8:
            *out = XR_FROM_INT((int64_t) *(int8_t *) fp);
            return true;
        case XR_NATIVE_U8:
            *out = XR_FROM_INT((int64_t) *(uint8_t *) fp);
            return true;
        case XR_NATIVE_F32: {
            float v;
            memcpy(&v, fp, sizeof v);
            *out = XR_FROM_FLOAT((double) v);
            return true;
        }
        case XR_NATIVE_STRING: {
            XrString *s;
            memcpy(&s, fp, sizeof s);
            *out = s ? XR_FROM_STR(s) : xr_null();
            return true;
        }
        case XR_NATIVE_NESTED_AGGREGATE:
            if (field->sub_layout && field->sub_layout_id == 0 && isolate)
                field->sub_layout_id =
                    xr_vm_struct_layout_register(&isolate->vm, field->sub_layout);
            *out = xr_aggregate_ref(fp, field->sub_layout_id);
            return true;
        case XR_NATIVE_ARRAY:
            *out = xr_array_ref(fp, field->elem_native_type, field->elem_count);
            return true;
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
            *out = *(XrValue *) fp;
            return true;
        default:
            return false;
    }
}

XR_FUNC bool xr_vm_struct_write_field_value(XrVMRuntime *isolate, uint8_t *fp,
                                            const XrAggregateFieldLayout *field, XrValue src) {
    if (!fp || !field)
        return false;
    if (field->is_flexible)
        return false;

    switch (field->native_type) {
        case XR_NATIVE_I64: {
            int64_t v = XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_U64: {
            uint64_t v = (uint64_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_ISIZE: {
            ptrdiff_t v = (ptrdiff_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_USIZE: {
            size_t v = (size_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_POINTER: {
            void *v = (void *) (uintptr_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_F64: {
            double v = XR_TO_FLOAT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_BOOL:
            *(uint8_t *) fp = (uint8_t) src.i;
            return true;
        case XR_NATIVE_I32: {
            int32_t v = (int32_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_U32: {
            uint32_t v = (uint32_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_I16: {
            int16_t v = (int16_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_U16: {
            uint16_t v = (uint16_t) XR_TO_INT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_I8:
            *(int8_t *) fp = (int8_t) XR_TO_INT(src);
            return true;
        case XR_NATIVE_U8:
            *(uint8_t *) fp = (uint8_t) XR_TO_INT(src);
            return true;
        case XR_NATIVE_F32: {
            float v = (float) XR_TO_FLOAT(src);
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_STRING: {
            XrString *v = (XrString *) src.ptr;
            memcpy(fp, &v, sizeof v);
            return true;
        }
        case XR_NATIVE_NESTED_AGGREGATE:
            if (XR_IS_AGG_REF(src)) {
                uint8_t *src_ptr = (uint8_t *) xr_to_struct_ptr(src);
                memcpy(fp, src_ptr, field->size);
                return true;
            }
            if (XR_IS_INSTANCE(src))
                return xr_struct_write_instance_bytes(isolate, fp, XR_TO_INSTANCE(src));
            return false;
        case XR_NATIVE_ARRAY:
            return xr_struct_write_array_bytes(fp, field, src);
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
        case XR_NATIVE_VALUE:
            *(XrValue *) fp = src;
            return true;
        default:
            return false;
    }
}

static bool xr_struct_write_instance_bytes(XrVMRuntime *isolate, uint8_t *dst, XrInstance *inst) {
    if (!dst || !inst || !inst->klass || !inst->klass->struct_layout)
        return false;

    XrAggregateLayout *layout = inst->klass->struct_layout;
    if (isolate)
        xr_vm_struct_layout_register(&isolate->vm, layout);

    void *body = xr_instance_native_body(inst);
    if (body) {
        memcpy(dst, body, layout->total_size);
        return true;
    }

    if (layout->field_count > xr_class_instance_field_count(inst->klass))
        return false;

    memset(dst, 0, layout->total_size);
    for (uint16_t i = 0; i < layout->field_count; i++) {
        XrAggregateFieldLayout *field = &layout->fields[i];
        uint8_t *fp = dst + field->offset;
        if (!xr_vm_struct_write_field_value(isolate, fp, field, inst->fields[i]))
            return false;
    }
    return true;
}

XR_FUNC uint8_t *xr_vm_instance_struct_field_ptr(XrVMRuntime *isolate, XrInstance *inst,
                                                 int field_index,
                                                 XrAggregateFieldLayout **field_out) {
    if (field_out)
        *field_out = NULL;
    if (!inst || !inst->klass || !inst->klass->struct_layout)
        return NULL;

    XrAggregateLayout *layout = inst->klass->struct_layout;
    if (field_index < 0 || field_index >= layout->field_count)
        return NULL;

    if (isolate)
        xr_vm_struct_layout_register(&isolate->vm, layout);

    uint8_t *payload = (uint8_t *) xr_instance_native_body(inst);
    if (!payload)
        return NULL;

    XrAggregateFieldLayout *field = &layout->fields[field_index];
    if (field_out)
        *field_out = field;
    return payload + field->offset;
}

XR_FUNC bool xr_vm_instance_struct_get_field(XrVMRuntime *isolate, XrInstance *inst,
                                             int field_index, XrValue *out) {
    XrAggregateFieldLayout *field = NULL;
    uint8_t *fp = xr_vm_instance_struct_field_ptr(isolate, inst, field_index, &field);
    return xr_vm_struct_read_field_value(isolate, fp, field, out);
}

XR_FUNC bool xr_vm_instance_struct_set_field(XrVMRuntime *isolate, XrInstance *inst,
                                             int field_index, XrValue value) {
    XrAggregateFieldLayout *field = NULL;
    uint8_t *fp = xr_vm_instance_struct_field_ptr(isolate, inst, field_index, &field);
    return xr_vm_struct_write_field_value(isolate, fp, field, value);
}

static XrValue xr_struct_materialize_instance_depth(XrVMRuntime *isolate, XrValue ref,
                                                    uint32_t depth) {
    if (!isolate || !XR_IS_AGG_REF(ref) || XR_IS_ARRAY_REF(ref) || XR_IS_SLICE_REF(ref) ||
        !ref.ptr || depth > 16)
        return xr_null();

    XrAggregateLayout *layout = xr_vm_struct_ref_layout(isolate, ref);
    XrClass *cls =
        layout ? xr_vm_struct_layout_class(&isolate->vm, xr_aggregate_layout_id(ref)) : NULL;
    if (!layout || !cls || !xr_aggregate_layout_semantically_equal(cls->struct_layout, layout) ||
        !cls->native_body)
        return xr_null();

    XrInstance *inst = xr_instance_new(isolate, cls);
    if (!inst)
        return xr_null();
    uint8_t *src = xr_vm_struct_ref_payload(isolate, ref, NULL);
    uint8_t *dst = (uint8_t *) xr_instance_native_body(inst);
    if (!src || !dst)
        return xr_null();
    memcpy(dst, src, layout->total_size);

    uint32_t field_count = xr_class_instance_field_count(cls);
    if (field_count < layout->field_count)
        return xr_null();
    for (uint16_t i = 0; i < layout->field_count; i++) {
        XrAggregateFieldLayout *field = &layout->fields[i];
        XrValue value = xr_null();
        if (!xr_vm_struct_read_field_value(isolate, dst + field->offset, field, &value))
            return xr_null();
        if (field->native_type == XR_NATIVE_NESTED_AGGREGATE) {
            value = xr_struct_materialize_instance_depth(isolate, value, depth + 1);
            if (XR_IS_NULL(value))
                return xr_null();
        } else if (field->native_type == XR_NATIVE_ARRAY) {
            /* Fixed arrays live inline in the native body. The tagged field
             * slot is only an ownership anchor, so it stays null. */
            value = xr_null();
        }
        xr_rc_release_value(xr_current_coro_heap(), inst->fields[i]);
        inst->fields[i] = value;
        xr_rc_retain_value(value);
    }
    return XR_FROM_PTR(inst);
}

XrValue xr_vm_struct_materialize_instance(XrVMRuntime *isolate, XrValue ref) {
    return xr_struct_materialize_instance_depth(isolate, ref, 0);
}

/* ========== Runtime Error Handling ========== */

/*
 * Report runtime error (diagnostic print only)
 *
 * Prints error message and call stack to stderr.
 * Does NOT modify VM state (no flag setting, no stack reset).
 * For catchable errors, use VM_RUNTIME_ERROR macro instead.
 */
void xr_runtime_error(XrVMRuntime *isolate, const char *format, ...) {
    // Single authoritative ctx resolver — no bespoke fallback chain.
    XrVMContext *ctx = isolate ? xr_vm_current_ctx(isolate) : NULL;

    // Print error message
    fprintf(stderr, "\033[1;31merror\033[0m: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    // Get frame info
    int frame_count = ctx ? ctx->frame_count : 0;
    XrBcCallFrame *frames = ctx ? ctx->frames : NULL;

    // Show source code context (only top frame)
    XrSourceCache *source_cache = isolate ? isolate->vm.debug_source_cache : NULL;
    if (frame_count > 0 && source_cache) {
        XrBcCallFrame *top_frame = &frames[frame_count - 1];
        if (top_frame->closure && top_frame->closure->proto) {
            XrProto *proto = top_frame->closure->proto;
            size_t instruction = top_frame->pc - PROTO_CODE_BASE(proto) - 1;
            size_t line_count = PROTO_LINE_COUNT(proto);
            int line = 0;
            if (line_count > 0) {
                size_t idx = (instruction < line_count) ? instruction : line_count - 1;
                line = PROTO_LINE(proto, idx);
            }

            if (line > 0 && proto->source_file) {
                // Try to get source code line
                const char *src_line =
                    xr_source_cache_get_line(source_cache, proto->source_file, line);
                if (src_line) {
                    int line_len =
                        xr_source_cache_get_line_length(source_cache, proto->source_file, line);
                    fprintf(stderr, "   |\n");
                    fprintf(stderr, " \033[1;34m%d\033[0m | %.*s\n", line, line_len, src_line);
                    fprintf(stderr, "   |\n");
                }
            }
        }
    }

    // Print call stack
    fprintf(stderr, "\033[1;36mstack trace:\033[0m\n");
    for (int i = frame_count - 1; i >= 0; i--) {
        XrBcCallFrame *frame = &frames[i];
        if (!frame->closure || !frame->closure->proto)
            continue;
        XrProto *proto = frame->closure->proto;

        // Calculate instruction offset
        size_t instruction = frame->pc - PROTO_CODE_BASE(proto) - 1;

        // Print filename
        if (proto->source_file != NULL) {
            fprintf(stderr, "  at %s:", proto->source_file);
        } else {
            fprintf(stderr, "  at ");
        }

        // Print line number
        size_t line_count = PROTO_LINE_COUNT(proto);
        int line = 0;
        if (line_count > 0) {
            size_t idx = (instruction < line_count) ? instruction : line_count - 1;
            line = PROTO_LINE(proto, idx);
            while (line == 0 && idx > 0) {
                idx--;
                line = PROTO_LINE(proto, idx);
            }
        }
        if (line > 0) {
            fprintf(stderr, "%d", line);
        } else {
            fprintf(stderr, "?");
        }

        // Print function name
        if (proto->name != NULL) {
            fprintf(stderr, " in %s()\n", proto->name->data);
        } else {
            fprintf(stderr, " in <main>\n");
        }
    }
}

// ========== Debug Info Query ==========

/*
 * Find local variable name by register number and PC
 * @param proto Function prototype
 * @param reg Register number
 * @param pc Current instruction index
 * @return Variable name, NULL if not found
 */
const char *xr_debug_local_name(XrProto *proto, int reg, int pc) {
    if (!proto)
        return NULL;

    int count = (int) PROTO_LOCVAR_COUNT(proto);
    // Search backwards, prefer most recently defined variable (handle same-name variable shadowing)
    for (int i = count - 1; i >= 0; i--) {
        XrLocVar lv = PROTO_LOCVAR(proto, i);
        // Check register match and within scope
        if (lv.reg == reg && pc >= lv.start_pc && (lv.end_pc == -1 || pc <= lv.end_pc)) {
            return lv.name;
        }
    }
    return NULL;
}

// Cell / Context types and xr_cell_new live in runtime/closure/xcell.{h,c}.

// ========== C Function Operations ==========

/*
 * Create regular C function object
 */
XrCFunction *xr_vm_cfunction_new(XrVMRuntime *isolate, XrCFunctionPtr func, const char *name) {
    XR_DCHECK(func != NULL, "cfunction_new: NULL func");
    XrCFunction *cfunc = (XrCFunction *) xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Fully initialize object header. xr_malloc returns uninitialized memory,
    // so the fields not set below must be cleared explicitly.
    memset(&cfunc->hdr, 0, sizeof(XrObjHeader));
    cfunc->hdr.type = XR_TCFUNCTION;
    cfunc->hdr.objsize = (uint32_t) sizeof(XrCFunction);
    cfunc->as.func = func;
    cfunc->name = name;
    cfunc->is_yieldable = false;
    atomic_init(&cfunc->cfunc_class, XR_CFUNC_FAST);
    atomic_init(&cfunc->auto_slow_count, 0);

    return cfunc;
}

/*
 * Create yieldable C function object
 */
XrCFunction *xr_vm_yieldable_cfunction_new(XrVMRuntime *isolate, XrYieldableCFunctionPtr func,
                                           const char *name) {
    XR_DCHECK(func != NULL, "yieldable_cfunction_new: NULL func");
    XrCFunction *cfunc = (XrCFunction *) xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Fully initialize object header — see xr_vm_cfunction_new for rationale.
    memset(&cfunc->hdr, 0, sizeof(XrObjHeader));
    cfunc->hdr.type = XR_TCFUNCTION;
    cfunc->hdr.objsize = (uint32_t) sizeof(XrCFunction);
    cfunc->as.yieldable = func;
    cfunc->name = name;
    cfunc->is_yieldable = true;
    atomic_init(&cfunc->cfunc_class, XR_CFUNC_FAST);
    atomic_init(&cfunc->auto_slow_count, 0);

    return cfunc;
}

// Closure creation now lives in runtime/closure/xclosure.c.

// ========== Value Operation Helpers ==========

/*
 * Check if value is truthy (public API, for higher-order functions)
 * Uses vm_is_falsey from xvm_internal.h for consistent behavior
 */
bool xr_vm_is_truthy(XrValue value) {
    return vm_is_truthy(value);
}

// ========== VM Execution Loop ==========
