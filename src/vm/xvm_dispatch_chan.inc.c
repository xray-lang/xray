/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_chan.inc.c — channel opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, frame, ci, base, R, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_DISPATCH_COLD, VM_CURRENT_CORO,
 * TRACE_EXECUTION, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns the OP_CHAN_* family plus the OP_SELECT_* placeholder
 * stubs. The two timeout variants delegate straight to cold-path
 * helpers (vm_chan_send_timeout / vm_chan_recv_timeout).
 */

vmcase(OP_CHAN_NEW) {
    /* R[A] = Channel(Bx) - create Channel (GC managed)
     * Bx = buffer size (18 bits, supports 0~262143)
     */
    int a = GETARG_A(i);
    int buffer_size = GETARG_Bx(i);

    // Create GC-managed Channel
    XrChannel *ch = xr_channel_new(isolate, (uint32_t)buffer_size);
    if (!ch) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
    }

    // Store directly as Channel value
    R(a) = xr_value_from_channel(ch);
    vmbreak;
}

vmcase(OP_CHAN_NEW_NAMED) {
    /* R[A] = Channel(R[B], R[C]) - Named Channel
     * R[B] = buffer size (int)
     * R[C] = channel name (string)
     * If cluster is running, registers as Named Channel.
     * Otherwise creates a normal local channel.
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    uint32_t buf_size = 0;
    if (XR_IS_INT(R(b))) {
        int64_t v = XR_TO_INT(R(b));
        if (v > 0 && v <= 262143) buf_size = (uint32_t)v;
    }

    // Check for existing Named Channel (e.g. Proxy from CHANNEL_SYNC)
#ifdef XR_HAS_CLUSTER
    if (XR_IS_STRING(R(c))) {
        if (xr_cluster_is_running()) {
            XrChannel *existing_ch = xr_cluster_find_channel_local(
                XR_TO_STRING(R(c))->data);
            if (existing_ch) {
                R(a) = xr_value_from_channel(existing_ch);
                vmbreak;
            }
        }
    }
#endif

    XrChannel *ch = xr_channel_new(isolate, buf_size);
    if (!ch) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
    }

    // Register as Named Channel if cluster is running and name is string
    if (XR_IS_STRING(R(c))) {
        XrString *name_str = XR_TO_STRING(R(c));
#ifdef XR_HAS_CLUSTER
        if (xr_cluster_is_running()) {
            xr_cluster_register_channel(name_str->data, ch);
        }
#else
        (void)name_str;
#endif
    }

    R(a) = xr_value_from_channel(ch);
    vmbreak;
}

vmcase(OP_CHAN_SEND) {
    /* R[B].send(R[C]) - blocking send to Channel
     * A = result register (null on success)
     * B = Channel
     * C = value to send
     */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    // Check if resumed from blocking
    XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
    if (current && xr_coro_resume_load(current) == XR_RESUME_CHANNEL) {
        xr_coro_resume_store(current, XR_RESUME_OK);
        R(a) = xr_null();
        vmbreak;
    }

    // Get Channel
    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_null();
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "send: expected Channel");
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Deep copy mutable values for buffer safety
    XrValue send_v = vm_chan_copy_send(isolate, R(c));

    // Pre-save frame
    if (current) current->send_value = send_v;
    savepc();
    frame->pc = pc - 1;
    frame->call_status |= XR_CALL_YIELDED;
    // Blocking send
    XrChanResult result = xr_channel_send(ch, send_v, current);
    if (result == XR_CHAN_OK) {
        frame->call_status &= ~XR_CALL_YIELDED;
        R(a) = xr_null();
        vmbreak;
    } else if (result == XR_CHAN_CLOSED) {
        frame->call_status &= ~XR_CALL_YIELDED;
        VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel is closed");
    } else if (result == XR_CHAN_BLOCK) {
        return XR_VM_BLOCKED;
    } else {
        frame->call_status &= ~XR_CALL_YIELDED;
        VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "Channel send failed");
    }
}

