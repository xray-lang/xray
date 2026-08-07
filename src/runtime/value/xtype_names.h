/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_names.h - Type name constants
 *
 * KEY CONCEPT:
 *   Avoids hardcoded type name strings.
 *   Primitives (int/float/string/bool/null) lowercase.
 *   Object types (Array/Map/BigInt/DateTime etc.) PascalCase.
 */

#ifndef XTYPE_NAMES_H
#define XTYPE_NAMES_H

#include <stdint.h>
#include "../../base/xdefs.h"
#include "../../shared/xr_scalar_type.h"

/* ========== Compile-time String Length ========== */

#define XR_STRLEN_LITERAL(s) (sizeof(s) - 1)

/* ========== Primitive Types ========== */

#define TYPE_NAME_INT "int"
#define TYPE_NAME_I8 "i8"
#define TYPE_NAME_U8 "byte"
#define TYPE_NAME_I16 "i16"
#define TYPE_NAME_U16 "u16"
#define TYPE_NAME_I32 "i32"
#define TYPE_NAME_U32 "u32"
#define TYPE_NAME_I64 "int"
#define TYPE_NAME_U64 "u64"
#define TYPE_NAME_ISIZE "isize"
#define TYPE_NAME_USIZE "usize"
#define TYPE_NAME_FLOAT "float"
#define TYPE_NAME_F32 "f32"
#define TYPE_NAME_F64 "float"
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
#define TYPE_NAME_JSON "Json"
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

// Single source of truth for all user-visible types.
// Used by: typeof() return value, Type.xxx constants,
//          Analyzer method registry, LSP completion.
typedef enum {
    XR_TID_NULL = 0,
    XR_TID_BOOL,  // 1
    // Integer family
    XR_TID_I8,   // 2
    XR_TID_U8,   // 3
    XR_TID_I16,  // 4
    XR_TID_U16,  // 5
    XR_TID_I32,  // 6
    XR_TID_U32,  // 7
    XR_TID_INT,  // 8  (= int64, "int" is the canonical name)
    XR_TID_U64,  // 9
    // Float family
    XR_TID_F32,    // 10
    XR_TID_FLOAT,  // 11 (= float64, "float" is the canonical name)
    // Object types
    XR_TID_STRING,          // 12
    XR_TID_FUNCTION,        // 13
    XR_TID_ARRAY,           // 14
    XR_TID_SET,             // 15
    XR_TID_MAP,             // 16
    XR_TID_INSTANCE,        // 17
    XR_TID_OBJECT,          // 18 (Json object and structural object values)
    XR_TID_BIGINT,          // 19
    XR_TID_STRINGBUILDER,   // 20
    XR_TID_CHANNEL,         // 21
    XR_TID_REGEX,           // 22
    XR_TID_DATETIME,        // 23
    XR_TID_PANIC_INFO,      // 24
    XR_TID_ENUM_VALUE,      // 25
    XR_TID_ENUM_TYPE,       // 26
    XR_TID_BOUND_METHOD,    // 27
    XR_TID_ITERATOR,        // 28
    XR_TID_MODULE,          // 29
    XR_TID_COROUTINE,       // 30
    XR_TID_RANGE,           // 31
    XR_TID_TASK,            // 32
    XR_TID_NETCONN,         // 33
    XR_TID_NETLISTENER,     // 34
    XR_TID_ATOMIC,          // 35
    XR_TID_WORKQUEUE,       // 36
    XR_TID_RESULTGROUP,     // 37
    XR_TID_COUNTDOWNLATCH,  // 38
    XR_TID_SEMAPHORE,       // 39
    XR_TID_EVENTCOUNT,      // 40
    XR_TID_THREAD,          // 41
    XR_TID_BUFFER,          // 42
    // Analyzer-only type ID (not returned by typeof at runtime).
    XR_TID_RUNE,
    /* The Json value domain. Never returned by typeof either: a Json value
     * reports the tag it actually carries, which is any of null, bool, int,
     * float, string, array or object. This id names the domain those forms
     * belong to, so `is` against it asks about membership rather than tag
     * equality. XR_TID_OBJECT stays what it says -- an object value -- and no
     * longer doubles as the answer for Json. */
    XR_TID_JSON,
    XR_TID_COUNT
} XrTypeId;

