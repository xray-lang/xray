/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_call.inc.c — function call / return / tail-call dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, ci, frame, base, k, R, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_DISPATCH, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT, VM_DEC_FRAME_COUNT, VM_STACK, VM_STACK_TOP,
 * VM_STACK_CHECK, VM_BARRIER_*, TRACE_EXECUTION, checkGC,
 * startfunc / handle_closure_pending labels, ...) provided by
 * the surrounding scope. CMake excludes *.inc.c from the
 * VM_SRC glob.
 *
 * Owns:
 *   - OP_CALL          : unified closure / cfunc / class / bound-method call
 *   - OP_CALL_KEEP     : preserves caller register window
 *   - OP_CALL_STATIC   : direct closure call, no closure-resolve
 *   - OP_LOOP_BACK     : back-edge to entry of same closure
 *   - OP_CALLSELF      : recursive self-call shortcut
 *   - OP_TAILCALL      : tail call rewriting current frame
 *   - OP_RETURN / RETURN0 / RETURN1
 *
 * Companion OOP / IC / property / invoke opcodes follow these in
 * the dispatch order; they live in their own includes.
 */

/* ========================================================
** Function Call Instructions
** ======================================================== */

vmcase(OP_CALL) {
op_call_entry:;
    /* Unified callable object handling
    ** Principle: no backward compatibility, use best design
    ** Strategy: distinguish two most common types for performance
    */
    TRACE_EXECUTION();

    /* GC safe point: function call boundary is ideal for GC.
    ** Stack is consistent, all locals are valid.
    ** Reductions check intentionally absent here: only OP_JMP backward
    ** jumps check reductions. This reduces overhead from <3% to <1%.
    ** Preemption for pure call chains relies on sysmon + handoff. */
    VM_GC_SAFEPOINT();

    int a = GETARG_A(i);
    int nargs = GETARG_B(i);

    XrValue func_val = R(a);

    // Fast dispatch: skip slow paths for most common call types
    if (XR_LIKELY(XR_IS_FUNCTION(func_val)))
        goto op_call_closure;
    if (XR_IS_CFUNCTION(func_val))
        goto op_call_cfunc;
    // Enum conversion: Status(200) -> Status.Success
    // Check if this is a call on enum type object
    if (XR_IS_PTR(func_val)) {
        XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(func_val);

        // Check enum type via class flag
        if (XR_IS_ENUM_TYPE(func_val) && nargs == 1) {
            // Enum conversion: Status(200)
            XrValue value = R(a + 1);  // First argument
            XrEnumValue *result = xr_enum_from_value((XrEnumType *) gc, value);

            if (result) {
                R(a) = XR_FROM_PTR(result);
                vmbreak;  // Successfully converted as enum
            } else {
                // Conversion failed, return Null
                R(a) = xr_null();
                vmbreak;
            }
        }

        // ADT enum variant construction: Result.Ok(42)
        // XrEnumValue with ADT parent → construct instance with tag + payload
        if (XR_IS_ENUM_VALUE(func_val)) {
            XrEnumValue *eval = (XrEnumValue *) gc;
            XrEnumType *etype = eval->parent_type;
            if (etype && etype->is_adt && etype->payload_counts &&
                etype->payload_counts[eval->member_index] > 0) {
                XrInstance *inst =
                    xr_enum_adt_construct(isolate, etype, eval->member_index, &R(a + 1), nargs);
                if (!inst) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "failed to construct ADT variant '%s.%s'",
                                     etype->name, eval->member_name);
                }
                R(a) = XR_FROM_PTR(inst);
                vmbreak;
            }
        }

        // Class call: Array() or User() -> invoke constructor
        if (XR_GC_GET_TYPE(gc) == XR_TCLASS) {
            XrClass *klass = (XrClass *) gc;
            XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
            XrMethod *constructor = NULL;

            // First look for "call" static method (native class)
            int call_symbol = xr_symbol_lookup_in_table(sym_table, "call");
            if (call_symbol >= 0) {
                constructor = xr_class_lookup_method(klass, call_symbol);
            }

            if (constructor && constructor->type == XMETHOD_PRIMITIVE) {
                // Native class constructor
                XrPrimitiveMethodFn func = constructor->as.primitive;
                XrValue result = func(isolate, R(a), &R(a + 1), nargs);
                R(a) = result;
                vmbreak;
            }

            // Look for constructor method (user defined class)
            int ctor_symbol = xr_symbol_lookup_in_table(sym_table, XR_KEYWORD_CONSTRUCTOR);
            if (ctor_symbol >= 0) {
                constructor = xr_class_lookup_method(klass, ctor_symbol);
            }

            // Create instance (allocation based on storage mode context)
            XrInstance *instance;
            uint8_t storage_mode = isolate->current_storage_mode;
            isolate->current_storage_mode = 0;  // Reset context

            if (storage_mode != 0 && isolate->sys_heap) {
                // shared: allocate on system heap
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
                // normal: allocate on coroutine heap
                instance = xr_instance_new(isolate, klass);
            }

            if (!instance) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "failed to create instance: '%s'",
                                 xr_class_display_name(klass));
            }
            XrValue inst_val = XR_FROM_PTR(instance);

            if (constructor && constructor->type == XMETHOD_CLOSURE) {
                // User class constructor: create instance and call constructor
                XrClosure *closure = constructor->as.closure;
                XrProto *proto = closure->proto;

                // Check stack space
                if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                    VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                }

                VM_STACK_CHECK(a + 1 + nargs + 1);

                // Save current frame's pc
                savepc();

                /* Shift user args right by 1: R[a+1..a+nargs] -> R[a+2..a+nargs+1]
                 * This makes room for 'this' at R[a+1], matching standard call convention
                 * where base_offset = base+a+1, so return goes to R[a] */
                for (int j = nargs; j > 0; j--) {
                    R(a + 1 + j) = R(a + j);
                }
                R(a + 1) = inst_val;

                // Create new call frame (standard convention)
                int _fidx = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(proto);
                new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);

                // Jump to constructor
                goto startfunc;
            }

            // No constructor, return instance directly
            R(a) = inst_val;
            vmbreak;
        }
    }

    // Bound method call - must be before closure check!
    if (xr_value_is_bound_method(func_val)) {
        XrBoundMethod *bm = xr_value_to_bound_method(func_val);
        XrValue result = bm->handler(isolate, bm->receiver, &R(a + 1), nargs);
        if (XR_UNLIKELY(XR_IS_NOTFOUND(result))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bound method call failed: method not found");
        }
        R(a) = result;
        vmbreak;
    }

