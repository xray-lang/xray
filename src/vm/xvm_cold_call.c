/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_cold_call.c - Cold-path implementations for invoke / superinvoke
 *
 * Holds the noinline bodies for OP_INVOKE / OP_SUPERINVOKE cold
 * dispatches. Each function takes the interpreter state as
 * parameters, so this is a normal translation unit (unlike the
 * xvm_dispatch_*.inc.c files included from inside the run()
 * switch). Function declarations live in xvm_cold_paths.h.
 *
 * Owns:
 *   - vm_invoke_channel       (Channel send / recv / try variants)
 *   - vm_invoke_task_handle   (Task .wait / .cancel / ...)
 *   - vm_invoke_coro_handle   (Coroutine handle methods)
 *   - vm_invoke_enum          (Enum.values / Enum.fromValue / ...)
 *   - vm_invoke_class         (Class constructor / static method)
 *   - vm_superinvoke          (super.method() dispatch)
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

XR_NOINLINE int vm_invoke_channel(XrayIsolate *isolate, XrVMContext *vm_ctx, XrChannel *ch,
                                  int method_symbol, int nargs, XrValue *base, int a,
                                  XrBcCallFrame *frame, XrInstruction *pc) {
    XR_DCHECK(isolate != NULL, "vm_invoke_channel: NULL isolate");
    XR_DCHECK(ch != NULL, "vm_invoke_channel: NULL channel");
    XR_DCHECK(base != NULL, "vm_invoke_channel: NULL base");
    // ch.trySend(value) — unified helper
    if (nargs == 1 && method_symbol == SYMBOL_TRYSEND) {
        base[a] = xr_bool(xr_chan_try_send(isolate, ch, base[a + 2]));
        return VM_COLD_BREAK;
    }

    // ch.tryRecv() → (value, ok) multi-return — unified helper
    if (nargs == 0 && method_symbol == SYMBOL_TRYRECV) {
        XrCoroutine *recv_coro = vm_ctx ? (XrCoroutine *) vm_ctx->current_coro : NULL;
        XrValue recv_val;
        bool recv_ok = xr_chan_try_recv(isolate, ch, &recv_val, recv_coro);
        base[a] = recv_val;
        base[a + 1] = xr_bool(recv_ok);
        return VM_COLD_BREAK;
    }

    // ch.send(value) - blocking send
    if (nargs == 1 && method_symbol == SYMBOL_SEND) {
        XrCoroutine *current = (XrCoroutine *) vm_ctx->current_coro;
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }
        XrValue send_v = vm_chan_copy_send(isolate, base[a + 2]);
        // Pre-save frame — see hot path comment.
        if (current)
            current->send_value = send_v;
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
        XrCoroutine *current = (XrCoroutine *) vm_ctx->current_coro;
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            base[a] = vm_chan_copy_recv(isolate, base[a], vm_ctx);  // Deep copy recv_slot value
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
        XrCoroutine *current = (XrCoroutine *) vm_ctx->current_coro;
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
        XrCoroutine *current = (XrCoroutine *) vm_ctx->current_coro;
        int64_t timeout_ms = XR_TO_INT(base[a + 2]);

        // Check if woken from timeout
        if (current && xr_coro_resume_load(current) == XR_RESUME_TIMEOUT) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            base[a + 1] = xr_bool(true);  // Value already in recv_slot
            return VM_COLD_BREAK;
        }
        if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL_CLOSED) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
            return VM_COLD_BREAK;
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
            base[a + 1] = xr_bool(true);
            return VM_COLD_BREAK;
        } else if (result == XR_CHAN_CLOSED) {
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
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
        base[a + 1] = xr_bool(false);
        return VM_COLD_BREAK;
    }

    // toString fallback for Channel
    if (method_symbol == SYMBOL_TOSTRING) {
        base[a] = xr_string_value(xr_value_to_string(isolate, base[a + 1]));
        return VM_COLD_BREAK;
    }

    // Unknown method
    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
    const char *method_name = xr_symbol_get_name_in_table(sym_table, method_symbol);
    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
                  "Channel has no method '%s', available: send(), recv(), trySend(), tryRecv(), "
                  "sendTimeout(), recvTimeout(), close(), isClosed()",
                  method_name ? method_name : "?");
}

/* ========== Cold Path: OP_INVOKE Task Handle ========== */

/*
 * Task-level method dispatch — works even after executor detach (coro=NULL).
 * cancel(): if executor alive and task pending, cancel it; otherwise no-op.
 * toString(): returns string representation.
 */