/* Pinned for xr_elem_type.h, which hand-copies this id to stay
 * dependency-free. See the matching block in xr_type_names_core.h: RUNE lives
 * only in this enum, so its assertion has to be here. A renumbering that moves
 * it degrades every Slice<rune> to XR_ELEM_ANY at runtime and nowhere else. */
_Static_assert(XR_TID_RUNE == 43, "xr_elem_type.h: update xr_tid_to_elem_type case for RUNE");
/* Pinned so the two parallel enums cannot drift: xr_type_names_core.h stops at
 * BUFFER and has no RUNE, so it has to spell this id out numerically. */
_Static_assert(XR_TID_JSON == 44, "xr_type_names_core.h: XR_TID_JSON must match");

// Range check macros
#define XR_TID_IS_INT(tid) ((tid) >= XR_TID_I8 && (tid) <= XR_TID_U64)
#define XR_TID_IS_FLOAT(tid) ((tid) == XR_TID_F32 || (tid) == XR_TID_FLOAT)
#define XR_TID_IS_NUMBER(tid) (XR_TID_IS_INT(tid) || XR_TID_IS_FLOAT(tid))

/* The dynamic tag carries only the i64/f64 family; the static width lives in
 * the scalar representation. Fixed-width type tests need the way back from a
 * public type id to that representation. Returns XR_SCALAR_REP_NONE for ids
 * that name no scalar. */
static inline uint8_t xr_typeid_scalar_rep(XrTypeId tid) {
    switch (tid) {
        case XR_TID_I8:
            return XR_NATIVE_I8;
        case XR_TID_U8:
            return XR_NATIVE_U8;
        case XR_TID_I16:
            return XR_NATIVE_I16;
        case XR_TID_U16:
            return XR_NATIVE_U16;
        case XR_TID_I32:
            return XR_NATIVE_I32;
        case XR_TID_U32:
            return XR_NATIVE_U32;
        case XR_TID_INT:
            return XR_NATIVE_I64;
        case XR_TID_U64:
            return XR_NATIVE_U64;
        case XR_TID_F32:
            return XR_NATIVE_F32;
        case XR_TID_FLOAT:
            return XR_NATIVE_F64;
        default:
            return XR_SCALAR_REP_NONE;
    }
}

/* Inverse of xr_typeid_scalar_rep. isize/usize have no public id of their own
 * and collapse onto the widest id of their signedness, which is exact on 64-bit
 * targets and the closest available answer elsewhere. */
static inline XrTypeId xr_scalar_rep_typeid(uint8_t scalar_rep) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
            return XR_TID_I8;
        case XR_NATIVE_U8:
            return XR_TID_U8;
        case XR_NATIVE_I16:
            return XR_TID_I16;
        case XR_NATIVE_U16:
            return XR_TID_U16;
        case XR_NATIVE_I32:
            return XR_TID_I32;
        case XR_NATIVE_U32:
            return XR_TID_U32;
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return XR_TID_U64;
        case XR_NATIVE_F32:
            return XR_TID_F32;
        case XR_NATIVE_F64:
            return XR_TID_FLOAT;
        case XR_NATIVE_I64:
        case XR_NATIVE_ISIZE:
        default:
            return XR_TID_INT;
    }
}

/* ========== Utility Functions ========== */

XR_FUNC int xr_type_from_name(const char *type_name);
XR_FUNC int xr_is_valid_type_name(const char *type_name);

// Type ID → name string (defined in xvalue.c)
XR_FUNC const char *xr_typeid_name(XrTypeId tid);

// XrType kind → XrTypeId (for reified generics, defined in xvalue.c)
struct XrType;
XR_FUNC uint8_t xr_type_to_tid(const struct XrType *type);

#endif  // XTYPE_NAMES_H