// C function call
op_call_cfunc:
    if (xr_value_is_cfunction(func_val)) {
        XrCFunction *cfunc = xr_value_to_cfunction(func_val);

        // Track current C function for sysmon auto-upgrade
        {
            XrWorker *_w = xr_current_worker();
            if (_w && _w->m)
                _w->m->current_cfunc = cfunc;
        }

        /* SLOW C functions release P before execution.
        ** This prevents long-running C code from blocking the
        ** scheduler. Other coroutines continue on other M's. */
        bool is_slow =
            (atomic_load_explicit(&cfunc->cfunc_class, memory_order_acquire) == XR_CFUNC_SLOW);
        if (is_slow) {
            xr_worker_entersyscall();
        }

        if (cfunc->is_yieldable) {
            // Yieldable C function (GC safe zone)
            // Set to current frame before call
            ci->u.c.result_slot = (int16_t) (GETARG_A(i));
            ci->u.c.has_cfunc_result = false;

            XrValue result;
            XrCFuncResult status = cfunc->as.yieldable(isolate, &R(a + 1), nargs, &result);

            if (is_slow) {
                xr_worker_exitsyscall();
            }

            switch (status) {
                case XR_CFUNC_DONE:
                    R(a) = result;
                    vmbreak;

                case XR_CFUNC_BLOCKED:
                    // Coroutine needs to block, save state and yield
                    savepc();
                    XR_DBG_CORO("VM BLOCKED: result_slot=%d, frame_idx=%d", (int) (GETARG_A(i)),
                                VM_FRAME_COUNT - 1);
                    return XR_VM_BLOCKED;

                case XR_CFUNC_YIELD:
                    // Active yield, continue next time
                    savepc();
                    return XR_VM_YIELD;

                case XR_CFUNC_CALL_CLOSURE:
                    /* Closure frame pushed by xr_yield_call_closure,
                     * execute it via normal VM path */
                    goto startfunc;

                case XR_CFUNC_ERROR:
                    return XR_VM_RUNTIME_ERROR;
            }
        } else {
            // Normal C function
            XrValue result = cfunc->as.func(isolate, &R(a + 1), nargs);

            if (is_slow) {
                xr_worker_exitsyscall();
            }

            R(a) = result;
            vmbreak;
        }
    }

