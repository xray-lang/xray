/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue.h - Tagged Union value system (16 bytes, full 64-bit int)
 *
 * KEY CONCEPT:
 *   XrValue is a 16-byte struct-of-union. The tag field indicates the
 *   precise type; the value is stored in a union of int64/double/pointer.
 *
 * MEMORY LAYOUT (16 bytes, struct-of-union):
 *   [0]    tag       uint8_t   - XrValueTag, determines value interpretation
 *   [1]    flags     uint8_t   - XR_FLAG_* bits (weak/frozen/interned/cow)
 *   [2-3]  heap_type uint16_t  - GC object type (valid only when tag==PTR)
 *   [4-7]  ext       uint32_t  - reserved for future extensions (= 0 now)
 *   [8-15] payload   union     - int64 / double / pointer
 *
 * WHY THIS DESIGN:
 *   - tag at byte 0: ARM64 ldrb [reg] needs no offset immediate (optimal)
 *   - flags at byte 1: adjacent to tag, ldrb [reg, #1]
 *   - no _pad: all bytes used, natural alignment for all fields
 *   - [0-7] as uint64 descriptor: one ldr loads entire type metadata
 *   - heap_type uint16: xray GC types < 100, 16-bit sufficient
 *   - ext uint32: future use without changing layout
 *   - payload at byte 8: 8-byte aligned, single ldr/str on all architectures
 */

#ifndef XVALUE_H
#define XVALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "../../base/xdefs.h"
#include "../../base/xchecks.h"

// Internal base types
typedef int64_t xr_Integer;
typedef double xr_Number;

// Forward declarations
#include "../../base/xforward_decl.h"

// GC header
#include "../gc/xgc_header.h"

/* ========== Platform Detection ========== */

#if INTPTR_MAX == INT64_MAX
#define XR_64BIT 1
#else
#define XR_64BIT 0
#endif

#if !XR_64BIT
#error "xray requires 64-bit platform"
#endif  // !XR_64BIT

typedef enum {
    XR_TAG_NULL = 0,        // null singleton
    XR_TAG_BOOL = 1,        // bool: payload 0=false, 1=true
    XR_TAG_I64 = 3,         // integer (stored in .i as int64)
    XR_TAG_F64 = 4,         // float (stored in .f as double)
    XR_TAG_PTR = 5,         // GC heap object pointer (stored in .ptr)
    XR_TAG_STRUCT_REF = 6,  // stack-allocated struct ref (stored in .ptr)
    XR_TAG_NOTFOUND = 7,    // sentinel: map lookup miss
} XrValueTag;

/* ========== Tagged Union Value (16 bytes, struct-of-union) ========== */

typedef struct XrValue {
    union {
        struct {
            uint8_t tag;         // [0]   XrValueTag
            uint8_t flags;       // [1]   XR_FLAG_* bits
            uint16_t heap_type;  // [2-3] GC type (PTR only)
            uint32_t ext;        // [4-7] reserved = 0
        };
        uint64_t descriptor;  // [0-7] full descriptor (bulk load/compare)
    };
    union {
        int64_t i;  // [8-15] integer payload (I64)
        double f;   // [8-15] float payload (F64)
        void *ptr;  // [8-15] GC heap pointer (PTR)
    };
} XrValue;

// Layout offset constants — change only here if layout ever changes
#define XRVAL_OFF_TAG 0        // offsetof(XrValue, tag)       uint8_t
#define XRVAL_OFF_FLAGS 1      // offsetof(XrValue, flags)     uint8_t
#define XRVAL_OFF_HEAP_TYPE 2  // offsetof(XrValue, heap_type) uint16_t
#define XRVAL_OFF_EXT 4        // offsetof(XrValue, ext)       uint32_t
#define XRVAL_OFF_PAYLOAD 8    // offsetof(XrValue, i/f/ptr)   int64_t
#define XRVAL_SIZE 16          // sizeof(XrValue)

#define XR_VALUE_DEFINED

/* ========== Singleton Values ========== */

static inline XrValue xr_make_int_val(int64_t x, uint8_t t) {
    XrValue v = {0};
    v.tag = t;
    v.i = x;
    return v;
}
static inline XrValue xr_make_float_val(double x, uint8_t t) {
    XrValue v = {0};
    v.tag = t;
    v.f = x;
    return v;
}
static inline XrValue xr_make_ptr_val(void *p) {
    XrValue v = {0};
    v.tag = XR_TAG_PTR;
    v.ptr = p;
    v.heap_type = p ? (uint16_t) XR_GC_GET_TYPE((XrGCHeader *) p) : 0;
    return v;
}

#define XR_NULL_VAL (xr_make_int_val(0, XR_TAG_NULL))
#define XR_TRUE_VAL (xr_make_int_val(1, XR_TAG_BOOL))
#define XR_FALSE_VAL (xr_make_int_val(0, XR_TAG_BOOL))
#define XR_NOTFOUND (xr_make_int_val(0, XR_TAG_NOTFOUND))

/* ========== Type Check Macros ========== */

// Singletons
#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_BOOL(v) ((v).tag == XR_TAG_BOOL)
#define XR_IS_NOTFOUND(v) ((v).tag == XR_TAG_NOTFOUND)
#define XR_IS_TRUE(v) (XR_IS_BOOL(v) && (v).i != 0)
#define XR_IS_FALSE(v) (XR_IS_BOOL(v) && (v).i == 0)

// Numeric
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_NUM(v) ((v).tag == XR_TAG_I64 || (v).tag == XR_TAG_F64)

// Heap pointer (no SSO — all objects are PTR)
#define XR_IS_PTR(v) ((v).tag == XR_TAG_PTR)
#define XR_HEAP_TYPE(v) ((int) (v).heap_type)

// Heap object type checks (all single-branch, no SSO fallback)
#define XR_IS_STRING(v) (XR_IS_PTR(v) && (v).heap_type == XR_TSTRING)
#define XR_IS_FUNCTION(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TFUNCTION)
#define XR_IS_CFUNCTION(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCFUNCTION)
#define XR_IS_ARRAY(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TARRAY)
#define XR_IS_ARRAY_OR_SLICE(v)                                                                    \
    (XR_IS_PTR(v) && (XR_HEAP_TYPE(v) == XR_TARRAY || XR_HEAP_TYPE(v) == XR_TARRAY_SLICE))
#define XR_IS_SET(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TSET)
#define XR_IS_MAP(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TMAP)
#define XR_IS_CLASS(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCLASS)
#define XR_IS_INSTANCE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TINSTANCE)
#define XR_IS_BOUND_METHOD(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TBOUND_METHOD)
#define XR_IS_ENUM_TYPE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TENUM_TYPE)
#define XR_IS_ENUM_VALUE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TENUM_VALUE)
#define XR_IS_COROUTINE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCOROUTINE)
#define XR_IS_TASK(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TTASK)
#define XR_IS_CHANNEL(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCHANNEL)
#define XR_IS_COROPOOL(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCOROPOOL)
#define XR_IS_RANGE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TRANGE)
#define XR_IS_MODULE(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TMODULE)
#define XR_IS_ITERATOR(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TITERATOR)
#define XR_IS_BIGINT(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TBIGINT)
#define XR_IS_JSON(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TJSON)
#define XR_IS_STRUCT_REF(v) ((v).tag == XR_TAG_STRUCT_REF)
#define XR_IS_DATETIME(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TDATETIME)
#define XR_IS_EXCEPTION(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TEXCEPTION)
#define XR_IS_ERROR(v) (XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TERROR)

/* ========== Struct Ref ========== */

/* Construct a struct ref: ptr points into frame struct_area,
 * heap_type is repurposed as layout_id. */
static inline XrValue xr_struct_ref(void *ptr, uint16_t layout_id) {
    XrValue v = {0};
    v.tag = XR_TAG_STRUCT_REF;
    v.heap_type = layout_id;
    v.ptr = ptr;
    return v;
}
static inline void *xr_to_struct_ptr(XrValue v) {
    return v.ptr;
}
/* Construct an array ref within a struct: ptr points to array data start,
 * ext encodes (elem_count << 8) | elem_native_type.
 * Uses XR_TAG_STRUCT_REF with ext != 0 to distinguish from nested struct refs. */
static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint16_t elem_count) {
    XrValue v = {0};
    v.tag = XR_TAG_STRUCT_REF;
    v.heap_type = 0;
    v.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    v.ptr = ptr;
    return v;
}
#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_STRUCT_REF && (v).ext != 0)
#define XR_ARRAY_REF_ELEM_TYPE(v) ((uint8_t) ((v).ext & 0xFF))
#define XR_ARRAY_REF_ELEM_COUNT(v) ((uint16_t) ((v).ext >> 8))

