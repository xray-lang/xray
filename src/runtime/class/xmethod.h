/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod.h - Method system for OOP
 *
 * KEY CONCEPT:
 *   Value-type method descriptors with tagged union design.
 *   Supports closures, C primitives, getters, setters, and operators.
 */

#ifndef XMETHOD_H
#define XMETHOD_H

#include "../value/xvalue.h"
#include <stdbool.h>

struct XrClosure;

#ifndef XR_CFUNCTION_PTR_DEFINED
typedef XrValue (*XrCFunctionPtr)(XrayIsolate *isolate, XrValue *args, int nargs);
#define XR_CFUNCTION_PTR_DEFINED
#endif

/*
 * Calling convention for class primitive methods.
 *
 *   iso   - owning isolate (always non-NULL).
 *   self  - receiver value (instance for instance methods, class for statics).
 *   args  - pointer to first user argument (excludes self).
 *   argc  - user argument count (excludes self).
 *
 * Matches VM register layout: fn(iso, R(a+1), &R(a+2), nargs).
 * Static methods receive the class as self and may ignore it.
 */
#ifndef XR_PRIMITIVE_METHOD_FN_DEFINED
typedef XrValue (*XrPrimitiveMethodFn)(XrayIsolate *isolate, XrValue self, XrValue *args, int argc);
#define XR_PRIMITIVE_METHOD_FN_DEFINED
#endif

typedef enum {
    XMETHOD_NONE,
    XMETHOD_CLOSURE,
    XMETHOD_PRIMITIVE,
    XMETHOD_GETTER,
    XMETHOD_SETTER,
    XMETHOD_OPERATOR,
} XrMethodType;

#define XMETHOD_FLAG_PRIVATE 0x01
#define XMETHOD_FLAG_STATIC 0x02
#define XMETHOD_FLAG_CONSTRUCTOR 0x04
#define XMETHOD_FLAG_ABSTRACT 0x08
#define XMETHOD_FLAG_FINAL 0x10
#define XMETHOD_FLAG_PROTECTED 0x20
#define XMETHOD_FLAG_NATIVE 0x40

/*
 * Value-type method descriptor.
 * Tagged union: closure, primitive, getter, setter, or operator.
 * Lifetime bound to class (no GC needed).
 */
typedef struct XrMethod {
    XrMethodType type;

    union {
        struct XrClosure *closure;      // CLOSURE/GETTER/SETTER/OPERATOR
        XrPrimitiveMethodFn primitive;  // PRIMITIVE
    } as;

    uint8_t flags;
    uint8_t op_type;  // Valid only when type==OPERATOR
    int32_t symbol;

    const char *name;
    uint8_t param_count;

    int32_t vtable_index;  // -1 if not in vtable
} XrMethod;

/* ========== Property Accessors (inline) ========== */

static inline bool xr_method_is_private(const XrMethod *method) {
    return (method->flags & XMETHOD_FLAG_PRIVATE) != 0;
}

static inline bool xr_method_is_static(const XrMethod *method) {
    return (method->flags & XMETHOD_FLAG_STATIC) != 0;
}

static inline bool xr_method_is_constructor(const XrMethod *method) {
    return (method->flags & XMETHOD_FLAG_CONSTRUCTOR) != 0;
}

static inline bool xr_method_is_abstract(const XrMethod *method) {
    return (method->flags & XMETHOD_FLAG_ABSTRACT) != 0;
}

/* ========== Debug ========== */

XR_FUNC void xr_method_print(const XrMethod *method);
XR_FUNC const char *xr_method_type_name(XrMethodType type);

#endif  // XMETHOD_H
