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
            if (!xr_vm_is_catch_reachable(isolate))                                                \
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
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    // Cold path: legacy coroutine handle methods
    if (xr_value_is_coro(receiver)) {
        savepc();
        int _cr = vm_invoke_coro_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (!xr_vm_is_catch_reachable(isolate))
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
        if (!xr_vm_is_catch_reachable(isolate))
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

    /* === Type-based dispatch === */

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

    /* Resolve XrObjType for the receiver. Value types (int/float/bool)
     * use the enum directly; heap objects read the GC header type.
     * This is the single resolution point for all type-based dispatch. */
    XrObjType _obj_type;
    if (XR_IS_INT(receiver))
        _obj_type = XR_TINT;
    else if (XR_IS_FLOAT(receiver))
        _obj_type = XR_TFLOAT;
    else if (XR_IS_BOOL(receiver))
        _obj_type = XR_TBOOL;
    else if (XR_IS_PTR(receiver))
        _obj_type = XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(receiver));
    else if (XR_IS_NULL(receiver)) {
        /* null only supports toString(); anything else is a type error */
        if (method_symbol == SYMBOL_TOSTRING) {
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
            vmbreak;
        }
        goto invoke_type_error;
    } else
        goto invoke_type_error;

    /* Route types that need special VM control flow (can push frames,
     * block, yield, or have closure methods). Everything else goes
     * through the unified native dispatch below. */
    switch (_obj_type) {
        case XR_TINSTANCE: {
            XrInstance *_inst = (XrInstance *) XR_TO_PTR(receiver);
            if (_inst->klass && (_inst->klass->flags & (XR_CLASS_ENUM_VALUE | XR_CLASS_ENUM_TYPE)))
                goto invoke_enum;
            goto invoke_class_or_instance;
        }
        case XR_TCLASS:
            goto invoke_class_or_instance;
        case XR_TMODULE:
            goto invoke_module;
        default:
            break;
    }

    /* === Unified native type dispatch via native_type_classes[] ===
     *
     * All types registered through xr_register_native_type() are
     * dispatched here: value types (int, float, bool), collections
     * (Array, Map, Set, String, Json), stdlib extensions (DateTime,
     * Regex, Logger, Net), and internal types (Iterator, Range,
     * StringBuilder, BigInt). One code path replaces
     * the former per-type invoke_* blocks. */
    {
        XrClass *_cls =
            ((int) _obj_type < XR_NATIVE_TYPE_MAX) ? isolate->native_type_classes[_obj_type] : NULL;
        if (_cls) {
            XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
            if (likely(_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)) {
                R(a) = _m->as.primitive(isolate, receiver, &R(a + 2), nargs);
                VM_BUILTIN_INVOKE_CHECK_EXC();
                vmbreak;
            }
        }
        /* toString fallback for any type */
        if (method_symbol == SYMBOL_TOSTRING) {
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
            vmbreak;
        }
        XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "type '%s' has no method '%s'",
                         xr_typeid_name(xr_value_typeid(receiver)), _mn ? _mn : "?");
    }

// Cold path: enum methods
invoke_enum: {
    savepc();
    int _cr = vm_invoke_enum(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
    if (_cr == VM_COLD_BREAK)
        vmbreak;
    if (_cr == VM_COLD_ERROR) {
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }
    // VM_COLD_CONTINUE: fall through to class/instance path
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
            if (!xr_vm_is_catch_reachable(isolate))
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

    // Cold path: class constructor / static method
    if (xr_value_is_class(receiver)) {
        int _cr = vm_invoke_class(isolate, vm_ctx, receiver, method_symbol, method_name_chars,
                                  nargs, base, a, ci, pc, invoke_is_tail);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (!xr_vm_is_catch_reachable(isolate))
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
    if (!xr_vm_is_catch_reachable(isolate))
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
     * Dispatch:
     *   1. Monomorphic IC keyed on (XrTypeId, fn). On hit, call fn
     *      directly — skipping the XrClass lookup entirely.
     *   2. On miss: resolve XrClass from native_type_classes or
     *      xr_value_get_class, then xr_class_lookup_method. Cache
     *      the resolved fn (sticky first-write-wins).
     *   3. Fallback for null / toString / unregistered types.
     */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int method_symbol = PROTO_SYMBOL(cl->proto, GETARG_B(i));
    int nargs = GETARG_C(i);

    XrValue receiver = R(a + 1);
    XrValue *args = &R(a + 2);

    /* Persist local pc for throw-site recording. */
    savepc();

    /* Lazy IC table allocation. */
    XrICBuiltinTable *_btab = xr_vm_ctx_ensure_ic_builtin(vm_ctx, cl->proto);
    if (unlikely(!_btab)) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_INVOKE_BUILTIN: failed to allocate IC table");
    }
    size_t _cidx = (size_t) (pc - 1 - PROTO_CODE_BASE(cl->proto));
    XrICBuiltin *_ic = &_btab->caches[_cidx];

    int _rcv_tid = (int) xr_value_typeid(receiver);

    if (likely(_ic->fn != NULL && _ic->cached_tid == _rcv_tid)) {
        /* IC hit: direct dispatch. */
        if (likely(_ic->hits != UINT16_MAX))
            _ic->hits++;
        R(a) = _ic->fn(isolate, receiver, args, nargs);
    } else {
        if (_ic->fn) {
            /* IC miss on filled cache (poly site). */
            if (likely(_ic->misses != UINT16_MAX))
                _ic->misses++;
        }

        /* Resolve XrClass for the receiver type. For value types
         * (int/float/bool) use the gc_type enum directly; for
         * heap objects read the GC header type. Then look up in
         * native_type_classes. */
        XrObjType _obj_type;
        if (XR_IS_INT(receiver))
            _obj_type = XR_TINT;
        else if (XR_IS_FLOAT(receiver))
            _obj_type = XR_TFLOAT;
        else if (XR_IS_BOOL(receiver))
            _obj_type = XR_TBOOL;
        else if (XR_IS_PTR(receiver))
            _obj_type = XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(receiver));
        else
            _obj_type = XR_TNULL;

        /* Try native_type_classes first (covers all registered types). */
        XrClass *_cls =
            ((int) _obj_type < XR_NATIVE_TYPE_MAX) ? isolate->native_type_classes[_obj_type] : NULL;
        /* Fall back to xr_value_get_class for core-class types
         * (StringBuilder, etc.) */
        if (!_cls)
            _cls = xr_value_get_class(isolate, receiver);

        XrPrimitiveMethodFn _resolved_fn = NULL;
        if (_cls) {
            XrMethod *_m = xr_class_lookup_method(_cls, method_symbol);
            if (_m && _m->type == XMETHOD_PRIMITIVE && _m->as.primitive)
                _resolved_fn = _m->as.primitive;
        }

        if (_resolved_fn) {
            R(a) = _resolved_fn(isolate, receiver, args, nargs);
            if (!_ic->fn) {
                /* First-write-wins IC fill. */
                _ic->fn = _resolved_fn;
                _ic->cached_tid = (int16_t) _rcv_tid;
            }
        } else if (XR_IS_NULL(receiver)) {
            if (method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_string_intern(isolate, "null", 4, 0));
            } else {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "null has no method");
            }
        } else if (method_symbol == SYMBOL_TOSTRING) {
            /* Universal toString fallback for all types. */
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
        } else {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                             "this type does not support builtin method call");
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
        if (!xr_vm_is_catch_reachable(isolate))
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