// Xray closure call (most common)
op_call_closure:
    if (XR_IS_FUNCTION(func_val)) {
        XrClosure *closure = xr_value_to_closure(func_val);
        XrProto *proto = closure->proto;

        // Argument count check
        if (proto->is_vararg) {
            // vararg function: check minimum required args
            if (XR_UNLIKELY(nargs < proto->min_params)) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected at least %d arguments, got %d",
                                 proto->min_params, nargs);
            }

            if (XR_UNLIKELY(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                 "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
            }

            // Stack boundary check: ensure new frame has enough stack space
            VM_STACK_CHECK(a + 1 + proto->maxstacksize);

            // Fill missing optional fixed params with null
            for (int j = nargs; j < proto->numparams; j++) {
                R(a + 1 + j) = xr_null();
            }

            // Collect extra arguments into rest array
            int extra_args = nargs > proto->numparams ? nargs - proto->numparams : 0;
            XrArray *rest_array = xr_array_new(VM_CURRENT_CORO);
            if (extra_args > 0) {
                XrValue *arg_base = &R(a + 1 + proto->numparams);
                for (int j = 0; j < extra_args; j++) {
                    xr_array_push(rest_array, arg_base[j]);
                }
            }

            // Rest parameter at numparams position (after all fixed params)
            R(a + 1 + proto->numparams) = xr_value_from_array(rest_array);

            savepc();

            int _fidx = VM_FRAME_COUNT;
            VM_INC_FRAME_COUNT;
            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);

            goto startfunc;
        } else if (XR_LIKELY(nargs >= proto->min_params && nargs <= proto->numparams)) {
            // Non vararg: argument count in valid range (supports default params)

            // Type feedback: collect argument types for profile-guided compilation
            if (proto->type_feedback) {
                XmTypeFeedback *fb = proto->type_feedback;
                for (int fi = 0; fi < nargs && fi < XFB_MAX_PARAMS; fi++)
                    xfb_record_arg(fb, fi, R(a + 1 + fi));
            } else if (atomic_load_explicit(&proto->call_count, memory_order_relaxed) >=
                           XFB_ALLOC_THRESHOLD &&
                       !proto->param_types) {
                proto->type_feedback = xfb_create();
            }

#ifndef XRAY_HAS_JIT
            /* AOT fast path: call pre-compiled native code directly.
            ** Used by --native build mode: AOT thunks are registered
            ** into proto->jit_entry before execution begins.
            ** Calling convention: int64_t fn(intptr_t coro, int64_t *raw_args) */
            if (proto->jit_entry) {
                typedef int64_t (*AotThunkFn)(intptr_t, int64_t *);
                XrCoroutine *_aot_coro = VM_CURRENT_CORO;
                int64_t raw_args[16];  // 16 slots: tagged params use 2 each
                int ai = 0;
                for (int ri = 0; ri < nargs && ri < 8 && ai < 16; ri++) {
                    uint8_t gc = 0;
                    if (proto->param_types && ri < proto->param_types_count &&
                        proto->param_types[ri])
                        gc = xr_type_to_slot_type(proto->param_types[ri]);
                    bool is_f = XR_SLOT_IS_FLOAT(gc);
                    bool is_tagged = (gc == XR_SLOT_PTR || gc == XR_SLOT_ANY);
                    if (is_f) {
                        memcpy(&raw_args[ai], &R(a + 1 + ri).f, sizeof(double));
                        ai++;
                    } else if (is_tagged) {
                        // Pack full XrValue (16 bytes = 2 slots)
                        memcpy(&raw_args[ai], &R(a + 1 + ri), sizeof(XrValue));
                        ai += 2;
                    } else {
                        raw_args[ai] = R(a + 1 + ri).i;
                        ai++;
                    }
                }
                if (_aot_coro)
                    _aot_coro->jit_ctx->call_closure = closure;
                int64_t ret = ((AotThunkFn) proto->jit_entry)((intptr_t) _aot_coro, raw_args);
                uint8_t rtype = proto->return_type_info
                                    ? xr_type_to_slot_type(proto->return_type_info)
                                    : XR_SLOT_ANY;
                if (XR_SLOT_IS_FLOAT(rtype)) {
                    memcpy(&R(a).f, &ret, sizeof(double));
                    R(a).tag = XR_TAG_F64;
                } else if (rtype == XR_SLOT_BOOL) {
                    R(a).i = ret ? 1 : 0;
                    R(a).tag = XR_TAG_BOOL;
                } else {
                    R(a).i = ret;
                    R(a).tag = XR_TAG_I64;
                }
                R(a).heap_type = 0;
                vmbreak;
            }
#endif

#ifdef XRAY_HAS_JIT
            // Install pending background JIT compilation
            if (!proto->jit_entry) {
                void *pending =
                    atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire);
                if (pending && (uintptr_t) pending > 1) {
                    xm_jit_install_bg_result(proto);
                    // Promote feedback to param_types so entry guards
                    // match the compiled code's type specialization.
                    if (proto->numparams > 0 && proto->type_feedback &&
                        proto->type_feedback->stable) {
                        if (!proto->param_types) {
                            proto->param_types = (struct XrType **) xr_calloc(
                                proto->numparams, sizeof(struct XrType *));
                            if (proto->param_types)
                                proto->param_types_count = proto->numparams;
                        }
                        if (proto->param_types) {
                            for (int pi = 0; pi < proto->numparams && pi < 8; pi++) {
                                if (pi < proto->param_types_count && !proto->param_types[pi] &&
                                    pi < XFB_MAX_PARAMS &&
                                    xfb_is_monomorphic(proto->type_feedback->arg_types[pi])) {
                                    uint8_t st =
                                        xfb_to_slot_type(proto->type_feedback->arg_types[pi]);
                                    proto->param_types[pi] = xr_slot_type_to_type(NULL, st);
                                }
                            }
                        }
                        if (!proto->return_type_info) {
                            uint8_t fb_ret = xfb_to_slot_type(proto->type_feedback->return_type);
                            if (fb_ret != XR_SLOT_ANY)
                                proto->return_type_info = xr_slot_type_to_type(NULL, fb_ret);
                        }
                    }
                }
            }
            // JIT fast path: call compiled code directly
            if (proto->jit_entry) {
                XrValue jit_result;
                XrCoroutine *_jit_coro = (XrCoroutine *) vm_ctx->current_coro;
                _jit_coro->jit_ctx->call_proto = proto;
                _jit_coro->jit_ctx->call_closure = closure;
                _jit_coro->jit_ctx->call_base_offset = (int32_t) ((base + a + 1) - VM_STACK);
                int _jrc1 = xm_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                        proto->return_type_info, &jit_result);
                if (_jrc1 == XM_JIT_OK) {
                    R(a) = jit_result;
                    // Multi-return: fill R[a+1..] from jit_ctx->ret_vals[]
                    if (_jit_coro->jit_ctx->ret_count > 1)
                        xm_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                    vmbreak;
                }
                if (_jrc1 == XM_JIT_SUSPEND) {
                    if (proto->nosr > 0)
                        proto->osr_pending = true;
                    savepc();
                    return XR_VM_BLOCKED;
                }
                // JIT exception: skip deopt recovery, let VM handle
                if (!XR_IS_NULL(VM_EXCEPTION)) {
                    if (!xr_vm_is_catch_reachable(isolate))
                        return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }
                /* Deopt: try mid-function recovery first.
                 * Restores VM slots from JIT register state and resumes
                 * the interpreter at the deopt bytecode PC. */
                proto->deopt_count++;
                if (proto->deopt_count <= 3) {
                    XrCoroutine *_dc = (XrCoroutine *) vm_ctx->current_coro;
                    int32_t recover_pc = xm_jit_deopt_recover(_dc, &R(a + 1), proto->maxstacksize);
                    if (recover_pc >= 0) {
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                             "stack overflow: recursion exceeds %d levels",
                                             XR_FRAMES_MAX);
                        }
                        VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                        savepc();
                        int _fidx = VM_FRAME_COUNT;
                        VM_INC_FRAME_COUNT;
                        XrBcCallFrame *_nf = &VM_FRAMES[_fidx];
                        _nf->closure = closure;
                        _nf->pc = PROTO_CODE_BASE(proto) + recover_pc;
                        _nf->base_offset = (int) ((base + a + 1) - VM_STACK);
                        if (proto->nosr > 0)
                            proto->osr_pending = true;
                        goto startfunc;
                    }
                }
                // Full deopt: invalidate JIT entry after threshold
                if (proto->deopt_count > 3) {
                    proto->jit_entry = NULL;
                }
            }

            // Hot function detection: try JIT compilation
            if (isolate->vm.jit && !proto->jit_entry &&
                atomic_fetch_add_explicit(&proto->call_count, 1, memory_order_relaxed) + 1 ==
                    (uint32_t) isolate->vm.jit_threshold) {
                xm_jit_try_compile(isolate->vm.jit, proto);
                if (proto->jit_entry) {
                    XrValue jit_result;
                    XrCoroutine *_jit_coro = (XrCoroutine *) vm_ctx->current_coro;
                    _jit_coro->jit_ctx->call_proto = proto;
                    _jit_coro->jit_ctx->call_closure = closure;
                    _jit_coro->jit_ctx->call_base_offset = (int32_t) ((base + a + 1) - VM_STACK);
                    int _jrc2 = xm_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                            proto->return_type_info, &jit_result);
                    if (_jrc2 == XM_JIT_OK) {
                        R(a) = jit_result;
                        if (_jit_coro->jit_ctx->ret_count > 1)
                            xm_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                        vmbreak;
                    }
                    if (_jrc2 == XM_JIT_SUSPEND) {
                        if (proto->nosr > 0)
                            proto->osr_pending = true;
                        savepc();
                        return XR_VM_BLOCKED;
                    }
                    // JIT exception: skip deopt recovery, let VM handle
                    if (!XR_IS_NULL(VM_EXCEPTION)) {
                        if (!xr_vm_is_catch_reachable(isolate))
                            return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // Deopt: try mid-function recovery
                    proto->deopt_count++;
                    if (proto->deopt_count <= 3) {
                        XrCoroutine *_dc2 = (XrCoroutine *) vm_ctx->current_coro;
                        int32_t rpc2 = xm_jit_deopt_recover(_dc2, &R(a + 1), proto->maxstacksize);
                        if (rpc2 >= 0) {
                            if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                                 "stack overflow: recursion exceeds %d levels",
                                                 XR_FRAMES_MAX);
                            }
                            VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                            savepc();
                            int _fi2 = VM_FRAME_COUNT;
                            VM_INC_FRAME_COUNT;
                            XrBcCallFrame *_nf2 = &VM_FRAMES[_fi2];
                            _nf2->closure = closure;
                            _nf2->pc = PROTO_CODE_BASE(proto) + rpc2;
                            _nf2->base_offset = (int) ((base + a + 1) - VM_STACK);
                            if (proto->nosr > 0)
                                proto->osr_pending = true;
                            goto startfunc;
                        }
                    }
                }
            }
