/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_info.h - Runtime type metadata
 *
 * KEY CONCEPT:
 *   Focused on GC and object management.
 *   Uses XrTypeId, decoupled from compile-time types.
 */

#ifndef XTYPE_INFO_H
#define XTYPE_INFO_H

#include "xtype_names.h"
#include "../../base/xhashmap.h"

typedef struct XrRuntimeTypeInfo {
    const char *name;
    size_t size;
    XrTypeId kind;
    void (*mark_fn)(void *gc, void *obj, void *X);
    void (*free_fn)(void *obj);
    XrHashMap *methods;
} XrRuntimeTypeInfo;

/* ========== Global Type Metadata (singletons) ========== */

// Container types
extern XrRuntimeTypeInfo rti_string;
extern XrRuntimeTypeInfo rti_array;
extern XrRuntimeTypeInfo rti_map;
extern XrRuntimeTypeInfo rti_set;
extern XrRuntimeTypeInfo rti_bytes;
extern XrRuntimeTypeInfo rti_typed_array;

// Function types
extern XrRuntimeTypeInfo rti_function;
extern XrRuntimeTypeInfo rti_closure;
extern XrRuntimeTypeInfo rti_cfunction;
extern XrRuntimeTypeInfo rti_upvalue;
extern XrRuntimeTypeInfo rti_bound_method;

// OOP types
extern XrRuntimeTypeInfo rti_class;
extern XrRuntimeTypeInfo rti_instance;
extern XrRuntimeTypeInfo rti_enum_type;
extern XrRuntimeTypeInfo rti_enum_value;

// Concurrency types
extern XrRuntimeTypeInfo rti_coroutine;
extern XrRuntimeTypeInfo rti_channel;

// Utility types
extern XrRuntimeTypeInfo rti_module;
extern XrRuntimeTypeInfo rti_iterator;
extern XrRuntimeTypeInfo rti_stringbuilder;
extern XrRuntimeTypeInfo rti_json;
extern XrRuntimeTypeInfo rti_bigint;
extern XrRuntimeTypeInfo rti_datetime;
extern XrRuntimeTypeInfo rti_regex;

// Error types
extern XrRuntimeTypeInfo rti_error;
extern XrRuntimeTypeInfo rti_exception;

/* ========== Type Metadata API ========== */

XR_FUNC void runtime_type_info_init(void);
XR_FUNC XrRuntimeTypeInfo* rti_get(XrTypeId kind);
XR_FUNC void rti_set_methods(XrRuntimeTypeInfo *rti, XrHashMap *methods);

#endif // XTYPE_INFO_H
