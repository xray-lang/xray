/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_invoke.inc.c -- OOP method invocation dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, ci, frame, base, k, R, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_DISPATCH_COLD, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT, VM_BARRIER_*, VM_STACK*, VM_CURRENT_CORO,
 * checkGC, startfunc label, ...) provided by the surrounding
 * scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   OOP method invocation:
 *     - OP_INVOKE              (unified calling convention with method IC)
 *     - OP_INVOKE_TAIL         (tail-call form of OP_INVOKE)
 *     - OP_SUPERINVOKE         (super-class method dispatch)
 *     - OP_INVOKE_DIRECT       (resolved class method, no IC needed)
 *     - OP_INVOKE_BUILTIN      (per-type builtin method table dispatch)
 *
 * Split out of xvm_dispatch_object.inc.c when the merged file crossed
 * the 1500-line per-file gate; OP_INVOKE alone is ~700 lines because
 * it inlines the full mono / poly / mega IC promotion ladder plus
 * fall-through to the slow class-lookup path.
 */

vmcase(OP_INVOKE) {
    /* Unified calling convention (eliminates argument shifting!)
     *
     * Register layout:
     *   R[A]   = return value position
     *   R[A+1] = this (receiver)
     *   R[A+2] = arg1
     *   R[A+3] = arg2
     *   ...
     *
     * Instruction format: OP_INVOKE A B C
     *   A = base register
     *   B = method symbol
     *   C = argument count (excluding this)
     *
     * OP_INVOKE_TAIL shares this dispatch via invoke_dispatch label.
     * invoke_is_tail=1 causes closure methods to reuse current frame.
     */
    TRACE_EXECUTION();
    invoke_is_tail = 0;
invoke_dispatch:;

    /* Reductions check intentionally absent here: only OP_JMP checks.
    ** GC safepoint handled at startfunc label. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int nargs = GETARG_C(i);

    /* Persist pc once for every dispatch path. Builtin
     * handlers that throw a contract exception (e.g.
     * WeakMap.set / WeakSet.add receiver-type guards)
     * call xr_vm_throw_exception, which reads frame->pc
     * to compute the throw line and rewrites it to the
     * matching catch handler. */
    savepc();

/* After a builtin handler returns, surface any
 * pending exception thrown from inside it. The throw
 * already redirected frame->pc, so we just refresh
 * dispatch locals via startfunc (or fall out to the
 * embedder when no handler is on the stack). */
#define VM_BUILTIN_INVOKE_CHECK_EXC()                                                              \
    do {                                                                                           \
        if (unlikely(!XR_IS_NULL(VM_EXCEPTION))) {                                                 \
            if (VM_HANDLER_COUNT == 0)                                                             \
                return XR_VM_RUNTIME_ERROR;                                                        \
            goto startfunc;                                                                        \
        }                                                                                          \
    } while (0)

    // Declared here (before all invoke_* labels) so jumps past
    // the assignment below still observe a deterministic NULL.
    const char *method_name_chars = NULL;

    // receiver at R[a+1] (new calling convention)
    XrValue receiver = R(a + 1);

    // Dereference local index → global symbol via per-function symbol table
    int method_symbol = PROTO_SYMBOL(cl->proto, b);
    // Method inline optimization (fast path)
    // Property access .length is the standard way to get collection size

    // Cold path: task handle methods (works even after executor detach)
    if (xr_value_is_task(receiver)) {
        savepc();
        int _cr = vm_invoke_task_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    // Cold path: legacy coroutine handle methods
    if (xr_value_is_coro(receiver)) {
        savepc();
        int _cr = vm_invoke_coro_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    // Channel methods: inline send/recv hot path, cold path for rest
    if (xr_value_is_channel(receiver)) {
        XrChannel *ch = xr_value_to_channel(receiver);

        // Hot path: ch.send(value) - inline blocking send
        if (nargs == 1 && method_symbol == SYMBOL_SEND) {
            XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
            if (_cur && xr_coro_resume_load(_cur) == XR_RESUME_CHANNEL) {
                xr_coro_resume_store(_cur, XR_RESUME_OK);
                R(a) = xr_null();
                vmbreak;
            }
            XrValue _sv = vm_chan_copy_send(isolate, R(a + 2));
            // Pre-save frame state before channel call.  If send blocks, channel func sets BLOCKED
            // under lock — coro must already have saved state.
            if (_cur)
                _cur->send_value = _sv;
            savepc();
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            XrChanResult _cr = xr_channel_send(ch, _sv, _cur);
            if (_cr == XR_CHAN_OK) {
                frame->call_status &= ~XR_CALL_YIELDED;
                R(a) = xr_null();
                vmbreak;
            } else if (_cr == XR_CHAN_BLOCK) {
                return XR_VM_BLOCKED;
            } else {
                frame->call_status &= ~XR_CALL_YIELDED;
            }
            // Closed or error: fall through to cold path
        }

        // Hot path: ch.recv() - inline blocking recv
        if (nargs == 0 && method_symbol == SYMBOL_RECV) {
            XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
            if (_cur) {
                int _rs = xr_coro_resume_load(_cur);
                if (_rs == XR_RESUME_CHANNEL) {
                    xr_coro_resume_store(_cur, XR_RESUME_OK);
                    R(a) = vm_chan_copy_recv(isolate, R(a), vm_ctx);
                    vmbreak;
                }
                if (_rs == XR_RESUME_CHANNEL_CLOSED) {
                    xr_coro_resume_store(_cur, XR_RESUME_OK);
                    _cur->wait_channel = NULL;
                }
            }
            // Set recv_slot BEFORE xr_channel_recv: once coro enters
            // recvq, a sender on another worker may immediately dequeue
            // and write to recv_slot.  Setting it after return races.
            if (_cur)
                _cur->recv_slot = &R(a);
            // Pre-save frame state — see send path.
            savepc();
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            XrValue _rv;
            XrChanResult _cr = xr_channel_recv(ch, &_rv, _cur);
            if (_cr == XR_CHAN_OK) {
                frame->call_status &= ~XR_CALL_YIELDED;
                R(a) = vm_chan_copy_recv(isolate, _rv, vm_ctx);
                vmbreak;
            } else if (_cr == XR_CHAN_CLOSED) {
                frame->call_status &= ~XR_CALL_YIELDED;
                R(a) = xr_null();
                vmbreak;
            } else if (_cr == XR_CHAN_BLOCK) {
                return XR_VM_BLOCKED;
            } else {
                frame->call_status &= ~XR_CALL_YIELDED;
            }
            // Error: fall through to cold path
        }

        // Cold path: other channel methods
        savepc();
        int _cr = vm_invoke_channel(isolate, vm_ctx, ch, method_symbol, nargs, base, a, frame, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_BLOCKED)
            return XR_VM_BLOCKED;
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    if (nargs == 0 && method_symbol == SYMBOL_IS_EMPTY) {
        // isEmpty() method inline: directly check count == 0
        if (XR_IS_ARRAY(receiver)) {
            XrArray *arr = XR_TO_ARRAY(receiver);
            R(a) = xr_bool(arr->length == 0);
            vmbreak;  // Skip method call!
        } else if (XR_IS_STRING(receiver)) {
            R(a) = xr_bool(xr_value_str_len(&receiver) == 0);
            vmbreak;  // Skip method call!
        } else if (XR_IS_MAP(receiver)) {
            XrMap *map = XR_TO_MAP(receiver);
            R(a) = xr_bool(map->count == 0);
            vmbreak;  // Skip method call!
        } else if (XR_IS_SET(receiver)) {
            XrSet *set = XR_TO_SET(receiver);
            R(a) = xr_bool(set->count == 0);
            vmbreak;  // Skip method call!
        }
    }

    /* === Type-based dispatch: O(1) jump table === */

    /* Struct ref: value-type constructor/method call.
     * struct_ref layout: [XrClass* 8B][fields...] in struct_area */
    if (XR_IS_STRUCT_REF(receiver)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(receiver);
        XrClass *scls = *(XrClass **) sptr;
        XrMethod *method = xr_class_lookup_method(scls, method_symbol);

        if (method == NULL || method->type == XMETHOD_NONE) {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "struct '%s' has no method '%s'", scls->name,
                             _mn ? _mn : "?");
        }
        if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
            R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
            vmbreak;
        }
        if (method->type == XMETHOD_CLOSURE && method->as.closure != NULL) {
            XrClosure *closure = method->as.closure;
            XrProto *proto = closure->proto;
            if (nargs + 1 != proto->numparams) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "constructor expects %d arguments, got %d",
                                 proto->numparams - 1, nargs);
            }
            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
            }
            // this (struct_ref) already in R[a+1]
            savepc();
            int _fidx = VM_FRAME_COUNT;
            VM_INC_FRAME_COUNT;
            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
            goto startfunc;
        }
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "struct method has invalid type");
    }

    if (!XR_IS_PTR(receiver)) {
        if (XR_IS_INT(receiver))
            goto invoke_int;
        if (XR_IS_FLOAT(receiver))
            goto invoke_float;
        if (XR_IS_BOOL(receiver))
            goto invoke_bool;
        // SSO removed: all strings are heap PTR, handled by XR_TSTRING case
        goto invoke_type_error;
    }
    switch (XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(receiver))) {
        case XR_TINSTANCE:
        case XR_TCLASS:
            goto invoke_class_or_instance;
        case XR_TSTRING:
            goto invoke_string;
        case XR_TARRAY:
            goto invoke_array;
        case XR_TMAP:
            goto invoke_map;
        case XR_TSET:
            goto invoke_set;
        case XR_TJSON:
            goto invoke_json;
        case XR_TMODULE:
            goto invoke_module;
        case XR_TENUM_VALUE:
        case XR_TENUM_TYPE:
            goto invoke_enum;
        case XR_TITERATOR:
            goto invoke_iterator;
        case XR_TBIGINT:
            goto invoke_bigint;
        case XR_TSTRINGBUILDER:
            goto invoke_stringbuilder;
        case XR_TARRAY_SLICE:
            goto invoke_slice;
        case XR_TRANGE:
            goto invoke_range;
        default:
            goto invoke_native_type;
    }

