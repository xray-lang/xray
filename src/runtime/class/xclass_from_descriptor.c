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
#include "../xisolate_api.h"
#include "xclass_system.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "../object/xstring.h"
#include "../value/xchunk.h"
#include "../gc/xgc.h"
#include "../gc/xcoro_gc.h"
#include "../../coro/xcoroutine.h"
#include "../../vm/xvm.h"
#include "../../vm/xvm_internal.h"
#include "../symbol/xsymbol_table.h"
#include "../xglobals_table.h"
#include "xclass_lookup.h"
#include <stdio.h>
#include <string.h>

// Create method closure (Context model: inherit enclosing context)
static XrClosure* create_method_closure_with_context(
    XrayIsolate *isolate,
    XrVMContext *ctx,
    XrProto *method_proto,
    XrClosure *enclosing_cl,
    XrValue *base
) {
    XR_DCHECK(isolate != NULL, "create_method_closure: NULL isolate");
    XR_DCHECK(method_proto != NULL, "create_method_closure: NULL method_proto");
    (void)ctx;
    (void)base;
    
    XrCoroutine *coro = ctx ? (XrCoroutine *)ctx->current_coro : NULL;
    XrClosure *closure = xr_vm_closure_new(isolate, method_proto, coro);
    if (!closure) return NULL;
    
    (void)enclosing_cl; // context chain no longer used
    
    return closure;
}

// Determine XrMethodType from descriptor entry
static XrMethodType determine_method_type(const XrMethodDescriptorEntry *method) {
    if (method->is_operator) return XMETHOD_OPERATOR;
    if (method->name && strncmp(method->name, "get:", 4) == 0) return XMETHOD_GETTER;
    if (method->name && strncmp(method->name, "set:", 4) == 0) return XMETHOD_SETTER;
    return XMETHOD_CLOSURE;
}

XrClass* xr_class_from_descriptor(XrayIsolate *isolate, const XrClassDescriptor *desc, 
                                   XrProto *proto, XrClosure *cl, XrValue *base,
                                   XrVMContext *vm_ctx) {
    if (!isolate || !desc) {
        xr_log_warning("class", "from_descriptor: invalid parameters");
        return NULL;
    }
    
    if (!xr_class_descriptor_validate(desc)) {
        xr_log_warning("class", "from_descriptor: invalid descriptor for class '%s'", 
                desc->class_name ? desc->class_name : "(null)");
        return NULL;
    }
    
    // Resolve super class
    XrClass *super = NULL;
    if (desc->super_name && strlen(desc->super_name) > 0) {
        if (desc->super_global_index >= 0) {
            if (desc->super_global_index < xr_isolate_get_vm_state(isolate)->builtin_count) {
                XrValue super_val = xr_isolate_get_vm_state(isolate)->builtins[desc->super_global_index];
                if (XR_IS_CLASS(super_val)) {
                    super = XR_TO_CLASS(super_val);
                }
            }
        }
        if (!super) {
            super = xr_class_lookup_by_name(isolate, desc->super_name);
            if (!super) {
                xr_log_warning("class", "from_descriptor: super class '%s' not found", desc->super_name);
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
    
    // Add instance fields
    for (uint32_t i = 0; i < desc->instance_field_count; i++) {
        XrFieldDescriptorEntry *field = &desc->instance_fields[i];
        xr_class_builder_add_field(builder, field->name, field->flags);
    }
    
    // Add static fields
    for (uint32_t i = 0; i < desc->static_field_count; i++) {
        XrFieldDescriptorEntry *field = &desc->static_fields[i];
        xr_class_builder_add_static_field(builder, field->name,
                                           field->default_value,
                                           field->flags | XR_FIELD_STATIC);
    }
    
    // Add instance methods with closures (all set BEFORE finalize)
    for (uint32_t i = 0; i < desc->instance_method_count; i++) {
        XrMethodDescriptorEntry *method = &desc->instance_methods[i];
        
        if (!proto || method->closure_index >= (uint32_t)DYNARRAY_COUNT(&proto->protos)) {
            xr_log_warning("class", "from_descriptor: invalid proto_index %u for method '%s'",
                    method->closure_index, method->name ? method->name : "(null)");
            continue;
        }
        
        XrProto *method_proto = DYNARRAY_GET(&proto->protos, method->closure_index, XrProto*);
        if (!method_proto) {
            xr_log_warning("class", "from_descriptor: NULL method proto for '%s'",
                    method->name ? method->name : "(null)");
            continue;
        }
        
        XrClosure *closure = create_method_closure_with_context(isolate, vm_ctx, method_proto, cl, base);
        if (!closure) continue;
        
        XrMethodType mtype = determine_method_type(method);
        xr_class_builder_add_method_closure(builder, method->name, closure,
                                             mtype, method->param_count,
                                             method->flags, method->op_type);
    }
    
    // Add static methods with closures
    for (uint32_t i = 0; i < desc->static_method_count; i++) {
        XrMethodDescriptorEntry *method = &desc->static_methods[i];
        
        if (!proto || method->closure_index >= (uint32_t)DYNARRAY_COUNT(&proto->protos)) {
            xr_log_warning("class", "from_descriptor: invalid proto_index %u for static method '%s'",
                    method->closure_index, method->name ? method->name : "(null)");
            continue;
        }
        
        XrProto *method_proto = DYNARRAY_GET(&proto->protos, method->closure_index, XrProto*);
        if (!method_proto) {
            xr_log_warning("class", "from_descriptor: NULL method proto for static method '%s'",
                    method->name ? method->name : "(null)");
            continue;
        }
        
        XrClosure *closure = create_method_closure_with_context(isolate, vm_ctx, method_proto, cl, base);
        if (!closure) continue;
        
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
    
    // Finalize - class is immutable after this point
    XrClass *cls = xr_class_builder_finalize(builder);
    if (!cls) {
        xr_log_warning("class", "from_descriptor: failed to finalize class '%s'", desc->class_name);
        return NULL;
    }
    
    // Backfill field type_name from descriptor entries (for reflection)
    if (cls->fields) {
        for (uint32_t i = 0; i < desc->instance_field_count && (int)i < cls->own_field_count; i++) {
            cls->fields[i].type_name = desc->instance_fields[i].type_name;
        }
        int sf_base = cls->own_field_count - cls->static_field_count;
        if (sf_base < 0) sf_base = 0;
        for (uint32_t i = 0; i < desc->static_field_count; i++) {
            int idx = sf_base + (int)desc->instance_field_count + (int)i;
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
            int global_idx = parent_field_count + (int)i;
            if (global_idx < xr_class_instance_field_count(cls)) {
                cls->field_default_values[global_idx] = desc->instance_fields[i].default_value;
            }
        }
    }
    
    // Set struct_layout for native struct storage
    if (desc->struct_layout) {
        cls->struct_layout = desc->struct_layout;
    }
    
    // Not yet initialized by static constructor
    cls->flags &= ~XR_CLASS_INITIALIZED;
    
    return cls;
}