#endif

            if (XR_UNLIKELY(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                 "stack overflow: recursion exceeds %d levels", XR_FRAMES_MAX);
            }

            // Stack boundary check: ensure new frame has enough stack space
            VM_STACK_CHECK(a + 1 + proto->maxstacksize);

            /* Fill missing optional arguments with null.
            ** NORMAL functions: nargs == numparams always (checked above),
            ** skip the loop entirely via entry_type fast path. */
            if (proto->entry_type != XR_ENTRY_NORMAL) {
                for (int j = nargs; j < proto->numparams; j++) {
                    R(a + 1 + j) = xr_null();
                }
            }

            // Key: save current frame's pc for return continuation
            savepc();

            // Create new call frame (zero overhead)
            int _fidx = VM_FRAME_COUNT;
            VM_INC_FRAME_COUNT;
            XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);

            // Jump directly to new function
            goto startfunc;
        } else {
            if (proto->min_params == proto->numparams) {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                                 proto->numparams, nargs);
            } else {
                VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d-%d arguments, got %d",
                                 proto->min_params, proto->numparams, nargs);
            }
        }
    }

    // Error: non-callable value (catchable by try/catch)
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
}

vmcase(OP_CALL_KEEP) {
    /* OP_CALL_KEEP A B C: call R[A] with B args, result to R[C], R[A] preserved
    ** Used by higher-order function inline (map/filter/reduce/forEach)
    ** to avoid callback register being overwritten by return value
    */
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);
    int result_reg = GETARG_C(i);
    XrValue func_val = R(a);

    if (XR_UNLIKELY(!XR_IS_FUNCTION(func_val))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
    }

    XrClosure *closure = xr_value_to_closure(func_val);
    XrProto *proto = closure->proto;

    // Argument count: silently truncate extra args
    int effective_nargs = nargs;
    if (!proto->is_vararg && nargs > proto->numparams) {
        effective_nargs = proto->numparams;
    }

    if (XR_UNLIKELY(effective_nargs < proto->min_params)) {
        VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d", proto->min_params,
                         effective_nargs);
    }

    if (XR_UNLIKELY(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow: recursion exceeds %d levels",
                         XR_FRAMES_MAX);
    }

    VM_STACK_CHECK(a + 1 + proto->maxstacksize);

    // Fill missing optional arguments with null
    for (int j = effective_nargs; j < proto->numparams; j++) {
        R(a + 1 + j) = xr_null();
    }

    savepc();

    int _fidx = VM_FRAME_COUNT;
    VM_INC_FRAME_COUNT;
    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
    new_frame->result_offset = (int) ((base + result_reg) - VM_STACK);
    new_frame->call_status = XR_CALL_KEEP_FUNC;

    goto startfunc;
}

