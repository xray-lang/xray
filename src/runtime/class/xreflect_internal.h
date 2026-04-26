/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_internal.h - Reflection system internals (zero-copy view)
 *
 * KEY CONCEPT:
 *   Zero-copy: metadata stores pointers+indices, data fetched from XrClass at runtime.
 *   Per-class cache for Field/Method wrapper objects.
 *   O(1) lookup with minimal memory footprint.
 */

#ifndef XREFLECT_INTERNAL_H
#define XREFLECT_INTERNAL_H

#include "xreflect_api.h"
#include "xclass.h"
#include "../value/xtype.h"
#include "xinstance.h"
#include "../object/xarray.h"
#include "../object/xstring.h"
#include "xmethod.h"
#include "../xisolate_api.h"
#include "../value/xvalue.h"
#include "../../base/xmalloc.h"
#include "xclass_system.h"
#include "../symbol/xsymbol_table.h"
#include "../value/xtype_names.h"
#include "../../base/xhashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #define REFLECTION_API_DEBUG
#ifdef REFLECTION_API_DEBUG
#define API_DEBUG_PRINT(...) fprintf(stderr, "[ReflectAPI] " __VA_ARGS__)
#else
#define API_DEBUG_PRINT(...) ((void)0)
#endif

typedef struct XrTypeMetadata XrTypeMetadata;
typedef struct XrFieldMetadata XrFieldMetadata;
typedef struct XrMethodMetadata XrMethodMetadata;
typedef struct XrParameterMetadata XrParameterMetadata;
typedef struct TypeWrapper TypeWrapper;
typedef struct FieldWrapper FieldWrapper;
typedef struct MethodWrapper MethodWrapper;
typedef struct ParameterWrapper ParameterWrapper;

/* ========== Metadata (Zero-copy) ========== */

// klass is sole data source; NULL for special types like void
struct XrTypeMetadata {
    XrClass *klass;
    const char *name;             // Only used when klass==NULL
};

// Data fetched from owner->fields[field_index]
struct XrFieldMetadata {
    XrClass *owner;
    int field_index;
};

// Data fetched from owner->methods[method_index]
struct XrMethodMetadata {
    XrClass *owner;
    int method_index;
};

/* ========== Wrapper Objects ========== */

struct TypeWrapper {
    XrGCHeader gc;
    XrTypeMetadata metadata;
};

struct FieldWrapper {
    XrGCHeader gc;
    XrFieldMetadata metadata;
};

struct MethodWrapper {
    XrGCHeader gc;
    XrMethodMetadata metadata;
};

struct XrParameterMetadata {
    const char *name;
    int index;
};

struct ParameterWrapper {
    XrGCHeader gc;
    XrParameterMetadata metadata;
};

/* ========== Helpers ========== */

static inline XrValue wrapper_to_value(void *wrapper) {
    return XR_FROM_PTR(wrapper);
}

XR_FUNC XrValue xr_create_type_object(XrayIsolate *X, XrTypeMetadata *meta);
XR_FUNC XrValue xr_create_field_object(XrayIsolate *X, XrFieldMetadata *field);
XR_FUNC XrValue xr_create_method_object(XrayIsolate *X, XrMethodMetadata *method);
XR_FUNC XrValue xr_create_constructor_object(XrayIsolate *X, XrMethodMetadata *ctor);
XR_FUNC XrValue xr_create_parameter_object(XrayIsolate *X, XrParameterMetadata *param);

XR_FUNC XrTypeMetadata* xr_get_type_metadata(XrValue type_obj);
XR_FUNC XrFieldMetadata* xr_get_field_metadata(XrValue field_obj);
XR_FUNC XrMethodMetadata* xr_get_method_metadata(XrValue method_obj);
XR_FUNC XrParameterMetadata* xr_get_parameter_metadata(XrValue param_obj);

#endif // XREFLECT_INTERNAL_H
