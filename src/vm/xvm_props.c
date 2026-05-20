/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_props.c - Dispatch helpers for property access and module invoke
 *
 * Implements per-type dispatch for OP_GETPROP / OP_SETPROP,
 * instance getter / setter fallbacks, and vm_invoke_module.
 * Declarations live in xvm_dispatch_helpers.h.
 *
 * Owns:
 *   - vm_setprop_type_dispatch    (per-type SETPROP fallback)
 *   - vm_setprop_instance_setter  (instance setter resolution)
 *   - vm_getprop_type_dispatch    (per-type GETPROP fallback)
 *   - vm_getprop_instance_getter  (instance getter resolution)
 *   - vm_invoke_module            (module member invocation)
 */

#include "xvm_dispatch_helpers.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"

#include "../runtime/object/xjson.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"
#include "../runtime/object/xexception.h"

/* ========== Dispatch: OP_SETPROP Type Dispatch ========== */

/*
 * Handles property set for non-instance types (Map error, Module, Class static, Json).
 * Returns XR_DISP_NEXT on success, XR_DISP_FALLTHROUGH for instance, XR_DISP_RAISE on error.
 */
XR_FUNC XrDispatchAction vm_setprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue value,
                                                  XrValue *base, int a, XrBcCallFrame *frame,
                                                  XrInstruction *pc) {
    (void) base;
    (void) a;
    // Map dot assignment: forbidden
    if (XR_IS_MAP(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                 "Map does not support dot syntax assignment '%s', use map[\"%s\"] = value or "
                 "map.set(\"%s\", value)",
                 name ? name : "?", name ? name : "?", name ? name : "?");
    }

    // Module export variable assignment
    if (xr_value_is_module(obj)) {
        XrModule *module = xr_value_to_module(obj);
        if (module && xr_module_has_sym(module, prop_symbol)) {
            if (xr_module_is_const_sym(module, prop_symbol)) {
                XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
                const char *_name = xr_symbol_get_name_in_table(_st, prop_symbol);
                VM_THROW(frame, pc, XR_ERR_CMP_CONST_ASSIGN,
                         "cannot modify module constant '%s.%s'", module->name ? module->name : "?",
                         _name ? _name : "?");
            }
            xr_module_set_sym(module, prop_symbol, value);
            XrCoroutine *_bc = vm_ctx->current_coro;
            if (_bc && _bc->coro_gc)
                XR_GC_BARRIER_VAL(_bc->coro_gc, module, value);
            return XR_DISP_NEXT;
        }
        XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, prop_symbol);
        VM_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT, "module '%s' has no export variable '%s'",
                 module && module->name ? module->name : "?", _name ? _name : "?");
    }

    // Static field assignment
    if (xr_value_is_class(obj)) {
        XrClass *cls = xr_value_to_class(obj);
        int field_index = xr_class_lookup_field(cls, prop_symbol);
        if (field_index < 0) {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                     "static field '%s' not found in class '%s'", pname ? pname : "?", cls->name);
        }
        const XrFieldDescriptor *field = xr_class_get_field(cls, field_index);
        if (!field) {
            VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: field descriptor not found");
        }
        if (!(field->flags & XR_FIELD_STATIC)) {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY, "field '%s' is not a static field",
                     pname ? pname : "?");
        }
        if (field->flags & XR_FIELD_FINAL) {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_CMP_CONST_ASSIGN, "cannot modify const static field '%s'",
                     pname ? pname : "?");
        }
        int static_field_idx = field->static_slot;
        if (static_field_idx < 0 || static_field_idx >= cls->static_field_count) {
            VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                     "internal error: static field index out of bounds");
        }
        cls->static_field_values[static_field_idx] = value;
        XrCoroutine *_bc = vm_ctx->current_coro;
        if (_bc && _bc->coro_gc)
            XR_GC_BARRIER_VAL(_bc->coro_gc, cls, value);
        return XR_DISP_NEXT;
    }

    // Json property set
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        if (!xr_json_set(isolate, json, prop_symbol, value)) {
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                     "cannot add property to sealed Json object");
        }
        XrCoroutine *_bc = vm_ctx->current_coro;
        if (_bc && _bc->coro_gc)
            xr_coro_gc_barrierback(_bc->coro_gc, XR_OBJ2GC(json));
        return XR_DISP_NEXT;
    }

    // Null type error
    if (XR_IS_NULL(obj)) {
        VM_THROW(frame, pc, XR_ERR_NULL_PROPERTY, "null type does not support property access");
    }

    // Struct ref: stored field write or setter method
    if (XR_IS_STRUCT_REF(obj)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass **) sptr;

        // Try stored field first
        int fidx = xr_class_lookup_field(scls, prop_symbol);
        if (fidx >= 0 && scls->struct_layout && fidx < scls->struct_layout->field_count) {
            XrStructFieldLayout *sf = &scls->struct_layout->fields[fidx];
            uint8_t *fp = sptr + 8 + sf->offset;
            switch (sf->native_type) {
                case XR_NATIVE_I64:
                    *(int64_t *) fp = XR_TO_INT(value);
                    break;
                case XR_NATIVE_F64:
                    *(double *) fp = XR_TO_FLOAT(value);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) fp = (uint8_t) value.i;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) fp = (int32_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) fp = (uint32_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) fp = (int16_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) fp = (uint16_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) fp = (int8_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) fp = (uint8_t) XR_TO_INT(value);
                    break;
                case XR_NATIVE_F32:
                    *(float *) fp = (float) XR_TO_FLOAT(value);
                    break;
                case XR_NATIVE_STRING:
                    *(XrString **) fp = (XrString *) value.ptr;
                    break;
                default:
                    break;
            }
            return XR_DISP_NEXT;
        }

        // Try setter method: set:<prop_name>
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        if (prop_name) {
            char setter_name[256];
            snprintf(setter_name, sizeof(setter_name), "set:%s", prop_name);
            int setter_symbol = xr_symbol_register_in_table(sym_table, setter_name);
            XrMethod *setter =
                (setter_symbol >= 0) ? xr_class_lookup_method(scls, setter_symbol) : NULL;
            if (setter && setter->as.closure) {
                XrClosure *closure = setter->as.closure;
                XrProto *proto = closure->proto;
                if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                    VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
                }
                // Place this + value on stack above caller's registers
                int setter_base =
                    (int) (base - vm_ctx->stack) + frame->closure->proto->maxstacksize;
                vm_ctx->stack[setter_base] = obj;        // this
                vm_ctx->stack[setter_base + 1] = value;  // argument
                frame->pc = pc;
                int _fidx = vm_ctx->frame_count;
                memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
                vm_ctx->frame_count++;
                XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(proto);
                new_frame->base_offset = setter_base;
                return XR_DISP_RESTART;
            }
        }

        // No field and no setter found
        VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                 "struct '%s' has no writable field or setter for this property", scls->name);
    }

    // Non-instance type error
    if (!xr_value_is_instance(obj)) {
        VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                 "only instance, Map, Json and class can set properties");
    }

    return XR_DISP_FALLTHROUGH;  // Instance: handled by caller
}