vmcase(OP_CALL_STATIC) {
    /* OP_CALL_STATIC A B: call R[A] with B args, result to R[A]
    ** Emitted when codegen knows R[A] is a plain closure (no class call,
    ** no C function, no enum). Falls through to OP_CALL which handles
    ** the fast-path XR_IS_FUNCTION branch first.
    */
    goto op_call_entry;
}

vmcase(OP_LOOP_BACK) {
    /* Tail recursion → loop: single instruction replaces
     * CLOSE + N×MOVE + backward JMP.
     * A=func_reg, B=nargs, C=skip (0=regular/static, 1=instance method).
     * R[skip..skip+B-1] = R[A+1..A+B]; PC = function entry.
     * When C=1, R[0]=this is preserved.
     */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);
    int skip = GETARG_C(i);

    // Copy args to param positions (skip=1 preserves R[0]=this)
    if (nargs > 0) {
        memmove(base + skip, &R(a + 1), sizeof(XrValue) * nargs);
    }

    // Reset PC to function entry
    pc = PROTO_CODE_BASE(frame->closure->proto);

    // GC safe point + reduction check (same as backward JMP)
    if (vm_ctx && vm_ctx->current_coro) {
        XrCoroutine *coro = (XrCoroutine *) vm_ctx->current_coro;
        VM_GC_SAFEPOINT();
        if (--coro->reductions <= 0) {
            if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                return XR_VM_CANCELLED;
            }
            coro->reductions = XR_CORO_REDUCTIONS;
            frame->pc = pc;
            return XR_VM_YIELD;
        }
    }

    vmbreak;
}

vmcase(OP_CALLSELF) {
    /* Optimization: recursive self-call without GETGLOBAL
     * Supports both normal recursive calls and tail recursive calls
     */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);
    int nresults = GETARG_C(i);  // Check return count, 0=tail call

    // Called function is current function
    XrClosure *closure = frame->closure;

    // Argument count check
    if (XR_UNLIKELY(nargs != closure->proto->numparams)) {
        VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                         closure->proto->numparams, nargs);
    }

#ifdef XRAY_HAS_JIT
    {
        XrProto *proto = closure->proto;
        // JIT fast path: call compiled code directly
        if (proto->jit_entry) {
            XrValue jit_result;
            XrCoroutine *_jit_coro = (XrCoroutine *) vm_ctx->current_coro;
            _jit_coro->jit_ctx->call_proto = proto;
            _jit_coro->jit_ctx->call_closure = closure;
            _jit_coro->jit_ctx->call_base_offset = (int32_t) ((base + a + 1) - VM_STACK);
            int _jrc3 = xm_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                    proto->return_type_info, &jit_result);
            if (_jrc3 == XM_JIT_OK) {
                R(a) = jit_result;
                if (_jit_coro->jit_ctx->ret_count > 1)
                    xm_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                vmbreak;
            }
            if (_jrc3 == XM_JIT_SUSPEND) {
                if (proto->nosr > 0)
                    proto->osr_pending = true;
                savepc();
                return XR_VM_BLOCKED;
            }
            // JIT exception: skip deopt recovery, let VM handle
            if (!XR_IS_NULL(VM_EXCEPTION)) {
                if (!xr_vm_is_catch_reachable(isolate))
                    return XR_VM_RUNTIME_ERROR;
                goto startfunc;
            }
            // Deopt: try mid-function recovery
            proto->deopt_count++;
            if (proto->deopt_count <= 3) {
                XrCoroutine *_dc = (XrCoroutine *) vm_ctx->current_coro;
                int32_t recover_pc = xm_jit_deopt_recover(_dc, &R(a + 1), proto->maxstacksize);
                if (recover_pc >= 0) {
                    if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                                         "stack overflow: recursion exceeds %d levels",
                                         XR_FRAMES_MAX);
                    }
                    VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                    savepc();
                    int _fidx = VM_FRAME_COUNT;
                    VM_INC_FRAME_COUNT;
                    XrBcCallFrame *_nf = &VM_FRAMES[_fidx];
                    _nf->closure = closure;
                    _nf->pc = PROTO_CODE_BASE(proto) + recover_pc;
                    _nf->base_offset = (int) ((base + a + 1) - VM_STACK);
                    if (proto->nosr > 0)
                        proto->osr_pending = true;
                    goto startfunc;
                }
            }
            if (proto->deopt_count > 3) {
                proto->jit_entry = NULL;
            }
        }

        // Hot function detection: try JIT compilation
        if (isolate->vm.jit && !proto->jit_entry &&
            atomic_fetch_add_explicit(&proto->call_count, 1, memory_order_relaxed) + 1 ==
                (uint32_t) isolate->vm.jit_threshold) {
            xm_jit_try_compile(isolate->vm.jit, proto);
            if (proto->jit_entry) {
                XrValue jit_result;
                XrCoroutine *_jit_coro = (XrCoroutine *) vm_ctx->current_coro;
                _jit_coro->jit_ctx->call_proto = proto;
                _jit_coro->jit_ctx->call_closure = closure;
                _jit_coro->jit_ctx->call_base_offset = (int32_t) ((base + a + 1) - VM_STACK);
                int _jrc4 = xm_jit_call(proto->jit_entry, _jit_coro, &R(a + 1), nargs,
                                        proto->return_type_info, &jit_result);
                if (_jrc4 == XM_JIT_OK) {
                    R(a) = jit_result;
                    if (_jit_coro->jit_ctx->ret_count > 1)
                        xm_jit_read_multi_ret(_jit_coro, &R(a), _jit_coro->jit_ctx->ret_count);
                    vmbreak;
                }
                if (_jrc4 == XM_JIT_SUSPEND) {
                    if (proto->nosr > 0)
                        proto->osr_pending = true;
                    savepc();
                    return XR_VM_BLOCKED;
                }
            }
        }
    }