static inline uint16_t xr_struct_layout_id(XrValue v) {
    return v.heap_type;
}

/* ========== Value Creation Macros ========== */

#define XR_FROM_INT(x) xr_make_int_val((int64_t) (x), XR_TAG_I64)
#define XR_FROM_FLOAT(x) xr_make_float_val((double) (x), XR_TAG_F64)
#define XR_FROM_PTR(p) xr_make_ptr_val((void *) (p))
#define XR_FROM_STR(s) xr_make_ptr_val((void *) (s))

/* ========== Value Decoding Macros ========== */

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)
#define XR_TO_PTR(v) ((v).ptr)
#define XR_TO_BOOL(v) ((int) (v).i)

/* ========== Value Set Macros ========== */

// SET macros must zero descriptor first to clear flags/heap_type/ext
#define XR_SET_INT(v, x)                                                                           \
    do {                                                                                           \
        (v).descriptor = 0;                                                                        \
        (v).i = (int64_t) (x);                                                                     \
        (v).tag = XR_TAG_I64;                                                                      \
    } while (0)
#define XR_SET_FLOAT(v, x)                                                                         \
    do {                                                                                           \
        (v).descriptor = 0;                                                                        \
        (v).f = (double) (x);                                                                      \
        (v).tag = XR_TAG_F64;                                                                      \
    } while (0)