/* ========== Dispatch: OP_SETPROP Instance Setter ========== */

XR_FUNC XrDispatchAction vm_setprop_instance_setter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue value, XrValue *base, int c,
                                                    XrBcCallFrame *frame, XrInstruction *pc) {
    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name)
        return XR_DISP_FALLTHROUGH;

    size_t prop_name_len = strlen(prop_name);
    if (prop_name_len + 5 > XR_MAX_METHOD_NAME_LEN) {
        VM_THROW(frame, pc, XR_ERR_OVERFLOW, "property name too long");
    }

    char setter_name[XR_MAX_METHOD_NAME_LEN];
    snprintf(setter_name, sizeof(setter_name), "set:%s", prop_name);

    int setter_symbol = xr_symbol_register_in_table(sym_table, setter_name);
    XrMethod *setter = NULL;
    if (setter_symbol >= 0) {
        setter = xr_class_lookup_method(inst->klass, setter_symbol);
    }

    if (!setter)
        return XR_DISP_FALLTHROUGH;

    XrClosure *closure = setter->as.closure;
    XrProto *proto = closure->proto;

    if (proto->numparams != 2) {
        VM_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT, "setter should have one parameter");
    }

    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
        VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
    }

    /* Place setter frame above caller's maxstacksize to avoid
     * clobbering caller registers. */
    int safe_base = (int) (base - vm_ctx->stack) + frame->closure->proto->maxstacksize;
    vm_ctx->stack[safe_base] = obj;
    vm_ctx->stack[safe_base + 1] = value;

    frame->pc = pc;  // savepc

    int _fidx = vm_ctx->frame_count;
    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
    vm_ctx->frame_count++;
    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = safe_base;

    return XR_DISP_RESTART;
}
/* ========== Dispatch: OP_GETPROP Type Dispatch ========== */

