/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_from_descriptor.c - Create XrClass from ClassDescriptor
 *
 * KEY CONCEPT:
 *   All methods (including closures) are added to builder BEFORE finalize.
 *   The finalized class is immutable - no post-finalize mutation.
 */

#include "xclass_descriptor.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xclass_builder.h"
#include "xclass.h"
#include "xinstance.h"
#include "../mem/xcycle_detector.h"
#include "../xisolate_api.h"
#include "xclass_system.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "../object/xstring.h"
#include "../value/xchunk.h"
#include "../mem/xheap.h"
#include "../mem/xcoro_heap.h"
#include "../../coro/xcoroutine.h"
#include "../xexec_frame.h"  // XrVMContext
#include "../xexec_state.h"  // XrVMState (via xr_isolate_get_vm_state)
#include "../symbol/xsymbol_table.h"
#include "../xglobals_table.h"
#include "xclass_lookup.h"
#include "../value/xstruct_layout.h"
#include <stdio.h>
#include <string.h>

// Create method closure (Context model: inherit enclosing context)
static XrClosure *create_method_closure_with_context(XrVMRuntime *isolate, XrVMContext *ctx,
                                                     XrProto *method_proto, XrClosure *enclosing_cl,
                                                     XrValue *base) {
    XR_DCHECK(isolate != NULL, "create_method_closure: NULL isolate");
    XR_DCHECK(method_proto != NULL, "create_method_closure: NULL method_proto");
    (void) ctx;
    (void) base;

    XrCoroutine *coro = ctx ? (XrCoroutine *) ctx->current_coro : NULL;
    XrClosure *closure = xr_closure_new(isolate, method_proto, coro);
    if (!closure)
        return NULL;

    (void) enclosing_cl;  // context chain no longer used

    return closure;
}

// Determine XrMethodType from descriptor entry
static XrMethodType determine_method_type(const XrMethodDescriptorEntry *method) {
    if (method->is_operator)
        return XMETHOD_OPERATOR;
    return XMETHOD_CLOSURE;
}