XR_NOINLINE int vm_invoke_task_handle(XrayIsolate *isolate, XrValue receiver, int method_symbol,
                                      int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
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
            // Task already finished: send the task itself immediately and
            // close the single-shot monitor channel so create/close counters
            // stay balanced (parity with xr_task_fire_completion).
            xr_channel_notify_send(ch, xr_value_from_task(task));
            xr_channel_close(ch);
        } else {
            // Task still running: register completion listener
            XrCompletionNode *cn = (XrCompletionNode *) xr_calloc(1, sizeof(XrCompletionNode));
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
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "task.link: argument must be a Task");
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
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "task.unlink: argument must be a Task");
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
    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "task handle does not support this method");
}

/* ========== Cold Path: OP_INVOKE Coroutine Handle ========== */

XR_NOINLINE int vm_invoke_coro_handle(XrayIsolate *isolate, XrValue receiver, int method_symbol,
                                      int nargs, XrValue *base, int a, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
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

XR_NOINLINE int vm_invoke_enum(XrayIsolate *isolate, XrValue receiver, int method_symbol, int nargs,
                               XrValue *base, int a, XrBcCallFrame *frame, XrInstruction *pc) {
    if (!XR_IS_PTR(receiver))
        return VM_COLD_CONTINUE;
    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);

    if (XR_GC_GET_TYPE(gc) == XR_TENUM_VALUE) {
        XrEnumValue *enum_val = (XrEnumValue *) gc;

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
            snprintf(buffer, sizeof(buffer), "%s.%s", enum_val->enum_name, enum_val->member_name);
            size_t len = strlen(buffer);
            XrString *str = xr_string_intern(isolate, buffer, len, 0);
            base[a] = xr_string_value(str);
            return VM_COLD_BREAK;
        }
        // If no match, fall through to class/instance path
        return VM_COLD_CONTINUE;
    }

    if (XR_GC_GET_TYPE(gc) == XR_TENUM_TYPE) {
        XrEnumType *enum_type = (XrEnumType *) gc;

        if (nargs == 1 && method_symbol == SYMBOL_GET_MEMBER) {
            XrValue index_val = base[a + 2];
            if (XR_IS_INT(index_val)) {
                int index = XR_TO_INT(index_val);
                if (index >= 0 && index < (int) enum_type->member_count) {
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
XR_NOINLINE int vm_invoke_class(XrayIsolate *isolate, XrVMContext *vm_ctx, XrValue receiver,
                                int method_symbol, const char *method_name_chars, int nargs,
                                XrValue *base, int a, XrBcCallFrame *frame, XrInstruction *pc,
                                int is_tail) {
    XR_DCHECK(isolate != NULL, "vm_invoke_class: NULL isolate");
    XR_DCHECK(base != NULL, "vm_invoke_class: NULL base");
    XR_DCHECK(method_name_chars != NULL, "vm_invoke_class: NULL method_name");
    XrClass *cls = xr_value_to_class(receiver);

    if (strcmp(method_name_chars, XR_KEYWORD_CONSTRUCTOR) == 0) {
        if (!xr_class_can_instantiate(cls)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_CALL, "cannot instantiate abstract class '%s'",
                          cls->name);
        }

        XrInstance *inst = xr_instance_new(isolate, cls);
        XrValue inst_val = xr_value_from_instance(inst);

        // Find constructor via inline cache (per-ctx, lazily ensured by hot dispatcher)
        size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
        XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
        XrICMethodTable *ic_methods = xr_vm_ctx_get_ic_methods(vm_ctx, frame->closure->proto);
        XrICMethod *cache = xr_ic_method_table_get(ic_methods, cache_index);
        if (cache) {
            XR_VM_IC_METHOD_BIND(cache, (int) cache_index);
        }

        XrMethod *ctor = cache ? xr_ic_method_lookup(cache, cls, method_symbol)
                               : xr_class_lookup_method(cls, method_symbol);

        // Primitive constructor (Map, Array, etc.)
        if (ctor != NULL && ctor->type == XMETHOD_PRIMITIVE && ctor->as.primitive != NULL) {
            XrValue result = ctor->as.primitive(isolate, inst_val, &base[a + 2], nargs);
            base[a] = result;
            return VM_COLD_BREAK;
        }

        // Closure constructor
        if (ctor != NULL && ctor->type == XMETHOD_CLOSURE && ctor->as.closure != NULL) {
            XrClosure *closure = ctor->as.closure;
            XrProto *proto = closure->proto;

            if (nargs + 1 != proto->numparams) {
                VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
                              "constructor expects %d arguments, got %d", proto->numparams - 1,
                              nargs);
            }
            if (vm_ctx->frame_count >= XR_FRAMES_MAX) {
                VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "stack overflow");
            }

            base[a + 1] = inst_val;  // this

            frame->pc = pc;  // savepc

            int fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);

            return VM_COLD_STARTFUNC;
        }

        // No constructor, return instance directly
        base[a] = inst_val;
        return VM_COLD_BREAK;
    } else {
        // Static method call
        XrMethod *method = xr_class_lookup_method(cls, method_symbol);

        if (method == NULL || !(method->flags & XMETHOD_FLAG_STATIC)) {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
                          "static method '%s' not found on class '%s'", method_name_chars,
                          cls->name);
        }

        if (method->type == XMETHOD_PRIMITIVE && method->as.primitive != NULL) {
            XrValue result = method->as.primitive(isolate, base[a + 1], &base[a + 2], nargs);
            base[a] = result;
            /* Check if the builtin raised an exception (e.g. Json.stringify
             * on non-serializable types calls xr_vm_unwind_with_trace). */
            if (!XR_IS_NULL(vm_ctx->current_exception)) {
                return VM_COLD_ERROR;
            }
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

            frame->pc = pc;  // savepc

            int fidx = vm_ctx->frame_count;
            memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
            vm_ctx->frame_count++;
            XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
            new_frame->closure = closure;
            new_frame->pc = PROTO_CODE_BASE(proto);
            new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);

            return VM_COLD_STARTFUNC;
        } else {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "invalid static method '%s'",
                          method_name_chars);
        }
    }
}