/*
 * Handles property access for all non-instance types .
 * Returns XR_DISP_NEXT if property was resolved, XR_DISP_FALLTHROUGH for instance,
 * or XR_DISP_RAISE/XR_DISP_RESTART for error/getter paths.
 */
XR_FUNC XrDispatchAction vm_getprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                  XrValue obj, int prop_symbol, XrValue *base,
                                                  int a, int b, XrBcCallFrame *frame,
                                                  XrInstruction *pc) {
    XR_DCHECK(isolate != NULL, "vm_getprop_type_dispatch: NULL isolate");
    XR_DCHECK(base != NULL, "vm_getprop_type_dispatch: NULL base");
    // Task properties: task.done, task.cancelled, task.result, task.error
    if (xr_value_is_task(obj)) {
        XrTask *task = xr_value_to_task(obj);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (prop_symbol == SYMBOL_DONE) {
            base[a] = xr_bool(tstate >= XR_TASK_COMPLETED);
        } else if (prop_symbol == SYMBOL_CANCELLED) {
            base[a] = xr_bool(tstate == XR_TASK_CANCELLING || tstate == XR_TASK_CANCELLED);
        } else if (prop_symbol == SYMBOL_RESULT) {
            base[a] = (tstate == XR_TASK_COMPLETED) ? task->result : xr_null();
        } else if (prop_symbol == SYMBOL_ERROR) {
            if (tstate == XR_TASK_FAILED && !XR_IS_NULL(task->error)) {
                XrValue err = task->error;
                if (xr_value_is_exception(isolate, err)) {
                    const char *m = xr_exception_get_message(isolate, err);
                    if (m) {
                        XrString *s = xr_string_intern(isolate, m, strlen(m), 0);
                        base[a] = xr_string_value(s);
                    } else {
                        base[a] = xr_null();
                    }
                } else {
                    base[a] = err;
                }
            } else {
                base[a] = xr_null();
            }
        } else {
            base[a] = xr_null();
        }
        return XR_DISP_NEXT;
    }

    // Legacy coroutine properties (fallback for old callers)
    if (xr_value_is_coro(obj)) {
        XrCoroutine *coro = xr_value_to_coro(obj);
        if (prop_symbol == SYMBOL_DONE) {
            base[a] = xr_bool(xr_coro_flags_has(coro, XR_CORO_FLG_DONE));
        } else if (prop_symbol == SYMBOL_CANCELLED) {
            base[a] = xr_bool(xr_coro_flags_has(coro, XR_CORO_FLG_CANCELLED));
        } else if (prop_symbol == SYMBOL_RESULT) {
            XrCoroutine *caller = (XrCoroutine *) vm_ctx->current_coro;
            base[a] = xr_deep_copy_to_coro(isolate, coro->result, caller);
        } else if (prop_symbol == SYMBOL_ERROR) {
            XrCoroutine *caller = (XrCoroutine *) vm_ctx->current_coro;
            base[a] = xr_deep_copy_to_coro(isolate, coro->error, caller);
        } else {
            base[a] = xr_null();
        }
        return XR_DISP_NEXT;
    }

    // Channel properties: ch.length, ch.capacity, ch.isClosed
    if (xr_value_is_channel(obj)) {
        XrChannel *ch = xr_value_to_channel(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer) ch->buf_count);
        } else if (prop_symbol == SYMBOL_CAPACITY) {
            base[a] = xr_int((xr_Integer) ch->buf_size);
        } else if (prop_symbol == SYMBOL_IS_CLOSED) {
            base[a] = xr_bool(xr_channel_is_closed(ch));
        } else {
            base[a] = xr_null();
        }
        return XR_DISP_NEXT;
    }

    // Enum property access
    if (XR_IS_PTR(obj)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(obj);

        if (XR_IS_ENUM_VALUE(obj)) {
            XrEnumValue *enum_val = (XrEnumValue *) gc;
            if (prop_symbol == SYMBOL_NAME) {
                size_t len = strlen(enum_val->member_name);
                XrString *str = xr_string_intern(isolate, enum_val->member_name, len, 0);
                base[a] = xr_string_value(str);
                return XR_DISP_NEXT;
            } else if (prop_symbol == SYMBOL_VALUE) {
                base[a] = enum_val->raw_value;
                return XR_DISP_NEXT;
            } else if (prop_symbol == SYMBOL_ORDINAL) {
                base[a] = xr_int(enum_val->member_index);
                return XR_DISP_NEXT;
            }
            // Other enum properties: fall through to instance path
        }

        if (XR_IS_ENUM_TYPE(obj)) {
            XrEnumType *enum_type = (XrEnumType *) gc;
            if (prop_symbol == SYMBOL_MEMBER_COUNT) {
                base[a] = xr_int(enum_type->member_count);
                return XR_DISP_NEXT;
            } else if (prop_symbol == SYMBOL_GET_MEMBER) {
                XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_enum_get_member_handler);
                base[a] = xr_value_from_bound_method(bm);
                return XR_DISP_NEXT;
            }
            XrEnumValue *found = xr_enum_get_member_by_symbol(enum_type, prop_symbol);
            if (found) {
                base[a] = XR_FROM_PTR(found);
                return XR_DISP_NEXT;
            }
            base[a] = xr_null();
            return XR_DISP_NEXT;
        }

        // Iterator: handled by standard instance method dispatch
        // (iteratorClass has builtin_kind == XR_BK_ITERATOR and registers
        //  hasNext/next/toString via XrClassBuilder).
    }

    // Map property access
    if (XR_IS_MAP(obj)) {
        XrMap *map = XR_TO_MAP(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer) xr_map_size(map));
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(xr_map_is_empty(map));
        } else if (prop_symbol == SYMBOL_KEYS) {
            XrArray *keys = xr_map_keys(vm_get_coro(vm_ctx), map);
            base[a] = keys ? xr_value_from_array(keys) : xr_null();
        } else if (prop_symbol == SYMBOL_VALUES) {
            XrArray *values = xr_map_values(vm_get_coro(vm_ctx), map);
            base[a] = values ? xr_value_from_array(values) : xr_null();
        } else if (prop_symbol == SYMBOL_ENTRIES) {
            XrArray *entries = xr_map_entries(vm_get_coro(vm_ctx), map);
            base[a] = entries ? xr_value_from_array(entries) : xr_null();
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_map_get_handler(isolate, SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DELETE) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_map_get_handler(isolate, SYMBOL_DELETE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_map_get_handler(isolate, SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FOREACH) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_map_get_handler(isolate, SYMBOL_FOREACH));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(
                frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "Map does not support dot syntax for key '%s', use map[\"%s\"] or map.get(\"%s\")",
                name ? name : "?", name ? name : "?", name ? name : "?");
        }
        return XR_DISP_NEXT;
    }

    // Set property access
    if (XR_IS_SET(obj)) {
        struct XrSet *set = XR_TO_SET(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer) xr_set_size(set));
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(xr_set_is_empty(set));
        } else if (prop_symbol == SYMBOL_ADD) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_ADD));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DELETE) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_DELETE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_UNION) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_UNION));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INTERSECTION) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_INTERSECTION));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DIFFERENCE) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_DIFFERENCE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TO_ARRAY) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_TO_ARRAY));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FOREACH) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_FOREACH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_MAP_METHOD) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_MAP_METHOD));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FILTER) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_set_get_handler(isolate, SYMBOL_FILTER));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            base[a] = xr_null();
        }
        return XR_DISP_NEXT;
    }

    // String property access
    if (XR_IS_STRING(obj)) {
        // Fast path: .length/.byteLength use str_data/len directly, no promote
        if (prop_symbol == SYMBOL_LENGTH || prop_symbol == SYMBOL_CHAR_LENGTH ||
            prop_symbol == SYMBOL_CHARS) {
            const char *d = xr_value_str_data(&obj);
            uint32_t bl = xr_value_str_len(&obj);
            base[a] = xr_int((xr_Integer) xr_utf8_strlen(d, bl));
            return XR_DISP_NEXT;
        }
        if (prop_symbol == SYMBOL_BYTE_LENGTH) {
            base[a] = xr_int((xr_Integer) xr_value_str_len(&obj));
            return XR_DISP_NEXT;
        }
        // Slow path: promote SSO to heap for bound method creation
        XrString *str = xr_value_to_string(isolate, obj);
        (void) str;
        if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CHARAT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_CHARAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SUBSTRING) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_SUBSTRING));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INDEXOF) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_INDEXOF));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CONTAINS) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_CONTAINS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_STARTSWITH) {
            XrBoundMethod *bm = xr_bound_method_new(
                isolate, obj, xr_string_get_handler(isolate, SYMBOL_STARTSWITH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ENDSWITH) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_ENDSWITH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TOLOWERCASE) {
            XrBoundMethod *bm = xr_bound_method_new(
                isolate, obj, xr_string_get_handler(isolate, SYMBOL_TOLOWERCASE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TOUPPERCASE) {
            XrBoundMethod *bm = xr_bound_method_new(
                isolate, obj, xr_string_get_handler(isolate, SYMBOL_TOUPPERCASE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TRIM) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_TRIM));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SPLIT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_SPLIT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPLACE) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_REPLACE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPLACEALL) {
            XrBoundMethod *bm = xr_bound_method_new(
                isolate, obj, xr_string_get_handler(isolate, SYMBOL_REPLACEALL));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPEAT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_REPEAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CONCAT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_CONCAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ITERATOR) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_string_get_handler(isolate, SYMBOL_ITERATOR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CODEPOINT_AT || prop_symbol == SYMBOL_CHARCODEAT) {
            XrBoundMethod *bm = xr_bound_method_new(
                isolate, obj, xr_string_get_handler(isolate, SYMBOL_CODEPOINT_AT));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY, "string has no property '%s'",
                     name ? name : "?");
        }
        return XR_DISP_NEXT;
    }

    // Tuple property access: digit-only names map to xr_tuple_get;
    // .length surfaces the arity so user code can introspect when the
    // type was lost to analyzer.
    if (xr_value_is_tuple(obj)) {
        XrTuple *tup = (XrTuple *) XR_TO_PTR(obj);
        uint16_t arity = xr_tuple_arity(tup);
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer) arity);
            return XR_DISP_NEXT;
        }
        if (name && name[0]) {
            bool digits_only = true;
            for (const char *p = name; *p; p++) {
                if (*p < '0' || *p > '9') {
                    digits_only = false;
                    break;
                }
            }
            if (digits_only) {
                long idx = strtol(name, NULL, 10);
                if (idx >= 0 && idx < (long) arity) {
                    base[a] = xr_tuple_get(tup, (uint16_t) idx);
                } else {
                    VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                             "tuple field index %ld out of range (arity %u)", idx,
                             (unsigned) arity);
                }
                return XR_DISP_NEXT;
            }
        }
        VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                 "tuple has no named field '%s'; use .N (zero-based) instead", name ? name : "?");
    }

    // Array property access
    if (XR_IS_ARRAY(obj)) {
        XrArray *array = XR_TO_ARRAY(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer) array->length);
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(array->length == 0);
        } else if (prop_symbol == SYMBOL_KEYS) {
            XrArray *keys = xr_array_with_capacity(vm_get_coro(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                xr_array_push(keys, xr_int(idx));
            }
            base[a] = xr_value_from_array(keys);
        } else if (prop_symbol == SYMBOL_VALUES) {
            XrArray *values = xr_array_with_capacity(vm_get_coro(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                xr_array_push(values, ((XrValue *) array->data)[idx]);
            }
            base[a] = xr_value_from_array(values);
        } else if (prop_symbol == SYMBOL_ENTRIES) {
            XrArray *entries = xr_array_with_capacity(vm_get_coro(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                XrArray *pair = xr_array_with_capacity(vm_get_coro(vm_ctx), 2);
                xr_array_push(pair, xr_int(idx));
                xr_array_push(pair, ((XrValue *) array->data)[idx]);
                xr_array_push(entries, xr_value_from_array(pair));
            }
            base[a] = xr_value_from_array(entries);
        } else if (prop_symbol == SYMBOL_PUSH) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_PUSH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_POP) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_POP));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SHIFT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_SHIFT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_UNSHIFT) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_UNSHIFT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INDEXOF) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_INDEXOF));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_JOIN) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_JOIN));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REVERSE) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_REVERSE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ITERATOR) {
            XrBoundMethod *bm =
                xr_bound_method_new(isolate, obj, xr_array_get_handler(isolate, SYMBOL_ITERATOR));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            base[a] = xr_null();
        }
        return XR_DISP_NEXT;
    }

    // Class object static field access
    if (xr_value_is_class(obj)) {
        XrClass *cls = xr_value_to_class(obj);
        int field_index = xr_class_lookup_field(cls, prop_symbol);
        if (field_index < 0) {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                     "static field '%s' not found in class '%s'", pname ? pname : "?", cls->name);
        }
        const XrFieldDescriptor *field = xr_class_get_field(cls, field_index);
        if (!field) {
            VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: field descriptor not found");
        }
        if (!(field->flags & XR_FIELD_STATIC)) {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY, "field '%s' is not a static field",
                     pname ? pname : "?");
        }
        int static_field_idx = field->static_slot;
        if (static_field_idx < 0 || static_field_idx >= cls->static_field_count) {
            VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                     "internal error: static field index out of bounds");
        }
        base[a] = cls->static_field_values[static_field_idx];
        return XR_DISP_NEXT;
    }

    // Module export property access
    if (xr_value_is_module(obj)) {
        XrModule *module = xr_value_to_module(obj);
        base[a] = xr_module_get_sym(module, prop_symbol);
        return XR_DISP_NEXT;
    }

    // Json property access
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        base[a] = xr_json_get(isolate, json, prop_symbol);
        return XR_DISP_NEXT;
    }

    // Channel property access error
    if (xr_value_is_channel(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        VM_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                 "Channel has no '.%s' property, available methods: send(), recv(), "
                 "trySend(), tryRecv(), close(), isClosed()",
                 name ? name : "?");
    }

    // Struct ref: getter/method lookup when field not found in layout
    if (XR_IS_STRUCT_REF(obj)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass **) sptr;
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        if (prop_name) {
            // Try getter method: get:<prop_name>
            char getter_name[256];
            snprintf(getter_name, sizeof(getter_name), "get:%s", prop_name);
            int getter_symbol = xr_symbol_register_in_table(sym_table, getter_name);
            XrMethod *getter =
                (getter_symbol >= 0) ? xr_class_lookup_method(scls, getter_symbol) : NULL;
            if (getter) {
                if (getter->type == XMETHOD_PRIMITIVE) {
                    base[a] = getter->as.primitive(isolate, obj, NULL, 0);
                    return XR_DISP_NEXT;
                }
                if (getter->as.closure) {
                    XrClosure *closure = getter->as.closure;
                    XrProto *proto = closure->proto;
                    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                        VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
                    }
                    base[a + 1] = obj;  // this = struct_ref
                    frame->pc = pc;
                    int _fidx = vm_ctx->frame_count;
                    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
                    vm_ctx->frame_count++;
                    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
                    new_frame->closure = closure;
                    new_frame->pc = PROTO_CODE_BASE(proto);
                    new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);
                    return XR_DISP_RESTART;
                }
            }
        }
        base[a] = xr_null();
        return XR_DISP_NEXT;
    }

    // Non-instance type error
    if (!xr_value_is_instance(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        const char *type_name = xr_typeid_name(xr_value_typeid(obj));
        int error_code = XR_IS_NULL(obj) ? XR_ERR_NULL_PROPERTY : XR_ERR_TYPE_NO_PROPERTY;

        int line = 0;
        int current_pc = 0;
        XrProto *proto = frame->closure->proto;
        if (proto) {
            current_pc = (int) (pc - PROTO_CODE_BASE(proto) - 1);
            size_t line_count = PROTO_LINE_COUNT(proto);
            if (line_count > 0) {
                size_t idx = (current_pc < (int) line_count) ? current_pc : line_count - 1;
                line = PROTO_LINE(proto, idx);
            }
        }
        (void) line;

        const char *var_name = xr_vm_get_local_name(proto, b, current_pc);
        if (var_name) {
            VM_THROW(frame, pc, error_code,
                     "variable '%s' has type '%s', does not support property access '.%s'",
                     var_name, type_name ? type_name : "unknown", pname ? pname : "?");
        } else {
            VM_THROW(frame, pc, error_code, "type '%s' does not support property access '.%s'",
                     type_name ? type_name : "unknown", pname ? pname : "?");
        }
    }

    return XR_DISP_FALLTHROUGH;  // Instance: handled by caller inline
}

