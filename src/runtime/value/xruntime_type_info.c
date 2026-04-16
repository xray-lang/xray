/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xruntime_type_info.c - Runtime type metadata implementation
 *
 * KEY CONCEPT:
 *   Global singleton type metadata for GC callbacks and method dispatch.
 */

#include "xtype_info.h"
#include "../../base/xchecks.h"
#include <stdlib.h>
#include <string.h>

// ========== Global Type Metadata Definitions ==========

// Macro to define runtime type info singleton
#define DEFINE_RTI(var_name, display_name, kind_enum) \
    XrRuntimeTypeInfo rti_##var_name = { \
        .name = display_name, \
        .size = 0, \
        .kind = kind_enum, \
        .mark_fn = NULL, \
        .free_fn = NULL, \
        .methods = NULL \
    }

// Container types
DEFINE_RTI(string, "String", XR_TID_STRING);
DEFINE_RTI(array, "Array", XR_TID_ARRAY);
DEFINE_RTI(map, "Map", XR_TID_MAP);
DEFINE_RTI(set, "Set", XR_TID_SET);
DEFINE_RTI(bytes, "Array", XR_TID_BYTES);
DEFINE_RTI(typed_array, "TypedArray", XR_TID_TYPED_ARRAY);

// Function types
DEFINE_RTI(function, "Function", XR_TID_FUNCTION);
DEFINE_RTI(closure, "Closure", XR_TID_FUNCTION);
DEFINE_RTI(cfunction, "CFunction", XR_TID_FUNCTION);
DEFINE_RTI(upvalue, "Upvalue", XR_TID_UPVALUE);
DEFINE_RTI(bound_method, "BoundMethod", XR_TID_BOUND_METHOD);

// OOP types
DEFINE_RTI(class, "Class", XR_TID_INSTANCE);
DEFINE_RTI(instance, "Instance", XR_TID_INSTANCE);
DEFINE_RTI(enum_type, "EnumType", XR_TID_ENUM_TYPE);
DEFINE_RTI(enum_value, "EnumValue", XR_TID_ENUM_VALUE);

// Concurrency types
DEFINE_RTI(coroutine, "Coroutine", XR_TID_COROUTINE);
DEFINE_RTI(channel, "Channel", XR_TID_CHANNEL);

// Utility types
DEFINE_RTI(module, "Module", XR_TID_MODULE);
DEFINE_RTI(iterator, "Iterator", XR_TID_ITERATOR);
DEFINE_RTI(stringbuilder, "StringBuilder", XR_TID_STRINGBUILDER);
DEFINE_RTI(json, "Json", XR_TID_JSON);
DEFINE_RTI(bigint, "BigInt", XR_TID_BIGINT);
DEFINE_RTI(datetime, "DateTime", XR_TID_DATETIME);
DEFINE_RTI(regex, "Regex", XR_TID_REGEX);

// Error types
DEFINE_RTI(error, "Error", XR_TID_ERROR);
DEFINE_RTI(exception, "Exception", XR_TID_EXCEPTION);

#undef DEFINE_RTI

// ========== Type Metadata API Implementation ==========

// TODO: RTI system is defined but not yet integrated into GC/method dispatch.
// Wire up mark_fn/free_fn/methods when connecting to GC and builtin method tables.

// Initialize all type metadata
void runtime_type_info_init(void) {
    // GC function pointers can be set here
    // Currently NULL, will be set during integration
}

// Get type metadata by XrTypeId
XrRuntimeTypeInfo* rti_get(XrTypeId kind) {
    switch (kind) {
        // Primitive types (value types, no heap metadata)
        case XR_TID_NULL:          return NULL;
        case XR_TID_BOOL:          return NULL;
        case XR_TID_INT:           return NULL;
        case XR_TID_FLOAT:         return NULL;
        
        // Container types
        case XR_TID_STRING:        return &rti_string;
        case XR_TID_ARRAY:         return &rti_array;
        case XR_TID_MAP:           return &rti_map;
        case XR_TID_SET:           return &rti_set;
        case XR_TID_BYTES:         return &rti_bytes;
        case XR_TID_TYPED_ARRAY:   return &rti_typed_array;
        
        // Function types
        case XR_TID_FUNCTION:      return &rti_function;
        case XR_TID_UPVALUE:       return &rti_upvalue;
        case XR_TID_BOUND_METHOD:  return &rti_bound_method;
        
        // OOP types
        case XR_TID_INSTANCE: return &rti_instance;
        case XR_TID_ENUM_TYPE:     return &rti_enum_type;
        case XR_TID_ENUM_VALUE:    return &rti_enum_value;
        
        // Concurrency types
        case XR_TID_COROUTINE:     return &rti_coroutine;
        case XR_TID_CHANNEL:       return &rti_channel;
        
        // Utility types
        case XR_TID_MODULE:        return &rti_module;
        case XR_TID_ITERATOR:      return &rti_iterator;
        case XR_TID_STRINGBUILDER: return &rti_stringbuilder;
        case XR_TID_JSON:          return &rti_json;
        case XR_TID_BIGINT:        return &rti_bigint;
        case XR_TID_DATETIME:      return &rti_datetime;
        case XR_TID_REGEX:         return &rti_regex;
        
        // Error types
        case XR_TID_ERROR:         return &rti_error;
        case XR_TID_EXCEPTION:     return &rti_exception;
        
        default:               return NULL;
    }
}

// Set method table
void rti_set_methods(XrRuntimeTypeInfo *rti, XrHashMap *methods) {
    if (rti) {
        rti->methods = methods;
    }
}