// Cold path: enum methods
invoke_enum: {
    savepc();
    int _cr = vm_invoke_enum(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
    if (_cr == VM_COLD_BREAK)
        vmbreak;
    if (_cr == VM_COLD_ERROR) {
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }
    // VM_COLD_CONTINUE: fall through to class/instance path
}

// Original logic: normal method call
// Args from R[a+2] (R[a]=return value, R[a+1]=this)

// Iterator methods (hot path: for-in loops call hasNext+next every iteration)
invoke_iterator:
    if (xr_is_iterator(receiver)) {
        XrIterator *iter = xr_value_to_iterator(receiver);
        if (unlikely(!iter)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid iterator object");
        }
        if (method_symbol == SYMBOL_HASNEXT) {
            R(a) = xr_bool(xr_iterator_has_next(iter));
        } else if (method_symbol == SYMBOL_NEXT) {
            R(a) = xr_iterator_next(iter);
        } else if (method_symbol == SYMBOL_TOSTRING) {
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
        } else {
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            const char *mname = xr_symbol_get_name_in_table(sym_table, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "iterator does not support method: %s",
                             mname ? mname : "?");
        }
        vmbreak;
    }

/* === Map builtin methods (XrClass dispatch) === */
invoke_map:
    if (XR_IS_MAP(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TMAP];
        XR_DCHECK(_cls != NULL, "Map XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Map has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Json builtin methods (XrClass dispatch) === */
invoke_json:
    if (xr_value_is_json(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TJSON];
        XR_DCHECK(_cls != NULL, "Json XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Json has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === String builtin methods (XrClass dispatch) === */
invoke_string:
    if (XR_IS_STRING(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TSTRING];
        XR_DCHECK(_cls != NULL, "String XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "String has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Array builtin methods (XrClass dispatch) === */
invoke_array:
    if (XR_IS_ARRAY(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TARRAY];
        XR_DCHECK(_cls != NULL, "Array XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Array has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Set builtin methods (XrClass dispatch) === */
invoke_set:
    if (XR_IS_SET(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TSET];
        XR_DCHECK(_cls != NULL, "Set XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Set has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Int builtin methods (XrClass dispatch) === */
invoke_int:
    if (XR_IS_INT(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TINT];
        XR_DCHECK(_cls != NULL, "Int XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "int has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Float builtin methods (XrClass dispatch) === */
invoke_float:
    if (XR_IS_FLOAT(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TFLOAT];
        XR_DCHECK(_cls != NULL, "Float XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "float has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Bool builtin methods (XrClass dispatch) === */
invoke_bool:
    if (XR_IS_BOOL(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TBOOL];
        XR_DCHECK(_cls != NULL, "Bool XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bool has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === BigInt builtin methods (XrClass dispatch) === */
invoke_bigint:
    if (XR_IS_BIGINT(receiver)) {
        XrClass *_cls = isolate->native_type_classes[XR_TBIGINT];
        XR_DCHECK(_cls != NULL, "BigInt XrClass not registered");
        XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
        if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
            R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
        } else {
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "BigInt has no method '%s'", _mn ? _mn : "?");
        }
        vmbreak;
    }

/* === Stdlib/third-party native type method call (via native_type_classes mapping) === */
invoke_native_type:
    if (XR_IS_PTR(receiver)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);
        XrObjType native_type = XR_GC_GET_TYPE(gc);

        // Find bound XrClass
        if (native_type < XR_NATIVE_TYPE_MAX) {
            XrClass *native_klass = isolate->native_type_classes[native_type];
            if (native_klass) {
                // Lookup through class methods
                XrMethod *method = xr_class_lookup_method(native_klass, method_symbol);
                if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                    // Native method: call C function directly
                    R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                    vmbreak;
                }
            }
        }
    }

// Cold path: module export function call
invoke_module:
    if (xr_value_is_module(receiver)) {
        savepc();
        int _cr =
            vm_invoke_module(isolate, vm_ctx, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (_cr == VM_COLD_BLOCKED)
            return XR_VM_BLOCKED;
        if (_cr == VM_COLD_YIELD)
            return XR_VM_YIELD;
        if (_cr == VM_COLD_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == VM_COLD_ERROR) {
            if (VM_HANDLER_COUNT == 0)
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
    }

// Class/Instance method call path.
// method_name_chars is declared at the top of invoke_dispatch
// so every `goto invoke_*` sibling label observes NULL.
invoke_class_or_instance:;
    if (xr_value_is_class(receiver) || xr_value_is_instance(receiver)) {
        // Get method name only when needed
        XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
        method_name_chars = xr_symbol_get_name_in_table(sym_table, method_symbol);
        if (method_name_chars == NULL) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "invalid method symbol: %d", method_symbol);
        }
    }

    // Lazily ensure the per-ctx method IC table for this proto.
    XrICMethodTable *ic_method_table = xr_vm_ctx_ensure_ic_methods(vm_ctx, frame->closure->proto);
    if (!ic_method_table) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_INVOKE: failed to allocate IC table");
    }

// Range method call
invoke_range:
    if (XR_IS_RANGE(receiver)) {
        XrRange *rng = xr_value_to_range(receiver);
        if (method_symbol == SYMBOL_TOSTRING) {
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
            vmbreak;
        } else if (method_symbol == SYMBOL_TO_ARRAY) {
            R(a) = xr_range_to_array(VM_CURRENT_CORO, rng);
            vmbreak;
        } else if (method_symbol == SYMBOL_CONTAINS) {
            if (nargs >= 1 && XR_IS_INT(R(a + 2))) {
                R(a) = xr_bool(xr_range_contains(rng, XR_TO_INT(R(a + 2))));
            } else {
                R(a) = xr_bool(false);
            }
            vmbreak;
        } else {
            XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
            const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Range has no method '%s'",
                             mname ? mname : "?");
        }
    }

// Support StringBuilder method call
invoke_stringbuilder:
    if (xr_is_stringbuilder(receiver)) {
        XrClass *klass = xr_value_get_class(isolate, receiver);
        if (klass) {
            XrMethod *method = xr_class_lookup_method(klass, method_symbol);
            if (method != NULL && method->type == XMETHOD_PRIMITIVE &&
                method->as.primitive != NULL) {
                R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                vmbreak;
            }
        }
    }

    // Support basic types (Int, Float, Bool) method call
    // Args from R[a+1] (this at a+1)
    if (XR_IS_INT(receiver) || XR_IS_FLOAT(receiver) || XR_IS_BOOL(receiver)) {
        XrClass *klass = xr_value_get_class(isolate, receiver);

        if (klass) {
            XrMethod *method = xr_class_lookup_method(klass, method_symbol);

            if (method != NULL && method->type == XMETHOD_PRIMITIVE &&
                method->as.primitive != NULL) {
                R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                vmbreak;
            } else {
                // Universal toString fallback for basic types
                if (method_symbol == SYMBOL_TOSTRING) {
                    R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                    vmbreak;
                }
                // Basic type method not found - show type and method name
                XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
                const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
                const char *tname =
                    XR_IS_INT(receiver) ? "int" : (XR_IS_FLOAT(receiver) ? "float" : "bool");
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "%s type has no method '%s'", tname,
                                 mname ? mname : "?");
            }
        }
    }

// Support ArraySlice method call
invoke_slice:
    if (XR_IS_PTR(receiver)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);
        int heap_type = XR_GC_GET_TYPE(gc);

        if (heap_type == XR_TARRAY_SLICE) {
            XrClass *klass = xr_value_get_class(isolate, receiver);
            if (klass) {
                XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                    R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                    vmbreak;
                }
            }
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                vmbreak;
            }
            XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
            const char *name = xr_symbol_get_name_in_table(st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "ArraySlice has no method '%s'",
                             name ? name : "?");
        }
    }

    // Cold path: class constructor / static method
    if (xr_value_is_class(receiver)) {
        int _cr = vm_invoke_class(isolate, vm_ctx, receiver, method_symbol, method_name_chars,
                                  nargs, base, a, ci, pc, invoke_is_tail);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    } else {
        // Instance method call
        if (!xr_value_is_instance(receiver)) {
            // Universal toString fallback for any remaining type
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                vmbreak;
            }
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on non-instance",
                             method_name_chars);
        }
        XrInstance *inst = xr_value_to_instance(receiver);

        // Find method via polymorphic inline cache
        size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
        XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
        XrICMethod *cache = xr_ic_method_table_get(ic_method_table, cache_index);
        if (cache) {
            XR_VM_IC_METHOD_BIND(cache, (int) cache_index);
        }

        XrMethod *method = NULL;
        if (cache) {
            method = xr_ic_method_lookup(cache, inst->klass, method_symbol);
        } else {
            method = xr_class_lookup_method(inst->klass, method_symbol);
        }

        if (method == NULL || method->type == XMETHOD_NONE) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' not found", method_name_chars);
        }

        // Support PRIMITIVE type instance methods (reflection API)
        // Args from R[a+1] (this at a+1)
        if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
            R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
            vmbreak;
        }

        if (method->type != XMETHOD_CLOSURE || method->as.closure == NULL) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' has invalid type",
                             method_name_chars);
        }

        // Reuse method closure directly
        XrClosure *closure = method->as.closure;
        XrProto *proto = closure->proto;

        // Check argument count
        if (nargs + 1 != proto->numparams) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' expects %d arguments but got %d",
                             method_name_chars, proto->numparams - 1, nargs);
        }

        if (invoke_is_tail) {
            // Tail call: reuse current stack frame
            memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
            ci->closure = closure;
            ci->pc = PROTO_CODE_BASE(proto);
            VM_SET_STACK_TOP(base + proto->maxstacksize);
            goto startfunc;
        }

        // Check stack space
        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
        }

        // Unified calling convention: this already in R[a+1], args in R[a+2]..., no shift needed!

        // Save current frame pc
        savepc();

        // Create new call frame
        int _fidx = VM_FRAME_COUNT;
        VM_INC_FRAME_COUNT;
        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(proto);
        new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);

        // Jump to new function
        goto startfunc;
    }