vmcase(OP_CHAN_RECV) {
    /* R[A], R[A+1] = R[B].recv() - receive from Channel (blocking), returns multi-value
     * R[A] = received value
     * R[A+1] = success (bool)
     * B = Channel
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);

    // Check if resumed from blocking (cache resume_load: 1 atomic instead of 2)
    XrCoroutine *current = (XrCoroutine *)VM_CURRENT_CORO;
    if (current) {
        int _rs = xr_coro_resume_load(current);
        if (_rs == XR_RESUME_CHANNEL) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
            R(a) = vm_chan_copy_recv(isolate, R(a), vm_ctx);
            R(a + 1) = xr_bool(true);
            vmbreak;
        }
        if (_rs == XR_RESUME_CHANNEL_CLOSED) {
            xr_coro_resume_store(current, XR_RESUME_OK);
            current->wait_channel = NULL;
        }
    }

    // Get Channel directly
    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_null();
        R(a + 1) = xr_bool(false);
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "recv: expected Channel");
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Set recv_slot before recv — see hot path comment for rationale
    if (current) current->recv_slot = &R(a);
    // Pre-save frame
    savepc();
    frame->pc = pc - 1;
    frame->call_status |= XR_CALL_YIELDED;
    XrValue value;
    XrChanResult result = xr_channel_recv(ch, &value, current);
    if (result == XR_CHAN_OK) {
        frame->call_status &= ~XR_CALL_YIELDED;
        R(a) = vm_chan_copy_recv(isolate, value, vm_ctx);
        R(a + 1) = xr_bool(true);
        vmbreak;
    } else if (result == XR_CHAN_CLOSED) {
        frame->call_status &= ~XR_CALL_YIELDED;
        R(a) = xr_null();
        R(a + 1) = xr_bool(false);
        vmbreak;
    } else if (result == XR_CHAN_BLOCK) {
        return XR_VM_BLOCKED;
    } else {
        frame->call_status &= ~XR_CALL_YIELDED;
        R(a) = xr_null();
        R(a + 1) = xr_bool(false);
        VM_RUNTIME_ERROR(XR_ERR_CORO_DEAD, "recv: need to use blocking recv in coroutine");
    }
}

vmcase(OP_CHAN_TRY_SEND) {
    /* R[A] = R[B].trySend(R[C]) - non-blocking send
     * A = result (bool)
     * B = Channel
     * C = value to send
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    // Get Channel directly
    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_bool(false);
        vmbreak;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Non-blocking send (deep copy mutable values for buffer safety)
    XrValue _send_v = vm_chan_copy_send(isolate, R(c));
    bool success = xr_channel_try_send(ch, _send_v);
    R(a) = xr_bool(success);

    // Send succeeded, wake waiting receivers
    if (success) {
        xr_runtime_wake_channel(isolate, ch, false); // Wake receivers
    }
    vmbreak;
}

vmcase(OP_CHAN_TRY_RECV) {
    /* R[A], R[A+1] = R[B].tryRecv() - non-blocking receive, returns multi-value
     * R[A] = received value (null on failure)
     * R[A+1] = success (bool)
     * B = Channel
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);

    // Get Channel directly
    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_null();
        R(a + 1) = xr_bool(false);
        vmbreak;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Timer channel no longer needs polling here.
    // Timer wheel callback writes data to buffer; try_recv finds it.

    // Non-blocking receive
    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);

    // Unbuffered Channel rendezvous: try to wake sender from Runtime queue
    if (!ok) {
        XrCoroutine *sender = xr_runtime_wake_channel(isolate, ch, true);
        if (sender) {
            value = sender->send_value;
            ok = true;
        }
    }

    // Return multi-value: value and ok
    R(a) = ok ? vm_chan_copy_recv(isolate, value, vm_ctx) : xr_null();
    R(a + 1) = xr_bool(ok);

    // Receive succeeded, wake waiting senders
    if (ok) {
        xr_runtime_wake_channel(isolate, ch, true); // Wake senders
    }
    vmbreak;
}

vmcase(OP_CHAN_SEND_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_chan_send_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_CHAN_RECV_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_chan_recv_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_CHAN_CLOSE) {
    // R[A].close() - close Channel
    int a = GETARG_A(i);

    // Get Channel directly
    XrValue ch_val = R(a);
    if (!xr_value_is_channel(ch_val)) {
        vmbreak; // Silently ignore non-Channel
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Close Channel
    xr_channel_close(ch);

    // Wake all waiting coroutines
    xr_runtime_wake_channel_all(isolate, ch);
    vmbreak;
}

vmcase(OP_CHAN_IS_CLOSED) {
    // R[A] = R[B].isClosed() - check if Channel is closed
    int a = GETARG_A(i);
    int b = GETARG_B(i);

    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_bool(false);
        vmbreak;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    R(a) = xr_bool(xr_channel_is_closed(ch));
    vmbreak;
}

/* === Select multiplexing (placeholder) === */

vmcase(OP_SELECT_START) {
    // Start select block
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select not yet implemented");
}

vmcase(OP_SELECT_CASE) {
    // Add select case
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select case not yet implemented");
}

vmcase(OP_SELECT_END) {
    // Execute select
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "select end not yet implemented");
}
