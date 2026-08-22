/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_type_names_core.h - Runtime-neutral public type names and TypeId values.
 *
 * This is the only place the XrTypeId enum and the tid -> display name mapping
 * exist. Both are expanded from xr_type_names.def, so an id and the name it
 * shows cannot drift apart. Depending on <stdint.h> alone keeps the header
 * usable from the standalone AOT profile, which links no runtime value layer.
 */

#ifndef XR_TYPE_NAMES_CORE_H
#define XR_TYPE_NAMES_CORE_H

#include <stdint.h>

/* ========== Compile-time String Length ========== */

#define XR_STRLEN_LITERAL(s) (sizeof(s) - 1)

/* ========== Primitive Types ========== */

#define TYPE_NAME_I8 "i8"
#define TYPE_NAME_U8 "u8"
#define TYPE_NAME_I16 "i16"
#define TYPE_NAME_U16 "u16"
#define TYPE_NAME_I32 "i32"
#define TYPE_NAME_U32 "u32"
#define TYPE_NAME_I64 "i64"
#define TYPE_NAME_U64 "u64"
#define TYPE_NAME_ISIZE "isize"
#define TYPE_NAME_USIZE "usize"
#define TYPE_NAME_F32 "f32"
#define TYPE_NAME_F64 "f64"
#define TYPE_NAME_STRING "string"
#define TYPE_NAME_BOOL "bool"
#define TYPE_NAME_RUNE "rune"
#define TYPE_NAME_NULL "null"
#define TYPE_NAME_VOID "void"
#define TYPE_NAME_UNIT "()"
#define TYPE_NAME_NEVER "never"

/* ========== Container Types ========== */

#define TYPE_NAME_ARRAY "Array"
#define TYPE_NAME_MAP "Map"
#define TYPE_NAME_SET "Set"
#define TYPE_NAME_SLICE "Slice"

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
#define TYPE_NAME_PANIC_INFO "PanicInfo"
#define TYPE_NAME_MODULE "module"
#define TYPE_NAME_ITERATOR "iterator"
#define TYPE_NAME_STRUCT "struct"
#define TYPE_NAME_JSON "JSON.Value"
#define TYPE_NAME_STRINGBUILDER "StringBuilder"
#define TYPE_NAME_BUFFER "Buffer"
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
#define TYPE_NAME_COUNTDOWNLATCH "CountdownLatch"
#define TYPE_NAME_SEMAPHORE "Semaphore"
#define TYPE_NAME_EVENTCOUNT "EventCount"
#define TYPE_NAME_THREAD "Thread"

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

/* ========== Builtin Global Names ========== */

#define GLOBAL_NAME_CORO "Coro"
#define GLOBAL_NAME_COROPOOL "CoroPool"

/* ========== Unified Type ID ========== */

/* Single source of truth for all user-visible types.
 * Used by typeof(), Type.xxx constants, analyzer registries, LSP completion,
 * VM runtime helpers and AOT direct codegen helpers. */
typedef enum {
#define XR_TYPE_NAME(suffix, id, display) XR_TID_##suffix = (id),
#include "xr_type_names.def"
#undef XR_TYPE_NAME
    XR_TID_COUNT
} XrTypeId;

/* Every id is pinned to its number. xr_elem_type.h hand-copies the scalar ids
 * as literals so it can stay dependency-free, and these are what keep that copy
 * honest: a renumbering breaks the BUILD rather than a Slice at runtime.
 * Update xr_tid_to_elem_type together with any change to the row order. */
#define XR_TYPE_NAME(suffix, id, display)                                                          \
    _Static_assert(XR_TID_##suffix == (id), "public type id drifted: " #suffix);
#include "xr_type_names.def"
#undef XR_TYPE_NAME

#define XR_TID_IS_INT(tid)                                                                    \
    (((tid) >= XR_TID_I8 && (tid) <= XR_TID_U64) || (tid) == XR_TID_ISIZE ||                  \
     (tid) == XR_TID_USIZE)
#define XR_TID_IS_FLOAT(tid) ((tid) == XR_TID_F32 || (tid) == XR_TID_F64)
#define XR_TID_IS_NUMBER(tid) (XR_TID_IS_INT(tid) || XR_TID_IS_FLOAT(tid))

/* The canonical display name for a public type id. Every consumer -- VM,
 * hosted AOT and standalone AOT -- answers through this one switch, so the
 * same id can never print two different names. */
static inline const char *xr_type_name_from_tid(XrTypeId tid) {
    switch (tid) {
#define XR_TYPE_NAME(suffix, id, display)                                                          \
    case XR_TID_##suffix:                                                                          \
        return display;
#include "xr_type_names.def"
#undef XR_TYPE_NAME
        case XR_TID_COUNT:
        default:
            return TYPE_NAME_UNKNOWN;
    }
}

#endif /* XR_TYPE_NAMES_CORE_H */