XrClass *xr_class_from_descriptor(XrVMRuntime *isolate, const XrClassDescriptor *desc,
                                  XrProto *proto, XrClosure *cl, XrValue *base, XrVMContext *vm_ctx,
                                  XrClass *super_override) {
    if (!isolate || !desc) {
        xr_log_warning("class", "from_descriptor: invalid parameters");
        return NULL;
    }

    if (!xr_class_descriptor_validate(desc)) {
        xr_log_warning("class", "from_descriptor: invalid descriptor for class '%s'",
                       desc->class_name ? desc->class_name : "(null)");
        return NULL;
    }

    // Resolve super class. An explicit override from the VM (for parents
    // resolved dynamically through local/upvalue/module-member expressions)
    // always wins over the descriptor-encoded paths.
    XrClass *super = NULL;
    if (super_override != NULL) {
        super = super_override;
    } else if (desc->super_name && strlen(desc->super_name) > 0) {
        if (desc->super_global_index >= 0) {
            if (desc->super_global_index < xr_isolate_get_vm_state(isolate)->builtin_count) {
                XrValue super_val =
                    xr_isolate_get_vm_state(isolate)->builtins[desc->super_global_index];
                if (XR_IS_CLASS(super_val)) {
                    super = XR_TO_CLASS(super_val);
                }
            }
        }
        if (!super) {
            super = xr_class_lookup_by_name(isolate, desc->super_name);
            if (!super) {
                xr_log_warning("class", "from_descriptor: super class '%s' not found",
                               desc->super_name);
            }
        }
    } else {
        super = xr_isolate_get_core_classes(isolate)->objectClass;
    }

    XrClassBuilder *builder = xr_class_builder_new(isolate, desc->class_name, super);
    if (!builder) {
        xr_log_warning("class", "from_descriptor: failed to create builder");
        return NULL;
    }
    xr_class_builder_set_flags(builder, desc->flags);

    /* Monomorphized generics: set origin, display name, and concrete type names */
    if (desc->is_monomorphized) {
        xr_class_builder_set_display_name(builder, desc->display_name);
        if (desc->generic_origin_name) {
            XrClass *origin = xr_class_lookup_by_name(isolate, desc->generic_origin_name);
            if (origin) {
                xr_class_builder_set_generic_origin(builder, origin);
            }
        }
        xr_class_builder_set_flags(builder, XR_CLASS_MONOMORPHIZED);

        if (desc->mono_type_arg_count > 0 && desc->mono_type_arg_names) {
            xr_class_builder_set_mono_type_arg_names(builder, desc->mono_type_arg_names,
                                                     (uint8_t) desc->mono_type_arg_count);
        }
    }

    // Add instance fields
    for (uint32_t i = 0; i < desc->instance_field_count; i++) {
        XrFieldDescriptorEntry *field = &desc->instance_fields[i];
        xr_class_builder_add_field(builder, field->name, field->flags);
    }

    // Add static fields
    for (uint32_t i = 0; i < desc->static_field_count; i++) {
        XrFieldDescriptorEntry *field = &desc->static_fields[i];
        xr_class_builder_add_static_field(builder, field->name, field->default_value,
                                          field->flags | XR_FIELD_STATIC);
    }

    // Add instance methods with closures (all set BEFORE finalize)
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        XrMethodDescriptorEntry *method = &desc->instance_methods[i];

        if (!proto || method->closure_index >= (uint32_t) DYNARRAY_COUNT(&proto->protos)) {
            xr_log_warning("class", "from_descriptor: invalid proto_index %u for method '%s'",
                           method->closure_index, method->name ? method->name : "(null)");
            continue;
        }

        XrProto *method_proto = DYNARRAY_GET(&proto->protos, method->closure_index, XrProto *);
        if (!method_proto) {
            xr_log_warning("class", "from_descriptor: NULL method proto for '%s'",
                           method->name ? method->name : "(null)");
            continue;
        }

        XrClosure *closure =
            create_method_closure_with_context(isolate, vm_ctx, method_proto, cl, base);
        if (!closure)
            continue;

        XrMethodType mtype = determine_method_type(method);
        xr_class_builder_add_method_closure(builder, method->name, closure, mtype,
                                            method->param_count, method->flags, method->op_type);
    }

    // Add static methods with closures
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        XrMethodDescriptorEntry *method = &desc->static_methods[i];

        if (!proto || method->closure_index >= (uint32_t) DYNARRAY_COUNT(&proto->protos)) {
            xr_log_warning("class",
                           "from_descriptor: invalid proto_index %u for static method '%s'",
                           method->closure_index, method->name ? method->name : "(null)");
            continue;
        }

        XrProto *method_proto = DYNARRAY_GET(&proto->protos, method->closure_index, XrProto *);
        if (!method_proto) {
            xr_log_warning("class", "from_descriptor: NULL method proto for static method '%s'",
                           method->name ? method->name : "(null)");
            continue;
        }

        XrClosure *closure =
            create_method_closure_with_context(isolate, vm_ctx, method_proto, cl, base);
        if (!closure)
            continue;

        xr_class_builder_add_static_method_closure(builder, method->name, closure,
                                                   method->param_count, method->flags);
    }

    // Add interfaces
    for (uint32_t i = 0; i < desc->interface_count; i++) {
        XrClass *iface_ptr = desc->interfaces[i].interface_ptr;
        if (iface_ptr) {
            xr_class_builder_add_interface(builder, iface_ptr);
        } else {
            xr_log_warning("class", "from_descriptor: interface '%s' has NULL pointer",
                           desc->interfaces[i].interface_name);
        }
    }

    if (desc->struct_layout && desc->struct_layout->total_size > 0) {
        XrNativeBodyDesc *body_desc = (XrNativeBodyDesc *) xr_calloc(1, sizeof(*body_desc));
        if (!body_desc) {
            xr_log_warning("class", "from_descriptor: failed to allocate aggregate body for '%s'",
                           desc->class_name);
            xr_class_builder_destroy(builder);
            return NULL;
        }
        body_desc->body_size = desc->struct_layout->total_size;
        body_desc->body_align = (uint16_t) desc->struct_layout->alignment;
        body_desc->copy_policy = XR_NATIVE_BODY_COPY_DEEP;
        xr_class_builder_set_native_body(builder, body_desc);
    }

    // Finalize - class is immutable after this point
    XrClass *cls = xr_class_builder_finalize(builder);
    if (!cls) {
        xr_log_warning("class", "from_descriptor: failed to finalize class '%s'", desc->class_name);
        return NULL;
    }

    // Backfill field type_name from descriptor entries (for type metadata).
    //
    // Two index spaces meet here and they are not the same one. A field
    // lookup answers in the flattened instance space -- inherited fields
    // first, then this class's own -- while cls->fields holds only the
    // fields this class declares, its own instance fields followed by its
    // own statics. Subtracting the inherited count converts one to the
    // other; using the flattened index directly walks off the end of the
    // array as soon as a superclass contributes a field.
    if (cls->fields) {
        int inherited_fields = (int) cls->field_count - (int) cls->own_field_count;
        if (inherited_fields < 0)
            inherited_fields = 0;
        for (uint32_t i = 0; i < desc->instance_field_count; i++) {
            int field_index =
                xr_class_lookup_field_by_name(isolate, cls, desc->instance_fields[i].name);
            // Below the inherited count the field belongs to a superclass,
            // which owns its own descriptor and backfills it there.
            if (field_index < inherited_fields || field_index >= xr_class_instance_field_count(cls))
                continue;
            XrFieldDescriptor *field = &cls->fields[field_index - inherited_fields];
            field->type_name = desc->instance_fields[i].type_name;
            if (!xr_json_decode_schema_clone_for_class(isolate,
                                                       &desc->instance_fields[i].json_decode_schema,
                                                       &field->json_decode_schema))
                memset(&field->json_decode_schema, 0, sizeof(field->json_decode_schema));
            field->json_value_kind = field->json_decode_schema.value_kind;
            if (xr_json_value_kind_base(field->json_value_kind) == XR_JSON_VALUE_STRUCT_OBJECT)
                field->json_struct_object_class =
                    (XrClass *) field->json_decode_schema.target_descriptor;
        }
        // Statics already start where this class's own instance fields end,
        // so the descriptor's instance count must not be added a second time.
        int sf_base = cls->own_field_count - cls->static_field_count;
        if (sf_base < 0)
            sf_base = 0;
        for (uint32_t i = 0; i < desc->static_field_count; i++) {
            int idx = sf_base + (int) i;
            if (idx < cls->own_field_count) {
                cls->fields[idx].type_name = desc->static_fields[i].type_name;
            }
        }
    }

    // Set field default values (these are data, not structure - safe to set)
    if (cls->field_default_values && desc->instance_field_count > 0) {
        int parent_field_count = 0;
        if (cls->super) {
            parent_field_count = xr_class_instance_field_count(cls->super);
        }

        for (uint32_t i = 0; i < desc->instance_field_count; i++) {
            int global_idx = parent_field_count + (int) i;
            if (global_idx < xr_class_instance_field_count(cls)) {
                cls->field_default_values[global_idx] = desc->instance_fields[i].default_value;
            }
        }
    }

    // Set struct_layout and VALUE_TYPE / FLAT_COPYABLE flags
    if (desc->struct_layout) {
        cls->struct_layout = desc->struct_layout;
        xr_vm_struct_layout_register(xr_isolate_get_vm_state(isolate), cls->struct_layout);
        cls->flags |= XR_CLASS_VALUE_TYPE;

        /* A struct is flat-copyable when every field can preserve value
         * semantics through a shallow field copy. Inline fixed arrays and
         * reference-backed containers need recursive copy/ownership handling
         * on the heap fallback path, just like nested structs. */
        bool flat = true;
        for (uint16_t fi = 0; fi < desc->struct_layout->field_count; fi++) {
            switch (desc->struct_layout->fields[fi].native_type) {
                case XR_NATIVE_NESTED_AGGREGATE:
                case XR_NATIVE_ARRAY:
                case XR_NATIVE_ARRAY_REF:
                case XR_NATIVE_MAP_REF:
                case XR_NATIVE_SET_REF:
                    flat = false;
                    break;
                default:
                    break;
            }
            if (!flat)
                break;
        }
        if (flat)
            cls->flags |= XR_CLASS_FLAT_COPYABLE;
    }

    // Propagate cycle candidate flag from compile-time type graph analysis
    if (desc->flags & XR_CLASS_CYCLE_CANDIDATE) {
        cls->flags |= XR_CLASS_CYCLE_CANDIDATE;
#ifdef XR_ENABLE_CYCLE_DETECTOR
        /* Snapshot the name while it is still valid. By the time a coroutine's
         * heap is scanned at teardown, the interned string may be gone and the
         * report would print raw bytes for the type of every object on the
         * cycle. Only candidates are registered, so an acyclic program pays
         * nothing. */
        xr_cycle_detector_register_class(cls, desc->class_name);
#endif
    }
    if (desc->flags & XR_CLASS_HAS_WEAK_FIELDS)
        cls->flags |= XR_CLASS_HAS_WEAK_FIELDS;

    // Not yet initialized by static constructor
    cls->flags &= ~XR_CLASS_INITIALIZED;

    return cls;
}
