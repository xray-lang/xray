/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_cold_paths.c - VM cold path function implementations
 *
 * KEY CONCEPT:
 *   These are infrequently-executed VM operations extracted from xvm.c.
 *   All functions are __attribute__((noinline)) to reduce I-cache pressure
 *   on the hot dispatch loop in run().
 */

#include "xvm_cold_paths.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xslice.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"

/* ========== Cold Path: OP_INVOKE Channel Methods ========== */

__attribute__((noinline))
int vm_invoke_channel(XrayIsolate *isolate, XrVMContext *vm_ctx,
                             XrChannel *ch, int method_symbol, int nargs,
                             XrValue *base, int a, XrBcCallFrame *frame,
                             XrInstruction *pc) {
    XR_DCHECK(isolate != NULL, "vm_invoke_channel: NULL isolate");
    XR_DCHECK(ch != NULL, "vm_invoke_channel: NULL channel");
    XR_DCHECK(base != NULL, "vm_invoke_channel: NULL base");
    // ch.trySend(value)
    if (nargs == 1 && method_symbol == SYMBOL_TRYSEND) {
        XrValue send_v = vm_chan_copy_send(isolate, base[a + 2]);
        bool success = xr_channel_try_send(ch, send_v);
        base[a] = xr_bool(success);
        if (success) {
            xr_runtime_wake_channel(isolate, ch, false);
        }
        return VM_COLD_BREAK;
    }

    // ch.tryRecv()
    if (nargs == 0 && method_symbol == SYMBOL_TRYRECV) {
        bool ok;
        XrValue value = xr_channel_try_recv(ch, &ok);
        base[a] = ok ? vm_chan_copy_recv(isolate, value, vm_ctx) : xr_null();
        if (ok) {
            xr_runtime_wake_channel(isolate, ch, true);
        }
        return VM_COLD_BREAK;
    }

    // ch.send(value) - blocking send
    if (nargs == 1 && method_symbol == SYMBOL_SEND) {
        XrCoroutine *current = (XrCoroutine *)vm_ctx->current_coro;
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }
        XrValue send_v = vm_chan_copy_send(isolate, base[a + 2]);
        // Pre-save frame — see hot path comment.
        if (current) current->send_value = send_v;
        frame->pc = pc - 1;
        frame->call_status |= XR_CALL_YIELDED;
        XrChanResult result = xr_channel_send(ch, send_v, current);
        if (result == XR_CHAN_OK) {
            frame->call_status &= ~XR_CALL_YIELDED;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_CLOSED) {
            frame->call_status &= ~XR_CALL_YIELDED;
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel is closed");
        } else if (result == XR_CHAN_BLOCK) {
            return VM_COLD_BLOCKED;
        } else {
            frame->call_status &= ~XR_CALL_YIELDED;
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel send failed");
        }
    }

    // ch.recv() - blocking receive
    if (nargs == 0 && method_symbol == SYMBOL_RECV) {
        XrCoroutine *current = (XrCoroutine *)vm_ctx->current_coro;
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            base[a] = vm_chan_copy_recv(isolate, base[a], vm_ctx); // Deep copy recv_slot value
            return VM_COLD_BREAK;
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL_CLOSED) {
            // Close wakeup: need to re-execute recv logic to check buffer
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            // Continue with recv logic below
        }
        // Set recv_slot to result register address
        if (current) {
            current->recv_slot = &base[a];
        }
        // Pre-save frame — see hot path comment.
        frame->pc = pc - 1;
        frame->call_status |= XR_CALL_YIELDED;
        XrValue value;
        XrChanResult result = xr_channel_recv(ch, &value, current);
        if (result == XR_CHAN_OK) {
            frame->call_status &= ~XR_CALL_YIELDED;
            base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_CLOSED) {
            frame->call_status &= ~XR_CALL_YIELDED;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_BLOCK) {
            return VM_COLD_BLOCKED;
        } else {
            frame->call_status &= ~XR_CALL_YIELDED;
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel receive failed");
        }
    }

    // ch.close()
    if (nargs == 0 && method_symbol == SYMBOL_CLOSE) {
        xr_channel_close(ch);
        xr_runtime_wake_channel_all(isolate, ch);
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    // ch.isClosed()
    if (nargs == 0 && method_symbol == SYMBOL_IS_CLOSED) {
        base[a] = xr_bool(xr_channel_is_closed(ch));
        return VM_COLD_BREAK;
    }

    // ch.sendTimeout(value, timeout) - send with timeout
    if (nargs == 2 && method_symbol == SYMBOL_SENDTIMEOUT) {
        XrCoroutine *current = (XrCoroutine *)vm_ctx->current_coro;
        int64_t timeout_ms = XR_TO_INT(base[a + 3]);

        // Check if woken from timeout
        if (current && xr_coro_resume_load(current) == XR_RESUME_TIMEOUT) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        // Check if woken from channel close
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL_CLOSED) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_bool(true);
            return VM_COLD_BREAK;
        }

        // Try to send (deep copy mutable values for buffer safety)
        XrValue send_val = vm_chan_copy_send(isolate, base[a + 2]);
        XrChanResult result = xr_channel_send(ch, send_val, current);
        if (result == XR_CHAN_OK) {
            base[a] = xr_bool(true);
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_CLOSED) {
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_BLOCK) {
            // Blocked: set timeout timer
            current->send_value = send_val;
            current->channel_deadline = xr_monotonic_ticks() + timeout_ms;
            XrWorker *worker = xr_current_worker();
            if (worker) {
                xr_worker_add_sleep_timer(worker, current, timeout_ms);
            }
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            return VM_COLD_BLOCKED;
        }
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }

    // ch.recvTimeout(timeout) - receive with timeout
    if (nargs == 1 && method_symbol == SYMBOL_RECVTIMEOUT) {
        XrCoroutine *current = (XrCoroutine *)vm_ctx->current_coro;
        int64_t timeout_ms = XR_TO_INT(base[a + 2]);

        // Check if woken from timeout
        if (current && xr_coro_resume_load(current) == XR_RESUME_TIMEOUT) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            return VM_COLD_BREAK; // Value already in recv_slot
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL_CLOSED) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
        }

        // Set recv_slot
        if (current) {
            current->recv_slot = &base[a];
        }

        // Try to receive
        XrValue value;
        XrChanResult result = xr_channel_recv(ch, &value, current);
        if (result == XR_CHAN_OK) {
            base[a] = value;
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_CLOSED) {
            base[a] = xr_null();
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_BLOCK) {
            // Blocked: set timeout timer
            current->channel_deadline = xr_monotonic_ticks() + timeout_ms;
            XrWorker *worker = xr_current_worker();
            if (worker) {
                xr_worker_add_sleep_timer(worker, current, timeout_ms);
            }
            frame->pc = pc - 1;
            frame->call_status |= XR_CALL_YIELDED;
            return VM_COLD_BLOCKED;
        }
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    // toString fallback for Channel
    if (method_symbol == SYMBOL_TOSTRING) {
        base[a] = xr_string_value(xr_value_to_string(isolate, base[a + 1]));
        return VM_COLD_BREAK;
    }

    // Unknown method
    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
    const char *method_name = xr_symbol_get_name_in_table(sym_table, method_symbol);
    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
        "Channel has no method '%s', available: send(), recv(), trySend(), tryRecv(), sendTimeout(), recvTimeout(), close(), isClosed()",
        method_name ? method_name : "?");
}

/* ========== Cold Path: OP_INVOKE Task Handle ========== */

/*
 * Task-level method dispatch — works even after executor detach (coro=NULL).
 * cancel(): if executor alive and task pending, cancel it; otherwise no-op.
 * toString(): returns string representation.
 */
__attribute__((noinline))
int vm_invoke_task_handle(XrayIsolate *isolate,
                                 XrValue receiver, int method_symbol, int nargs,
                                 XrValue *base, int a,
                                 XrBcCallFrame *frame, XrInstruction *pc) {
    XrTask *task = xr_value_to_task(receiver);
    if (nargs == 0 && method_symbol == SYMBOL_CANCEL) {
        XrCoroutine *coro = task->coro;
        if (coro && !xr_task_is_done(task)) {
            xr_coro_cancel(coro);
            xr_task_cancel(task);
            xr_coro_wake_waiter(isolate, coro);
        }
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (nargs == 0 && method_symbol == SYMBOL_MONITOR) {
        // task.monitor() — create buffered Channel that receives completion event
        XrChannel *ch = xr_channel_new(isolate, 1);
        if (!ch) {
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "task.monitor: failed to create channel");
        }
        if (xr_task_is_done(task)) {
            // Task already finished: send the task itself immediately
            xr_channel_notify_send(ch, xr_value_from_task(task));
        } else {
            // Task still running: register completion listener
            XrCompletionNode *cn = (XrCompletionNode *)xr_calloc(1, sizeof(XrCompletionNode));
            if (cn) {
                cn->type = XR_COMPLETION_CHANNEL;
                cn->as.channel = ch;
                xr_task_add_completion(task, cn);
            }
        }
        base[a] = xr_value_from_channel(ch);
        return VM_COLD_BREAK;
    }
    if (nargs == 1 && method_symbol == SYMBOL_LINK) {
        // task.link(other) — bidirectional error propagation
        XrValue arg = base[a + 2];
        if (!xr_value_is_task(arg)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                "task.link: argument must be a Task");
        }
        XrTask *other = xr_value_to_task(arg);
        xr_task_link(task, other);
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (nargs == 1 && method_symbol == SYMBOL_UNLINK) {
        // task.unlink(other) — remove bidirectional link
        XrValue arg = base[a + 2];
        if (!xr_value_is_task(arg)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                "task.unlink: argument must be a Task");
        }
        XrTask *other = xr_value_to_task(arg);
        xr_task_unlink(task, other);
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (method_symbol == SYMBOL_TOSTRING) {
        base[a] = xr_string_value(xr_value_to_string(isolate, receiver));
        return VM_COLD_BREAK;
    }
    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
        "task handle does not support this method");
}

/* ========== Cold Path: OP_INVOKE Coroutine Handle ========== */

__attribute__((noinline))
int vm_invoke_coro_handle(XrayIsolate *isolate,
                                 XrValue receiver, int method_symbol, int nargs,
                                 XrValue *base, int a,
                                 XrBcCallFrame *frame, XrInstruction *pc) {
    XrCoroutine *handle = xr_value_to_coro(receiver);
    if (nargs == 0 && method_symbol == SYMBOL_CANCEL) {
        xr_coro_cancel(handle);
        if (handle->task) {
            xr_task_cancel(handle->task);
        }
        xr_coro_wake_waiter(isolate, handle);
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (method_symbol == SYMBOL_TOSTRING) {
        base[a] = xr_string_value(xr_value_to_string(isolate, receiver));
        return VM_COLD_BREAK;
    }
    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
        "coroutine handle does not support this method");
}

/* ========== Cold Path: OP_INVOKE Enum Methods ========== */

