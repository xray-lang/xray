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
 * VM_RUNTIME_ERROR, VM_DISPATCH, VM_CURRENT_CORO,
 * TRACE_EXECUTION, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns the OP_CHAN_* family. The two timeout variants delegate to
 * dispatch helpers in xvm_chan_ops.c (vm_chan_send_timeout /
 * vm_chan_recv_timeout).
 */

vmcase(OP_CHAN_NEW) {
    /* R[A] = Channel(Bx) - create Channel (GC managed)
     * Bx = buffer size (18 bits, supports 0~262143)
     */
    int a = GETARG_A(i);
    int buffer_size = GETARG_Bx(i);

    // Create GC-managed Channel
    XrChannel *ch = xr_channel_new(isolate, (uint32_t) buffer_size);
    if (!ch) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
    }

    // Store directly as Channel value
    R(a) = xr_value_from_channel(ch);
    vmbreak;
}

vmcase(OP_CHAN_NEW_CAP) {
    /* R[A] = Channel(R[B]) - create Channel with runtime capacity. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int elem_tid = GETARG_C(i);

    uint32_t buffer_size = 0;
    if (XR_IS_INT(R(b))) {
        int64_t v = XR_TO_INT(R(b));
        if (v > 0 && (uint64_t) v <= MAXARG_Bx)
            buffer_size = (uint32_t) v;
    }

    XrChannel *ch = xr_channel_new(isolate, buffer_size);
    if (!ch) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Channel creation failed");
    }
    ch->elem_tid = (uint8_t) elem_tid;

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
        if (v > 0 && (uint64_t) v <= MAXARG_Bx)
            buf_size = (uint32_t) v;
    }

    // Check for existing Named Channel (e.g. Proxy from CHANNEL_SYNC)
#ifdef XR_HAS_CLUSTER
    if (XR_IS_STRING(R(c))) {
        if (xr_cluster_is_running()) {
            XrChannel *existing_ch = xr_cluster_find_channel_local(XR_TO_STRING(R(c))->data);
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
        (void) name_str;
#endif
    }

    R(a) = xr_value_from_channel(ch);
    vmbreak;
}

vmcase(OP_CHAN_SEND) {
    TRACE_EXECUTION();
    /* Inline buffered fast path: trylock hit + free slot + no waiters.
     * Flattens the three-call helper chain for the dominant buffered case.
     * Falls through to the out-of-line helper (the semantic authority) for
     * every other shape: rendezvous, full buffer, waiting receiver, select
     * waiters, deep-copy values, resume replay, dist channels, sched-stats
     * runs (which need the instrumented counters). */
    do {
        XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
        if (!_cur || xr_coro_resume_load(_cur) != 0)
            break;
        XrValue _chv = base[GETARG_B(i)];
        if (!xr_value_is_channel(_chv))
            break;
        XrChannel *_ch = xr_value_to_channel(_chv);
        if (_ch->buf_size == 0 || _ch->dist ||
            atomic_load_explicit(&_ch->is_timer, memory_order_relaxed))
            break;
        XrRuntime *_rt = (XrRuntime *) isolate->scheduler_runtime;
        if (XR_UNLIKELY(_rt && _rt->sched_stats_enabled))
            break;
        XrValue _v = base[GETARG_C(i)];
        if (XR_IS_PTR(_v) && xr_value_needs_copy(_v))
            break;
        if (!xr_amutex_trylock(&_ch->lock))
            break;
        if (XR_UNLIKELY(atomic_load_explicit(&_ch->closed, memory_order_relaxed) ||
                        _ch->recvq.first != NULL || _ch->buf_count >= _ch->buf_size ||
                        xr_channel_select_waiter_mask(_ch) != 0)) {
            xr_amutex_unlock(&_ch->lock);
            break;
        }
        _ch->buffer[_ch->send_idx] = _v;
        _ch->send_idx = _ch->send_idx + 1 >= _ch->buf_size ? 0 : _ch->send_idx + 1;
        _ch->buf_count++;
        xr_amutex_unlock(&_ch->lock);
        base[GETARG_A(i)] = xr_null();
        if (XR_UNLIKELY(xr_coro_consume_reds(_cur, XR_VM_CHAN_READY_REDUCTION_COST) <= 0) && _rt) {
            xr_coro_set_reds(_cur, XR_CORO_REDUCTIONS);
            frame->pc = pc;
            return XR_VM_YIELD;
        }
        vmbreak;
    } while (0);
    VM_DISPATCH(vm_chan_send(isolate, vm_ctx, i, base, frame, pc));
}