#endif  // Check if tail call (C = 0)
    if (nresults == 0) {
        // Recursive tail call: reuse current stack frame, no depth increase

        // Move arguments to base position (overwrite current args)
        if (nargs > 0) {
            memmove(base, &R(a + 1), sizeof(XrValue) * nargs);
        }

        // Reset PC to function start
        ci->pc = PROTO_CODE_BASE(closure->proto);

        // Jump to function start
        goto startfunc;
    } else {
        // Recursive normal call: create new call frame
        if (XR_UNLIKELY(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
        }

        // Stack boundary check: ensure new frame has enough stack space
        VM_STACK_CHECK(a + 1 + closure->proto->maxstacksize);

        // Save current frame's pc
        savepc();

        // Create new call frame
        int _fidx = VM_FRAME_COUNT;
        VM_INC_FRAME_COUNT;
        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
        new_frame->closure = closure;  // Use same closure
        new_frame->pc = PROTO_CODE_BASE(closure->proto);
        new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);  // Args from R[a+1]

        // Jump directly to startfunc
        goto startfunc;
    }
}

vmcase(OP_TAILCALL) {
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);

    /* Tail call optimization
    ** Key: reuse current stack frame, no call depth increase
    ** This allows infinite tail recursion without stack overflow
    */

    // Get the called function
    XrValue func_val = R(a);

    // Type check (class calls have tail call disabled at compile time)
    if (!XR_IS_FUNCTION(func_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "attempt to call a non-function value");
    }

    XrClosure *new_closure = xr_value_to_closure(func_val);

    // Argument count check
    if (XR_UNLIKELY(nargs != new_closure->proto->numparams)) {
        VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "expected %d arguments, got %d",
                         new_closure->proto->numparams, nargs);
    }

    // Check stack space (multi-core mode uses worker private stack)
    XrValue *stack_base_check = VM_STACK;
    (void) stack_base_check;
    size_t current_stack_offset = frame->base_offset;
    size_t required_stack_size = current_stack_offset + new_closure->proto->maxstacksize;
    size_t stack_max = vm_ctx->stack_capacity;

    if (required_stack_size > stack_max) {
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow: need %zu, max %zu",
                         required_stack_size, stack_max);
    }

    /* Step 1: move arguments to base position
    ** Use memmove to handle potentially overlapping memory
    */
    if (nargs > 0) {
        memmove(base, &R(a + 1), sizeof(XrValue) * nargs);
    }

    /* Step 3: update ci's closure and PC
    ** Note: don't change frame_count, reuse current stack frame!
    */
    ci->closure = new_closure;
    ci->pc = PROTO_CODE_BASE(new_closure->proto);

    /* Step 4: update stack top pointer
    ** Should not accumulate! Keep at base + maxstacksize
    */
    VM_SET_STACK_TOP(base + new_closure->proto->maxstacksize);

    /* Step 5: jump directly to startfunc
    ** No call depth increase, true tail call optimization
    */
    goto startfunc;
}