__attribute__((noinline))
int vm_invoke_enum(XrayIsolate *isolate,
                          XrValue receiver, int method_symbol, int nargs,
                          XrValue *base, int a,
                          XrBcCallFrame *frame, XrInstruction *pc) {
    if (!XR_IS_PTR(receiver)) return VM_COLD_CONTINUE;
    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);

    if (XR_GC_GET_TYPE(gc) == XR_TENUM_VALUE) {
        XrEnumValue *enum_val = (XrEnumValue*)gc;

        if (nargs == 0 && method_symbol == SYMBOL_NAME) {
            size_t len = strlen(enum_val->member_name);
            XrString *str = xr_string_intern(isolate, enum_val->member_name, len, 0);
            base[a] = xr_string_value(str);
            return VM_COLD_BREAK;
        }
        if (nargs == 0 && method_symbol == SYMBOL_VALUE) {
            base[a] = enum_val->raw_value;
            return VM_COLD_BREAK;
        }
        if (nargs == 0 && method_symbol == SYMBOL_ORDINAL) {
            base[a] = xr_int(enum_val->member_index);
            return VM_COLD_BREAK;
        }
        if (nargs == 0 && method_symbol == SYMBOL_TOSTRING) {
            size_t enum_name_len = strlen(enum_val->enum_name);
            size_t member_name_len = strlen(enum_val->member_name);
            if (enum_name_len + member_name_len + 2 > XR_TOSTRING_BUFFER_SIZE) {
                VM_COLD_THROW(frame, pc, XR_ERR_OVERFLOW, "enum name too long");
            }
            char buffer[XR_TOSTRING_BUFFER_SIZE];
            snprintf(buffer, sizeof(buffer), "%s.%s",
                   enum_val->enum_name, enum_val->member_name);
            size_t len = strlen(buffer);
            XrString *str = xr_string_intern(isolate, buffer, len, 0);
            base[a] = xr_string_value(str);
            return VM_COLD_BREAK;
        }
        // If no match, fall through to class/instance path
        return VM_COLD_CONTINUE;
    }

    if (XR_GC_GET_TYPE(gc) == XR_TENUM_TYPE) {
        XrEnumType *enum_type = (XrEnumType*)gc;

        if (nargs == 1 && method_symbol == SYMBOL_GET_MEMBER) {
            XrValue index_val = base[a + 2];
            if (XR_IS_INT(index_val)) {
                int index = XR_TO_INT(index_val);
                if (index >= 0 && index < (int)enum_type->member_count) {
                    XrEnumValue *eval = enum_type->members[index].instance;
                    base[a] = XR_FROM_PTR(eval);
                } else {
                    base[a] = xr_null();
                }
            } else {
                base[a] = xr_null();
            }
            return VM_COLD_BREAK;
        }
    }

    return VM_COLD_CONTINUE;
}

/* ========== Cold Path: OP_INVOKE Class Constructor/Static Method ========== */

/*
 * Handles class constructor invocation and static method calls via OP_INVOKE.
 * Returns VM_COLD_BREAK on direct result, VM_COLD_STARTFUNC for closure call,
 * or VM_COLD_ERROR on error.
 */
__attribute__((noinline))
int vm_invoke_class(XrayIsolate *isolate, XrVMContext *vm_ctx,
                            XrValue receiver, int method_symbol,
                            const char *method_name_chars, int nargs,
                            XrValue *base, int a,
                            XrBcCallFrame *frame, XrInstruction *pc,
                            int is_tail) {
    XR_DCHECK(isolate != NULL, "vm_invoke_class: NULL isolate");
    XR_DCHECK(base != NULL, "vm_invoke_class: NULL base");
    XR_DCHECK(method_name_chars != NULL, "vm_invoke_class: NULL method_name");
    XrClass *cls = xr_value_to_class(receiver);

    if (strcmp(method_name_chars, XR_KEYWORD_CONSTRUCTOR) == 0) {
        if (!xr_class_can_instantiate(cls)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_CALL, "cannot instantiate abstract class '%s'", cls->name);
        }

        XrInstance *inst = xr_instance_new(isolate, cls);
        XrValue inst_val = xr_value_from_instance(inst);

        // Find constructor via inline cache (per-ctx, lazily ensured by hot dispatcher)
        size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
        XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
        XrICMethodTable *ic_methods =
            xr_vm_ctx_get_ic_methods(vm_ctx, frame->closure->proto);
        XrICMethod *cache = xr_ic_method_table_get(ic_methods, cache_index);
        if (cache) { XR_VM_IC_METHOD_BIND(cache, (int)cache_index); }

        XrMethod *ctor = cache ? xr_ic_method_lookup(cache, cls, method_symbol)
                               : xr_class_lookup_method(cls, method_symbol);

        // Primitive constructor (Map, Array, etc.)
        if (ctor != NULL && ctor->type == XMETHOD_PRIMITIVE && ctor->as.primitive != NULL) {
            XrValue result = ctor->as.primitive(isolate, &base[a + 2], nargs);
            base[a] = result;
            return VM_COLD_BREAK;
        }

        // Closure constructor
        if (ctor != NULL && ctor->type == XMETHOD_CLOSURE && ctor->as.closure != NULL) {
            XrClosure *closure = ctor->as.closure;
            XrProto *proto = closure->proto;

            if (nargs + 1 != proto->numparams) {
                VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT, "constructor expects %d arguments, got %d",
                               proto->numparams - 1, nargs);
            }
            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            base[a + 1] = inst_val; // this

            frame->pc = pc; // savepc

            int fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);

            return VM_COLD_STARTFUNC;
        }

        // No constructor, return instance directly
        base[a] = inst_val;
        return VM_COLD_BREAK;
    } else {
        // Static method call
        XrMethod *method = xr_class_lookup_method(cls, method_symbol);

        if (method == NULL || !(method->flags & XMETHOD_FLAG_STATIC)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "static method '%s' not found on class '%s'",
                           method_name_chars, cls->name);
        }

        if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
            XrValue result = method->as.primitive(isolate, &base[a + 2], nargs);
            base[a] = result;
            return VM_COLD_BREAK;
        } else if (method->type == XMETHOD_CLOSURE && method->as.closure != NULL) {
            XrClosure *closure = method->as.closure;
            XrProto *proto = closure->proto;

            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            // Move args: from base[a+2] to base[a+1] (static methods have no 'this')
            for (int pi = 0; pi < nargs; pi++) {
                base[a + 1 + pi] = base[a + 2 + pi];
            }

            if (is_tail) {
                // Tail call: reuse current frame
                memmove(base, &base[a + 1], sizeof(XrValue) * nargs);
                frame->closure = closure;
                frame->pc = PROTO_CODE_BASE(proto);
                vm_ctx->stack_top = base + proto->maxstacksize;
                return VM_COLD_STARTFUNC;
            }

            frame->pc = pc; // savepc

            int fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);

            return VM_COLD_STARTFUNC;
        } else {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "invalid static method '%s'", method_name_chars);
        }
    }
}

/* ========== Cold Path: OP_SUPERINVOKE ========== */

/*
 * Handles the entire OP_SUPERINVOKE: constructor :super() and super.method() calls.
 * Returns VM_COLD_STARTFUNC on success, VM_COLD_ERROR on error.
 */
__attribute__((noinline))
int vm_superinvoke(XrayIsolate *isolate, XrVMContext *vm_ctx,
                           XrInstruction instr, XrValue *base,
                           XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int nargs = GETARG_C(instr);

    bool is_ctor_call = (a == 0);
    XrValue this_val = is_ctor_call ? base[0] : base[a + 1];

    if (!xr_value_is_instance(this_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "super can only be called on class instance");
    }

    XrInstance *inst_obj = xr_value_to_instance(this_val);
    XrClass *inst_class = inst_obj->klass;

    XrClass *current_method_class;
    if (isolate->vm.ctor_call_depth > 0) {
        current_method_class = (XrClass*)isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].class_ptr;
    } else {
        current_method_class = inst_class;
    }

    if (current_method_class == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "cannot determine current method's class");
    }

    XrClass *super_class = current_method_class->super;
    if (super_class == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "class '%s' has no superclass", current_method_class->name);
    }

    XrValue method_name_val = PROTO_CONSTANT(frame->closure->proto, b);
    if (!XR_IS_STRING(method_name_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "invalid method name in super call");
    }

    const char *method_name = xr_value_str_data(&method_name_val);

    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
    int method_symbol = xr_symbol_register_in_table(sym_table, method_name);

    XrMethod *method = xr_class_lookup_method(super_class, method_symbol);

    if (method == NULL || method->type == XMETHOD_NONE) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "superclass method '%s' not found in class '%s'",
                       method_name, super_class->name);
    }

    if (method->type != XMETHOD_CLOSURE || method->as.closure == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "superclass method '%s' has invalid type", method_name);
    }

    XrClosure *closure = method->as.closure;
    XrProto *proto = closure->proto;

    if (nargs + 1 != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "superclass method '%s' expects %d arguments but got %d",
                       method_name, proto->numparams - 1, nargs);
    }

    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
        VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
    }

    if (is_ctor_call) {
        int current_param_count = frame->closure->proto->numparams;
        int call_base_offset = current_param_count + 1;

        base[call_base_offset] = this_val;
        for (int idx = 0; idx < nargs; idx++) {
            base[call_base_offset + 1 + idx] = base[1 + idx];
        }

        if (isolate->vm.ctor_call_depth >= XR_CTOR_CALL_STACK_MAX) {
            VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "constructor call depth exceeded");
        }
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].class_ptr = super_class;
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].frame_count = vm_ctx->frame_count;
        isolate->vm.ctor_call_depth++;

        frame->pc = pc; // savepc

        int fidx = vm_ctx->frame_count;
        memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
        vm_ctx->frame_count++;
        XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(proto);
        new_frame->base_offset = (int)((base + call_base_offset) - vm_ctx->stack);
    } else {
        if (isolate->vm.ctor_call_depth >= XR_CTOR_CALL_STACK_MAX) {
            VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "super call depth exceeded");
        }
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].class_ptr = super_class;
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].frame_count = vm_ctx->frame_count;
        isolate->vm.ctor_call_depth++;

        frame->pc = pc; // savepc

        int fidx = vm_ctx->frame_count;
        memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
        vm_ctx->frame_count++;
        XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(proto);
        new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);
    }

    return VM_COLD_STARTFUNC;
}

/* ========== Cold Path: OP_SETPROP Type Dispatch ========== */

/*
 * Handles property set for non-instance types (Map error, Module, Class static, Json).
 * Returns VM_COLD_BREAK on success, VM_COLD_CONTINUE for instance, VM_COLD_ERROR on error.
 */