vmcase(OP_CHAN_RECV) {
    /* Inline buffered fast path: mirror of the OP_CHAN_SEND fast path.
     * Buffered pop with no blocked senders; everything else falls through
     * to the out-of-line helper. */
    do {
        XrCoroutine *_cur = (XrCoroutine *) VM_CURRENT_CORO;
        if (!_cur || xr_coro_resume_load(_cur) != 0)
            break;
        XrValue _chv = base[GETARG_B(i)];
        if (!xr_value_is_channel(_chv))
            break;
        XrChannel *_ch = xr_value_to_channel(_chv);
        if (_ch->buf_size == 0 || _ch->dist ||
            atomic_load_explicit(&_ch->is_timer, memory_order_relaxed))
            break;
        XrRuntime *_rt = (XrRuntime *) isolate->scheduler_runtime;
        if (XR_UNLIKELY(_rt && _rt->sched_stats_enabled))
            break;
        if (!xr_amutex_trylock(&_ch->lock))
            break;
        if (XR_UNLIKELY(_ch->sendq.first != NULL || _ch->buf_count == 0)) {
            xr_amutex_unlock(&_ch->lock);
            break;
        }
        XrValue _v = _ch->buffer[_ch->recv_idx];
        _ch->buffer[_ch->recv_idx] = xr_null();
        _ch->recv_idx = _ch->recv_idx + 1 >= _ch->buf_size ? 0 : _ch->recv_idx + 1;
        _ch->buf_count--;
        xr_amutex_unlock(&_ch->lock);
        _v = xr_chan_copy_recv(isolate, _v, _cur);
        base[GETARG_A(i)] = _v;
        base[GETARG_A(i) + 1] = xr_bool(true);
        if (XR_UNLIKELY(xr_coro_consume_reds(_cur, XR_VM_CHAN_READY_REDUCTION_COST) <= 0) && _rt) {
            xr_coro_set_reds(_cur, XR_CORO_REDUCTIONS);
            frame->pc = pc;
            return XR_VM_YIELD;
        }
        vmbreak;
    } while (0);
    VM_DISPATCH(vm_chan_recv(isolate, vm_ctx, i, base, frame, pc));
}

vmcase(OP_CHAN_TRY_SEND) {
    /* R[A] = R[B].trySend(R[C]) — non-blocking send.
     * Canonical logic in xr_chan_try_send (xchannel_ops.h). */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_bool(false);
        vmbreak;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    R(a) = xr_bool(xr_chan_try_send(isolate, ch, R(c)));
    vmbreak;
}

vmcase(OP_CHAN_TRY_RECV) {
    /* R[A] = R[B].tryRecv() — non-blocking receive result.
     * Canonical logic in xr_chan_try_recv (xchannel_ops.h). */
    int a = GETARG_A(i);
    int b = GETARG_B(i);

    XrValue ch_val = R(b);
    if (!xr_value_is_channel(ch_val)) {
        R(a) = xr_null();
        R(a + 1) = xr_bool(false);
        vmbreak;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrCoroutine *_recv_coro = (XrCoroutine *) vm_ctx->current_coro;

    XrValue _recv_val;
    bool _recv_ok = xr_chan_try_recv(isolate, ch, &_recv_val, _recv_coro);
    R(a) = _recv_val;
    R(a + 1) = xr_bool(_recv_ok);
    vmbreak;
}

vmcase(OP_CHAN_SEND_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_chan_send_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_CHAN_RECV_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_chan_recv_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_CHAN_CLOSE) {
    // R[A].close() - close Channel
    int a = GETARG_A(i);

    // Get Channel directly
    XrValue ch_val = R(a);
    if (!xr_value_is_channel(ch_val)) {
        vmbreak;  // Silently ignore non-Channel
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Close Channel
    xr_channel_close(ch);
    vmbreak;
}

vmcase(OP_CHAN_TIMER_DISPOSE) {
    // Release a select-owned timer channel (emitted at the select merge for the
    // `after` case). Drops the select handle reference and cancels the still-armed
    // wheel timer when on its owner worker. See design/885.
    int a = GETARG_A(i);
    XrValue ch_val = R(a);
    if (xr_value_is_channel(ch_val)) {
        xr_channel_timer_dispose(xr_value_to_channel(ch_val));
    }
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
