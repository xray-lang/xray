/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexception.c - Exception runtime API on top of XrInstance
 *
 * Exception is a regular class registered into core->exceptionClass
 * (built by xr_register_exception_class below; called from
 * xr_core_init). All field access here is direct indexing into
 * XrInstance.fields[] using the EXCEPTION_FIELD_* indices fixed in
 * xclass_system.h.
 */

#include "xexception.h"
#include "../../base/xchecks.h"
#include "../xerror_impl.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "xstring.h"
#include "xarray.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../class/xclass_system.h"
#include "../class/xinstance.h"
#include "../value/xtype_names.h"
#include "../../coro/xcoroutine.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ========== Local helpers ========== */

static XrInstance *exception_instance(XrayIsolate *X, XrValue v) {
    if (!xr_value_is_exception(X, v))
        return NULL;
    return (XrInstance *) XR_TO_PTR(v);
}

static XrClass *exception_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "exception: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "exception: core not initialised");
    XR_DCHECK(core->exceptionClass != NULL,
              "exception: core->exceptionClass not registered (core_init incomplete)");
    return core->exceptionClass;
}

/* ========== Type Check ========== */

bool xr_value_is_exception(XrayIsolate *X, XrValue v) {
    XR_DCHECK(X != NULL, "xr_value_is_exception: NULL isolate");
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    if (!core || !core->exceptionClass)
        return false;
    return xr_class_instanceof(inst->klass, core->exceptionClass);
}

/* ========== Construction ========== */

XrValue xr_exception_new(XrayIsolate *X, XrErrorCode code, const char *message) {
    XR_DCHECK(X != NULL, "exception_new: NULL isolate");
    XrClass *cls = exception_class(X);
    XrInstance *inst = xr_instance_new(X, cls);
    if (!inst)
        return xr_null();

    XrString *msg = NULL;
    if (message) {
        msg = xr_string_intern(X, message, strlen(message), 0);
    }
    inst->fields[EXCEPTION_FIELD_MESSAGE] = msg ? XR_FROM_PTR(msg) : xr_null();

    XrArray *stack = xr_array_new(xr_current_coro(X));
    inst->fields[EXCEPTION_FIELD_STACK] = stack ? xr_value_from_array(stack) : xr_null();

    inst->fields[EXCEPTION_FIELD_CAUSE] = xr_null();
    inst->fields[EXCEPTION_FIELD_CODE] = xr_int((int64_t) code);
    inst->fields[EXCEPTION_FIELD_DATA] = xr_null();

    return xr_value_from_instance(inst);
}

XrValue xr_exception_newf(XrayIsolate *X, XrErrorCode code, const char *fmt, ...) {
    XR_DCHECK(X != NULL, "exception_newf: NULL isolate");
    XR_DCHECK(fmt != NULL, "exception_newf: NULL fmt");
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return xr_exception_new(X, code, buffer);
}

XrValue xr_exception_from_error(XrayIsolate *X, XrError *error) {
    XR_DCHECK(X != NULL, "exception_from_error: NULL isolate");
    if (!error) {
        return xr_exception_new(X, XR_ERR_UNKNOWN, "Unknown error");
    }

    XrClass *cls = exception_class(X);
    XrInstance *inst = xr_instance_new(X, cls);
    if (!inst)
        return xr_null();

    inst->fields[EXCEPTION_FIELD_MESSAGE] =
        error->message ? XR_FROM_PTR(error->message) : xr_null();

    XrArray *stack = xr_array_new(xr_current_coro(X));
    inst->fields[EXCEPTION_FIELD_STACK] = stack ? xr_value_from_array(stack) : xr_null();

    inst->fields[EXCEPTION_FIELD_CAUSE] = xr_null();
    inst->fields[EXCEPTION_FIELD_CODE] = xr_int((int64_t) error->code);
    inst->fields[EXCEPTION_FIELD_DATA] = xr_null();

    return xr_value_from_instance(inst);
}

XrValue xr_exception_from_value(XrayIsolate *X, XrValue value) {
    XR_DCHECK(X != NULL, "exception_from_value: NULL isolate");
    if (xr_value_is_exception(X, value))
        return value;

    if (XR_IS_PTR(value)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(value);
        if (XR_GC_GET_TYPE(gc) == XR_TERROR) {
            return xr_exception_from_error(X, (XrError *) gc);
        }
    }

    XrValue exc = xr_exception_new(X, XR_ERR_RUNTIME, "Value thrown as exception");
    XrInstance *inst = exception_instance(X, exc);
    if (inst) {
        inst->fields[EXCEPTION_FIELD_DATA] = value;
    }
    return exc;
}

/* ========== Field Accessors ========== */