__attribute__((noinline))
int vm_setprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                     XrValue obj, int prop_symbol, XrValue value,
                                     XrValue *base, int a,
                                     XrBcCallFrame *frame, XrInstruction *pc) {
    (void)base;
    (void)a;
    // Map dot assignment: forbidden
    if (XR_IS_MAP(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
            "Map does not support dot syntax assignment '%s', use map[\"%s\"] = value or map.set(\"%s\", value)",
            name ? name : "?", name ? name : "?", name ? name : "?");
    }

    // Module export variable assignment
    if (xr_value_is_module(obj)) {
        XrModule *module = xr_value_to_module(obj);
        if (module && xr_module_has_sym(module, prop_symbol)) {
            if (xr_module_is_const_sym(module, prop_symbol)) {
                XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
                const char *_name = xr_symbol_get_name_in_table(_st, prop_symbol);
                VM_COLD_THROW(frame, pc, XR_ERR_CMP_CONST_ASSIGN,
                    "cannot modify module constant '%s.%s'",
                    module->name ? module->name : "?", _name ? _name : "?");
            }
            xr_module_set_sym(module, prop_symbol, value);
            XrCoroutine *_bc = vm_ctx->current_coro;
            if (_bc && _bc->coro_gc)
                XR_GC_BARRIER_VAL(_bc->coro_gc, module, value);
            return VM_COLD_BREAK;
        }
        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, prop_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT,
            "module '%s' has no export variable '%s'",
            module && module->name ? module->name : "?", _name ? _name : "?");
    }

    // Static field assignment
    if (xr_value_is_class(obj)) {
        XrClass *cls = xr_value_to_class(obj);
        int field_index = xr_class_lookup_field(cls, prop_symbol);
        if (field_index < 0) {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "static field '%s' not found in class '%s'", pname ? pname : "?", cls->name);
        }
        const XrFieldDescriptor *field = xr_class_get_field(cls, field_index);
        if (!field) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: field descriptor not found");
        }
        if (!(field->flags & XR_FIELD_STATIC)) {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "field '%s' is not a static field", pname ? pname : "?");
        }
        if (field->flags & XR_FIELD_FINAL) {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_CMP_CONST_ASSIGN,
                "cannot modify const static field '%s'", pname ? pname : "?");
        }
        int static_field_idx = field->static_slot;
        if (static_field_idx < 0 || static_field_idx >= cls->static_field_count) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: static field index out of bounds");
        }
        cls->static_field_values[static_field_idx] = value;
        XrCoroutine *_bc = vm_ctx->current_coro;
        if (_bc && _bc->coro_gc)
            XR_GC_BARRIER_VAL(_bc->coro_gc, cls, value);
        return VM_COLD_BREAK;
    }

    // Json property set
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        xr_json_set(isolate, json, prop_symbol, value);
        XrCoroutine *_bc = vm_ctx->current_coro;
        if (_bc && _bc->coro_gc)
            xr_coro_gc_barrierback(_bc->coro_gc, XR_OBJ2GC(json));
        return VM_COLD_BREAK;
    }

    // Null type error
    if (XR_IS_NULL(obj)) {
        VM_COLD_THROW(frame, pc, XR_ERR_NULL_PROPERTY, "null type does not support property access");
    }

    // Struct ref: stored field write or setter method
    if (XR_IS_STRUCT_REF(obj)) {
        uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass**)sptr;

        // Try stored field first
        int fidx = xr_class_lookup_field(scls, prop_symbol);
        if (fidx >= 0 && scls->struct_layout && fidx < scls->struct_layout->field_count) {
            XrStructFieldLayout *sf = &scls->struct_layout->fields[fidx];
            uint8_t *fp = sptr + 8 + sf->offset;
            switch (sf->native_type) {
                case XR_NATIVE_I64:  *(int64_t*)fp  = XR_TO_INT(value); break;
                case XR_NATIVE_F64:  *(double*)fp   = XR_TO_FLOAT(value); break;
                case XR_NATIVE_BOOL: *(uint8_t*)fp  = (uint8_t)value.i; break;
                case XR_NATIVE_I32:  *(int32_t*)fp  = (int32_t)XR_TO_INT(value); break;
                case XR_NATIVE_U32:  *(uint32_t*)fp = (uint32_t)XR_TO_INT(value); break;
                case XR_NATIVE_I16:  *(int16_t*)fp  = (int16_t)XR_TO_INT(value); break;
                case XR_NATIVE_U16:  *(uint16_t*)fp = (uint16_t)XR_TO_INT(value); break;
                case XR_NATIVE_I8:   *(int8_t*)fp   = (int8_t)XR_TO_INT(value); break;
                case XR_NATIVE_U8:   *(uint8_t*)fp  = (uint8_t)XR_TO_INT(value); break;
                case XR_NATIVE_F32:  *(float*)fp    = (float)XR_TO_FLOAT(value); break;
                case XR_NATIVE_STRING: *(XrString**)fp = (XrString*)value.ptr; break;
                default: break;
            }
            return VM_COLD_BREAK;
        }

        // Try setter method: set:<prop_name>
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
        const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        if (prop_name) {
            char setter_name[256];
            snprintf(setter_name, sizeof(setter_name), "set:%s", prop_name);
            int setter_symbol = xr_symbol_register_in_table(sym_table, setter_name);
            XrMethod *setter = (setter_symbol >= 0)
                ? xr_class_lookup_method(scls, setter_symbol) : NULL;
            if (setter && setter->as.closure) {
                XrClosure *closure = setter->as.closure;
                XrProto *proto = closure->proto;
                if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                    VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
                }
                // Place this + value on stack above caller's registers
                int setter_base = (int)(base - vm_ctx->stack) + frame->closure->proto->maxstacksize;
                vm_ctx->stack[setter_base]     = obj; // this
                vm_ctx->stack[setter_base + 1] = value; // argument
                frame->pc = pc;
                int _fidx = vm_ctx->frame_count;
                memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
                vm_ctx->frame_count++;
                XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(proto);
                new_frame->base_offset = setter_base;
                return VM_COLD_STARTFUNC;
            }
        }

        // No field and no setter found
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
            "struct '%s' has no writable field or setter for this property", scls->name);
    }

    // Non-instance type error
    if (!xr_value_is_instance(obj)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
            "only instance, Map, Json and class can set properties");
    }

    return VM_COLD_CONTINUE; // Instance: handled by caller
}

/* ========== Cold Path: OP_SETPROP Instance Setter ========== */

__attribute__((noinline))
int vm_setprop_instance_setter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                       XrInstance *inst, XrValue obj, int prop_symbol,
                                       XrValue value, XrValue *base, int c,
                                       XrBcCallFrame *frame, XrInstruction *pc) {
    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name) return VM_COLD_CONTINUE;

    size_t prop_name_len = strlen(prop_name);
    if (prop_name_len + 5 > XR_MAX_METHOD_NAME_LEN) {
        VM_COLD_THROW(frame, pc, XR_ERR_OVERFLOW, "property name too long");
    }

    char setter_name[XR_MAX_METHOD_NAME_LEN];
    snprintf(setter_name, sizeof(setter_name), "set:%s", prop_name);

    int setter_symbol = xr_symbol_register_in_table(sym_table, setter_name);
    XrMethod *setter = NULL;
    if (setter_symbol >= 0) {
        setter = xr_class_lookup_method(inst->klass, setter_symbol);
    }

    if (!setter) return VM_COLD_CONTINUE;

    XrClosure *closure = setter->as.closure;
    XrProto *proto = closure->proto;

    if (proto->numparams != 2) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT, "setter should have one parameter");
    }

    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
        VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
    }

    int setter_base = c + 1;
    base[setter_base] = obj;
    base[setter_base + 1] = value;

    frame->pc = pc; // savepc

    int _fidx = vm_ctx->frame_count;
    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
    vm_ctx->frame_count++;
    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = (int)((base + setter_base) - vm_ctx->stack);

    return VM_COLD_STARTFUNC;
}

/* ========== Helper: Collect all coroutines from all workers ========== */

// VmCoroEntry and VM_CORO_COLLECT_MAX defined in xvm_cold_paths.h

// Collect coroutines from all workers into a flat array for diagnostic sub-ops.
// Returns the number of entries written. Best-effort snapshot (not atomic).
int vm_collect_all_coros(XrayIsolate *isolate, VmCoroEntry *out, int max_out) {
    XR_DCHECK(isolate != NULL, "vm_collect_all_coros: NULL isolate");
    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) return 0;

    int count = 0;

    // Temporary buffer for steal queue snapshots
    XrCoroutine *snap_buf[256];

    for (int wi = 0; wi < runtime->worker_count && count < max_out; wi++) {
        XrWorker *w = &runtime->workers[wi];

        // 1) Ready coroutines from Chase-Lev deque + overflow
        for (int p = 0; p < XR_RUNQ_COUNT && count < max_out; p++) {
            XrRunQueue *rq = &w->p.runq[p];

            // Deque snapshot
            int n = xr_steal_queue_snapshot(&rq->deque, snap_buf,
                        (max_out - count < 256) ? (max_out - count) : 256);
            for (int i = 0; i < n && count < max_out; i++) {
                out[count].coro = snap_buf[i];
                out[count].state = "ready";
                count++;
            }

            // Overflow list
            XrCoroutine *ov = rq->overflow_first;
            while (ov && count < max_out) {
                out[count].coro = ov;
                out[count].state = "ready";
                count++;
                ov = ov->next;
            }
        }

        // 2) LIFO slot
        XrCoroutine *_ls = atomic_load_explicit(&w->p.lifo_slot, memory_order_relaxed);
        if (_ls && count < max_out) {
            out[count].coro = _ls;
            out[count].state = "ready";
            count++;
        }

        // 3) Blocked coroutines
        XrCoroutine *bc = w->p.blocked_head;
        while (bc && count < max_out) {
            out[count].coro = bc;
            out[count].state = "blocked";
            count++;
            bc = bc->next;
        }
    }

    return count;
}

/* ========== Cold Path: OP_CORO_CTRL Sub-operations ========== */

