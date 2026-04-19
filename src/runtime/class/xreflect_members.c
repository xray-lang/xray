/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_members.c - Field, Method and Constructor reflection methods
 *
 * CONSOLIDATES:
 *   This file replaces the old xreflect_field.c / xreflect_method_stubs.c /
 *   xreflect_constructor.c trio. Each of those was under 200 lines and
 *   did a small slice of "expose an XrClass member through the reflection
 *   wrappers". The layout was historical -- splitting them by member
 *   kind just meant three tiny .c files sharing the same includes, the
 *   same two static metadata accessors (with mildly divergent names),
 *   and the same style of short property getters. Keeping them as one
 *   TU makes the code easier to navigate and avoids the duplicated
 *   static helper.
 *
 * CONVENTIONS:
 *   - Zero-copy access: every getter reads the underlying XrClass or
 *     XrFieldDescriptor directly; we never copy strings or allocate
 *     unless the public API requires a fresh XrString/XrInstance.
 *   - All metadata pointers come from the wrapper instances cached in
 *     klass->reflect_cache (built by xr_class_builder_finalize).
 */

#include "xreflect_internal.h"
#include "../../base/xchecks.h"
#include "xclass.h"
#include "xmethod.h"
#include "xreflect_registry.h"
#include "../object/xstring.h"
#include "../xvm_call.h"
#include <string.h>

/* ========== Shared wrapper -> metadata accessors ========== */

static inline XrMethodMetadata *get_method_meta(XrValue val) {
    if (!XR_IS_INSTANCE(val)) return NULL;
    MethodWrapper *wrapper = (MethodWrapper *)XR_TO_INSTANCE(val);
    return &wrapper->metadata;
}

static inline XrFieldMetadata *get_field_meta(XrValue val) {
    if (!XR_IS_INSTANCE(val)) return NULL;
    FieldWrapper *wrapper = (FieldWrapper *)XR_TO_INSTANCE(val);
    return &wrapper->metadata;
}

/* ========================================================== */
/* === Field section (was xreflect_field.c)               === */
/* ========================================================== */

/* ========== Field Property Getters ========== */

XrValue xr_field_getName(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "field_getName: NULL isolate");
    if (nargs < 1) return xr_null();

    XrFieldMetadata *meta = get_field_meta(args[0]);
    if (!meta || !meta->owner) return xr_null();

    XrFieldDescriptor *desc = &meta->owner->fields[meta->field_index];
    if (!desc->name) return xr_null();

    return xr_string_value(xr_string_intern(isolate, desc->name, strlen(desc->name), 0));
}

XrValue xr_field_getType(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "field_getType: NULL isolate");
    if (nargs < 1) return xr_null();

    XrFieldMetadata *field = get_field_meta(args[0]);
    if (!field || !field->owner) return xr_null();

    XrFieldDescriptor *desc = &field->owner->fields[field->field_index];
    if (desc->type_name && desc->type_name[0] != '\0') {
        XrTypeMetadata *meta = xr_registry_find_type(isolate, desc->type_name);
        if (meta) return xr_create_type_object(isolate, meta);
        // Return type name as string if not in registry
        XrString *s = xr_string_new(isolate, desc->type_name, strlen(desc->type_name));
        if (s) return xr_string_value(s);
    }
    return xr_null();
}

XrValue xr_field_getIsStatic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1) return xr_bool(false);

    XrFieldMetadata *field = get_field_meta(args[0]);
    if (!field || !field->owner) return xr_bool(false);

    XrFieldDescriptor *desc = &field->owner->fields[field->field_index];
    return xr_bool((desc->flags & XR_FIELD_STATIC) != 0);
}

XrValue xr_field_getIsReadonly(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1) return xr_bool(false);

    XrFieldMetadata *field = get_field_meta(args[0]);
    if (!field || !field->owner) return xr_bool(false);

    XrFieldDescriptor *desc = &field->owner->fields[field->field_index];
    return xr_bool(desc->flags & XR_FIELD_FINAL);
}

XrValue xr_field_getIsPrivate(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    XrFieldMetadata *meta = get_field_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);

    return xr_bool(xr_class_is_field_private(meta->owner, meta->field_index));
}

XrValue xr_field_getDeclaringType(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return xr_null();

    XrFieldMetadata *meta = get_field_meta(args[0]);
    if (!meta || !meta->owner) return xr_null();

    XrTypeMetadata type_meta = { .klass = meta->owner };
    return xr_create_type_object(X, &type_meta);
}

/* ========== Field Methods ========== */

XrValue xr_field_get(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 2) {
        fprintf(stderr, "Field.get: requires 2 arguments\n");
        return xr_null();
    }

    XrFieldMetadata *field = get_field_meta(args[0]);
    XrValue instance = args[1];

    if (!field || !field->owner) return xr_null();
    if (!XR_IS_INSTANCE(instance)) {
        fprintf(stderr, "Field.get: argument must be an instance\n");
        return xr_null();
    }

    return xr_instance_get_field_by_index(XR_TO_INSTANCE(instance), field->field_index);
}

