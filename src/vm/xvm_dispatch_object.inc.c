/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_object.inc.c — class / instance / field / json / invoke /
 *                              property / IC dispatch
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
 *   Class / instance setup:
 *     - OP_CLASS_CREATE_FROM_DESCRIPTOR
 *     - OP_CLINIT_CALL
 *     - OP_ABSTRACT_ERROR
 *     - OP_SET_STORAGE_CTX / OP_TO_SHARED
 *     - OP_MAP_SETKS  (descriptor literal helper)
 *
 *   Field R/W (with field IC):
 *     - OP_GETFIELD / OP_SETFIELD / OP_GETFIELD_IC
 *
 *   Json:
 *     - OP_NEWJSON
 *     - OP_JSON_GET / SET / GETK / SETK
 *     - OP_JSON_INIT / OP_JSON_INIT_I / OP_JSON_INIT_N
 *
 *   Invoke (method dispatch):
 *     - OP_INVOKE  (full mega-cache + monomorphic IC machinery)
 *     - OP_INVOKE_TAIL
 *     - OP_SUPERINVOKE
 *     - OP_INVOKE_DIRECT
 *     - OP_INVOKE_BUILTIN  (per-type method table + invoke-IC)
 *
 *   Property R/W:
 *     - OP_GETPROP / OP_SETPROP / OP_GETSUPER (placeholder)
 *
 * The OP_SPILL / OP_RELOAD pair lives next to these in source
 * order but is just register-window machinery, so it stays
 * inline in xvm.c with the rest of the data hot path.
 */

            /* ========================================================
            ** OOP Instructions
            ** ======================================================== */

            vmcase(OP_CLASS_CREATE_FROM_DESCRIPTOR) {
                // R[A] optionally holds a runtime-resolved super class
                // (for `extends` whose parent comes from a local, upvalue
                // or imported module member). The codegen side computes
                // the parent into R[A] before this instruction; a non-
                // class value (nil) means "fall back to descriptor-
                // encoded resolution".
                int a = GETARG_A(i);
                int bx = GETARG_Bx(i);
                XrValue desc_val = k[bx];
                XrClassDescriptor *desc = (XrClassDescriptor*)XR_TO_PTR(desc_val);
                XrProto *proto = cl->proto;
                XrClass *super_override = NULL;
                XrValue super_slot = R(a);
                if (XR_IS_CLASS(super_slot)) {
                    super_override = XR_TO_CLASS(super_slot);
                }
                XrClass *cls = xr_class_from_descriptor(isolate, desc, proto, cl, base,
                                                        vm_ctx, super_override);
                R(a) = XR_FROM_PTR(cls);
                vmbreak;
            }

            vmcase(OP_CLINIT_CALL) {
                // OP_CLINIT_CALL: call static constructor
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrClass *cls = xr_value_to_class(R(a));

                // Skip if class already initialized
                if (cls->flags & XR_CLASS_INITIALIZED) {
                    vmbreak;
                }

                // Get class descriptor
                XrProto *proto = cl->proto;
                XrValue desc_val = PROTO_CONSTANT(proto, b);
                XrClassDescriptor *desc = (XrClassDescriptor*)XR_TO_PTR(desc_val);

                // Skip if no static constructor
                if (desc->clinit_proto_index < 0) {
                    vmbreak;
                }

                // Get and execute static constructor
                XrProto *clinit_proto = DYNARRAY_GET(&proto->protos, desc->clinit_proto_index, XrProto*);
                XrCoroutine *_clinit_coro = (XrCoroutine *)vm_ctx->current_coro;
                XrClosure *clinit_closure;
                if (_clinit_coro && _clinit_coro->coro_gc) {
                    clinit_closure = (XrClosure*)xr_coro_gc_newobj(_clinit_coro->coro_gc, XR_TFUNCTION, sizeof(XrClosure));
                } else {
                    clinit_closure = (XrClosure*)xr_gc_alloc(&isolate->gc, sizeof(XrClosure), XR_TFUNCTION);
                }
                xr_gc_header_init_type(&clinit_closure->gc, XR_TFUNCTION);
                clinit_closure->proto = clinit_proto;

                cls->flags |= XR_CLASS_INITIALIZED;
                savepc();
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = clinit_closure;
                new_frame->pc = PROTO_CODE_BASE(clinit_proto);
                new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                goto startfunc;
            }

            // Abstract method support
            vmcase(OP_ABSTRACT_ERROR) {
                // OP_ABSTRACT_ERROR: abstract method call error
                int a = GETARG_A(i);
                XrValue method_name_val = k[a];
                const char *method_name = XR_IS_STRING(method_name_val) ? XR_TO_STRING(method_name_val)->data : "<unknown>";
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "cannot call abstract method '%s'", method_name);
            }

            vmcase(OP_SET_STORAGE_CTX) {
                /* OP_SET_STORAGE_CTX: set storage mode context
                ** A = storage mode (0=normal, 1=shared)
                **
                ** For class instance shared support
                ** Set before constructor call, OP_INVOKE reads this context
                */
                int storage_mode = GETARG_A(i);
                isolate->current_storage_mode = (uint8_t)storage_mode;
                vmbreak;
            }

            vmcase(OP_TO_SHARED) {
                /* OP_TO_SHARED: convert to shared object
                ** A = destination register
                ** B = source register
                **
                ** If already shared, increment reference count
                ** Otherwise deep copy to system heap
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                XrValue src = R(b);
                R(a) = xr_to_shared(isolate, src);
                vmbreak;
            }

            vmcase(OP_MAP_SETKS) {
                // OP_MAP_SETKS: batch set fields
                int a = GETARG_A(i);
                int count = GETARG_B(i);
                XrInstance *inst_obj = xr_value_to_instance(R(a));
                for (int j = 0; j < count; j++) {
                    XrValue val = R(a + 1 + j);
                    inst_obj->fields[j] = val;
                }
                VM_BARRIER_BACK(inst_obj);
                vmbreak;
            }

            vmcase(OP_GETFIELD) {
                // OP_GETFIELD: instance field read by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int field_idx = GETARG_C(i);

                XrValue inst_val = R(b);
                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                R(a) = inst_obj->fields[field_idx];
                vmbreak;
            }

            vmcase(OP_SETFIELD) {
                // OP_SETFIELD: instance field write by index
                int a = GETARG_A(i);
                int field_idx = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue inst_val = R(a);
                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                XrValue val = R(c);
                inst_obj->fields[field_idx] = val;
                VM_BARRIER_VAL(inst_obj, val);
                vmbreak;
            }

            vmcase(OP_GETFIELD_IC) {
                /* R[A] = R[B].K[C] - inline cache field access
                ** Uses field_name_idx as IC key (constant per call site). */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int field_name_idx = GETARG_C(i);

                XrValue inst_val = R(b);
                if (!xr_value_is_instance(inst_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field access requires instance object");
                }

                XrInstance *inst_obj = xr_value_to_instance(inst_val);
                XrClass *cls = inst_obj->klass;

                // Lazily ensure the per-ctx IC table for this proto.
                XrICFieldTable *ic_table =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_GETFIELD_IC: failed to allocate IC table");
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table, (int)cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                int field_idx = -1;

                // Fast path: monomorphic IC hit
                if (cache && xr_ic_field_lookup_mono(cache, cls, field_name_idx, &field_idx)) {
                    R(a) = inst_obj->fields[field_idx];
                    vmbreak;
                }

                // Fast path: polymorphic IC hit
                if (cache && xr_ic_field_lookup_poly(cache, cls, field_name_idx, &field_idx)) {
                    R(a) = inst_obj->fields[field_idx];
                    vmbreak;
                }

                // Slow path: string lookup
                XrValue field_name_val = K(field_name_idx);
                if (!XR_IS_STRING(field_name_val)) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "field name must be a string");
                }
                XrString *field_name = XR_TO_STRING(field_name_val);
                field_idx = xr_class_lookup_field_by_name(isolate, cls, field_name->data);

                if (field_idx < 0) {
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not found", field_name->data);
                }

                R(a) = inst_obj->fields[field_idx];

                // Update IC cache
                if (cache) {
                    xr_ic_field_update(cache, cls, field_idx, field_name_idx);
                }
                vmbreak;
            }

            // Json dynamic object instructions (V2 zero-copy design)

            vmcase(OP_NEWJSON) {
                /* OP_NEWJSON: create Json object
                ** A = destination register
                ** B = Shape constant index
                ** C = storage mode (0=normal, 1=shared)
                */
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int storage_mode = GETARG_C(i);
                XrValue shape_val = k[b];
                // Shape stored as integer pointer (not GC managed)
                XrShape *shape = (XrShape*)(intptr_t)XR_TO_INT(shape_val);

                XrJson *json;
                if (storage_mode != 0 && isolate->sys_heap) {
                    // shared: allocate on system heap
                    int field_count = shape->in_object_capacity;
                    size_t size = xr_json_size(field_count);
                    json = (XrJson*)xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TJSON);
                    if (json) {
                        xr_json_init_inplace(json, shape);
                        XR_GC_SET_STORAGE(&json->gc, storage_mode);
                        if (storage_mode == XR_GC_STORAGE_SHARED) {
                            xr_shared_set_refc(&json->gc, 1);
                        }
                    }
                } else {
                    // normal: allocate on coroutine heap
                    json = xr_json_new_with_shape(VM_CURRENT_CORO, shape);
                }

                R(a) = xr_json_value(json);
                if (storage_mode == 0) checkGC(base + a + 1);
                vmbreak;
            }

            vmcase(OP_JSON_GET) {
                // OP_JSON_GET: read field by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(b));
                R(a) = xr_json_get_field(json, (uint16_t)c);
                vmbreak;
            }

            vmcase(OP_JSON_SET) {
                // OP_JSON_SET: write field by index
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set_field(json, (uint16_t)b, val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_GETK) {
                // OP_JSON_GETK: read field by Symbol
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i); // Local symbol index
                XrJson *json = xr_value_to_json(R(b));
                R(a) = xr_json_get(isolate, json, (SymbolId)PROTO_SYMBOL(cl->proto, c));
                vmbreak;
            }

            vmcase(OP_JSON_SETK) {
                // OP_JSON_SETK: write field by Symbol (supports zero-copy conversion)
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Local symbol index
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set(isolate, json, (SymbolId)PROTO_SYMBOL(cl->proto, b), val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_INIT) {
                // OP_JSON_INIT: direct index write during initialization
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                int c = GETARG_C(i);
                XrJson *json = xr_value_to_json(R(a));
                XrValue val = R(c);
                xr_json_set_field(json, (uint16_t)b, val);
                VM_BARRIER_VAL(json, val);
                vmbreak;
            }

            vmcase(OP_JSON_INIT_I) {
                // OP_JSON_INIT_I: init field with immediate integer
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                int c = GETARG_sC(i); // Signed immediate value
                XrJson *json = xr_value_to_json(R(a));
                xr_json_set_field(json, (uint16_t)b, xr_int(c));
                vmbreak;
            }

            vmcase(OP_JSON_INIT_N) {
                // OP_JSON_INIT_N: init field with null
                int a = GETARG_A(i);
                int b = GETARG_B(i); // Field index
                XrJson *json = xr_value_to_json(R(a));
                xr_json_set_field(json, (uint16_t)b, xr_null());
                vmbreak;
            }

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
                invoke_dispatch: ;

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
                #define VM_BUILTIN_INVOKE_CHECK_EXC() do { \
                    if (unlikely(!XR_IS_NULL(VM_EXCEPTION))) { \
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR; \
                        goto startfunc; \
                    } \
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
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                // Cold path: legacy coroutine handle methods
                if (xr_value_is_coro(receiver)) {
                    savepc();
                    int _cr = vm_invoke_coro_handle(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                // Channel methods: inline send/recv hot path, cold path for rest
                if (xr_value_is_channel(receiver)) {
                    XrChannel *ch = xr_value_to_channel(receiver);

                    // Hot path: ch.send(value) - inline blocking send
                    if (nargs == 1 && method_symbol == SYMBOL_SEND) {
                        XrCoroutine *_cur = (XrCoroutine *)VM_CURRENT_CORO;
                        if (_cur && xr_coro_resume_load(_cur) == XR_RESUME_CHANNEL) {
                            xr_coro_resume_store(_cur, XR_RESUME_OK);
                            R(a) = xr_null();
                            vmbreak;
                        }
                        XrValue _sv = vm_chan_copy_send(isolate, R(a + 2));
                        // Pre-save frame state before channel call.  If send blocks, channel func sets BLOCKED
                        // under lock — coro must already have saved state.
                        if (_cur) _cur->send_value = _sv;
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
                        XrCoroutine *_cur = (XrCoroutine *)VM_CURRENT_CORO;
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
                        if (_cur) _cur->recv_slot = &R(a);
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
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_BLOCKED) return XR_VM_BLOCKED;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }

                if (nargs == 0 && method_symbol == SYMBOL_IS_EMPTY) {
                    // isEmpty() method inline: directly check count == 0
                    if (XR_IS_ARRAY(receiver)) {
                        XrArray *arr = XR_TO_ARRAY(receiver);
                        R(a) = xr_bool(arr->length == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_STRING(receiver)) {
                        R(a) = xr_bool(xr_value_str_len(&receiver) == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_MAP(receiver)) {
                        XrMap *map = XR_TO_MAP(receiver);
                        R(a) = xr_bool(map->count == 0);
                        vmbreak; // Skip method call!
                    }
                    else if (XR_IS_SET(receiver)) {
                        XrSet *set = XR_TO_SET(receiver);
                        R(a) = xr_bool(set->count == 0);
                        vmbreak; // Skip method call!
                    }
                }

                /* === Type-based dispatch: O(1) jump table === */

                /* Struct ref: value-type constructor/method call.
                 * struct_ref layout: [XrClass* 8B][fields...] in struct_area */
                if (XR_IS_STRUCT_REF(receiver)) {
                    uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(receiver);
                    XrClass *scls = *(XrClass**)sptr;
                    XrMethod *method = xr_class_lookup_method(scls, method_symbol);

                    if (method == NULL || method->type == XMETHOD_NONE) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "struct '%s' has no method '%s'", scls->name, _mn ? _mn : "?");
                    }
                    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                        R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                        vmbreak;
                    }
                    if (method->type == XMETHOD_CLOSURE && method->as.closure != NULL) {
                        XrClosure *closure = method->as.closure;
                        XrProto *proto = closure->proto;
                        if (nargs + 1 != proto->numparams) {
                            VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT,
                                "constructor expects %d arguments, got %d",
                                proto->numparams - 1, nargs);
                        }
                        if (VM_FRAME_COUNT >= XR_FRAMES_MAX) {
                            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow");
                        }
                        // this (struct_ref) already in R[a+1]
                        savepc();
                        int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                        XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                        new_frame->closure = closure;
                        new_frame->pc = PROTO_CODE_BASE(proto);
                        new_frame->base_offset = (int)((base + a + 1) - VM_STACK);
                        goto startfunc;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "struct method has invalid type");
                }

                if (!XR_IS_PTR(receiver)) {
                    if (XR_IS_INT(receiver)) goto invoke_int;
                    if (XR_IS_FLOAT(receiver)) goto invoke_float;
                    if (XR_IS_BOOL(receiver)) goto invoke_bool;
                    // SSO removed: all strings are heap PTR, handled by XR_TSTRING case
                    goto invoke_type_error;
                }
                switch (XR_GC_GET_TYPE((XrGCHeader*)XR_TO_PTR(receiver))) {
                    case XR_TINSTANCE:
                    case XR_TCLASS:          goto invoke_class_or_instance;
                    case XR_TSTRING:         goto invoke_string;
                    case XR_TARRAY:          goto invoke_array;
                    case XR_TMAP:            goto invoke_map;
                    case XR_TSET:            goto invoke_set;
                    case XR_TJSON:           goto invoke_json;
                    case XR_TMODULE:         goto invoke_module;
                    case XR_TENUM_VALUE:
                    case XR_TENUM_TYPE:      goto invoke_enum;
                    case XR_TITERATOR:       goto invoke_iterator;
                    case XR_TBIGINT:         goto invoke_bigint;
                    case XR_TSTRINGBUILDER:  goto invoke_stringbuilder;
                    case XR_TARRAY_SLICE:    goto invoke_slice;
                    case XR_TRANGE:          goto invoke_range;
                    default:                 goto invoke_native_type;
                }

                // Cold path: enum methods
                invoke_enum: {
                    savepc();
                    int _cr = vm_invoke_enum(isolate, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
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
                        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                        const char *mname = xr_symbol_get_name_in_table(sym_table, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "iterator does not support method: %s", mname ? mname : "?");
                    }
                    vmbreak;
                }

                /* === Map builtin methods === */
                invoke_map:
                if (XR_IS_MAP(receiver)) {
                    /* See xmap_methods.h — unified method table dispatch.
                     * WeakMap-blocked methods return XR_NOTFOUND from the
                     * method body itself. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_MAP, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Map has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Json builtin methods === */
                invoke_json:
                if (xr_value_is_json(receiver)) {
                    /* See xjson_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_JSON, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Json has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === String builtin methods === */
                invoke_string:
                if (XR_IS_STRING(receiver)) {
                    /* See xstring_methods.h — unified method table dispatch.
                     * Receiver stays in heap form to skip the SSO→promote
                     * round-trip the legacy dispatcher used to do. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_STRING, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "String has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Array builtin methods === */
                invoke_array:
                if (XR_IS_ARRAY(receiver)) {
                    /* See xarray_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_ARRAY, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Array has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Set builtin methods === */
                invoke_set:
                if (XR_IS_SET(receiver)) {
                    /* See xset_methods.h — unified method table dispatch.
                     * WeakSet-blocked methods return XR_NOTFOUND from the
                     * method body itself, so the same
                     * "method not found" diagnostic surfaces. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_SET, method_symbol, SYMBOL_BUILTIN_COUNT);
                    R(a) = _slot ? _slot->fn(isolate, receiver, &R(a + 2), nargs)
                                 : XR_NOTFOUND;
                    VM_BUILTIN_INVOKE_CHECK_EXC();
                    if (unlikely(XR_IS_NOTFOUND(R(a)))) {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "Set has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Int builtin methods === */
                invoke_int:
                if (XR_IS_INT(receiver)) {
                    /* See xint_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_INT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "int has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Float builtin methods === */
                invoke_float:
                if (XR_IS_FLOAT(receiver)) {
                    /* See xfloat_methods.h — unified method table dispatch. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_FLOAT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "float has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Bool builtin methods === */
                invoke_bool:
                if (XR_IS_BOOL(receiver)) {
                    /* Bool dispatches through the unified method table.
                     * Hot small methods (e.g. toString) are static-inline
                     * in xbool_methods.h, so AOT inlines them at the call
                     * site; the VM reaches the same body via this table
                     * indirection. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_BOOL, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "bool has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === BigInt builtin methods === */
                invoke_bigint:
                if (XR_IS_BIGINT(receiver)) {
                    /* BigInt dispatches through the unified method table.
                     * See xbigint_methods.h — every method is a static
                     * inline wrapper around an extern xr_bigint_*
                     * primitive, so AOT inlines the wrapper at the call
                     * site and the VM reaches a single out-of-line copy
                     * via this indirection. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        XR_TID_BIGINT, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (likely(_slot != NULL)) {
                        R(a) = _slot->fn(isolate, receiver, &R(a + 2), nargs);
                        VM_BUILTIN_INVOKE_CHECK_EXC();
                    } else {
                        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "BigInt has no method '%s'", _mn ? _mn : "?");
                    }
                    vmbreak;
                }

                /* === Stdlib/third-party native type method call (via native_type_classes mapping) === */
                invoke_native_type:
                if (XR_IS_PTR(receiver)) {
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    XrObjType native_type = XR_GC_GET_TYPE(gc);

                    // Find bound XrClass
                    if (native_type < XR_NATIVE_TYPE_MAX) {
                        XrClass *native_klass = isolate->native_type_classes[native_type];
                        if (native_klass) {
                            // Lookup through class methods
                            XrMethod *method = xr_class_lookup_method(native_klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                // Native method: call C function directly
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                    }
                }

                // Cold path: module export function call
                invoke_module:
                if (xr_value_is_module(receiver)) {
                    savepc();
                    int _cr = vm_invoke_module(isolate, vm_ctx, receiver, method_symbol, nargs, base, a, ci, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_BLOCKED) return XR_VM_BLOCKED;
                    if (_cr == VM_COLD_YIELD) return XR_VM_YIELD;
                    if (_cr == VM_COLD_FATAL) return XR_VM_RUNTIME_ERROR;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                }

                // Class/Instance method call path.
                // method_name_chars is declared at the top of invoke_dispatch
                // so every `goto invoke_*` sibling label observes NULL.
                invoke_class_or_instance: ;
                if (xr_value_is_class(receiver) || xr_value_is_instance(receiver)) {
                    // Get method name only when needed
                    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
                    method_name_chars = xr_symbol_get_name_in_table(sym_table, method_symbol);
                    if (method_name_chars == NULL) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "invalid method symbol: %d", method_symbol);
                    }
                }

                // Lazily ensure the per-ctx method IC table for this proto.
                XrICMethodTable *ic_method_table =
                    xr_vm_ctx_ensure_ic_methods(vm_ctx, frame->closure->proto);
                if (!ic_method_table) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_INVOKE: failed to allocate IC table");
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
                        XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                        const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                            "Range has no method '%s'", mname ? mname : "?");
                    }
                }

                // Support StringBuilder method call
                invoke_stringbuilder:
                if (xr_is_stringbuilder(receiver)) {
                    XrClass *klass = xr_value_get_class(isolate, receiver);
                    if (klass) {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                        if (method != NULL && method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                            R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
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

                        if (method != NULL && method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
                            XrCFunctionPtr cfunc = method->as.primitive;
                            XrValue result = cfunc(isolate, &R(a + 1), nargs + 1);

                            R(a) = result;
                            vmbreak;
                        } else {
                            // Universal toString fallback for basic types
                            if (method_symbol == SYMBOL_TOSTRING) {
                                R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                                vmbreak;
                            }
                            // Basic type method not found - show type and method name
                            XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                            const char *mname = xr_symbol_get_name_in_table(st, method_symbol);
                            const char *tname = XR_IS_INT(receiver) ? "int" :
                                               (XR_IS_FLOAT(receiver) ? "float" : "bool");
                            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                                "%s type has no method '%s'", tname, mname ? mname : "?");
                        }
                    }
                }

                // Support ArraySlice method call
                invoke_slice:
                if (XR_IS_PTR(receiver)) {
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    int heap_type = XR_GC_GET_TYPE(gc);

                    if (heap_type == XR_TARRAY_SLICE) {
                        XrClass *klass = xr_value_get_class(isolate, receiver);
                        if (klass) {
                            XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                        const char *name = xr_symbol_get_name_in_table(st, method_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "ArraySlice has no method '%s'", name ? name : "?");
                    }
                }

                // Cold path: class constructor / static method
                if (xr_value_is_class(receiver)) {
                    int _cr = vm_invoke_class(isolate, vm_ctx, receiver, method_symbol,
                                              method_name_chars, nargs, base, a, ci, pc,
                                              invoke_is_tail);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                } else {
                    // Instance method call
                    if (!xr_value_is_instance(receiver)) {
                        // Universal toString fallback for any remaining type
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on non-instance", method_name_chars);
                    }
                    XrInstance *inst = xr_value_to_instance(receiver);

                    // Find method via polymorphic inline cache
                    size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                    XrICMethod *cache = xr_ic_method_table_get(ic_method_table, cache_index);
                    if (cache) { XR_VM_IC_METHOD_BIND(cache, (int)cache_index); }

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
                        R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                        vmbreak;
                    }

                    if (method->type != XMETHOD_CLOSURE || method->as.closure == NULL) {
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' has invalid type", method_name_chars);
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
                    int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                    new_frame->closure = closure;
                    new_frame->pc = PROTO_CODE_BASE(proto);
                    new_frame->base_offset = (int)((base + a + 1) - VM_STACK);

                    // Jump to new function
                    goto startfunc;
                }

                invoke_type_error: {
                    XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                    const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "method '%s' called on unsupported type", _mn ? _mn : "?");
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
                if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
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
                    cls = *(XrClass**)xr_to_struct_ptr(receiver);
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
                int _fidx = VM_FRAME_COUNT; VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(closure->proto);
                new_frame->base_offset = (int)((base + a + 1) - VM_STACK); // this at R[a+1]
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
                XrICBuiltinTable *_btab =
                    xr_vm_ctx_ensure_ic_builtin(vm_ctx, cl->proto);
                if (unlikely(!_btab)) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                        "OP_INVOKE_BUILTIN: failed to allocate IC table");
                }
                size_t _cidx = (size_t)(pc - 1 - PROTO_CODE_BASE(cl->proto));
                XrICBuiltin *_ic = &_btab->caches[_cidx];

                int _rcv_tid = (int)xr_value_typeid(receiver);

                if (likely(_ic->slot != NULL && _ic->cached_tid == _rcv_tid)) {
                    /* IC hit: direct dispatch, no chain, no table lookup. */
                    if (likely(_ic->hits != UINT16_MAX)) _ic->hits++;
                    R(a) = _ic->slot->fn(isolate, receiver, args, nargs);
                } else {
                    if (_ic->slot) {
                        /* IC miss on already-filled cache (poly site).
                         * We don't replace the cached slot — sticky
                         * first-write-wins keeps the hot type fast. */
                        if (likely(_ic->misses != UINT16_MAX)) _ic->misses++;
                    }

                    /* Unified slow path: single lookup covers all ten
                     * migrated types. */
                    const XrMethodSlot *_slot = xr_method_table_lookup(
                        _rcv_tid, method_symbol, SYMBOL_BUILTIN_COUNT);
                    if (_slot) {
                        R(a) = _slot->fn(isolate, receiver, args, nargs);
                        if (!_ic->slot) {
                            /* First-write-wins. cached_tid fits in
                             * int16_t because XR_TID_COUNT < 256. */
                            _ic->slot = _slot;
                            _ic->cached_tid = (int16_t)_rcv_tid;
                        }
                    } else if (xr_is_stringbuilder(receiver)) {
                    // StringBuilder: dispatch through native_type_classes
                    XrClass *klass = isolate->native_type_classes[XR_TSTRINGBUILDER];
                    if (klass) {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                        if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                            R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                            vmbreak;
                        }
                    }
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                        vmbreak;
                    }
                    XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                    const char *mn = xr_symbol_get_name_in_table(st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "StringBuilder has no method '%s'", mn ? mn : "?");
                } else if (XR_IS_PTR(receiver)) {
                    // Slice type method dispatch
                    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
                    int heap_type = XR_GC_GET_TYPE(gc);

                    if (heap_type == XR_TARRAY_SLICE) {
                        // Get class for slice type, lookup method through class
                        XrClass *klass = xr_value_get_class(isolate, receiver);
                        if (klass) {
                            XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                                R(a) = method->as.primitive(isolate, &R(a + 1), nargs + 1);
                                vmbreak;
                            }
                        }
                        if (method_symbol == SYMBOL_TOSTRING) {
                            R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                            vmbreak;
                        }
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "slice type has no such method (symbol %d)", method_symbol);
                    }
                    // Universal toString fallback for all other heap types
                    if (method_symbol == SYMBOL_TOSTRING) {
                        R(a) = xr_string_value(xr_value_to_string(isolate, receiver));
                        vmbreak;
                    }
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "this type does not support builtin method call");
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
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "this type does not support builtin method call");
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
                    if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                    goto startfunc;
                }
                // Unified method-not-found check for all builtin dispatch functions
                if (XR_IS_NOTFOUND(R(a))) {
                    XrSymbolTable *st = (XrSymbolTable*)isolate->symbol_table;
                    const char *name = xr_symbol_get_name_in_table(st, method_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD,
                        "type '%s' has no method '%s'", xr_typeid_name(xr_value_typeid(receiver)), name ? name : "?");
                }
                vmbreak;
            }


            /* ========================================================
            ** Property access instructions
            ** ======================================================== */

            vmcase(OP_GETPROP) {
                // R[A] = R[B].Symbol[C] - Get property/method
                // Use Symbol direct dispatch, 10x performance improvement
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue obj = R(b);
                int prop_symbol = PROTO_SYMBOL(cl->proto, c); // Dereference local index → global symbol

                // Fixed array .length
                if (XR_IS_ARRAY_REF(obj) && prop_symbol == SYMBOL_LENGTH) {
                    R(a) = XR_FROM_INT((int64_t)XR_ARRAY_REF_ELEM_COUNT(obj));
                    vmbreak;
                }

                // Stack-allocated struct field access
                if (XR_IS_STRUCT_REF(obj)) {
                    uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(obj);
                    XrClass *scls = *(XrClass**)sptr;
                    int fidx = xr_class_lookup_field(scls, prop_symbol);
                    if (fidx >= 0 && scls->struct_layout && fidx < scls->struct_layout->field_count) {
                        XrStructFieldLayout *sf = &scls->struct_layout->fields[fidx];
                        uint8_t *fp = sptr + 8 + sf->offset;
                        switch (sf->native_type) {
                            case XR_NATIVE_I64:  R(a) = XR_FROM_INT(*(int64_t*)fp); break;
                            case XR_NATIVE_F64:  R(a) = XR_FROM_FLOAT(*(double*)fp); break;
                            case XR_NATIVE_BOOL: R(a).descriptor = 0; R(a).i = *(uint8_t*)fp ? 1 : 0; R(a).tag = XR_TAG_BOOL; break;
                            case XR_NATIVE_I32:  R(a) = XR_FROM_INT((int64_t)*(int32_t*)fp); break;
                            case XR_NATIVE_F32:  R(a) = XR_FROM_FLOAT((double)*(float*)fp); break;
                            case XR_NATIVE_STRING: {
                                XrString *s = *(XrString**)fp;
                                R(a) = s ? XR_FROM_STR(s) : xr_null();
                                break;
                            }
                            default: R(a) = xr_null(); break;
                        }
                        vmbreak;
                    }
                    // Field not found: might be a method, fall through to cold path
                }

                // Fast path: Instance is the most common target, skip if-else chain
                if (xr_value_is_instance(obj)) goto getprop_instance;

                // Json fast path with Shape IC
                if (xr_value_is_json(obj)) {
                    XrJson *json = xr_value_to_json(obj);
                    uint16_t shape_id = xr_gc_get_shape_id(&json->gc);

                    // Lazily ensure the per-ctx IC table for this proto.
                    XrICFieldTable *ic_table_j =
                        xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                    if (!ic_table_j) {
                        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                         "OP_GETPROP: failed to allocate IC table");
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(ic_table_j, (int)jic_index);

                    // IC hit: shape_id + symbol match → direct field access (in-object only)
                    uint16_t jic_idx;
                    if (jic && xr_ic_json_lookup(jic, shape_id, prop_symbol, &jic_idx)) {
                        R(a) = json->fields[jic_idx];
                        vmbreak;
                    }

                    // IC miss: full shape lookup
                    XrShape *shape = xr_shape_get_by_id(isolate, shape_id);
                    if (shape && shape->symbol_to_index &&
                        prop_symbol >= (int)shape->min_symbol &&
                        prop_symbol <= (int)shape->max_symbol) {
                        int idx = shape->symbol_to_index[prop_symbol - shape->min_symbol];
                        if (idx >= 0) {
                            if (idx < shape->in_object_capacity) {
                                R(a) = json->fields[idx];
                                // Update IC for next hit (in-object fields only)
                                if (jic) xr_ic_json_update(jic, shape_id, (uint16_t)idx, prop_symbol);
                                vmbreak;
                            }
                            // Overflow field: fall through to slow path
                        }
                    }

                    // Slow path: overflow field or field not found
                    R(a) = xr_json_get(isolate, json, prop_symbol);
                    vmbreak;
                }

                // Cold path: all non-instance, non-json type dispatch
                {
                    savepc();
                    int _cr = vm_getprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, base, a, b, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: fall through to instance path
                }

                getprop_instance: ;
                XrInstance *inst = xr_value_to_instance(obj);

                // Cold path: getter method lookup
                {
                    int _cr = vm_getprop_instance_getter(isolate, vm_ctx, inst, obj, prop_symbol, base, a, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: no getter, fall through to field access
                }

                // No getter: access as regular field

                // Field access Inline Cache optimization (per-ctx)
                XrICFieldTable *ic_table_g =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table_g) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_GETPROP: failed to allocate IC table");
                }

                // Get IC for current instruction
                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table_g, cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                XrClass *inst_class = inst->klass;
                int field_index = -1;

                // Fast path 1: Monomorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
                    // Monomorphic hit: direct field access!
                    R(a) = inst->fields[field_index];
                    vmbreak;
                }

                // Fast path 2: Polymorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
                    // Polymorphic hit: direct field access!
                    R(a) = inst->fields[field_index];
                    vmbreak;
                }

                // Slow path: Symbol lookup for field index
                field_index = xr_class_lookup_field(inst_class, prop_symbol);

                if (field_index >= 0) {
                    // Get instance field count for bounds check
                    uint32_t inst_field_count = xr_class_instance_field_count(inst_class);

                    // Bounds check: prevent out-of-bounds access
                    if ((uint32_t)field_index >= inst_field_count) {
                        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "field index out of bounds: %d >= %d", field_index, inst_field_count);
                    }

                    // Field exists: access and update IC
                    R(a) = inst->fields[field_index];

                    // Update IC cache (pass symbol)
                    if (cache) {
                        xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
                    }
                } else {
                    // Field not found: try method lookup for method reference
                    XrMethod *_m = xr_class_lookup_method(inst_class, prop_symbol);
                    if (_m && _m->as.closure) {
                        R(a) = XR_FROM_PTR(_m->as.closure);
                    } else {
                        const char* class_name = inst_class->name ? inst_class->name : "?";
                        XrSymbolTable *_st2 = (XrSymbolTable*)isolate->symbol_table;
                        const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
                        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                                         _pn ? _pn : "?", class_name);
                    }
                }
                vmbreak;
            }

            vmcase(OP_SETPROP) {
                /* === OP_SETPROP R[A].symbol(B) = R[C] ===
                ** Use Symbol direct dispatch, 10x performance improvement */
                TRACE_EXECUTION();
                int a = GETARG_A(i);
                int b = GETARG_B(i);
                int c = GETARG_C(i);

                XrValue obj = R(a);
                int prop_symbol = PROTO_SYMBOL(cl->proto, b); // Dereference local index → global symbol
                XrValue value = R(c);

                // Json fast path with Shape IC (set existing field)
                if (xr_value_is_json(obj)) {
                    XrJson *json = xr_value_to_json(obj);
                    uint16_t shape_id = xr_gc_get_shape_id(&json->gc);

                    // Lazily ensure the per-ctx IC table for this proto.
                    XrICFieldTable *ic_table_sj =
                        xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                    if (!ic_table_sj) {
                        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                         "OP_SETPROP: failed to allocate IC table");
                    }
                    size_t jic_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                    XrICField *jic = xr_ic_field_table_get(ic_table_sj, (int)jic_index);

                    // IC hit: shape_id + symbol match → direct field write (in-object only)
                    uint16_t jic_idx;
                    if (jic && xr_ic_json_lookup(jic, shape_id, prop_symbol, &jic_idx)) {
                        json->fields[jic_idx] = value;
                        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                        vmbreak;
                    }

                    // IC miss: try inline fast path for existing in-object field
                    XrShape *shape = xr_shape_get_by_id(isolate, shape_id);
                    if (shape && shape->symbol_to_index &&
                        prop_symbol >= (int)shape->min_symbol &&
                        prop_symbol <= (int)shape->max_symbol) {
                        int idx = shape->symbol_to_index[prop_symbol - shape->min_symbol];
                        if (idx >= 0) {
                            if (idx < shape->in_object_capacity) {
                                json->fields[idx] = value;
                                XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                                if (jic) xr_ic_json_update(jic, shape_id, (uint16_t)idx, prop_symbol);
                                vmbreak;
                            }
                            // Overflow field: fall through to slow path
                        }
                    }

                    // Slow path: overflow field, new field addition
                    xr_json_set(isolate, json, prop_symbol, value);
                    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
                    vmbreak;
                }

                // Cold path: non-instance type dispatch (Map/Module/Class/null)
                {
                    savepc();
                    int _cr = vm_setprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, value, base, a, frame, pc);
                    if (_cr == VM_COLD_BREAK) vmbreak;
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: fall through to instance path
                }

                XrInstance *inst = xr_value_to_instance(obj);

                // Cold path: setter method lookup
                {
                    int _cr = vm_setprop_instance_setter(isolate, vm_ctx, inst, obj, prop_symbol, value, base, c, frame, pc);
                    if (_cr == VM_COLD_STARTFUNC) goto startfunc;
                    if (_cr == VM_COLD_ERROR) {
                        if (VM_HANDLER_COUNT == 0) return XR_VM_RUNTIME_ERROR;
                        goto startfunc;
                    }
                    // VM_COLD_CONTINUE: no setter, fall through to field access
                }

                // No setter: assign as regular field
                XrClass *inst_class = inst->klass;

                // Field access Inline Cache optimization (per-ctx)
                XrICFieldTable *ic_table_s =
                    xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
                if (!ic_table_s) {
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                     "OP_SETPROP: failed to allocate IC table");
                }

                size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
                XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
                XrICField *cache = xr_ic_field_table_get(ic_table_s, cache_index);
                if (cache) { XR_VM_IC_FIELD_BIND(cache, (int)cache_index); }

                int field_index = -1;

                // Fast path 1: Monomorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);
                    vmbreak;
                }

                // Fast path 2: Polymorphic IC hit (verify symbol match)
                if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);
                    vmbreak;
                }

                // Slow path: Symbol lookup for field index
                field_index = xr_class_lookup_field(inst_class, prop_symbol);

                if (field_index >= 0) {
                    inst->fields[field_index] = value;
                    VM_BARRIER_VAL(inst, value);

                    // Update IC cache (pass symbol)
                    if (cache) {
                        xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
                    }
                } else {
                    // Field not found: generate error message
                    XrSymbolTable *_st2 = (XrSymbolTable*)isolate->symbol_table;
                    const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
                    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                                     _pn ? _pn : "?", inst_class->name);
                }
                vmbreak;
            }

            vmcase(OP_GETSUPER) {
                // GETSUPER: Get superclass method (not implemented)
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "OP_GETSUPER not yet implemented");
            }