__attribute__((noinline))
int vm_coro_ctrl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                        XrInstruction instr, XrValue *base) {
    XR_DCHECK(isolate != NULL, "vm_coro_ctrl: NULL isolate");
    XR_DCHECK(base != NULL, "vm_coro_ctrl: NULL base");
    int coro_sub = GETARG_C(instr);
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    switch (coro_sub) {
    case CORO_CTRL_STATS: {
        XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
        if (!runtime) { base[a] = xr_null(); return VM_COLD_BREAK; }

        int blocked_count = 0, ready_count = 0;
        int active_count = xr_runtime_active_coros(runtime);
        uint64_t total_created = 0;
        for (int _si = 0; _si < runtime->worker_count; _si++)
            total_created += runtime->workers[_si].p.stats.spawned_count;

        for (int wi = 0; wi < runtime->worker_count; wi++) {
            XrWorker *w = &runtime->workers[wi];
            blocked_count += w->p.blocked_count;
            for (int p = 0; p < XR_RUNQ_COUNT; p++) {
                ready_count += xr_runq_len(&w->p.runq[p]);
            }
        }

        int total_alive = ready_count + blocked_count + active_count;
        XrMap *result = xr_map_new(COLD_CORO(vm_ctx));
        xr_map_set(result, VM_INTERN_KEY("active"), xr_int(active_count));
        xr_map_set(result, VM_INTERN_KEY("blocked"), xr_int(blocked_count));
        xr_map_set(result, VM_INTERN_KEY("ready"), xr_int(ready_count));
        xr_map_set(result, VM_INTERN_KEY("total"), xr_int(total_alive));
        xr_map_set(result, VM_INTERN_KEY("created"), xr_int((int)total_created));
        base[a] = xr_value_from_map(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_LIST: {
        int limit = 0;
        int state_filter = 0;  // 0=all, 1=ready, 2=blocked

        XrValue limit_val = base[b];
        if (XR_IS_INT(limit_val)) limit = (int)XR_TO_INT(limit_val);
        if (limit <= 0) limit = 100000;

        XrValue state_val = base[a + 1];
        if (XR_IS_INT(state_val)) {
            state_filter = (int)XR_TO_INT(state_val);
        } else if (XR_IS_STRING(state_val)) {
            XrString *s = (XrString *)XR_TO_PTR(state_val);
            if (strcmp(s->data, "ready") == 0) state_filter = 1;
            else if (strcmp(s->data, "blocked") == 0) state_filter = 2;
        }

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        int count = 0;

        for (int i = 0; i < total && count < limit; i++) {
            XrCoroutine *coro = entries[i].coro;
            const char *st = entries[i].state;
            bool is_ready = (strcmp(st, "ready") == 0);
            bool is_blocked = (strcmp(st, "blocked") == 0);

            if (state_filter == 1 && !is_ready) continue;
            if (state_filter == 2 && !is_blocked) continue;

            XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
            xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
            xr_map_set(info, VM_INTERN_KEY("name"),
                       coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());
            xr_map_set(info, VM_INTERN_KEY("state"),
                       xr_string_value(xr_string_intern(isolate, st, strlen(st), 0)));
            if (coro->source_file) {
                char source_buf[XR_MAX_PROPERTY_NAME_LEN];
                snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
                xr_map_set(info, VM_INTERN_KEY("source"),
                           xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
            }
            xr_array_push(result, xr_value_from_map(info));
            count++;
        }

        xr_free(entries);
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_INFO: {
        XrValue coro_val = base[b];
        if (!xr_value_is_coro(coro_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }

        XrCoroutine *coro = xr_value_to_coro(coro_val);
        XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
        uint32_t flags = xr_coro_flags_load(coro);

        xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
        xr_map_set(info, VM_INTERN_KEY("name"),
                   coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());

        const char *state_str = "unknown";
        if (flags & XR_CORO_FLG_DONE) state_str = "done";
        else if (flags & XR_CORO_FLG_BLOCKED) state_str = "blocked";
        else if (flags & XR_CORO_FLG_RUNNING) state_str = "running";
        else if (flags & XR_CORO_FLG_READY) state_str = "ready";
        xr_map_set(info, VM_INTERN_KEY("state"),
                   xr_string_value(xr_string_intern(isolate, state_str, strlen(state_str), 0)));

        xr_map_set(info, VM_INTERN_KEY("priority"), xr_int(xr_coro_get_priority(flags)));
        xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));

        if (coro->source_file) {
            char source_buf[XR_MAX_PROPERTY_NAME_LEN];
            snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
            xr_map_set(info, VM_INTERN_KEY("source"),
                       xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
        }

        struct XrMap *coro_locals = (coro->ext) ? coro->ext->locals : NULL;
        if (coro_locals) {
            xr_map_set(info, VM_INTERN_KEY("locals"), xr_value_from_map(coro_locals));
        } else {
            xr_map_set(info, VM_INTERN_KEY("locals"), xr_value_from_map(xr_map_new(COLD_CORO(vm_ctx))));
        }

        xr_map_set(info, VM_INTERN_KEY("waitCount"), xr_int(atomic_load(&coro->wait_count)));
        xr_map_set(info, VM_INTERN_KEY("cancelled"), xr_bool(flags & XR_CORO_FLG_CANCELLED));

        if (flags & XR_CORO_FLG_DONE) {
            xr_map_set(info, VM_INTERN_KEY("result"), coro->result);
        }
        if (flags & XR_CORO_FLG_BLOCKED) {
            const char *reason = coro->wait_channel ? "channel" : "await";
            xr_map_set(info, VM_INTERN_KEY("blockedOn"),
                       xr_string_value(xr_string_intern(isolate, reason, strlen(reason), 0)));
        }

        base[a] = xr_value_from_map(info);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DUMP: {
        int limit = a; // A = limit
        if (limit == 0) limit = 100;

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        int ready_count = 0, blocked_count = 0;
        for (int i = 0; i < total; i++) {
            if (strcmp(entries[i].state, "ready") == 0) ready_count++;
            else if (strcmp(entries[i].state, "blocked") == 0) blocked_count++;
        }

        printf("┌─────────────────────────────────────────────────────────────────────────┐\n");
        printf("│                     Coroutine Status Snapshot                           │\n");
        printf("├─────────────────────────────────────────────────────────────────────────┤\n");
        printf("│ Stats: Total %-5d | Ready %-4d | Blocked %-4d                        │\n",
               total, ready_count, blocked_count);
        printf("├──────┬────────────────┬─────────┬─────────────────┬─────────────────────┤\n");
        printf("│ ID   │ Name           │ State   │ Block Reason    │ Location            │\n");
        printf("├──────┼────────────────┼─────────┼─────────────────┼─────────────────────┤\n");

        int shown = 0;
        for (int i = 0; i < total && shown < limit; i++) {
            XrCoroutine *coro = entries[i].coro;
            const char *state_upper = (strcmp(entries[i].state, "blocked") == 0) ? "BLOCKED" : "READY";
            const char *block_reason = "-";
            if (strcmp(entries[i].state, "blocked") == 0) {
                block_reason = coro->wait_channel ? "channel" : "await";
            }

            const char *name = coro->name ? coro->name : "(anonymous)";
            char name_buf[15];
            snprintf(name_buf, sizeof(name_buf), "%.14s", name);

            char source_buf[20] = "-";
            if (coro->source_file) {
                const char *fname = strrchr(coro->source_file, '/');
                fname = fname ? fname + 1 : coro->source_file;
                snprintf(source_buf, sizeof(source_buf), "%.12s:%d", fname, coro->source_line);
            }

            printf("│ %-4d │ %-14s │ %-7s │ %-15s │ %-19s │\n",
                   coro->id, name_buf, state_upper, block_reason, source_buf);
            shown++;
        }

        printf("└──────┴────────────────┴─────────┴─────────────────┴─────────────────────┘\n");
        xr_free(entries);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_STALLED: {
        (void)b;
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched) {
            base[a] = xr_value_from_array(xr_array_new(COLD_CORO(vm_ctx)));
            return VM_COLD_BREAK;
        }
        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DEADLOCKS: {
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched) {
            base[a] = xr_value_from_array(xr_array_new(COLD_CORO(vm_ctx)));
            return VM_COLD_BREAK;
        }
        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_TOP: {
        int top_n = 10;
        int metric = 0;  // 0=id, 2=reductions

        XrValue n_val = base[b];
        if (XR_IS_INT(n_val)) {
            top_n = (int)XR_TO_INT(n_val);
            if (top_n <= 0) top_n = 10;
            if (top_n > 1000) top_n = 1000;
        }

        XrValue metric_val = base[a + 1];
        if (XR_IS_STRING(metric_val)) {
            XrString *s = (XrString *)XR_TO_PTR(metric_val);
            if (strcmp(s->data, "reductions") == 0) metric = 2;
            else if (strcmp(s->data, "id") == 0) metric = 0;
        }

        typedef struct { XrCoroutine *coro; const char *state; int64_t value; } TopEntry;
        TopEntry *entries = xr_malloc(sizeof(TopEntry) * VM_CORO_COLLECT_MAX);

        VmCoroEntry *raw = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int count = vm_collect_all_coros(isolate, raw, VM_CORO_COLLECT_MAX);
        for (int i = 0; i < count; i++) {
            entries[i].coro = raw[i].coro;
            entries[i].state = raw[i].state;
            entries[i].value = (metric == 2) ? raw[i].coro->reductions : raw[i].coro->id;
        }
        xr_free(raw);

        // Partial selection sort for top N
        for (int j = 0; j < top_n && j < count; j++) {
            int max_idx = j;
            for (int k = j + 1; k < count; k++) {
                if (entries[k].value > entries[max_idx].value) max_idx = k;
            }
            if (max_idx != j) {
                TopEntry tmp = entries[j];
                entries[j] = entries[max_idx];
                entries[max_idx] = tmp;
            }
        }

        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        int result_count = (top_n < count) ? top_n : count;
        for (int j = 0; j < result_count; j++) {
            XrCoroutine *coro = entries[j].coro;
            XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
            xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
            xr_map_set(info, VM_INTERN_KEY("name"),
                       coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());
            xr_map_set(info, VM_INTERN_KEY("state"),
                       xr_string_value(xr_string_intern(isolate, entries[j].state, strlen(entries[j].state), 0)));
            xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));
            xr_map_set(info, VM_INTERN_KEY("priority"), xr_int(xr_coro_get_priority(xr_coro_flags_load(coro))));
            if (coro->source_file) {
                char source_buf[XR_MAX_PROPERTY_NAME_LEN];
                snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
                xr_map_set(info, VM_INTERN_KEY("source"),
                           xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
            }
            xr_array_push(result, xr_value_from_map(info));
        }

        xr_free(entries);
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_GROUP_BY: {
        int group_by = 0;  // 0=name, 1=state, 2=priority

        XrValue field_val = base[b];
        if (XR_IS_STRING(field_val)) {
            XrString *s = (XrString *)XR_TO_PTR(field_val);
            if (strcmp(s->data, "state") == 0) group_by = 1;
            else if (strcmp(s->data, "priority") == 0) group_by = 2;
        }

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        XrMap *result = xr_map_new(COLD_CORO(vm_ctx));

        for (int i = 0; i < total; i++) {
            XrCoroutine *coro = entries[i].coro;
            const char *key_str;
            char prio_str[16];
            if (group_by == 0) {
                key_str = coro->name ? coro->name : "(anonymous)";
            } else if (group_by == 1) {
                key_str = entries[i].state;
            } else {
                snprintf(prio_str, sizeof(prio_str), "P%d", xr_coro_get_priority(xr_coro_flags_load(coro)));
                key_str = prio_str;
            }
            XrValue key = xr_string_value(xr_string_intern(isolate, key_str, strlen(key_str), 0));
            bool found = false;
            XrValue existing = xr_map_get(result, key, &found);
            if (found && XR_IS_INT(existing)) {
                xr_map_set(result, key, xr_int(XR_TO_INT(existing) + 1));
            } else {
                xr_map_set(result, key, xr_int(1));
            }
        }

        xr_free(entries);
        base[a] = xr_value_from_map(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_WHEREIS: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        XrCoroutine *found = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        base[a] = xr_bool(found != NULL);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_MONITOR: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrChannel *ch = xr_coro_monitor(isolate, sched->coro_registry, name_cstr);
        base[a] = ch ? xr_value_from_channel(ch) : xr_null();
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DEMONITOR: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrValue ch_val = base[a + 1];
        if (!xr_value_is_channel(ch_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrChannel *ch = xr_value_to_channel(ch_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrCoroutine *coro = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        if (coro) {
            xr_coro_demonitor(sched->coro_registry, coro, ch);
        }
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_KILL: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        XrCoroutine *target = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        if (!target || xr_coro_flags_has(target, XR_CORO_FLG_DONE)) {
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        xr_coro_flags_set(target, XR_CORO_FLG_CANCEL_REQUESTED);
        xr_coro_request_yield(target);
        base[a] = xr_bool(true);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_SELF: {
        XrCoroutine *current = COLD_CORO(vm_ctx);
        if (current && current->name) {
            size_t len = strlen(current->name);
            XrString *s = xr_string_intern(isolate, current->name, len, 0);
            base[a] = s ? xr_string_value(s) : xr_null();
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    default:
        return VM_COLD_BREAK;
    }
}

/* ========== Cold Path: OP_GETPROP Type Dispatch ========== */

/*
 * Handles property access for all non-instance types (cold path).
 * Returns VM_COLD_BREAK if property was resolved, VM_COLD_CONTINUE for instance,
 * or VM_COLD_ERROR/VM_COLD_STARTFUNC for error/getter paths.
 */
__attribute__((noinline))
int vm_getprop_type_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                     XrValue obj, int prop_symbol,
                                     XrValue *base, int a, int b,
                                     XrBcCallFrame *frame, XrInstruction *pc) {
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
            base[a] = (tstate == XR_TASK_FAILED) ? task->error : xr_null();
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // Legacy coroutine properties (fallback for old callers)
    if (xr_value_is_coro(obj)) {
        XrCoroutine *coro = xr_value_to_coro(obj);
        if (prop_symbol == SYMBOL_DONE) {
            base[a] = xr_bool(xr_coro_flags_has(coro, XR_CORO_FLG_DONE));
        } else if (prop_symbol == SYMBOL_CANCELLED) {
            base[a] = xr_bool(xr_coro_flags_has(coro, XR_CORO_FLG_CANCELLED));
        } else if (prop_symbol == SYMBOL_RESULT) {
            XrCoroutine *caller = (XrCoroutine *)vm_ctx->current_coro;
            base[a] = xr_deep_copy_to_coro(isolate, coro->result, caller);
        } else if (prop_symbol == SYMBOL_ERROR) {
            XrCoroutine *caller = (XrCoroutine *)vm_ctx->current_coro;
            base[a] = xr_deep_copy_to_coro(isolate, coro->error, caller);
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // Channel properties: ch.length, ch.capacity, ch.isClosed
    if (xr_value_is_channel(obj)) {
        XrChannel *ch = xr_value_to_channel(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer)ch->buf_count);
        } else if (prop_symbol == SYMBOL_CAPACITY) {
            base[a] = xr_int((xr_Integer)ch->buf_size);
        } else if (prop_symbol == SYMBOL_IS_CLOSED) {
            base[a] = xr_bool(xr_channel_is_closed(ch));
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // Enum property access
    if (XR_IS_PTR(obj)) {
        XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(obj);

        if (XR_GC_GET_TYPE(gc) == XR_TENUM_VALUE) {
            XrEnumValue *enum_val = (XrEnumValue*)gc;
            if (prop_symbol == SYMBOL_NAME) {
                size_t len = strlen(enum_val->member_name);
                XrString *str = xr_string_intern(isolate, enum_val->member_name, len, 0);
                base[a] = xr_string_value(str);
                return VM_COLD_BREAK;
            } else if (prop_symbol == SYMBOL_VALUE) {
                base[a] = enum_val->raw_value;
                return VM_COLD_BREAK;
            } else if (prop_symbol == SYMBOL_ORDINAL) {
                base[a] = xr_int(enum_val->member_index);
                return VM_COLD_BREAK;
            }
            // Other enum properties: fall through to instance path
        }

        if (XR_GC_GET_TYPE(gc) == XR_TENUM_TYPE) {
            XrEnumType *enum_type = (XrEnumType*)gc;
            if (prop_symbol == SYMBOL_MEMBER_COUNT) {
                base[a] = xr_int(enum_type->member_count);
                return VM_COLD_BREAK;
            } else if (prop_symbol == SYMBOL_GET_MEMBER) {
                XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_enum_get_member_handler);
                base[a] = xr_value_from_bound_method(bm);
                return VM_COLD_BREAK;
            }
            XrEnumValue *found = xr_enum_get_member_by_symbol(enum_type, prop_symbol);
            if (found) {
                base[a] = XR_FROM_PTR(found);
                return VM_COLD_BREAK;
            }
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Iterator property access
        if (XR_GC_GET_TYPE(gc) == XR_TITERATOR) {
            if (prop_symbol == SYMBOL_HASNEXT) {
                XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_iterator_get_handler(SYMBOL_HASNEXT));
                base[a] = xr_value_from_bound_method(bm);
            } else if (prop_symbol == SYMBOL_NEXT) {
                XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_iterator_get_handler(SYMBOL_NEXT));
                base[a] = xr_value_from_bound_method(bm);
            } else {
                base[a] = xr_null();
            }
            return VM_COLD_BREAK;
        }
    }

    // Range property access
    if (XR_IS_RANGE(obj)) {
        XrRange *rng = xr_value_to_range(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int(xr_range_length(rng));
        } else if (prop_symbol == SYMBOL_TOSTRING) {
            // "Range(start, end)"
            char buf[80];
            snprintf(buf, sizeof(buf), "Range(%lld, %lld)", (long long)rng->start, (long long)rng->end);
            XrString *s = xr_string_intern(isolate, buf, strlen(buf), 0);
            base[a] = xr_string_value(s);
        } else if (prop_symbol == SYMBOL_TO_ARRAY) {
            base[a] = xr_range_to_array(COLD_CORO(vm_ctx), rng);
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // Map property access
    if (XR_IS_MAP(obj)) {
        XrMap *map = XR_TO_MAP(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer)xr_map_size(map));
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(xr_map_is_empty(map));
        } else if (prop_symbol == SYMBOL_KEYS) {
            XrArray *keys = xr_map_keys(COLD_CORO(vm_ctx), map);
            base[a] = keys ? xr_value_from_array(keys) : xr_null();
        } else if (prop_symbol == SYMBOL_VALUES) {
            XrArray *values = xr_map_values(COLD_CORO(vm_ctx), map);
            base[a] = values ? xr_value_from_array(values) : xr_null();
        } else if (prop_symbol == SYMBOL_ENTRIES) {
            XrArray *entries = xr_map_entries(COLD_CORO(vm_ctx), map);
            base[a] = entries ? xr_value_from_array(entries) : xr_null();
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_map_get_handler(SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DELETE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_map_get_handler(SYMBOL_DELETE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_map_get_handler(SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FOREACH) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_map_get_handler(SYMBOL_FOREACH));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "Map does not support dot syntax for key '%s', use map[\"%s\"] or map.get(\"%s\")",
                name ? name : "?", name ? name : "?", name ? name : "?");
        }
        return VM_COLD_BREAK;
    }

    // Set property access
    if (XR_IS_SET(obj)) {
        struct XrSet *set = XR_TO_SET(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer)xr_set_size(set));
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(xr_set_is_empty(set));
        } else if (prop_symbol == SYMBOL_ADD) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_ADD));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DELETE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_DELETE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_UNION) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_UNION));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INTERSECTION) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_INTERSECTION));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_DIFFERENCE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_DIFFERENCE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TO_ARRAY) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_TO_ARRAY));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FOREACH) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_FOREACH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_MAP_METHOD) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_MAP_METHOD));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_FILTER) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_set_get_handler(SYMBOL_FILTER));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // String property access
    if (XR_IS_STRING(obj)) {
        // Fast path: .length/.byteLength use str_data/len directly, no promote
        if (prop_symbol == SYMBOL_LENGTH || prop_symbol == SYMBOL_CHAR_LENGTH || prop_symbol == SYMBOL_CHARS) {
            const char *d = xr_value_str_data(&obj);
            uint32_t bl = xr_value_str_len(&obj);
            base[a] = xr_int((xr_Integer)xr_utf8_strlen(d, bl));
            return VM_COLD_BREAK;
        }
        if (prop_symbol == SYMBOL_BYTE_LENGTH) {
            base[a] = xr_int((xr_Integer)xr_value_str_len(&obj));
            return VM_COLD_BREAK;
        }
        // Slow path: promote SSO to heap for bound method creation
        XrString *str = xr_value_to_string(isolate, obj);
        (void)str;
        if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CHARAT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_CHARAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SUBSTRING) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_SUBSTRING));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INDEXOF) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_INDEXOF));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CONTAINS) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_CONTAINS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_STARTSWITH) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_STARTSWITH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ENDSWITH) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_ENDSWITH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TOLOWERCASE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_TOLOWERCASE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TOUPPERCASE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_TOUPPERCASE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_TRIM) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_TRIM));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SPLIT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_SPLIT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPLACE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_REPLACE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPLACEALL) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_REPLACEALL));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REPEAT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_REPEAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CONCAT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_CONCAT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ITERATOR) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_ITERATOR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CODEPOINT_AT || prop_symbol == SYMBOL_CHARCODEAT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_string_get_handler(SYMBOL_CODEPOINT_AT));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "string has no property '%s'", name ? name : "?");
        }
        return VM_COLD_BREAK;
    }

    // Array property access
    if (XR_IS_ARRAY(obj)) {
        XrArray *array = XR_TO_ARRAY(obj);
        if (prop_symbol == SYMBOL_LENGTH) {
            base[a] = xr_int((xr_Integer)array->length);
        } else if (prop_symbol == SYMBOL_IS_EMPTY) {
            base[a] = xr_bool(array->length == 0);
        } else if (prop_symbol == SYMBOL_KEYS) {
            XrArray *keys = xr_array_with_capacity(COLD_CORO(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                xr_array_push(keys, xr_int(idx));
            }
            base[a] = xr_value_from_array(keys);
        } else if (prop_symbol == SYMBOL_VALUES) {
            XrArray *values = xr_array_with_capacity(COLD_CORO(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                xr_array_push(values, ((XrValue*)array->data)[idx]);
            }
            base[a] = xr_value_from_array(values);
        } else if (prop_symbol == SYMBOL_ENTRIES) {
            XrArray *entries = xr_array_with_capacity(COLD_CORO(vm_ctx), array->length);
            for (int idx = 0; idx < array->length; idx++) {
                XrArray *pair = xr_array_with_capacity(COLD_CORO(vm_ctx), 2);
                xr_array_push(pair, xr_int(idx));
                xr_array_push(pair, ((XrValue*)array->data)[idx]);
                xr_array_push(entries, xr_value_from_array(pair));
            }
            base[a] = xr_value_from_array(entries);
        } else if (prop_symbol == SYMBOL_PUSH) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_PUSH));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_POP) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_POP));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_SHIFT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_SHIFT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_UNSHIFT) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_UNSHIFT));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_INDEXOF) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_INDEXOF));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_HAS) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_HAS));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_JOIN) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_JOIN));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_REVERSE) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_REVERSE));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_CLEAR) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_CLEAR));
            base[a] = xr_value_from_bound_method(bm);
        } else if (prop_symbol == SYMBOL_ITERATOR) {
            XrBoundMethod *bm = xr_bound_method_new(isolate, obj, xr_array_get_handler(SYMBOL_ITERATOR));
            base[a] = xr_value_from_bound_method(bm);
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    // Class object static field access
    if (xr_value_is_class(obj)) {
        XrClass *cls = xr_value_to_class(obj);
        int field_index = xr_class_lookup_field(cls, prop_symbol);
        if (field_index < 0) {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "static field '%s' not found in class '%s'", pname ? pname : "?", cls->name);
        }
        const XrFieldDescriptor *field = xr_class_get_field(cls, field_index);
        if (!field) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: field descriptor not found");
        }
        if (!(field->flags & XR_FIELD_STATIC)) {
            XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
            const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
                "field '%s' is not a static field", pname ? pname : "?");
        }
        int static_field_idx = field->static_slot;
        if (static_field_idx < 0 || static_field_idx >= cls->static_field_count) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "internal error: static field index out of bounds");
        }
        base[a] = cls->static_field_values[static_field_idx];
        return VM_COLD_BREAK;
    }

    // Module export property access
    if (xr_value_is_module(obj)) {
        XrModule *module = xr_value_to_module(obj);
        base[a] = xr_module_get_sym(module, prop_symbol);
        return VM_COLD_BREAK;
    }

    // Json property access
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        base[a] = xr_json_get(isolate, json, prop_symbol);
        return VM_COLD_BREAK;
    }

    // ArraySlice .length property
    if (XR_IS_PTR(obj)) {
        XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(obj);
        if (XR_GC_GET_TYPE(gc) == XR_TARRAY_SLICE && prop_symbol == SYMBOL_LENGTH) {
            XrArraySlice *slice = (XrArraySlice*)gc;
            base[a] = xr_int(xr_array_slice_length(slice));
            return VM_COLD_BREAK;
        }
    }

    // Channel property access error
    if (xr_value_is_channel(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
        const char *name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_PROPERTY,
            "Channel has no '.%s' property, available methods: send(), recv(), trySend(), tryRecv(), close(), isClosed()",
            name ? name : "?");
    }

    // Struct ref: getter/method lookup when field not found in layout
    if (XR_IS_STRUCT_REF(obj)) {
        uint8_t *sptr = (uint8_t*)xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass**)sptr;
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
        const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        if (prop_name) {
            // Try getter method: get:<prop_name>
            char getter_name[256];
            snprintf(getter_name, sizeof(getter_name), "get:%s", prop_name);
            int getter_symbol = xr_symbol_register_in_table(sym_table, getter_name);
            XrMethod *getter = (getter_symbol >= 0)
                ? xr_class_lookup_method(scls, getter_symbol) : NULL;
            if (getter) {
                if (getter->type == XMETHOD_PRIMITIVE) {
                    XrValue args[1] = { obj };
                    base[a] = getter->as.primitive(isolate, args, 1);
                    return VM_COLD_BREAK;
                }
                if (getter->as.closure) {
                    XrClosure *closure = getter->as.closure;
                    XrProto *proto = closure->proto;
                    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                        VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
                    }
                    base[a + 1] = obj; // this = struct_ref
                    frame->pc = pc;
                    int _fidx = vm_ctx->frame_count;
                    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
                    vm_ctx->frame_count++;
                    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
                    new_frame->closure = closure;
                    new_frame->pc = PROTO_CODE_BASE(proto);
                    new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);
                    return VM_COLD_STARTFUNC;
                }
            }
        }
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    // Non-instance type error
    if (!xr_value_is_instance(obj)) {
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
        const char *pname = xr_symbol_get_name_in_table(sym_table, prop_symbol);
        const char *type_name = xr_typeid_name(xr_value_typeid(obj));
        int error_code = XR_IS_NULL(obj) ? XR_ERR_NULL_PROPERTY : XR_ERR_TYPE_NO_PROPERTY;

        int line = 0;
        int current_pc = 0;
        XrProto *proto = frame->closure->proto;
        if (proto) {
            current_pc = (int)(pc - PROTO_CODE_BASE(proto) - 1);
            size_t line_count = PROTO_LINE_COUNT(proto);
            if (line_count > 0) {
                size_t idx = (current_pc < (int)line_count) ? current_pc : line_count - 1;
                line = PROTO_LINE(proto, idx);
            }
        }
        (void)line;

        const char *var_name = xr_vm_get_local_name(proto, b, current_pc);
        if (var_name) {
            VM_COLD_THROW(frame, pc, error_code,
                "variable '%s' has type '%s', does not support property access '.%s'",
                var_name, type_name ? type_name : "unknown", pname ? pname : "?");
        } else {
            VM_COLD_THROW(frame, pc, error_code,
                "type '%s' does not support property access '.%s'",
                type_name ? type_name : "unknown", pname ? pname : "?");
        }
    }

    return VM_COLD_CONTINUE; // Instance: handled by caller inline
}

