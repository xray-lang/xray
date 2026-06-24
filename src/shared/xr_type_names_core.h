/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_type_names_core.h - Runtime-neutral public type names and TypeId values.
 */

#ifndef XR_TYPE_NAMES_CORE_H
#define XR_TYPE_NAMES_CORE_H

#include <stdint.h>

/* ========== Compile-time String Length ========== */

#define XR_STRLEN_LITERAL(s) (sizeof(s) - 1)

/* ========== Primitive Types ========== */

#define TYPE_NAME_INT "int"
#define TYPE_NAME_INT8 "int8"
#define TYPE_NAME_UINT8 "uint8"
#define TYPE_NAME_INT16 "int16"
#define TYPE_NAME_UINT16 "uint16"
#define TYPE_NAME_INT32 "int32"
#define TYPE_NAME_UINT32 "uint32"
#define TYPE_NAME_INT64 "int64"
#define TYPE_NAME_UINT64 "uint64"
#define TYPE_NAME_FLOAT "float"
#define TYPE_NAME_FLOAT32 "float32"
#define TYPE_NAME_FLOAT64 "float64"
#define TYPE_NAME_STRING "string"
#define TYPE_NAME_BOOL "bool"
#define TYPE_NAME_NULL "null"
#define TYPE_NAME_VOID "void"
#define TYPE_NAME_UNIT "()"
#define TYPE_NAME_NEVER "never"

/* ========== Container Types ========== */

#define TYPE_NAME_ARRAY "Array"
#define TYPE_NAME_MAP "Map"
#define TYPE_NAME_SET "Set"
#define TYPE_NAME_WEAKMAP "WeakMap"
#define TYPE_NAME_WEAKSET "WeakSet"
#define TYPE_NAME_BYTES "Bytes"

/* ========== Runtime Types ========== */

#define TYPE_NAME_OBJECT "object"
#define TYPE_NAME_FUNCTION "function"
#define TYPE_NAME_CFUNCTION "cfunction"
#define TYPE_NAME_CLASS "class"
#define TYPE_NAME_CLASS_LITE "class_lite"
#define TYPE_NAME_INSTANCE "instance"
#define TYPE_NAME_BOUND_METHOD "bound_method"
#define TYPE_NAME_ENUM_TYPE "enum_type"
#define TYPE_NAME_ENUM_VALUE "enum_value"
#define TYPE_NAME_ERROR "error"
#define TYPE_NAME_EXCEPTION "Exception"
#define TYPE_NAME_MODULE "module"
#define TYPE_NAME_ITERATOR "iterator"
#define TYPE_NAME_STRUCT "struct"
#define TYPE_NAME_JSON "Json"
#define TYPE_NAME_STRINGBUILDER "StringBuilder"
#define TYPE_NAME_UNKNOWN "unknown"
#define TYPE_NAME_BIGINT "BigInt"
#define TYPE_NAME_CLOSURE "closure"
#define TYPE_NAME_UPVALUE "upvalue"
#define TYPE_NAME_OPTIONAL "optional"
#define TYPE_NAME_TYPE_PARAM "type_param"
#define TYPE_NAME_RESERVED "reserved"

/* ========== Coroutine/Concurrency Types ========== */

#define TYPE_NAME_COROUTINE "Coroutine"
#define TYPE_NAME_CHANNEL "Channel"
#define TYPE_NAME_COROPOOL "CoroPool"
#define TYPE_NAME_TASK "Task"
#define TYPE_NAME_ATOMIC "Atomic"
#define TYPE_NAME_WORKQUEUE "WorkQueue"
#define TYPE_NAME_RESULTGROUP "ResultGroup"

/* ========== DateTime Types ========== */

#define TYPE_NAME_DATETIME "DateTime"
#define TYPE_NAME_REGEX "Regex"

/* ========== Logger Type ========== */

#define TYPE_NAME_LOGGER "Logger"
#define TYPE_NAME_RANGE "Range"
#define TYPE_NAME_NETCONN "NetConn"
#define TYPE_NAME_NETLISTENER "NetListener"