vmcase(OP_RETURN) {
    // OP_RETURN: function return (multi-value support)

    /* GC safe point: function exit is ideal for GC
    ** Stack frame is about to be popped, good time to collect */
    VM_GC_SAFEPOINT();

    // Clean up exception handlers belonging to current frame
    while (VM_HANDLER_COUNT > 0 &&
           VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
        VM_DEC_HANDLER_COUNT;
    }

    int a = GETARG_A(i);
    int nret = GETARG_B(i);  // Return value count

    // Save return count for caller
    vm_ctx->last_nret = nret;

return_with_defer:;  // Label for RETURN0/RETURN1 fallback when defer exists
    /* Re-read a and nret: OP_RETURN0/OP_RETURN1 goto here
     * skipping the local variable initialization above */
    a = GETARG_A(i);
    nret = vm_ctx->last_nret;
    // Get first return value (for defer/toString compatibility)
    XrValue ret_result = (nret > 0) ? R(a) : xr_null();

    /* Execute current frame's defer (LIFO order)
     * Only execute defer belonging to current frame
     *
     * defer stack format: [closure, arg_count, arg1, arg2, ...]
     * Read from back: pop args, then arg count, then closure
     */
    if (vm_ctx->defer_count > 0 && vm_ctx->defer_frame_marks) {
        // Get current frame's defer start position
        int frame_defer_start = vm_ctx->defer_frame_marks[VM_FRAME_COUNT - 1];

        // Only execute defer registered by current frame
        if (vm_ctx->defer_count > frame_defer_start) {
            // Save current frame state
            ci->pc = pc;

            // Execute current frame's defer from stack top (LIFO)
            while (vm_ctx->defer_count > frame_defer_start) {
                // Temporary array for args (bounded)
                XrValue defer_args[XR_DEFER_ARGS_MAX];

                // Simplified implementation: iterate all entries and execute
                int pos = frame_defer_start;
                int entries[XR_DEFER_ENTRIES_MAX];  // Start position of each defer entry
                int entry_count = 0;
                int end = vm_ctx->defer_count;

                // Collect all defer entry positions (with bounds check)
                while (pos < end && entry_count < XR_DEFER_ENTRIES_MAX) {
                    entries[entry_count++] = pos;
                    // Skip: closure + arg count + args
                    int nargs = (int) XR_TO_INT(vm_ctx->defer_stack[pos + 1]);
                    pos += 2 + nargs;
                }

                // Error if defer entries exceed limit
                if (pos < end) {
                    VM_RUNTIME_ERROR(XR_ERR_OVERFLOW, "defer: too many entries (%d), max=%d",
                                     entry_count + (end - pos), XR_DEFER_ENTRIES_MAX);
                }

                // LIFO execution: from back to front
                for (int e = entry_count - 1; e >= 0; e--) {
                    int start = entries[e];
                    XrValue closure_val = vm_ctx->defer_stack[start];
                    int nargs = (int) XR_TO_INT(vm_ctx->defer_stack[start + 1]);

                    // Error if defer args exceed limit
                    if (nargs > XR_DEFER_ARGS_MAX) {
                        VM_RUNTIME_ERROR(XR_ERR_OVERFLOW, "defer: too many arguments (%d), max=%d",
                                         nargs, XR_DEFER_ARGS_MAX);
                    }

                    // Collect args
                    for (int j = 0; j < nargs; j++) {
                        defer_args[j] = vm_ctx->defer_stack[start + 2 + j];
                    }

                    // Execute
                    if (xr_value_is_closure(closure_val)) {
                        struct XrClosure *closure = xr_value_to_closure(closure_val);
                        xr_vm_call_closure(isolate, closure, defer_args, nargs);
                    }
                }

                // Clear current frame's defer
                vm_ctx->defer_count = frame_defer_start;
                break;
            }
        }
    }

    // Save toString print flags (before popping frame)
    uint8_t tostring_flags = ci->flags;
    ci->flags = 0;  // Clear flags

    /* Calculate return position and write multiple return values
    ** Key: when base_offset is 0, base_offset - 1 would overflow!
    ** This is top-level frame (main/module), no need to write return value
    */
    XrValue *return_slot = NULL;
    if (ci->base_offset > 0) {
        if (XR_UNLIKELY(ci->call_status & XR_CALL_KEEP_FUNC)) {
            return_slot = VM_STACK + ci->result_offset;
        } else {
            return_slot = VM_STACK + ci->base_offset - 1;
        }
        for (int j = 0; j < nret; j++) {
            return_slot[j] = R(a + j);
        }
        // Write null when no return value
        if (nret == 0) {
            *return_slot = xr_null();
        }
    }

    // Pop call frame
    VM_DEC_FRAME_COUNT;

    // Constructor call stack management
    if (isolate->vm.ctor_call_depth > 0) {
        int expected_frame_count =
            isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
        if (VM_FRAME_COUNT == expected_frame_count) {
            isolate->vm.ctor_call_depth--;
        }
    }

    // Handle toString print: if toString call returned, print result
    if (tostring_flags) {
        XrString *ts = xr_value_to_string(isolate, ret_result);
        printf("%s", ts->data);
        if (tostring_flags & 0x02)
            printf("\n");
    }

    if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE) {
        // Module execution complete, return to caller
        return XR_VM_OK;
    }

    // Restore caller frame
    ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

    // Closure called via xr_yield_call_closure returned
    if (XR_UNLIKELY(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
        XrCoroutine *_pcoro = (XrCoroutine *) vm_ctx->current_coro;
        _pcoro->pending_closure_result = return_slot ? *return_slot : xr_null();
        goto handle_closure_pending;
    }

    if (!ci->closure || !ci->closure->proto) {
        // Defensive check: closure invalid, return directly
        return XR_VM_OK;
    }
    VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

    // Handle operator overload conditional jump
    if (return_slot && ci->u.l.pending_operator_check) {
        bool op_result = XR_TO_BOOL(*return_slot);
        if (op_result != ci->u.l.operator_check_k) {
            ci->pc++;
        }
        ci->u.l.pending_operator_check = false;
    }

    goto startfunc;
}

vmcase(OP_RETURN0) {
    /* OP_RETURN0: fast return with no values
    ** Optimized path: skip defer/upvalue checks when not needed
    */

    // Clean up exception handlers belonging to current frame
    while (VM_HANDLER_COUNT > 0 &&
           VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
        VM_DEC_HANDLER_COUNT;
    }

    // Check if we have defer to execute
    if (vm_ctx->defer_count > 0 && vm_ctx->defer_frame_marks) {
        int frame_defer_start = vm_ctx->defer_frame_marks[VM_FRAME_COUNT - 1];
        if (vm_ctx->defer_count > frame_defer_start) {
            // Has defer, fall back to full RETURN
            vm_ctx->last_nret = 0;
            goto return_with_defer;
        }
    }

    // Fast path: no defer
    vm_ctx->last_nret = 0;

    // Write null to return slot
    if (ci->base_offset > 0) {
        if (XR_UNLIKELY(ci->call_status & XR_CALL_KEEP_FUNC)) {
            VM_STACK[ci->result_offset] = xr_null();
        } else {
            VM_STACK[ci->base_offset - 1] = xr_null();
        }
    }

    // Pop call frame
    VM_DEC_FRAME_COUNT;

    // Constructor call stack management
    if (isolate->vm.ctor_call_depth > 0) {
        int expected = isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
        if (VM_FRAME_COUNT == expected) {
            isolate->vm.ctor_call_depth--;
        }
    }

    // Check module boundary
    if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE) {
        return XR_VM_OK;
    }

    // Restore caller frame
    ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

    // Closure called via xr_yield_call_closure returned
    if (XR_UNLIKELY(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
        XrCoroutine *_pcoro = (XrCoroutine *) vm_ctx->current_coro;
        _pcoro->pending_closure_result = xr_null();
        goto handle_closure_pending;
    }

    if (!ci->closure || !ci->closure->proto) {
        return XR_VM_OK;
    }
    VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

    goto startfunc;
}