/* ========== Cold Path: OP_GETPROP Instance Getter ========== */

__attribute__((noinline))
int vm_getprop_instance_getter(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                       XrInstance *inst, XrValue obj, int prop_symbol,
                                       XrValue *base, int a,
                                       XrBcCallFrame *frame, XrInstruction *pc) {
    XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name) return VM_COLD_CONTINUE;

    size_t prop_name_len = strlen(prop_name);
    if (prop_name_len + 5 > 256) {
        VM_COLD_THROW(frame, pc, XR_ERR_OVERFLOW, "property name too long");
    }

    char getter_name[256];
    snprintf(getter_name, sizeof(getter_name), "get:%s", prop_name);

    int getter_symbol = xr_symbol_register_in_table(sym_table, getter_name);
    XrMethod *getter = NULL;
    if (getter_symbol >= 0) {
        getter = xr_class_lookup_method(inst->klass, getter_symbol);
    }

    if (!getter) return VM_COLD_CONTINUE;

    // PRIMITIVE type getter
    if (getter->type == XMETHOD_PRIMITIVE) {
        XrCFunctionPtr cfunc = getter->as.primitive;
        XrValue getter_args[1];
        getter_args[0] = obj;
        base[a] = cfunc(isolate, getter_args, 1);
        return VM_COLD_BREAK;
    }

    // Closure getter: set up call frame
    XrClosure *closure = getter->as.closure;
    XrProto *proto = closure->proto;

    if (proto->numparams != 1) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT, "getter should have no parameters");
    }

    if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
        VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
    }

    base[a + 1] = obj; // this

    frame->pc = pc; // savepc

    int _fidx = vm_ctx->frame_count;
    memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
    vm_ctx->frame_count++;
    XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
    new_frame->closure = closure;
    new_frame->pc = PROTO_CODE_BASE(proto);
    new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);

    return VM_COLD_STARTFUNC;
}