/* ========== Precise Value Extraction ========== */

// Extract int64; returns 0 for non-integer tags.
static inline int64_t xr_value_to_i64(XrValue v) {
    return (v.tag == XR_TAG_I64) ? v.i : 0;
}

// Extract double from any numeric tag; integer is promoted to double.
static inline double xr_value_to_f64(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v))
        return (double) v.i;
    return 0.0;
}

// Coerce any numeric/bool value to int64; panics on non-numeric (typed array use).
static inline int64_t xr_value_to_int64_coerce(XrValue v) {
    if (XR_IS_INT(v))
        return XR_TO_INT(v);
    if (XR_IS_FLOAT(v))
        return (int64_t) XR_TO_FLOAT(v);
    if (XR_IS_BOOL(v))
        return XR_IS_TRUE(v) ? 1 : 0;
    XR_CHECK(false, "type confusion: non-numeric value written to typed array");
    return 0;
}

// Coerce any numeric/bool value to double; panics on non-numeric (typed array use).
static inline double xr_value_to_f64_coerce(XrValue v) {
    if (XR_IS_FLOAT(v))
        return XR_TO_FLOAT(v);
    if (XR_IS_INT(v))
        return (double) XR_TO_INT(v);
    if (XR_IS_BOOL(v))
        return XR_IS_TRUE(v) ? 1.0 : 0.0;
    XR_CHECK(false, "type confusion: non-numeric value written to typed array");
    return 0.0;
}

/* ========== tonumber Macro ========== */

#define XR_TONUMBER(v, result)                                                                     \
    (XR_IS_FLOAT(v) ? ((result) = (v).f, 1) : XR_IS_INT(v) ? ((result) = (double) (v).i, 1) : 0)

/* ========== Value Comparison ========== */

static inline bool xr_value_same(XrValue a, XrValue b) {
    if (a.tag != b.tag)
        return false;
    switch (a.tag) {
        case XR_TAG_NULL:
            return true;
        case XR_TAG_BOOL:
            return a.i == b.i;
        case XR_TAG_F64:
            return a.f == b.f;
        case XR_TAG_PTR:
            return a.ptr == b.ptr;
        default:
            return a.i == b.i;
    }
}

/* ========== GC Traversal Macros ========== */

#define XR_VALUE_NEEDS_GC(v) ((v).tag == XR_TAG_PTR)
#define XR_VALUE_GCPTR(v) ((XrGCHeader *) (v).ptr)

/* ========== String Data Access (always heap, no SSO) ========== */

XR_FUNC const char *xr_value_str_data(const XrValue *v);
XR_FUNC uint32_t xr_value_str_len(const XrValue *v);

/* ========== Value Cast Macros ========== */

// String: always heap, direct cast (no SSO)
#define XR_TO_STRING(v) ((struct XrString *) (v).ptr)
#define XR_TO_FUNCTION(v) ((struct XrClosure *) (v).ptr)
#define XR_TO_ARRAY(v) ((struct XrArray *) (v).ptr)
#define XR_TO_MAP(v) ((struct XrMap *) (v).ptr)
#define XR_TO_SET(v) ((struct XrSet *) (v).ptr)
#define XR_TO_CLASS(v) ((struct XrClass *) (v).ptr)
#define XR_TO_INSTANCE(v) ((struct XrInstance *) (v).ptr)
#define XR_TO_MODULE(v) ((struct XrModule *) (v).ptr)
#define XR_TO_CORO(v) ((struct XrCoroutine *) (v).ptr)
#define XR_TO_TASK(v) ((struct XrTask *) (v).ptr)

/* ========== Value Creation Functions ========== */

XR_FUNC XrValue xr_null(void);
XR_FUNC XrValue xr_bool(int b);
static inline XrValue xr_int(xr_Integer i) {
    return XR_FROM_INT(i);
}
static inline XrValue xr_float(xr_Number n) {
    return XR_FROM_FLOAT(n);
}