XrErrorCode xr_exception_get_code(XrayIsolate *X, XrValue exception) {
    XrInstance *inst = exception_instance(X, exception);
    if (!inst)
        return XR_ERR_UNKNOWN;
    XrValue v = inst->fields[EXCEPTION_FIELD_CODE];
    return XR_IS_INT(v) ? (XrErrorCode) XR_TO_INT(v) : XR_ERR_UNKNOWN;
}

const char *xr_exception_get_message(XrayIsolate *X, XrValue exception) {
    XrInstance *inst = exception_instance(X, exception);
    if (!inst)
        return "Not an exception";
    XrValue v = inst->fields[EXCEPTION_FIELD_MESSAGE];
    if (!XR_IS_STRING(v))
        return "";
    XrString *s = (XrString *) XR_TO_PTR(v);
    return s->data;
}

XrValue xr_exception_get_stacktrace(XrayIsolate *X, XrValue exception) {
    XrInstance *inst = exception_instance(X, exception);
    if (!inst)
        return xr_null();
    XrValue v = inst->fields[EXCEPTION_FIELD_STACK];
    if (XR_IS_ARRAY(v))
        return v;
    // Lazily create the stack array if it was nulled out (defensive).
    XrArray *stack = xr_array_new(xr_current_coro(X));
    if (!stack)
        return xr_null();
    XrValue av = xr_value_from_array(stack);
    inst->fields[EXCEPTION_FIELD_STACK] = av;
    return av;
}

XrValue xr_exception_get_data(XrayIsolate *X, XrValue exception) {
    XrInstance *inst = exception_instance(X, exception);
    if (!inst)
        return xr_null();
    return inst->fields[EXCEPTION_FIELD_DATA];
}

/* ========== Stack Trace ========== */

void xr_exception_add_frame(XrayIsolate *X, XrValue exception, const char *funcName, int line) {
    XR_DCHECK(funcName != NULL, "exception_add_frame: NULL funcName");
    XrInstance *inst = exception_instance(X, exception);
    if (!inst)
        return;

    XrValue stack_val = inst->fields[EXCEPTION_FIELD_STACK];
    XrArray *stack;
    if (XR_IS_ARRAY(stack_val)) {
        stack = (XrArray *) XR_TO_PTR(stack_val);
    } else {
        stack = xr_array_new(xr_current_coro(X));
        if (!stack)
            return;
        inst->fields[EXCEPTION_FIELD_STACK] = xr_value_from_array(stack);
    }

    char frameStr[256];
    snprintf(frameStr, sizeof(frameStr), "at %s (line %d)", funcName, line);
    XrString *frameString = xr_string_intern(X, frameStr, strlen(frameStr), 0);
    if (frameString) {
        xr_array_push(stack, xr_string_value(frameString));
    }
}

/* ========== Primitive Class Methods ==========
 *
 * These run from vm_invoke_class / vm_superinvoke as XMETHOD_PRIMITIVE
 * entries on core->exceptionClass. They receive the pre-allocated
 * XrInstance via `self` (either Exception or a subclass — Exception
 * fields occupy indices 0..4 regardless), write parent fields by index,
 * and return the same value back so the caller's base[a] points at it.
 */

static XrValue exception_primitive_constructor(XrayIsolate *X, XrValue self, XrValue *args,
                                               int argc) {
    XR_DCHECK(X != NULL, "Exception.ctor: NULL isolate");
    if (!XR_IS_INSTANCE(self))
        return xr_null();

    XrInstance *inst = (XrInstance *) XR_TO_PTR(self);

    // message: string = ""
    XrString *msg_str = NULL;
    if (argc >= 1 && XR_IS_STRING(args[0])) {
        msg_str = (XrString *) XR_TO_PTR(args[0]);
    } else {
        msg_str = xr_string_intern(X, "", 0, 0);
    }
    inst->fields[EXCEPTION_FIELD_MESSAGE] = msg_str ? XR_FROM_PTR(msg_str) : xr_null();

    // stack: Array<string> — fresh empty array per instance
    XrArray *stack = xr_array_new(xr_current_coro(X));
    inst->fields[EXCEPTION_FIELD_STACK] = stack ? xr_value_from_array(stack) : xr_null();

    // cause: Exception? = null
    if (argc >= 2 && xr_value_is_exception(X, args[1])) {
        inst->fields[EXCEPTION_FIELD_CAUSE] = args[1];
    } else {
        inst->fields[EXCEPTION_FIELD_CAUSE] = xr_null();
    }

    // code: int = 0 (only the C runtime sets non-zero codes)
    inst->fields[EXCEPTION_FIELD_CODE] = xr_int(0);

    // data: Json? = null (used by xr_exception_from_value to wrap thrown values)
    inst->fields[EXCEPTION_FIELD_DATA] = xr_null();

    return self;
}