/* ========== Cold Path: OP_INVOKE Module Methods ========== */

__attribute__((noinline))
int vm_invoke_module(XrayIsolate *isolate, XrVMContext *vm_ctx,
                            XrValue receiver, int method_symbol, int nargs,
                            XrValue *base, int a, XrBcCallFrame *frame,
                            XrInstruction *pc) {
    XrModule *module = xr_value_to_module(receiver);
    if (!module || module->export_count == 0) return VM_COLD_CONTINUE;

    XrValue fn_val = xr_module_get_sym(module, method_symbol);
    if (XR_IS_NULL(fn_val)) {
        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT, "module '%s' has no export '%s'",
            module->name ? module->name : "?", _name ? _name : "?");
    }

    if (xr_value_is_cfunction(fn_val)) {
        XrCFunction *cfunc = xr_value_to_cfunction(fn_val);

        if (cfunc->is_yieldable) {
            frame->u.c.result_slot = (int16_t)a;
            frame->u.c.has_cfunc_result = false;

            XrValue result;
            XrCFuncResult status = cfunc->as.yieldable(isolate, &base[a + 2], nargs, &result);

            switch (status) {
                case XR_CFUNC_DONE:
                    base[a] = result;
                    return VM_COLD_BREAK;
                case XR_CFUNC_BLOCKED:
                    frame->pc = pc;
                    return VM_COLD_BLOCKED;
                case XR_CFUNC_YIELD:
                    frame->pc = pc;
                    return VM_COLD_YIELD;
                case XR_CFUNC_CALL_CLOSURE:
                    // Closure frame pushed, return to VM main loop
                    return VM_COLD_STARTFUNC;
                case XR_CFUNC_ERROR:
                    return VM_COLD_FATAL;
            }
        } else {
            XrValue result = cfunc->as.func(isolate, &base[a + 2], nargs);
            base[a] = result;
            return VM_COLD_BREAK;
        }
    } else if (xr_value_is_closure(fn_val)) {
        XrClosure *closure = xr_value_to_closure(fn_val);

        frame->pc = pc; // savepc

        if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
            VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
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
        new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);

        return VM_COLD_STARTFUNC;
    } else if (xr_value_is_class(fn_val)) {
        XrClass *klass = xr_value_to_class(fn_val);

        // Find constructor
        XrSymbolTable *sym_table = (XrSymbolTable*)isolate->symbol_table;
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
            instance = (XrInstance*)xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TINSTANCE);
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
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_CALL,
                "failed to create instance: '%s'", klass->name);
        }
        XrValue inst_val = XR_FROM_PTR(instance);

        if (constructor && constructor->type == XMETHOD_CLOSURE) {
            XrClosure *closure = constructor->as.closure;

            frame->pc = pc; // savepc

            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            base[a + 1] = inst_val; // this

            int _fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[_fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[_fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(closure->proto);
            new_frame->base_offset = (int)((base + a + 1) - vm_ctx->stack);

            return VM_COLD_STARTFUNC;
        }

        // No constructor, return instance directly
        base[a] = inst_val;
        return VM_COLD_BREAK;
    } else {
        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
        const char *_name = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_MOD_NO_EXPORT,
            "module export '%s' is not callable", _name ? _name : "?");
    }
    return VM_COLD_BREAK; // unreachable
}

/* ========== Cold Path: Coroutine Operations ========== */

static inline XrCoroutine *vm_cold_get_coro(XrVMContext *vm_ctx) {
    if (vm_ctx->current_coro) return (XrCoroutine*)vm_ctx->current_coro;
    XrWorker *w = xr_current_worker();
    return w ? (XrCoroutine*)w->m->current_coro : NULL;
}

__attribute__((noinline))
int vm_go(XrayIsolate *isolate, XrVMContext *vm_ctx,
                  XrInstruction instr, XrValue *base,
                  XrBcCallFrame *frame) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);
    XrInstruction *pc = frame->pc;

    XrValue fn_val = base[b];
    if (!xr_value_is_closure(fn_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: expected closure");
    }

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            "go: closure captures non-thread-safe variables, cannot run in coroutine\n"
            "hint: use 'shared const' to declare shared variables, or pass via arguments");
    }
    if (c != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
            "go: argument count mismatch (expected %d, got %d)", proto->numparams, c);
    }

    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    const char *coro_name = NULL;

    XrInstruction next_inst = *pc;
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 1) {
        int name_idx = GETARG_Bx(next_inst);
        XrValue name_val = PROTO_CONSTANT(frame->closure->proto, name_idx);
        if (XR_IS_STRING(name_val))
            coro_name = xr_value_to_string(isolate, name_val)->data;
        pc++;
        next_inst = *pc;
    }

    int coro_priority = 1;
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 2) {
        coro_priority = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }

    XrValue *args = (c > 0) ? &base[b + 1] : NULL;
    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    XrCoroutine *coro = xr_coro_create(isolate, closure, args, c,
                                        coro_name, NULL, 0);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }

    if (coro_priority != 1) {
        uint32_t flags = atomic_load(&coro->flags);
        flags = xr_coro_set_priority_flags(flags, coro_priority);
        atomic_store(&coro->flags, flags);
    }

    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: Runtime not initialized");
    }

    XrCoroutine *parent = vm_cold_get_coro(vm_ctx);
    if (parent && parent->current_scope) {
        coro->parent_scope = parent->current_scope;
        atomic_fetch_add_explicit(&parent->current_scope->count, 1, memory_order_relaxed);
    } else if (runtime->current_scope) {
        coro->parent_scope = runtime->current_scope;
        atomic_fetch_add_explicit(&runtime->current_scope->count, 1, memory_order_relaxed);
    }

    frame->pc = pc;
    // coro_init_common already sets XR_CORO_FLG_READY
    xr_runtime_spawn(runtime, coro);

    base[a] = xr_value_from_coro(coro);
    frame->pc = pc;
    return VM_COLD_BREAK;
}

__attribute__((noinline))
int vm_go_invoke(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    (void)vm_ctx;
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int nargs = GETARG_C(instr);

    XrValue receiver = base[a + 1];
    int method_symbol = PROTO_SYMBOL(frame->closure->proto, b);

    XrValue result = xr_null();

    if (XR_IS_ARRAY(receiver)) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_ARRAY, method_symbol, SYMBOL_BUILTIN_COUNT);
        result = slot ? slot->fn(isolate, receiver, &base[a + 2], nargs)
                      : XR_NOTFOUND;
    } else if (XR_IS_MAP(receiver)) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_MAP, method_symbol, SYMBOL_BUILTIN_COUNT);
        result = slot ? slot->fn(isolate, receiver, &base[a + 2], nargs)
                      : XR_NOTFOUND;
    } else if (XR_IS_STRING(receiver)) {
        XrString *str = xr_value_to_string(isolate, receiver);
        result = string_method_call_by_symbol(isolate, str, method_symbol, &base[a + 2], nargs);
    } else if (XR_IS_PTR(receiver)) {
        uint8_t gc_type = XR_HEAP_TYPE(receiver);
        XrClass *native_class = isolate->native_type_classes[gc_type];
        if (native_class) {
            XrMethod *method = xr_class_lookup_method(native_class, method_symbol);
            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                result = method->as.primitive(isolate, &base[a + 1], nargs + 1);
            } else {
                VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method not found");
            }
        } else {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method call not supported for this type");
        }
    } else {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method call not supported for this type");
    }

    if (unlikely(XR_IS_NOTFOUND(result))) {
        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
            "type '%s' has no method '%s'", xr_typeid_name(xr_value_typeid(receiver)), _mn ? _mn : "?");
    }

    XrCoroutine *coro = (XrCoroutine *)xr_gc_alloc(&isolate->gc, sizeof(XrCoroutine), XR_TCOROUTINE);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }
    memset(&coro->jit_ctx, 0, sizeof(XrCoroutine) - offsetof(XrCoroutine, jit_ctx));
    coro->isolate = isolate;
    xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
    coro->result = result;

    base[a] = xr_value_from_coro(coro);
    return VM_COLD_BREAK;
}