invoke_type_error: {
    XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
    const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on unsupported type",
                     _mn ? _mn : "?");
}
    vmbreak;
}

vmcase(OP_INVOKE_TAIL) {
    /* Method tail call: reuse OP_INVOKE's full dispatch,
     * but reuse current stack frame for closure methods.
     * Set flag and jump into OP_INVOKE's code. */
    TRACE_EXECUTION();
    invoke_is_tail = 1;
    goto invoke_dispatch;
}

vmcase(OP_SUPERINVOKE) {
    TRACE_EXECUTION();
    savepc();
    int _cr = vm_superinvoke(isolate, vm_ctx, i, base, ci, pc);
    if (_cr == VM_COLD_STARTFUNC)
        goto startfunc;
    if (VM_HANDLER_COUNT == 0)
        return XR_VM_RUNTIME_ERROR;
    goto startfunc;
}

/* ========================================================
** Optimized instructions: compile-time determined index (fully inlined)
** ======================================================== */

vmcase(OP_INVOKE_DIRECT) {
    /* ========== OP_INVOKE_DIRECT - Direct method call (unified calling convention) ==========
     *
     * Format: OP_INVOKE_DIRECT A B C
     *   A = base register (return value in R[A], this in R[A+1])
     *   B = method index
     *   C = nargs | tail_flag (bit 7: 0x80 = tail call)
     *
     * Layout: R[A]=return value, R[A+1]=this, R[A+2]=arg1, ...
     */
    int a = GETARG_A(i);
    int method_idx = GETARG_B(i);
    int c_raw = GETARG_C(i);
    int is_tail_direct = (c_raw & 0x80);
    int nargs = c_raw & 0x7F;

    XrValue receiver = R(a + 1);
    XrClass *cls;
    if (XR_IS_STRUCT_REF(receiver)) {
        // Stack-allocated struct: class ptr at head
        cls = *(XrClass **) xr_to_struct_ptr(receiver);
    } else {
        XrInstance *inst_obj = xr_value_to_instance(receiver);
        cls = xr_instance_get_class(inst_obj);
    }
    XrMethod *method = &cls->methods[method_idx];
    XrClosure *closure = method->as.closure;

    if (is_tail_direct) {
        // Tail call: reuse current stack frame
        memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
        ci->closure = closure;
        ci->pc = PROTO_CODE_BASE(closure->proto);
        VM_SET_STACK_TOP(base + closure->proto->maxstacksize);
        goto startfunc;
    }

    savepc();
    int _fidx = VM_FRAME_COUNT;
    VM_INC_FRAME_COUNT;
    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(closure->proto);
    new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);  // this at R[a+1]
    goto startfunc;
}