/* ========== Type Query ========== */

XR_FUNC XrTypeId xr_value_typeid(XrValue v);
XR_FUNC bool xr_value_deep_eq(XrValue a, XrValue b);

/* ========== Object Operations ========== */

XR_FUNC XrValue xr_string_value(XrString *str);

struct XrClosure;
XR_FUNC XrValue xr_value_from_closure(struct XrClosure *closure);
XR_FUNC bool xr_value_is_closure(XrValue v);
XR_FUNC struct XrClosure *xr_value_to_closure(XrValue v);

struct XrCFunction;
XR_FUNC XrValue xr_value_from_cfunction(struct XrCFunction *cfunc);
XR_FUNC bool xr_value_is_cfunction(XrValue v);
XR_FUNC struct XrCFunction *xr_value_to_cfunction(XrValue v);

struct XrArray;
XR_FUNC XrValue xr_value_from_array(struct XrArray *arr);
XR_FUNC bool xr_value_is_array(XrValue v);
XR_FUNC struct XrArray *xr_value_to_array(XrValue v);

struct XrMap;
XR_FUNC XrValue xr_value_from_map(struct XrMap *map);
XR_FUNC bool xr_value_is_map(XrValue v);
XR_FUNC struct XrMap *xr_value_to_map(XrValue v);

struct XrSet;
XR_FUNC XrValue xr_value_from_set(struct XrSet *set);
XR_FUNC bool xr_value_is_set(XrValue v);
XR_FUNC struct XrSet *xr_value_to_set(XrValue v);

struct XrClass;
struct XrInstance;

XR_FUNC XrValue xr_value_from_class(struct XrClass *cls);
XR_FUNC bool xr_value_is_class(XrValue v);
XR_FUNC struct XrClass *xr_value_to_class(XrValue v);

XR_FUNC XrValue xr_value_from_instance(struct XrInstance *inst);
XR_FUNC bool xr_value_is_instance(XrValue v);
XR_FUNC struct XrInstance *xr_value_to_instance(XrValue v);

struct XrModule;
XR_FUNC XrValue xr_value_from_module(struct XrModule *module);
XR_FUNC bool xr_value_is_module(XrValue v);
XR_FUNC struct XrModule *xr_value_to_module(XrValue v);

struct XrCoroutine;
XR_FUNC XrValue xr_value_from_coro(struct XrCoroutine *coro);
XR_FUNC bool xr_value_is_coro(XrValue v);
XR_FUNC struct XrCoroutine *xr_value_to_coro(XrValue v);

struct XrTask;
XR_FUNC XrValue xr_value_from_task(struct XrTask *task);
XR_FUNC bool xr_value_is_task(XrValue v);
XR_FUNC struct XrTask *xr_value_to_task(XrValue v);

XR_FUNC bool xr_value_is_datetime(XrValue v);
XR_FUNC void *xr_value_to_datetime(XrValue v);

/* ========== Compile-time Checks ========== */

XR_STATIC_ASSERT(sizeof(XrValue) == 16, "XrValue must be 16 bytes");
XR_STATIC_ASSERT(offsetof(XrValue, tag) == 0, "XrValue.tag must be at byte 0");
XR_STATIC_ASSERT(offsetof(XrValue, i) == 8, "payload must be at byte 8");
XR_STATIC_ASSERT(sizeof(int64_t) == 8, "int64_t must be 8 bytes");
XR_STATIC_ASSERT(sizeof(double) == 8, "double must be 8 bytes");

/* ========== C11 Generic API ========== */

#if __STDC_VERSION__ >= 201112L

#define xr_value(x)                                                                                \
    _Generic((x),                                                                                  \
        int: xr_int,                                                                               \
        long: xr_int,                                                                              \
        long long: xr_int,                                                                         \
        unsigned int: xr_int,                                                                      \
        unsigned long: xr_int,                                                                     \
        unsigned long long: xr_int,                                                                \
        float: xr_float,                                                                           \
        double: xr_float,                                                                          \
        bool: xr_bool,                                                                             \
        _Bool: xr_bool,                                                                            \
        XrString *: xr_string_value,                                                               \
        struct XrString *: xr_string_value,                                                        \
        struct XrArray *: xr_value_from_array,                                                     \
        struct XrMap *: xr_value_from_map,                                                         \
        struct XrSet *: xr_value_from_set,                                                         \
        struct XrClass *: xr_value_from_class,                                                     \
        struct XrInstance *: xr_value_from_instance,                                               \
        struct XrClosure *: xr_value_from_closure,                                                 \
        struct XrCFunction *: xr_value_from_cfunction,                                             \
        struct XrModule *: xr_value_from_module)(x)

#endif  // C11

#endif  // XVALUE_H