static XrValue exception_primitive_to_string(XrayIsolate *X, XrValue self, XrValue *args,
                                             int argc) {
    (void) args;
    (void) argc;
    if (!XR_IS_INSTANCE(self))
        return xr_null();

    XrInstance *inst = (XrInstance *) XR_TO_PTR(self);
    const char *class_name = inst->klass && inst->klass->name ? inst->klass->name : "Exception";
    const char *message = "";
    XrValue msg_val = inst->fields[EXCEPTION_FIELD_MESSAGE];
    if (XR_IS_STRING(msg_val)) {
        message = ((XrString *) XR_TO_PTR(msg_val))->data;
    }

    char buffer[1024];
    int n = snprintf(buffer, sizeof(buffer), "%s: %s", class_name, message);
    if (n < 0)
        n = 0;
    if ((size_t) n >= sizeof(buffer))
        n = (int) sizeof(buffer) - 1;
    XrString *result = xr_string_intern(X, buffer, (size_t) n, 0);
    return result ? XR_FROM_PTR(result) : xr_null();
}

/* ========== Class Registration ==========
 *
 * Called from xr_core_init after Object is ready. Builds the Exception
 * class with 5 fields and 2 primitive methods, then asserts that field
 * indices match the EXCEPTION_FIELD_* constants — any drift between
 * builder ordering and the constants would silently corrupt every throw.
 */

void xr_register_exception_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "register_exception_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "register_exception_class: core not initialised");
    XR_DCHECK(core->objectClass != NULL, "register_exception_class: Object not registered yet");
    XR_DCHECK(core->exceptionClass == NULL, "register_exception_class: already registered");

    XrClassBuilder *builder = xr_class_builder_new(X, TYPE_NAME_EXCEPTION, core->objectClass);
    XR_CHECK(builder != NULL, "register_exception_class: builder alloc failed");

    /* Field order MUST match EXCEPTION_FIELD_* in xclass_system.h. */
    xr_class_builder_add_field(builder, "message", 0);
    xr_class_builder_add_field(builder, "stack", 0);
    xr_class_builder_add_field(builder, "cause", 0);
    xr_class_builder_add_field(builder, "code", 0);
    xr_class_builder_add_field(builder, "data", 0);

    xr_class_builder_add_method(builder, XR_KEYWORD_CONSTRUCTOR, exception_primitive_constructor,
                                /* param_count */ -1, XMETHOD_FLAG_CONSTRUCTOR);
    xr_class_builder_add_method(builder, "toString", exception_primitive_to_string,
                                /* param_count */ 0, 0);

    XrClass *cls = xr_class_builder_finalize(builder);
    XR_CHECK(cls != NULL, "register_exception_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN;

    /* Sanity-check that builder layout matches the indices the entire VM
     * relies on. If finalize ever reorders fields these asserts trip
     * immediately at init, not at the first throw. */
    XR_DCHECK(xr_class_lookup_field_by_name(X, cls, "message") == EXCEPTION_FIELD_MESSAGE,
              "Exception field 'message' index drift");
    XR_DCHECK(xr_class_lookup_field_by_name(X, cls, "stack") == EXCEPTION_FIELD_STACK,
              "Exception field 'stack' index drift");
    XR_DCHECK(xr_class_lookup_field_by_name(X, cls, "cause") == EXCEPTION_FIELD_CAUSE,
              "Exception field 'cause' index drift");
    XR_DCHECK(xr_class_lookup_field_by_name(X, cls, "code") == EXCEPTION_FIELD_CODE,
              "Exception field 'code' index drift");
    XR_DCHECK(xr_class_lookup_field_by_name(X, cls, "data") == EXCEPTION_FIELD_DATA,
              "Exception field 'data' index drift");

    core->exceptionClass = cls;
}

/* ========== Output ========== */

void xr_exception_print(XrayIsolate *X, XrValue exception) {
    XrInstance *inst = exception_instance(X, exception);
    if (!inst) {
        fprintf(stderr, "Error: Not an exception object\n");
        return;
    }

    XrErrorCode code = xr_exception_get_code(X, exception);
    const char *message = xr_exception_get_message(X, exception);
    fprintf(stderr, "\033[1;31mUncaught %s [%d]:\033[0m %s\n",
            inst->klass && inst->klass->name ? inst->klass->name : "Exception", (int) code,
            message ? message : "");

    XrValue stack_val = inst->fields[EXCEPTION_FIELD_STACK];
    if (XR_IS_ARRAY(stack_val)) {
        XrArray *stack = (XrArray *) XR_TO_PTR(stack_val);
        if (stack->length > 0) {
            fprintf(stderr, "\nStack trace:\n");
            for (int i = 0; i < stack->length; i++) {
                XrValue frame = ((XrValue *) stack->data)[i];
                if (XR_IS_STRING(frame)) {
                    XrString *str = (XrString *) XR_TO_PTR(frame);
                    fprintf(stderr, "  %s\n", str->data);
                }
            }
        }
    }
}