vmcase(OP_INVOKE_BUILTIN) {
    /* ========== OP_INVOKE_BUILTIN — builtin-type method call ==========
     *
     * Format: OP_INVOKE_BUILTIN A B C
     *   A = base register (return value in R[A], this in R[A+1])
     *   B = method symbol
     *   C = nargs (argument count, excluding this)
     *
     * Dispatch fast path (this opcode hits ~all .map/.push/.has
     * call sites in real code, so it has to be tight):
     *
     *   1. Per-call-site monomorphic IC keyed on (XrTypeId,
     *      slot*). On hit, jump straight to slot->fn — skipping
     *      both the 10-branch type chain and the unified
     *      table indirection.
     *
     *   2. On miss (uninitialized, type changed, etc.) fall
     *      into the unified xr_method_table_lookup over the
     *      ten migrated types. Cache (sticky first-write-wins)
     *      after a successful dispatch.
     *
     *   3. If the type isn't migrated (StringBuilder, slice,
     *      null, raw heap object), fall through to the
     *      type-specific branches at the bottom.
     */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int method_symbol = PROTO_SYMBOL(cl->proto, GETARG_B(i));
    int nargs = GETARG_C(i);

    XrValue receiver = R(a + 1);
    XrValue *args = &R(a + 2);

    /* Persist the local pc into the frame before calling
     * any builtin handler. If the handler throws (see e.g.
     * the WeakMap.set / WeakSet.add receiver-type guards),
     * xr_vm_add_stacktrace and xr_vm_throw_exception read
     * frame->pc to record the throw site and to redirect
     * to the catch handler. */
    savepc();

    /* Lazy IC table allocation. cache_index = pc - 1 - base
     * because pc has already advanced past this instruction.
     * Pre-sized to PROTO_CODE_COUNT in xr_ic_builtin_table_new
     * so the index is always valid. */
    XrICBuiltinTable *_btab = xr_vm_ctx_ensure_ic_builtin(vm_ctx, cl->proto);
    if (unlikely(!_btab)) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_INVOKE_BUILTIN: failed to allocate IC table");
    }
    size_t _cidx = (size_t) (pc - 1 - PROTO_CODE_BASE(cl->proto));
    XrICBuiltin *_ic = &_btab->caches[_cidx];

    int _rcv_tid = (int) xr_value_typeid(receiver);

    if (likely(_ic->slot != NULL && _ic->cached_tid == _rcv_tid)) {
        /* IC hit: direct dispatch, no chain, no table lookup. */
        if (likely(_ic->hits != UINT16_MAX))
            _ic->hits++;
        R(a) = _ic->slot->fn(isolate, receiver, args, nargs);
    } else {
        if (_ic->slot) {
            /* IC miss on already-filled cache (poly site).
             * We don't replace the cached slot — sticky
             * first-write-wins keeps the hot type fast. */
            if (likely(_ic->misses != UINT16_MAX))
                _ic->misses++;
        }

        /* Unified slow path: single lookup covers all ten
         * migrated types. */
        const XrMethodSlot *_slot =
            xr_method_table_lookup(_rcv_tid, method_symbol, SYMBOL_BUILTIN_COUNT);
        if (_slot) {
            R(a) = _slot->fn(isolate, receiver, args, nargs);
            if (!_ic->slot) {
                /* First-write-wins. cached_tid fits in
                 * int16_t because XR_TID_COUNT < 256. */
                _ic->slot = _slot;
                _ic->cached_tid = (int16_t) _rcv_tid;
            }
        } else if (xr_is_stringbuilder(receiver)) {
            // StringBuilder: dispatch through native_type_classes
            XrClass *klass = isolate->native_type_classes[XR_TSTRINGBUILDER];
            if (klass) {
                XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                    R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                    vmbreak;
                }
            }
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                vmbreak;
            }
            XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
            const char *mn = xr_symbol_get_name_in_table(st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "StringBuilder has no method '%s'",
                             mn ? mn : "?");
        } else if (XR_IS_PTR(receiver)) {
            // Slice type method dispatch
            XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);
            int heap_type = XR_GC_GET_TYPE(gc);

            if (heap_type == XR_TARRAY_SLICE) {
                // Get class for slice type, lookup method through class
                XrClass *klass = xr_value_get_class(isolate, receiver);
                if (klass) {
                    XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                    if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                        R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
                        vmbreak;
                    }
                }
                if (method_symbol == SYMBOL_TOSTRING) {
                    R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                    vmbreak;
                }
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "slice type has no such method (symbol %d)",
                                 method_symbol);
            }
            // Universal toString fallback for all other heap types
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                vmbreak;
            }
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                             "this type does not support builtin method call");
        } else if (XR_IS_NULL(receiver)) {
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_string_intern(isolate, "null", 4, 0));
            } else {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "null has no method");
            }
        } else {
            // Universal toString fallback
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
            } else {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                                 "this type does not support builtin method call");
            }
        }
    } /* close slow-path else (IC miss) */
    // If a builtin handler threw a catchable exception
    // (e.g. WeakMap.set with non-object key), the throw
    // logic already redirected frame->pc to the matching
    // handler and stashed the exception on the ctx; we
    // just need to refresh the dispatch locals and either
    // continue at the catch site or surface the error to
    // the embedder when nothing caught it.
    if (unlikely(!XR_IS_NULL(VM_EXCEPTION))) {
        if (VM_HANDLER_COUNT == 0)
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }
    // Unified method-not-found check for all builtin dispatch functions
    if (XR_IS_NOTFOUND(R(a))) {
        XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(st, method_symbol);
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "type '%s' has no method '%s'",
                         xr_typeid_name(xr_value_typeid(receiver)), name ? name : "?");
    }
    vmbreak;
}