/* ========== Dispatch: OP_GETPROP Instance Getter ========== */

XR_FUNC XrDispatchAction vm_getprop_instance_getter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                                    XrInstance *inst, XrValue obj, int prop_symbol,
                                                    XrValue *base, int a, XrBcCallFrame *frame,
                                                    XrInstruction *pc) {
    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name)
        return XR_DISP_FALLTHROUGH;

    size_t prop_name_len = strlen(prop_name);
    if (prop_name_len + 5 > 256) {
        VM_THROW(frame, pc, XR_ERR_OVERFLOW, "property name too long");
    }

    char getter_name[256];
    snprintf(getter_name, sizeof(getter_name), "get:%s", prop_name);

    int getter_symbol = xr_symbol_register_in_table(sym_table, getter_name);
    XrMethod *getter = NULL;
    if (getter_symbol >= 0) {
        getter = xr_class_lookup_method(inst->klass, getter_symbol);
    }

    if (!getter)
        return XR_DISP_FALLTHROUGH;

    // PRIMITIVE type getter
    if (getter->type == XMETHOD_PRIMITIVE) {
        base[a] = getter->as.primitive(isolate, obj, NULL, 0);
        return XR_DISP_NEXT;
    }

    // Closure getter: set up call frame
    XrClosure *closure = getter->as.closure;
    XrProto *proto = closure->proto;

    if (proto->numparams != 1) {
        VM_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT, "getter should have no parameters");
    }

    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
        VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
    }

    /* Place getter frame above caller's maxstacksize to avoid
     * clobbering caller registers (same strategy as operator
     * overload calls). */
    int safe_base = (int) (base - vm_ctx->stack) + frame->closure->proto->maxstacksize;
    vm_ctx->stack[safe_base] = obj;  // this

    frame->pc = pc;  // savepc

    int _fidx = vm_ctx->frame_count;
    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
    vm_ctx->frame_count++;
    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = safe_base;
    new_frame->result_offset = (int) ((base + a) - vm_ctx->stack);
    new_frame->call_status = XR_CALL_KEEP_FUNC;

    return XR_DISP_RESTART;
}