__attribute__((noinline))
int vm_spawn_cont(XrayIsolate *isolate, XrVMContext *vm_ctx,
                          XrInstruction instr, XrValue *base,
                          XrBcCallFrame *frame) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c_raw = GETARG_C(instr);
    // C bit 7: fire-and-forget flag (go used as statement, result never awaited)
    bool fire_and_forget = (c_raw & 0x80) != 0;
    int c = c_raw & 0x7F;  // actual argument count
    XrInstruction *pc = frame->pc;

    XrValue fn_val = base[b];
    if (!xr_value_is_closure(fn_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: expected closure");
    }

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            "go: closure captures non-thread-safe variables");
    }
    if (c != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
            "go: argument count mismatch (expected %d, got %d)", proto->numparams, c);
    }

    // Fast path: skip debug info computation, parse NOP annotations inline
    const char *coro_name = NULL;
    int coro_priority = 1;

    XrInstruction next_inst = *pc;
    int link_mode = 0;

    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 1) {
        int name_idx = GETARG_Bx(next_inst);
        XrValue name_val = PROTO_CONSTANT(frame->closure->proto, name_idx);
        if (XR_IS_STRING(name_val))
            coro_name = xr_value_to_string(isolate, name_val)->data;
        pc++;
        next_inst = *pc;
    }
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 2) {
        coro_priority = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 3) {
        link_mode = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }
    XrValue *args = (c > 0) ? &base[b + 1] : NULL;
    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    XrCoroutine *coro = xr_coro_create(isolate, closure, args, c,
                                        coro_name, NULL, 0);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }

    if (coro_priority != 1) {
        uint32_t flags = atomic_load(&coro->flags);
        flags = xr_coro_set_priority_flags(flags, coro_priority);
        atomic_store(&coro->flags, flags);
    }
    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: Runtime not initialized");
    }

    XrCoroutine *parent = vm_cold_get_coro(vm_ctx);

    // Allocate GC-managed Task handle on executor's heap
    XrTask *task = xr_task_create(parent, coro);
    if (!task) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create task");
    }

    // Store link_mode on task for runtime association
    task->link_mode = (uint8_t)link_mode;

    // Mark fire-and-forget coros as recyclable for deferred recycle
    if (fire_and_forget)
        coro->gc_flags |= XR_CORO_GC_RECYCLABLE;

    // Scope tracking: link coro to scope, register waiter on task
    {
        XrScopeContext *_scope = parent ? parent->current_scope : NULL;
        if (!_scope && runtime->current_scope)
            _scope = runtime->current_scope;
        if (_scope && parent) {
            coro->parent_scope = _scope;
            // Protect child list prepend with the scope spinlock
            while (atomic_exchange_explicit(&_scope->child_lock, true, memory_order_acquire)) {}
            coro->scope_sibling = _scope->first_child;
            _scope->first_child = coro;
            atomic_store_explicit(&_scope->child_lock, false, memory_order_release);
            atomic_fetch_add_explicit(&_scope->count, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&parent->wait_count, 1, memory_order_relaxed);
            task->waiter = parent;
            task->waiter_index = -2;  // scope mode

            // In linked scope, auto-link children for error propagation.
            // Supervisor scope children stay XR_LINK_NONE — errors are collected
            // by the scope itself, no per-child propagation needed.
            if (link_mode == XR_LINK_NONE && _scope->mode == XR_SCOPE_LINKED) {
                task->link_mode = XR_LINK_LINKED;
            }
        }
    }

    /* linked go (standalone, NOT in scope): establish parent-child Task hierarchy.
     * Scope children use scope-based error propagation (first_error + SCOPE_EXIT).
     * Only standalone linked go uses Task hierarchy (fail_with_propagation). */
    if (task->link_mode == XR_LINK_LINKED && parent && parent->task && !coro->parent_scope) {
        xr_task_attach_child(parent->task, task);
    }

    /* monitored go: no auto-Channel here. Codegen only allocates one register
     * for go result, writing base[a+1] would corrupt the stack frame.
     * Users should call task.monitor() to get the notification Channel. */

    base[a] = xr_value_from_task(task);
    frame->pc = pc;

    // coro_init_common already sets XR_CORO_FLG_READY — no redundant atomic OR needed
    if (!parent) {
        xr_runtime_spawn(runtime, coro);
        return VM_COLD_BREAK;
    }

    parent->pending_spawn = coro;
    return VM_COLD_SPAWN_CONT;
}

#define AWAIT_TIMEOUT_SPINS 100000000

/* Recycle completed coro back to pool after await consumes result.
 * Uses recycle_local (thorough reset + pool addition) to prevent
 * memory leak from pool-allocated coros that GC cannot reclaim. */
static inline void vm_await_recycle_coro(XrCoroutine *coro) {
    XrWorker *w = xr_current_worker();
    if (w && xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        xr_coro_recycle_local(w, coro);
    } else {
        xr_coro_release_heap(coro);
    }
}

/* Read task->result with deep copy to dst_coro's heap, then detach + recycle executor.
 * After this call, task->result points to the copied value (safe for re-await). */
static inline XrValue vm_task_consume_result(XrayIsolate *isolate,
                                              XrTask *task,
                                              XrCoroutine *dst_coro,
                                              int discard_result) {
    XrValue res = task->result;
    if (!discard_result && dst_coro && xr_value_needs_copy(res)) {
        res = xr_deep_copy_to_coro(isolate, res, dst_coro);
        task->result = res; // update for re-await safety
    }
    /* Detach executor only — do NOT recycle.
     * Task lives on executor's Immix heap; parent's tasks array still
     * references it. Recycling frees the Immix block, causing
     * use-after-free when parent's GC scans the dangling Task pointer. */
    XrCoroutine *exec = task->coro;
    if (exec) {
        task->coro = NULL;
        exec->task = NULL;
    }
    return discard_result ? xr_null() : res;
}

// Read coro->result with deep copy if needed
static inline XrValue vm_await_read_result(XrayIsolate *isolate,
                                            XrCoroutine *coro,
                                            XrCoroutine *current,
                                            int discard_result) {
    if (discard_result) return xr_null();
    if (!XR_IS_PTR(coro->result)) return coro->result;
    int copy_count = 0;
    XrValue v = xr_deep_copy_to_coro_counted(isolate, coro->result, current, &copy_count);
    if (current && copy_count > 0) current->reductions -= copy_count * 10;
    return v;
}

__attribute__((noinline))
int vm_await(XrayIsolate *isolate, XrVMContext *vm_ctx,
                     XrInstruction instr, XrValue *base,
                     XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int discard_result = GETARG_C(instr);

    XrValue task_val = base[b];

    /* ============ Task path (new: go returns XrTask) ============ */
    if (xr_value_is_task(task_val)) {
        XrTask *task = xr_value_to_task(task_val);

        // Fast path: task already completed — works for re-await too
        XrCoroutine *current = vm_cold_get_coro(vm_ctx);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, current, discard_result);
            return VM_COLD_BREAK;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Slow path: task still active, need to suspend
        XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
        if (!rt) {
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: runtime not initialized");
        }

        // CAS on task->await_state (await coordination lives on the task)
        if (current) {
            __atomic_store_n(&task->waiter_index, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&task->waiter, current, __ATOMIC_RELEASE);

            int expected = XR_AWAIT_NONE;
            if (atomic_compare_exchange_strong_explicit(&task->await_state, &expected,
                                             XR_AWAIT_WAITING,
                                             memory_order_acq_rel, memory_order_acquire)) {
                // Store task in await_task for post-check
                atomic_store_explicit(&current->await_task, task, memory_order_release);
                uint32_t old_flags = xr_coro_flags_load(current);
                uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags,
                                      XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
                atomic_store_explicit(&current->flags, new_flags, memory_order_release);
                frame->pc = pc - 1;
                return VM_COLD_BLOCKED;
            }

            if (expected == XR_AWAIT_WAITING) {
                atomic_store_explicit(&current->await_task, task, memory_order_release);
                uint32_t old_flags2 = xr_coro_flags_load(current);
                uint32_t new_flags2 = xr_coro_set_wait_reason_flags(old_flags2,
                                      XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
                atomic_store_explicit(&current->flags, new_flags2, memory_order_release);
                frame->pc = pc - 1;
                return VM_COLD_BLOCKED;
            }

            // RESOLVED: result already cached in task by executor_complete
            XR_CHECK(expected == XR_AWAIT_RESOLVED, "await: unexpected state, expected RESOLVED");
            __atomic_store_n(&task->waiter, NULL, __ATOMIC_RELAXED);
            base[a] = vm_task_consume_result(isolate, task, current, discard_result);
            atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
            return VM_COLD_BREAK;
        }

        // Main thread: spin wait
        int spin_count = 0;
        int total_spins = 0;
        while (!xr_task_is_done(task)) {
            if (++total_spins > AWAIT_TIMEOUT_SPINS) {
                fprintf(stderr, "[xray] warn: await: task wait timeout\n");
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (!atomic_load(&rt->running)) {
                fprintf(stderr, "[xray] warn: await: runtime stopped\n");
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (++spin_count > 1000) {
                spin_count = 0;
                XrWorker *w = xr_current_worker();
                if (w && w->p.timer_wheel) {
                    int64_t now = xr_monotonic_ticks();
                    xr_bump_timers(w->p.timer_wheel, now);
                }
                sched_yield();
            }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, discard_result);
        return VM_COLD_BREAK;
    }

    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

__attribute__((noinline))
int vm_await_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                             XrInstruction instr, XrValue *base,
                             XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue await_val = base[b];
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val)) timeout_ms = (int64_t)XR_TO_FLOAT(timeout_val);

    XrCoroutine *caller = vm_cold_get_coro(vm_ctx);

    // Task path
    if (xr_value_is_task(await_val)) {
        XrTask *task = xr_value_to_task(await_val);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, caller, 0);
            return VM_COLD_BREAK;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Woken from timeout
        if (caller && xr_coro_resume_load(caller) == XR_RESUME_TIMEOUT) {
            xr_coro_resume_store(caller, XR_RESUME_OK);
            task->waiter = NULL;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Woken from normal completion (read task->await_state)
        if (caller && atomic_load_explicit(&task->await_state, memory_order_acquire) == XR_AWAIT_RESOLVED) {
            base[a] = vm_task_consume_result(isolate, task, caller, 0);
            atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
            if (caller->ext && atomic_load_explicit(&caller->ext->timer_active, memory_order_relaxed)) {
                XrWorker *worker = xr_current_worker();
                if (worker && worker->p.timer_wheel)
                    xr_twheel_cancel_timer(worker->p.timer_wheel, &caller->ext->timer);
                atomic_store_explicit(&caller->ext->timer_active, false, memory_order_relaxed);
            }
            return VM_COLD_BREAK;
        }

        XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
        frame->pc = pc;
        vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

        XrCoroutine *current = vm_cold_get_coro(vm_ctx);
        if (current && rt) {
            __atomic_store_n(&task->waiter_index, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&task->waiter, current, __ATOMIC_RELEASE);
            current->channel_deadline = xr_monotonic_ticks() + timeout_ms;

            XrWorker *worker = xr_current_worker();
            if (worker && timeout_ms > 0)
                xr_worker_add_sleep_timer(worker, current, timeout_ms);

            uint32_t old_flags = xr_coro_flags_load(current);
            uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
            atomic_store(&current->flags, new_flags);

            frame->pc = pc - 1;
            return VM_COLD_BLOCKED;
        }

        // Main thread: synchronous wait
        struct timeval start_time;
        gettimeofday(&start_time, NULL);
        int spin_count = 0;
        while (!xr_task_is_done(task)) {
            struct timeval now;
            gettimeofday(&now, NULL);
            int64_t elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                                 (now.tv_usec - start_time.tv_usec) / 1000;
            if (elapsed_ms >= timeout_ms) {
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            XrWorker *w = xr_current_worker();
            if (w && w->p.timer_wheel) {
                int64_t tnow = xr_monotonic_ticks();
                xr_bump_timers(w->p.timer_wheel, tnow);
            }
            if (++spin_count > 1000) { spin_count = 0; sched_yield(); }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, 0);
        return VM_COLD_BREAK;
    }

    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

__attribute__((noinline))
int vm_await_all(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await.all: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *caller = xr_current_coro(isolate);

    // Task-only helpers
    #define ELEM_IS_TASK(cv) xr_value_is_task(cv)

    // Fast path: check if all done
    bool all_done = true;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!ELEM_IS_TASK(cv)) continue;
        if (!xr_task_is_done(xr_value_to_task(cv))) {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        XrArray *results = xr_array_with_capacity(COLD_CORO(vm_ctx), count);
        results->length = count;
        XrValue *rdata = (XrValue*)results->data;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) { rdata[j] = xr_null(); continue; }
            rdata[j] = vm_task_consume_result(isolate, xr_value_to_task(cv), caller, 0);
        }
        base[a] = xr_value_from_array(results);
        return VM_COLD_BREAK;
    }

    frame->pc = pc - 1;
    vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

    XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
    if (!rt) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "await.all: runtime not initialized");
    }

    if (caller) {
        caller->await_results = NULL;
        atomic_store(&caller->wait_count, 1);

        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) continue;
            atomic_fetch_add(&caller->wait_count, 1);
            XrTask *t = xr_value_to_task(cv);
            __atomic_store_n(&t->waiter_index, j, __ATOMIC_RELAXED);
            __atomic_store_n(&t->waiter, caller, __ATOMIC_RELEASE);
            if (xr_task_is_done(t)) {
                XrCoroutine *w = __atomic_exchange_n(&t->waiter, NULL, __ATOMIC_ACQ_REL);
                if (w == caller)
                    atomic_fetch_sub(&caller->wait_count, 1);
            }
        }

        int remaining = atomic_fetch_sub(&caller->wait_count, 1) - 1;
        if (remaining == 0) {
            return VM_COLD_STARTFUNC;
        }

        uint32_t old_flags = xr_coro_flags_load(caller);
        atomic_store(&caller->flags,
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT));
        return VM_COLD_BLOCKED;
    }

    // Main thread: spin wait
    int total_spins = 0, spin_count = 0;
    for (;;) {
        if (++total_spins > AWAIT_TIMEOUT_SPINS) {
            fprintf(stderr, "[xray] warn: await.all: timeout\n");
            break;
        }
        if (!atomic_load(&rt->running)) break;
        bool ad = true;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) continue;
            if (!xr_task_is_done(xr_value_to_task(cv))) { ad = false; break; }
        }
        if (ad) break;
        if (++spin_count > 1000) { spin_count = 0; sched_yield(); }
    }
    return VM_COLD_STARTFUNC;

    #undef ELEM_IS_TASK
}