vmcase(OP_RETURN1) {
    /* OP_RETURN1: fast return with single value
    ** Optimized path for the most common case
    */
    int a = GETARG_A(i);
    XrValue ret_val = R(a);

    // Clean up exception handlers belonging to current frame
    while (VM_HANDLER_COUNT > 0 &&
           VM_HANDLERS[VM_HANDLER_COUNT - 1].frame_count >= VM_FRAME_COUNT) {
        VM_DEC_HANDLER_COUNT;
    }

    // Type feedback: record return value type
    if (ci->closure && ci->closure->proto && ci->closure->proto->type_feedback) {
        xfb_record_return(ci->closure->proto->type_feedback, ret_val);
    }

    // Check if we have defer to execute
    if (vm_ctx->defer_count > 0 && vm_ctx->defer_frame_marks) {
        int frame_defer_start = vm_ctx->defer_frame_marks[VM_FRAME_COUNT - 1];
        if (vm_ctx->defer_count > frame_defer_start) {
            // Has defer, fall back to full RETURN
            vm_ctx->last_nret = 1;
            goto return_with_defer;
        }
    }

    // Fast path: no defer
    vm_ctx->last_nret = 1;

    // Save toString print flags
    uint8_t tostring_flags = ci->flags;
    ci->flags = 0;

    // Write return value
    XrValue *return_slot = NULL;
    if (ci->base_offset > 0) {
        if (XR_UNLIKELY(ci->call_status & XR_CALL_KEEP_FUNC)) {
            return_slot = &VM_STACK[ci->result_offset];
        } else {
            return_slot = &VM_STACK[ci->base_offset - 1];
        }
        *return_slot = ret_val;
    }

    /* Rescue struct_ref pointing to callee's struct_area:
     * append to struct_ret_arena so it survives frame reuse */
    if (return_slot && XR_IS_STRUCT_REF(ret_val)) {
        int sa_idx = VM_FRAME_COUNT - 1;
        if (vm_ctx->struct_areas && sa_idx < vm_ctx->struct_areas_cap) {
            uint8_t *sa = vm_ctx->struct_areas[sa_idx];
            uint8_t *sptr = (uint8_t *) ret_val.ptr;
            uint16_t sa_cap = vm_ctx->struct_area_caps[sa_idx];
            if (sa && sptr >= sa && sptr < sa + sa_cap) {
                XrClass *rcls = *(XrClass **) sptr;
                if (rcls && rcls->struct_layout) {
                    uint32_t total = 8 + rcls->struct_layout->total_size;
                    // Align to 16 bytes
                    total = (total + 15) & ~15u;
                    uint32_t need = vm_ctx->struct_ret_arena_used + total;
                    if (need > vm_ctx->struct_ret_arena_cap) {
                        uint32_t new_cap = vm_ctx->struct_ret_arena_cap;
                        if (new_cap < 512)
                            new_cap = 512;
                        while (new_cap < need)
                            new_cap *= 2;
                        // Temp pointer + null check (project memory rule):
                        // never overwrite the live pointer on realloc failure.
                        uint8_t *new_arena =
                            (uint8_t *) xr_realloc(vm_ctx->struct_ret_arena, new_cap);
                        if (!new_arena) {
                            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                             "failed to grow struct_ret_arena to %u bytes",
                                             new_cap);
                        }
                        vm_ctx->struct_ret_arena = new_arena;
                        vm_ctx->struct_ret_arena_cap = new_cap;
                    }
                    uint8_t *dst = vm_ctx->struct_ret_arena + vm_ctx->struct_ret_arena_used;
                    memcpy(dst, sptr, 8 + rcls->struct_layout->total_size);
                    vm_ctx->struct_ret_arena_used = need;
                    return_slot->ptr = dst;
                }
            }
        }
    }

    // Pop call frame
    VM_DEC_FRAME_COUNT;

    // Constructor call stack management
    if (isolate->vm.ctor_call_depth > 0) {
        int expected = isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].frame_count;
        if (VM_FRAME_COUNT == expected) {
            isolate->vm.ctor_call_depth--;
        }
    }

    // Handle toString print
    if (tostring_flags) {
        XrString *ts = xr_value_to_string(isolate, ret_val);
        printf("%s", ts->data);
        if (tostring_flags & 0x02)
            printf("\n");
    }

    // Check module boundary
    if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE) {
        return XR_VM_OK;
    }
    // Restore caller frame
    ci = &VM_FRAMES[VM_FRAME_COUNT - 1];

    // Closure called via xr_yield_call_closure returned
    if (XR_UNLIKELY(ci->call_status & XR_CALL_CLOSURE_PENDING)) {
        XrCoroutine *_pcoro = (XrCoroutine *) vm_ctx->current_coro;
        _pcoro->pending_closure_result = ret_val;
        goto handle_closure_pending;
    }

    if (!ci->closure || !ci->closure->proto) {
        return XR_VM_OK;
    }
    VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);

    // Handle operator overload conditional jump
    if (return_slot && ci->u.l.pending_operator_check) {
        bool op_result = XR_TO_BOOL(*return_slot);
        if (op_result != ci->u.l.operator_check_k) {
            ci->pc++;
        }
        ci->u.l.pending_operator_check = false;
    }

    goto startfunc;
}
