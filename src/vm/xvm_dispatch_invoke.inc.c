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
 * VM_RUNTIME_ERROR, VM_DISPATCH, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT, VM_BARRIER_*, VM_STACK*, VM_CURRENT_CORO,
 * checkGC, startfunc label, ...) provided by the surrounding
 * scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   OOP method invocation:
 *     - OP_INVOKE              (unified XrClass + IC dispatch)
 *     - OP_INVOKE_TAIL         (tail-call form of OP_INVOKE)
 *     - OP_SUPERINVOKE         (super-class method dispatch)
 *     - OP_INVOKE_DIRECT       (resolved class method, no IC needed)
 *
 * All types that carry an XrClass (value types, native types,
 * instances, structs) go through one unified IC-cached lookup.
 * Only channel/task/coro/module/class-constructor require dedicated
 * paths due to VM control flow (blocking IO, frame push, yield).
 */

vmcase(OP_INVOKE) {
    /* Unified calling convention:
     *   R[A]   = return slot
     *   R[A+1] = receiver (this)
     *   R[A+2..A+1+C] = arguments
     * OP_INVOKE_TAIL shares this dispatch via invoke_dispatch label. */
    TRACE_EXECUTION();
    invoke_is_tail = 0;
invoke_dispatch:;

    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int nargs = GETARG_C(i);
    savepc();

/* Surface any pending exception from a builtin handler. */
#define VM_BUILTIN_INVOKE_CHECK_EXC()                                                              \
    do {                                                                                           \
        if (XR_UNLIKELY(!XR_IS_NULL(VM_EXCEPTION))) {                                              \
            if (!xr_vm_is_catch_reachable(isolate))                                                \
                return XR_VM_RUNTIME_ERROR;                                                        \
            goto startfunc;                                                                        \
        }                                                                                          \
    } while (0)

    XrValue receiver = R(a + 1);
    int method_symbol = PROTO_SYMBOL(cl->proto, b);

    /* ── Channel hot path: send/recv MUST be inlined (blocking IO) ── */
    if (xr_value_is_channel(receiver)) {
        XrChannel *ch = xr_value_to_channel(receiver);

        if (nargs == 1 && method_symbol == SYMBOL_SEND) {
            XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
            XrCoroBlockResult _resumed = xr_coro_chan_send_resume(_cur, xr_slot_none());
            if (_resumed.kind == XR_CORO_BLOCK_READY) {
                R(a) = xr_null();
                vmbreak;
            }
            if (_resumed.kind == XR_CORO_BLOCK_CLOSED) {
                R(a) = xr_null();
                VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel is closed");
            }
            savepc();
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            XrCoroBlockResult _cr =
                xr_coro_chan_send(isolate, _cur, ch, R(a + 2), xr_slot_none(), -1);
            if (_cr.kind == XR_CORO_BLOCK_READY) {
                frame->call_status &= ~XR_CALL_YIELDED;
                R(a) = xr_null();
                vmbreak;
            } else if (_cr.kind == XR_CORO_BLOCK_CLOSED) {
                frame->call_status &= ~XR_CALL_YIELDED;
                VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel is closed");
            } else if (_cr.kind == XR_CORO_BLOCK_BLOCKED) {
                return XR_VM_BLOCKED;
            } else {
                frame->call_status &= ~XR_CALL_YIELDED;
            }
        }

        if (nargs == 0 && method_symbol == SYMBOL_RECV) {
            XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
            if (_cur) {
                XrCoroBlockResult _resumed = xr_coro_chan_recv_resume(
                    isolate, _cur, xr_slot_xvalue_ptr(&R(a)), xr_slot_none());
                if (_resumed.kind == XR_CORO_BLOCK_READY) {
                    vmbreak;
                }
                if (_resumed.kind == XR_CORO_BLOCK_TIMEOUT) {
                    R(a) = xr_null();
                    vmbreak;
                }
            }
            savepc();
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            XrCoroBlockResult _cr =
                xr_coro_chan_recv(isolate, _cur, ch, xr_slot_xvalue_ptr(&R(a)), xr_slot_none(), -1);
            if (_cr.kind == XR_CORO_BLOCK_READY || _cr.kind == XR_CORO_BLOCK_CLOSED) {
                frame->call_status &= ~XR_CALL_YIELDED;
                vmbreak;
            } else if (_cr.kind == XR_CORO_BLOCK_BLOCKED) {
                return XR_VM_BLOCKED;
            } else {
                frame->call_status &= ~XR_CALL_YIELDED;
            }
        }

        /* Dispatch: other channel methods (tryRecv, close, etc.) */
        XrDispatchAction _cr =
            vm_invoke_channel(isolate, vm_ctx, ch, method_symbol, nargs, base, a, frame, pc);
        if (_cr == XR_DISP_NEXT)
            vmbreak;
        if (_cr == XR_DISP_BLOCKED)
            return XR_VM_BLOCKED;
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    /* ── Task handle methods (can block/yield) ── */
    if (xr_value_is_task(receiver)) {
        XrDispatchAction _cr =
            vm_invoke_task_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == XR_DISP_NEXT)
            vmbreak;
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    /* ── Coroutine handle methods ── */
    if (xr_value_is_coro(receiver)) {
        XrDispatchAction _cr =
            vm_invoke_coro_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == XR_DISP_NEXT)
            vmbreak;
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    /* ── Module export dispatch (different semantics: export lookup) ── */
    if (xr_value_is_module(receiver)) {
        XrDispatchAction _cr =
            vm_invoke_module(isolate, vm_ctx, receiver, method_symbol, nargs, base, a, ci, pc);
        if (_cr == XR_DISP_NEXT)
            vmbreak;
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (_cr == XR_DISP_BLOCKED)
            return XR_VM_BLOCKED;
        if (_cr == XR_DISP_YIELD)
            return XR_VM_YIELD;
        if (_cr == XR_DISP_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == XR_DISP_RAISE) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
    }

    /* ── Enum type: variant access or ADT construction ──
     * Handles Result.Ok(42) when lowered as OP_INVOKE with receiver=EnumType. */
    if (XR_IS_ENUM_TYPE(receiver)) {
        XrEnumType *etype = XR_TO_ENUM_TYPE(receiver);
        XrEnumValue *eval = xr_enum_get_member_by_symbol(etype, method_symbol);
        if (eval) {
            if (etype->is_adt && etype->payload_counts &&
                etype->payload_counts[eval->member_index] > 0 && nargs > 0) {
                XrInstance *inst =
                    xr_enum_adt_construct(isolate, etype, eval->member_index, &R(a + 2), nargs);
                if (!inst) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "failed to construct ADT variant '%s.%s'",
                                     etype->name, eval->member_name);
                }
                R(a) = XR_FROM_PTR(inst);
            } else {
                /* Simple enum member access or zero-payload ADT variant */
                R(a) = XR_FROM_PTR(eval);
            }
            vmbreak;
        }
        /* Fall through to class-based dispatch for enum methods (toString etc.) */
    }

    /* ── Class constructor / static method (creates instance) ── */
    if (xr_value_is_class(receiver)) {
        xr_vm_ctx_ensure_ic_methods(vm_ctx, frame->closure->proto);
        XrDispatchAction _cr = vm_invoke_class(isolate, vm_ctx, receiver, method_symbol, nargs,
                                               base, a, ci, pc, invoke_is_tail);
        if (_cr == XR_DISP_NEXT)
            vmbreak;
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }

    /* ── isEmpty() micro-inline (avoids function call for hot check) ── */
    if (nargs == 0 && method_symbol == SYMBOL_IS_EMPTY) {
        if (XR_IS_ARRAY(receiver)) {
            R(a) = xr_bool(XR_TO_ARRAY(receiver)->length == 0);
            vmbreak;
        } else if (XR_IS_STRING(receiver)) {
            R(a) = xr_bool(xr_value_str_len(&receiver) == 0);
            vmbreak;
        } else if (XR_IS_MAP(receiver)) {
            R(a) = xr_bool(XR_TO_MAP(receiver)->count == 0);
            vmbreak;
        } else if (XR_IS_SET(receiver)) {
            R(a) = xr_bool(XR_TO_SET(receiver)->count == 0);
            vmbreak;
        }
    }

    /* ══════════════════════════════════════════════════════════════════
     * UNIFIED CLASS-BASED DISPATCH
     *
     * All remaining types: value types (int/float/bool), collections
     * (string/array/map/set), native types (json/bigint/datetime/regex/
     * iterator/range/stringbuilder), instances, struct refs, and enum
     * values/types. Single code path with polymorphic inline cache.
     * ══════════════════════════════════════════════════════════════════ */
    {
        XrClass *klass = invoke_resolve_class(isolate, receiver);

        /* Null: only toString is valid */
        if (!klass) {
            if (XR_IS_NULL(receiver) && method_symbol == SYMBOL_TOSTRING) {
                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                vmbreak;
            }
            XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
            const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on unsupported type",
                             _mn ? _mn : "?");
        }

        /* Enum special routing (enum values/types use hardcoded methods
         * that access native body fields directly) */
        if (klass->builtin_kind == XR_BK_ENUM_VALUE || klass->builtin_kind == XR_BK_ENUM_TYPE) {
            XrDispatchAction _cr =
                vm_invoke_enum(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
            if (_cr == XR_DISP_NEXT)
                vmbreak;
            if (_cr == XR_DISP_RAISE) {
                if (!xr_vm_is_catch_reachable(isolate))
                    return XR_VM_RUNTIME_ERROR;
                goto startfunc;
            }
            /* XR_DISP_FALLTHROUGH: enum has no match, fall through to class lookup */
        }

        /* ADT enum instance methods (Result.isOk, unwrapOr, map, etc.) */
        if (klass->builtin_kind == XR_BK_ADT_ENUM) {
            XrDispatchAction _cr =
                vm_invoke_adt_instance(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
            if (_cr == XR_DISP_NEXT)
                vmbreak;
            if (_cr == XR_DISP_RAISE) {
                if (!xr_vm_is_catch_reachable(isolate))
                    return XR_VM_RUNTIME_ERROR;
                goto startfunc;
            }
        }

        /* IC lookup — unified for all types */
        XrICMethodTable *ic_table = xr_vm_ctx_ensure_ic_methods(vm_ctx, frame->closure->proto);
        if (!ic_table) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_INVOKE: IC table allocation failed");
        }
        size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
        XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
        XrICMethod *cache = xr_ic_method_table_get(ic_table, cache_index);
        if (cache) {
            XR_VM_IC_METHOD_BIND(cache, (int) cache_index);
        }

        XrMethod *method = cache ? xr_ic_method_lookup(cache, klass, method_symbol)
                                 : xr_class_lookup_method(klass, method_symbol);

        /* Primitive method (C function): all native type methods land here */
        if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
            R(a) = method->as.primitive(isolate, receiver, &R(a + 2), nargs);
            VM_BUILTIN_INVOKE_CHECK_EXC();
            vmbreak;
        }

        /* Closure method: user-defined instance methods */
        if (method && method->type == XMETHOD_CLOSURE && method->as.closure) {
            XrClosure *closure = method->as.closure;
            XrProto *proto = closure->proto;

            if (nargs + 1 != proto->numparams) {
                XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
                const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT,
                                 "method '%s' expects %d arguments but got %d", _mn ? _mn : "?",
                                 proto->numparams - 1, nargs);
            }

            if (invoke_is_tail) {
                /* Tail call reuses ci's frame slot but the callee may
                 * have a larger maxstacksize than the caller. Ensure
                 * the stack covers ci->base_offset + maxstacksize so
                 * register writes don't bleed into frames[]. */
                VM_STACK_CHECK(proto->maxstacksize);
                memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
                ci->closure = closure;
                ci->pc = PROTO_CODE_BASE(proto);
                VM_SET_STACK_TOP(base + proto->maxstacksize);
                goto startfunc;
            }

            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            /* Ensure stack/frames capacity for the callee's register file
             * before pushing the frame; without this, when stack and frames
             * share a combined slab allocation the callee's writes can
             * silently overflow into the frames array. */
            VM_STACK_CHECK(a + 1 + proto->maxstacksize);

            int _fidx = VM_FRAME_COUNT;
            VM_INC_FRAME_COUNT;
            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
            goto startfunc;
        }

        /* toString fallback: works for any type even without a registered method */
        if (method_symbol == SYMBOL_TOSTRING) {
            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
            vmbreak;
        }

        /* Method not found */
        XrSymbolTable *_st = (XrSymbolTable *) isolate->symbol_table;
        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' not found on type '%s'",
                         _mn ? _mn : "?", xr_typeid_name(xr_value_typeid(receiver)));
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
    XrDispatchAction _cr = vm_superinvoke(isolate, vm_ctx, i, base, ci, pc);
    /* XR_DISP_NEXT — primitive parent ctor / method returned a value into
     * R[A] without pushing a frame, so we fall through to the next bytecode
     * just like OP_INVOKE's primitive path.  Without this, primitive
     * super-method calls (e.g. Exception subclass super(msg)) crash with
     * "no catch reachable" because the dispatcher misreads the result. */
    if (_cr == XR_DISP_NEXT)
        vmbreak;
    if (_cr == XR_DISP_RESTART)
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
    XrClass *cls = NULL;
    if (XR_IS_STRUCT_REF(receiver)) {
        // Stack-allocated struct: class ptr at head
        cls = *(XrClass **) xr_to_struct_ptr(receiver);
    } else {
        if (XR_UNLIKELY(!XR_IS_INSTANCE(receiver))) {
            XrProto *cur_proto = cl ? cl->proto : NULL;
            const char *fn = (cur_proto && cur_proto->name) ? cur_proto->name->data : "<anonymous>";
            int pc_idx = (cur_proto && pc) ? (int) (pc - 1 - PROTO_CODE_BASE(cur_proto)) : -1;
            int line = (cur_proto && pc_idx >= 0 && pc_idx < (int) PROTO_LINE_COUNT(cur_proto))
                           ? PROTO_LINE(cur_proto, pc_idx)
                           : 0;
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                             "direct invoke receiver is not an instance in %s at pc %d line %d", fn,
                             pc_idx, line);
        }
        XrInstance *inst_obj = xr_value_to_instance(receiver);
        cls = xr_instance_get_class(inst_obj);
    }
    if (XR_UNLIKELY(cls == NULL || method_idx < 0 || method_idx >= cls->method_count)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "direct invoke method index out of bounds");
    }
    XrMethod *method = &cls->methods[method_idx];

    /* Primitive method: call C function directly and continue.
     * OP_INVOKE_DIRECT currently targets closure methods only, but if a
     * future optimisation indexes into a primitive slot (e.g. inherited
     * native method), accessing .as.closure would read the wrong union
     * member and crash.  Guard defensively. */
    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
        R(a) = method->as.primitive(isolate, R(a + 1), &R(a + 2), nargs);
        VM_BUILTIN_INVOKE_CHECK_EXC();
        vmbreak;
    }

    XrClosure *closure = method->as.closure;
    if (XR_UNLIKELY(closure == NULL || closure->proto == NULL)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "direct invoke target is not a closure");
    }

    if (is_tail_direct) {
        // Tail call: reuse current stack frame; callee maxstack may be larger.
        VM_STACK_CHECK(closure->proto->maxstacksize);
        memmove(base, &R(a + 1), sizeof(XrValue) * (nargs + 1));
        ci->closure = closure;
        ci->pc = PROTO_CODE_BASE(closure->proto);
        VM_SET_STACK_TOP(base + closure->proto->maxstacksize);
        goto startfunc;
    }

    /* Stack/frames capacity check before pushing a new frame; without
     * this the callee's register file can overflow into frames[] when
     * the coroutine still uses the combined slab layout. */
    VM_STACK_CHECK(a + 1 + closure->proto->maxstacksize);

    savepc();
    int _fidx = VM_FRAME_COUNT;
    VM_INC_FRAME_COUNT;
    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(closure->proto);
    new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);  // this at R[a+1]
    goto startfunc;
}