/* ========== Dispatch: OP_INVOKE Module Methods ========== */

XR_FUNC XrDispatchAction vm_invoke_module(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                          XrValue receiver, int method_symbol, int nargs,
                                          XrValue *base, int a, XrBcCallFrame *frame,
                                          XrInstruction *pc) {
    XrModule *module = xr_value_to_module(receiver);
    if (!module || module->export_count == 0)
        return XR_DISP_FALLTHROUGH;

    XrValue fn_val = xr_module_get_sym(module, method_symbol);
    if (XR_IS_NULL(fn_val)) {
        XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT, "module '%s' has no export '%s'",
                 module->name ? module->name : "?", _name ? _name : "?");
    }

    if (xr_value_is_cfunction(fn_val)) {
        XrCFunction *cfunc = xr_value_to_cfunction(fn_val);

        if (cfunc->is_yieldable) {
            frame->u.c.result_slot = (int16_t) a;
            frame->u.c.has_cfunc_result = false;

            XrValue result;
            XrCFuncResult status = cfunc->as.yieldable(isolate, &base[a + 2], nargs, &result);

            switch (status) {
                case XR_CFUNC_DONE:
                    base[a] = result;
                    return XR_DISP_NEXT;
                case XR_CFUNC_BLOCKED:
                    frame->pc = pc;
                    return XR_DISP_BLOCKED;
                case XR_CFUNC_YIELD:
                    frame->pc = pc;
                    return XR_DISP_YIELD;
                case XR_CFUNC_CALL_CLOSURE:
                    // Closure frame pushed, return to VM main loop
                    return XR_DISP_RESTART;
                case XR_CFUNC_ERROR:
                    return XR_DISP_FATAL;
            }
        } else {
            XrValue result = cfunc->as.func(isolate, &base[a + 2], nargs);
            base[a] = result;
            return XR_DISP_NEXT;
        }
    } else if (xr_value_is_closure(fn_val)) {
        XrClosure *closure = xr_value_to_closure(fn_val);

        frame->pc = pc;  // savepc

        if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
            VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
        }

        // Argument shift: from R[a+2..] to R[a+1..]
        for (int idx = 0; idx < nargs; idx++) {
            base[a + 1 + idx] = base[a + 2 + idx];
        }

        int _fidx = vm_ctx->frame_count;
        memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
        vm_ctx->frame_count++;
        XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(closure->proto);
        new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);

        return XR_DISP_RESTART;
    } else if (xr_value_is_class(fn_val)) {
        XrClass *klass = xr_value_to_class(fn_val);

        // Find constructor
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        int ctor_symbol = xr_symbol_lookup_in_table(sym_table, XR_KEYWORD_CONSTRUCTOR);
        XrMethod *constructor = NULL;
        if (ctor_symbol >= 0) {
            constructor = xr_class_lookup_method(klass, ctor_symbol);
        }

        // Create instance (allocation based on storage mode context)
        XrInstance *instance;
        uint8_t storage_mode = isolate->current_storage_mode;
        isolate->current_storage_mode = 0;

        if (storage_mode != 0 && isolate->sys_heap) {
            size_t size = xr_instance_size(klass);
            instance =
                (XrInstance *) xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TINSTANCE);
            if (instance) {
                xr_instance_init_inplace(instance, klass);
                XR_GC_SET_STORAGE(&instance->gc, storage_mode);
                if (storage_mode == XR_GC_STORAGE_SHARED) {
                    xr_shared_set_refc(&instance->gc, 1);
                }
            }
        } else {
            instance = xr_instance_new(isolate, klass);
        }

        if (!instance) {
            VM_THROW(frame, pc, XR_ERR_TYPE_NO_CALL, "failed to create instance: '%s'",
                     xr_class_display_name(klass));
        }
        XrValue inst_val = XR_FROM_PTR(instance);

        if (constructor && constructor->type == XMETHOD_CLOSURE) {
            XrClosure *closure = constructor->as.closure;

            frame->pc = pc;  // savepc

            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                VM_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            base[a + 1] = inst_val;  // this

            int _fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(closure->proto);
            new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);

            return XR_DISP_RESTART;
        }

        // No constructor, return instance directly
        base[a] = inst_val;
        return XR_DISP_NEXT;
    } else {
        XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT, "module export '%s' is not callable",
                 _name ? _name : "?");
    }
    return XR_DISP_NEXT;  // unreachable
}