/* ========== Language Keywords ========== */

#define XR_KEYWORD_CONSTRUCTOR "constructor"

/* ========== Builtin Class Names ========== */

#define CLASS_NAME_OBJECT "Object"
#define CLASS_NAME_ENUM "Enum"
#define CLASS_NAME_REFLECT "Reflect"
#define CLASS_NAME_TYPE "Type"
#define CLASS_NAME_FIELD "Field"
#define CLASS_NAME_METHOD "Method"
#define CLASS_NAME_CONSTRUCTOR "Constructor"
#define CLASS_NAME_PARAMETER "Parameter"

/* ========== Builtin Global Names ========== */

#define GLOBAL_NAME_CORO "Coro"
#define GLOBAL_NAME_COROPOOL "CoroPool"

/* ========== Unified Type ID ========== */

/* Single source of truth for all user-visible types.
 * Used by typeof(), Type.xxx constants, analyzer registries, LSP completion,
 * VM runtime helpers and AOT direct codegen helpers. */
typedef enum {
    XR_TID_NULL = 0,
    XR_TID_BOOL,          /* 1 */
    XR_TID_INT8,          /* 2 */
    XR_TID_UINT8,         /* 3 */
    XR_TID_INT16,         /* 4 */
    XR_TID_UINT16,        /* 5 */
    XR_TID_INT32,         /* 6 */
    XR_TID_UINT32,        /* 7 */
    XR_TID_INT,           /* 8  (= int64, "int" is the canonical name) */
    XR_TID_UINT64,        /* 9 */
    XR_TID_FLOAT32,       /* 10 */
    XR_TID_FLOAT,         /* 11 (= float64, "float" is the canonical name) */
    XR_TID_STRING,        /* 12 */
    XR_TID_FUNCTION,      /* 13 */
    XR_TID_ARRAY,         /* 14 */
    XR_TID_SET,           /* 15 */
    XR_TID_MAP,           /* 16 */
    XR_TID_INSTANCE,      /* 17 */
    XR_TID_JSON,          /* 18 */
    XR_TID_BIGINT,        /* 19 */
    XR_TID_STRINGBUILDER, /* 20 */
    XR_TID_CHANNEL,       /* 21 */
    XR_TID_REGEX,         /* 22 */
    XR_TID_DATETIME,      /* 23 */
    XR_TID_EXCEPTION,     /* 24 */
    XR_TID_ENUM_VALUE,    /* 25 */
    XR_TID_ENUM_TYPE,     /* 26 */
    XR_TID_BOUND_METHOD,  /* 27 */
    XR_TID_ITERATOR,      /* 28 */
    XR_TID_MODULE,        /* 29 */
    XR_TID_COROUTINE,     /* 30 */
    XR_TID_RANGE,         /* 31 */
    XR_TID_TASK,          /* 32 */
    XR_TID_NETCONN,       /* 33 */
    XR_TID_NETLISTENER,   /* 34 */
    XR_TID_ATOMIC,        /* 35 */
    XR_TID_WORKQUEUE,     /* 36 */
    XR_TID_RESULTGROUP,   /* 37 */
    XR_TID_WEAKMAP,       /* 38, analyzer-only */
    XR_TID_WEAKSET,       /* 39, analyzer-only */
    XR_TID_COUNT
} XrTypeId;

#define XR_TID_INT64 XR_TID_INT
#define XR_TID_FLOAT64 XR_TID_FLOAT

#define XR_TID_IS_INT(tid) ((tid) >= XR_TID_INT8 && (tid) <= XR_TID_UINT64)
#define XR_TID_IS_FLOAT(tid) ((tid) == XR_TID_FLOAT32 || (tid) == XR_TID_FLOAT)
#define XR_TID_IS_NUMBER(tid) (XR_TID_IS_INT(tid) || XR_TID_IS_FLOAT(tid))

#endif /* XR_TYPE_NAMES_CORE_H */