__attribute__((noinline))
int vm_await_any(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int mode = GETARG_C(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            mode == 0 ? "await.any: expected array" : "await.anySuccess: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *current = vm_cold_get_coro(vm_ctx);

    // Fast path: check if any already done
    int done_count = 0;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv)) continue;
        XrTask *t = xr_value_to_task(cv);
        if (xr_task_is_done(t)) {
            if (mode == 1) done_count++;
            if (mode == 0 || !XR_IS_STRING(t->error)) {
                base[a] = vm_task_consume_result(isolate, t, current, 0);
                return VM_COLD_BREAK;
            }
        }
    }

    if (mode == 1 && done_count == count) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    // Slow path: wait
    if (current) {
        frame->pc = pc - 1;
        atomic_store(&current->any_done, false);
        atomic_store(&current->wait_count, 1);

        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!xr_value_is_task(cv)) continue;
            atomic_fetch_add(&current->wait_count, 1);
            int widx = (mode == 0) ? -3 : -4;
            XrTask *t = xr_value_to_task(cv);
            __atomic_store_n(&t->waiter_index, widx, __ATOMIC_RELAXED);
            __atomic_store_n(&t->waiter, current, __ATOMIC_RELEASE);
            if (xr_task_is_done(t)) {
                XrCoroutine *w = __atomic_exchange_n(&t->waiter, NULL, __ATOMIC_ACQ_REL);
                if (w == current) {
                    if (mode == 0 || !XR_IS_STRING(t->error)) {
                        bool expected = false;
                        if (atomic_compare_exchange_strong(&current->any_done, &expected, true))
                            current->result = t->result;
                    }
                    atomic_fetch_sub(&current->wait_count, 1);
                }
            }
        }

        int remaining = atomic_fetch_sub(&current->wait_count, 1) - 1;
        if (atomic_load(&current->any_done) || (mode == 1 && remaining == 0)) {
            return VM_COLD_STARTFUNC;
        }

        uint32_t old_flags = xr_coro_flags_load(current);
        atomic_store(&current->flags,
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT_ANY >> XR_CORO_WAIT_SHIFT));
        return VM_COLD_BLOCKED;
    } else {
        // Main thread: poll wait
        int spin = 0;
        while (true) {
            done_count = 0;
            for (int j = 0; j < count; j++) {
                XrValue cv = xr_array_get(tasks, j);
                if (!xr_value_is_task(cv)) continue;
                XrTask *t = xr_value_to_task(cv);
                if (xr_task_is_done(t)) {
                    if (mode == 1) done_count++;
                    if (mode == 0 || !XR_IS_STRING(t->error)) {
                        base[a] = vm_task_consume_result(isolate, t, NULL, 0);
                        return VM_COLD_BREAK;
                    }
                }
            }
            if (mode == 1 && done_count == count) {
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (++spin > 1000) { spin = 0; sched_yield(); }
        }
    }
}

__attribute__((noinline))
int vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx,
                            XrInstruction instr, XrValue *base,
                            XrBcCallFrame *frame, XrInstruction *pc) {
    int base_reg = GETARG_A(instr);
    int ch_count = GETARG_B(instr);
    int case_count = GETARG_C(instr);

    XrCoroutine *coro = vm_cold_get_coro(vm_ctx);
    if (!coro) return VM_COLD_BREAK;

    XrWorker *worker = xr_current_worker();
    if (!worker) return VM_COLD_BREAK;

    void **channels = xr_malloc(ch_count * sizeof(void*));
    XrChannel *timer_ch = NULL;
    int valid_count = 0;
    (void)valid_count;

    for (int ci = 0; ci < ch_count; ci++) {
        XrValue ch_val = base[base_reg + ci];
        if (!xr_value_is_channel(ch_val)) {
            channels[ci] = NULL;
            continue;
        }
        XrChannel *ch = xr_value_to_channel(ch_val);
        channels[ci] = ch;
        valid_count++;
        if (atomic_load(&ch->is_timer)) timer_ch = ch;
    }

    XrSelectWait *sw = xr_malloc(sizeof(XrSelectWait));
    if (!sw) {
        xr_free(channels);
        VM_COLD_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }
    sw->cases = xr_malloc(case_count * sizeof(XrSelectCase));
    if (!sw->cases) {
        xr_free(sw);
        xr_free(channels);
        VM_COLD_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }

    for (int ci = 0; ci < ch_count && ci < case_count; ci++) {
        sw->cases[ci].channel = channels[ci];
        sw->cases[ci].is_send = false;
        sw->cases[ci].result_reg = base_reg + ci;
        sw->cases[ci].bucket_next = NULL;
        sw->cases[ci].owner = coro;
    }
    sw->case_count = ch_count < case_count ? ch_count : case_count;
    sw->timer_channel = timer_ch;
    sw->timer_case_index = -1;
    atomic_store(&sw->triggered, false);

    for (int ci = 0; ci < ch_count; ci++) {
        if (channels[ci] == timer_ch) { sw->timer_case_index = ci; break; }
    }

    coro->select_wait = sw;
    coro->select_ready_case = -1;

    // Arm sleep timer so the worker wakes the coro when the timer
    // channel fires.  The tw_timer callback writes data to the buffer; when
    // the coro re-polls after wakeup, OP_CHAN_TRY_RECV will find it.
    // Clamp remaining to at least 1ms so that xr_bump_timers fires both the
    // tw_timer and the sleep timer on the next tick (handles after 0 case).
    if (timer_ch && !atomic_load_explicit(&timer_ch->timer_fired, memory_order_acquire)) {
        int64_t now_ms = xr_monotonic_ticks();
        int64_t elapsed = now_ms - timer_ch->timer_start_ticks;
        int64_t remaining = timer_ch->timer_timeout_ms - elapsed;
        if (remaining < 1) remaining = 1;
        if (worker->p.timer_wheel) {
            if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
                xr_twheel_cancel_timer(worker->p.timer_wheel, &coro->ext->timer);
                atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            }
            xr_worker_add_sleep_timer(worker, coro, remaining);
        }
    }

    // Notify dist channels about entering select (subscribe for push model)
    XrChannelDistHooks *dhooks = isolate ? isolate->channel_dist_hooks : NULL;
    if (dhooks && dhooks->on_select_enter) {
        for (int ci = 0; ci < ch_count; ci++) {
            if (!channels[ci]) continue;
            XrChannel *dch = (XrChannel *)channels[ci];
            if (dch->dist) {
                dhooks->on_select_enter(dch);
            }
        }
    }

    frame->pc = pc;
    xr_worker_block_select(worker, coro, channels, ch_count);
    xr_free(channels);

    return VM_COLD_BLOCKED;
}

__attribute__((noinline))
int vm_chan_send_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                XrInstruction instr, XrValue *base,
                                XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue value = vm_chan_copy_send(isolate, base[c]);
    XrValue timeout_val = base[c + 1];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val)) timeout_ms = (int64_t)XR_TO_FLOAT(timeout_val);
    if (timeout_ms < 0) timeout_ms = 0;

    // Try immediate send
    if (xr_channel_try_send(ch, value)) {
        base[a] = xr_bool(true);
        xr_runtime_wake_channel(isolate, ch, false);
        return VM_COLD_BREAK;
    }
    if (xr_channel_is_closed(ch)) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }
    if (timeout_ms <= 0) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }

    XrCoroutine *current = vm_cold_get_coro(vm_ctx);
    if (current) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t now_us = (int64_t)now.tv_sec * 1000000LL + now.tv_usec;

        if (current->channel_deadline == 0)
            current->channel_deadline = now_us + timeout_ms * 1000LL;

        if (now_us >= current->channel_deadline) {
            current->channel_deadline = 0;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        if (xr_channel_try_send(ch, value)) {
            current->channel_deadline = 0;
            base[a] = xr_bool(true);
            xr_runtime_wake_channel(isolate, ch, false);
            return VM_COLD_BREAK;
        }
        if (xr_channel_is_closed(ch)) {
            current->channel_deadline = 0;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }

        frame->pc = pc - 1;
        return VM_COLD_YIELD;
    }

    // Main thread: synchronous polling
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
    while (1) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                             (now.tv_usec - start_time.tv_usec) / 1000;
        if (elapsed_ms >= timeout_ms) {
            base[a] = xr_bool(false);
            break;
        }
        if (xr_channel_try_send(ch, value)) {
            base[a] = xr_bool(true);
            xr_runtime_wake_channel(isolate, ch, false);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            base[a] = xr_bool(false);
            break;
        }
        sched_yield();
    }
    return VM_COLD_BREAK;
}

__attribute__((noinline))
int vm_chan_recv_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                XrInstruction instr, XrValue *base,
                                XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val)) timeout_ms = (int64_t)XR_TO_FLOAT(timeout_val);

    // Try immediate receive
    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);
    if (ok) {
        base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
        xr_runtime_wake_channel(isolate, ch, true);
        return VM_COLD_BREAK;
    }
    if (xr_channel_is_closed(ch)) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (timeout_ms <= 0) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    XrCoroutine *current = vm_cold_get_coro(vm_ctx);
    if (current) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t now_us = (int64_t)now.tv_sec * 1000000LL + now.tv_usec;

        if (current->channel_deadline == 0)
            current->channel_deadline = now_us + timeout_ms * 1000LL;

        if (now_us >= current->channel_deadline) {
            current->channel_deadline = 0;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }
        value = xr_channel_try_recv(ch, &ok);
        if (ok) {
            current->channel_deadline = 0;
            base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
            xr_runtime_wake_channel(isolate, ch, true);
            return VM_COLD_BREAK;
        }
        if (xr_channel_is_closed(ch)) {
            current->channel_deadline = 0;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        frame->pc = pc - 1;
        return VM_COLD_YIELD;
    }

    // Main thread: synchronous polling
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
    while (1) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                             (now.tv_usec - start_time.tv_usec) / 1000;
        if (elapsed_ms >= timeout_ms) {
            base[a] = xr_null();
            break;
        }
        value = xr_channel_try_recv(ch, &ok);
        if (ok) {
            base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
            xr_runtime_wake_channel(isolate, ch, true);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            base[a] = xr_null();
            break;
        }
        sched_yield();
    }
    return VM_COLD_BREAK;
}