/* ========== Cold Path: OP_SUPERINVOKE ========== */

/*
 * Handles the entire OP_SUPERINVOKE: constructor :super() and super.method() calls.
 * Returns VM_COLD_STARTFUNC on success, VM_COLD_ERROR on error.
 */
XR_NOINLINE int vm_superinvoke(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                               XrValue *base, XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int nargs = GETARG_C(instr);

    bool is_ctor_call = (a == 0);
    XrValue this_val = is_ctor_call ? base[0] : base[a + 1];

    if (!xr_value_is_instance(this_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                      "super can only be called on class instance");
    }

    XrInstance *inst_obj = xr_value_to_instance(this_val);
    XrClass *inst_class = inst_obj->klass;

    XrClass *current_method_class;
    if (isolate->vm.ctor_call_depth > 0) {
        current_method_class =
            (XrClass *) isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth - 1].class_ptr;
    } else {
        current_method_class = inst_class;
    }

    if (current_method_class == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "cannot determine current method's class");
    }

    XrClass *super_class = current_method_class->super;
    if (super_class == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "class '%s' has no superclass",
                      current_method_class->name);
    }

    XrValue method_name_val = PROTO_CONSTANT(frame->closure->proto, b);
    if (!XR_IS_STRING(method_name_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "invalid method name in super call");
    }

    const char *method_name = xr_value_str_data(&method_name_val);

    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->symbol_table;
    int method_symbol = xr_symbol_register_in_table(sym_table, method_name);

    XrMethod *method = xr_class_lookup_method(super_class, method_symbol);

    if (method == NULL || method->type == XMETHOD_NONE) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
                      "superclass method '%s' not found in class '%s'", method_name,
                      super_class->name);
    }

    if (method->type != XMETHOD_CLOSURE || method->as.closure == NULL) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "superclass method '%s' has invalid type",
                      method_name);
    }

    XrClosure *closure = method->as.closure;
    XrProto *proto = closure->proto;

    if (nargs + 1 != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
                      "superclass method '%s' expects %d arguments but got %d", method_name,
                      proto->numparams - 1, nargs);
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

        frame->pc = pc;  // savepc

        int fidx = vm_ctx->frame_count;
        memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
        vm_ctx->frame_count++;
        XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(proto);
        new_frame->base_offset = (int) ((base + call_base_offset) - vm_ctx->stack);
    } else {
        if (isolate->vm.ctor_call_depth >= XR_CTOR_CALL_STACK_MAX) {
            VM_COLD_THROW(frame, pc, XR_ERR_STACK_OVERFLOW, "super call depth exceeded");
        }
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].class_ptr = super_class;
        isolate->vm.ctor_call_stack[isolate->vm.ctor_call_depth].frame_count = vm_ctx->frame_count;
        isolate->vm.ctor_call_depth++;

        frame->pc = pc;  // savepc

        int fidx = vm_ctx->frame_count;
        memset(&vm_ctx->frames[fidx], 0, sizeof(XrBcCallFrame));
        vm_ctx->frame_count++;
        XrBcCallFrame *new_frame = &vm_ctx->frames[fidx];
        new_frame->closure = closure;
        new_frame->pc = PROTO_CODE_BASE(proto);
        new_frame->base_offset = (int) ((base + a + 1) - vm_ctx->stack);
    }

    return VM_COLD_STARTFUNC;
}