XrValue xr_field_set(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 3) {
        fprintf(stderr, "Field.set: requires 3 arguments\n");
        return xr_null();
    }

    XrFieldMetadata *field = get_field_meta(args[0]);
    XrValue instance = args[1];
    XrValue value = args[2];

    if (!field || !field->owner) return xr_null();
    if (!XR_IS_INSTANCE(instance)) {
        fprintf(stderr, "Field.set: argument must be an instance\n");
        return xr_null();
    }

    xr_instance_set_field_by_index(XR_TO_INSTANCE(instance), field->field_index, value);
    return xr_null();
}

XrValue xr_field_getStatic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1) {
        fprintf(stderr, "Field.getStatic: requires 1 argument\n");
        return xr_null();
    }

    XrFieldMetadata *field = get_field_meta(args[0]);
    if (!field || !field->owner) return xr_null();

    XrClass *klass = field->owner;
    XrFieldDescriptor *desc = &klass->fields[field->field_index];

    if (!(desc->flags & XR_FIELD_STATIC)) {
        fprintf(stderr, "Field.getStatic: field is not static\n");
        return xr_null();
    }

    int static_index = desc->static_slot;
    if (static_index >= 0 && static_index < klass->static_field_count && klass->static_field_values) {
        return klass->static_field_values[static_index];
    }

    return xr_null();
}

XrValue xr_field_setStatic(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 2) {
        fprintf(stderr, "Field.setStatic: requires 2 arguments\n");
        return xr_null();
    }

    XrFieldMetadata *field = get_field_meta(args[0]);
    XrValue value = args[1];

    if (!field || !field->owner) return xr_null();

    XrClass *klass = field->owner;
    XrFieldDescriptor *desc = &klass->fields[field->field_index];

    if (!(desc->flags & XR_FIELD_STATIC)) {
        fprintf(stderr, "Field.setStatic: field is not static\n");
        return xr_null();
    }

    int static_index = desc->static_slot;
    if (static_index >= 0 && static_index < klass->static_field_count && klass->static_field_values) {
        klass->static_field_values[static_index] = value;
    }

    return xr_null();
}

/* ========================================================== */
/* === Method property getters (was xreflect_method_stubs)=== */
/* ========================================================== */

XrValue xr_method_getIsStatic(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);

    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_STATIC) != 0);
}

XrValue xr_method_getIsPrivate(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);

    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_PRIVATE) != 0);
}

XrValue xr_method_getIsAbstract(XrayIsolate *X, XrValue *args, int nargs) {
    (void)X;
    if (nargs < 1) return xr_bool(false);

    XrMethodMetadata *meta = get_method_meta(args[0]);
    if (!meta || !meta->owner) return xr_bool(false);

    XrMethod *method = &meta->owner->methods[meta->method_index];
    return xr_bool((method->flags & XMETHOD_FLAG_ABSTRACT) != 0);
}

/* ========================================================== */
/* === Constructor section (was xreflect_constructor.c)   === */
/* ========================================================== */

XrValue xr_constructor_newInstance(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "constructor_newInstance: NULL isolate");
    if (nargs < 1) {
        fprintf(stderr, "Constructor.newInstance: requires at least 1 argument\n");
        return xr_null();
    }

    XrMethodMetadata *ctor = xr_get_method_metadata(args[0]);

    if (!ctor || !ctor->owner) {
        fprintf(stderr, "Constructor.newInstance: invalid constructor\n");
        return xr_null();
    }

    XrayIsolate *X = isolate;
    XrClass *klass = ctor->owner;

    XrInstance *instance = xr_instance_new(X, klass);
    XrValue instance_val = xr_value_from_instance(instance);

    XrArray *arg_array = NULL;
    int arg_count = 0;

    if (nargs >= 2 && XR_IS_ARRAY(args[1])) {
        arg_array = XR_TO_ARRAY(args[1]);
        arg_count = arg_array->length;
    }

    if (arg_count + 1 > 256) {
        fprintf(stderr, "Constructor.newInstance: too many arguments (%d), max 255\n", arg_count);
        return xr_null();
    }

    XrValue call_args[256];
    int total_args = 0;
    call_args[total_args++] = instance_val;

    if (arg_array && arg_count > 0) {
        for (int i = 0; i < arg_count; i++) {
            call_args[total_args++] = ((XrValue *)arg_array->data)[i];
        }
    }

    XrMethod *method = &ctor->owner->methods[ctor->method_index];

    if (method->type == XMETHOD_PRIMITIVE) {
        XrCFunctionPtr func = method->as.primitive;
        func(isolate, call_args, total_args);
    } else if (method->type == XMETHOD_CLOSURE) {
        XrClosure *closure = method->as.closure;
        if (closure) {
            xr_vm_call_closure(isolate, closure, call_args, total_args);
        }
    }

    return instance_val;
}
