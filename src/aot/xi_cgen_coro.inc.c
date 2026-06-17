/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_coro.inc.c - AOT coroutine frame emission helpers
 */

/* ========== AOT Coroutine Frame Emission ========== */

static const XiValue *cg_coro_box_scalar_source(const XiValue *v) {
    if (!v || v->op != XI_BOX || v->nargs < 1)
        return NULL;
    XrRep rep = cg_rep(v->args[0]);
    return (rep == XR_REP_I64 || rep == XR_REP_F64) ? v->args[0] : NULL;
}

static bool cg_coro_is_channel_recv_value(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_RECV)
        return true;
    if (v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_channel(v->args[0]))
        return false;
    const char *method = (const char *) v->aux;
    return method && strcmp(method, "recv") == 0;
}

static bool cg_coro_is_recv_status_for(const XiValue *user, const XiValue *recv) {
    return user && recv && user->op == XI_CHAN_RECV_STATUS && user->nargs >= 1 &&
           user->args[0] == recv;
}

static const XiValue *cg_coro_recv_status_user(const XiFunc *f, const XiValue *recv) {
    if (!f || !recv || recv->op != XI_CHAN_RECV)
        return NULL;
    const XiValue *status = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!cg_coro_is_recv_status_for(user, recv))
                continue;
            if (status && status != user)
                return NULL;
            status = user;
        }
    }
    return status;
}

static bool cg_coro_is_paired_recv_status(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_CHAN_RECV_STATUS || v->nargs < 1)
        return false;
    return cg_coro_recv_status_user(f, v->args[0]) == v;
}

static const XiValue *cg_coro_typed_recv_unbox_user(const XiFunc *f, const XiValue *recv) {
    if (!f || !cg_coro_is_channel_recv_value(recv) || cg_rep(recv) != XR_REP_TAGGED)
        return NULL;

    const XiValue *typed_unbox = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == recv)
            return NULL;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == recv)
                    return NULL;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != recv)
                    continue;
                if (recv->op == XI_CHAN_RECV && a == 0 && cg_coro_is_recv_status_for(user, recv))
                    continue;
                if (user->op != XI_UNBOX || a != 0)
                    return NULL;
                XrRep rep = cg_rep(user);
                if (rep != XR_REP_I64 && rep != XR_REP_F64)
                    return NULL;
                if (typed_unbox && typed_unbox != user)
                    return NULL;
                typed_unbox = user;
            }
        }
    }
    return typed_unbox;
}

static bool cg_coro_unbox_from_typed_recv(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_UNBOX || v->nargs < 1)
        return false;
    return cg_coro_typed_recv_unbox_user(f, v->args[0]) == v;
}

static const XiValue *cg_coro_typed_await_unbox_user(const XiFunc *f, const XiValue *await_value) {
    if (!f || !await_value || await_value->op != XI_AWAIT || cg_rep(await_value) != XR_REP_TAGGED)
        return NULL;

    const XiValue *typed_unbox = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == await_value)
            return NULL;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == await_value)
                    return NULL;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != await_value)
                    continue;
                if (user->op != XI_UNBOX || a != 0)
                    return NULL;
                XrRep rep = cg_rep(user);
                if (rep != XR_REP_I64 && rep != XR_REP_F64)
                    return NULL;
                if (typed_unbox && typed_unbox != user)
                    return NULL;
                typed_unbox = user;
            }
        }
    }
    return typed_unbox;
}

static bool cg_coro_unbox_from_typed_await(const XiFunc *f, const XiValue *v) {
    if (!v || v->op != XI_UNBOX || v->nargs < 1)
        return false;
    return cg_coro_typed_await_unbox_user(f, v->args[0]) == v;
}

static bool cg_coro_is_typed_send_use(const XiValue *user, const XiValue *target,
                                      uint16_t arg_idx) {
    if (!user || !target || !cg_coro_box_scalar_source(target) || arg_idx != 1)
        return false;
    if ((user->op == XI_CHAN_SEND || user->op == XI_CHAN_TRY_SEND) && user->nargs >= 2)
        return true;
    if (user->op != XI_CALL_METHOD || user->nargs < 2 || !cg_value_type_is_channel(user->args[0]))
        return false;
    const char *method = (const char *) user->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0);
}

static bool cg_coro_box_only_feeds_typed_send(const XiFunc *f, const XiValue *target) {
    if (!f || !cg_coro_box_scalar_source(target))
        return false;
    bool seen_use = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (!cg_coro_is_typed_send_use(user, target, a))
                    return false;
                seen_use = true;
            }
        }
    }
    return seen_use;
}

static bool cg_coro_value_has_storage(const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_PARAM)
        return false;
    if (cg_coro_typed_await_unbox_user(f, v))
        return false;
    if (cg_coro_typed_recv_unbox_user(f, v))
        return false;
    if (cg_coro_box_only_feeds_typed_send(f, v))
        return false;
    if (v->op == XI_YIELD || v->op == XI_TRY || v->op == XI_END_TRY)
        return false;
    if (cg_is_void_like(v))
        return false;
    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v))
        return false;
    if (v->op == XI_COPY && v->nargs >= 1) {
        const XiValue *origin = cg_trace_struct_new(v);
        if (origin && cg_struct_can_inline(f, origin))
            return false;
    }
    return true;
}

static const XiValue *cg_coro_release_origin(const XiValue *v) {
    const XiValue *cur = v;
    for (int depth = 0; cur && depth < 8; depth++) {
        if ((cur->op == XI_COPY || cur->op == XI_MOVE || cur->op == XI_BOX ||
             cur->op == XI_UNBOX) &&
            cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }
    return cur;
}

static const XiValue *cg_coro_builtin_origin(const XiValue *v) {
    const XiValue *origin = cg_coro_release_origin(v);
    return origin && origin->op == XI_GET_BUILTIN ? origin : NULL;
}

static bool cg_coro_builtin_field_needs_xrt_bridge(const XiValue *builtin, const char *field) {
    if (!builtin || builtin->op != XI_GET_BUILTIN || !field)
        return true;
    switch ((int) builtin->aux_int) {
        case XR_GLOBAL_VAR_ORDERING:
        case XR_GLOBAL_VAR_SEND_RESULT:
        case XR_GLOBAL_VAR_TASK_STATUS:
            return false;
        case XR_GLOBAL_VAR_RECV:
            return strcmp(field, "Value") == 0;
        case XR_GLOBAL_VAR_TASK_RESULT:
            return strcmp(field, "Success") == 0 || strcmp(field, "Failed") == 0;
        default:
            return true;
    }
}

static bool cg_coro_value_from_runtime_bridge(const XiValue *v) {
    const XiValue *origin = cg_coro_release_origin(v);
    if (!origin)
        return false;
    switch (origin->op) {
        case XI_GO:
        case XI_AWAIT:
        case XI_CHAN_SEND:
        case XI_CHAN_RECV:
        case XI_CHAN_RECV_STATUS:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_IS_CLOSED:
        case XI_TIME_AFTER:
        case XI_SELECT_BLOCK:
        case XI_CHAN_NEW:
        case XI_TUPLE_GET:
        case XI_GET_BUILTIN:
            return true;
        case XI_LOAD_FIELD:
            if (origin->nargs >= 1 && cg_coro_builtin_origin(origin->args[0]))
                return true;
            return origin->nargs >= 1 && cg_value_type_is_task(origin->args[0]);
        case XI_CALL_METHOD:
            return origin->nargs >= 1 && (cg_value_type_is_channel(origin->args[0]) ||
                                          cg_value_type_is_task(origin->args[0]));
        default:
            return false;
    }
}

static bool cg_coro_value_needs_arc_release(const XiValue *v) {
    return v && (cg_rep(v) == XR_REP_TAGGED || cg_rep(v) == XR_REP_PTR) &&
           xi_own_type_is_rc(v->type) && !cg_coro_value_from_runtime_bridge(v);
}

static bool cg_coro_value_rep_can_trace_root(const XiValue *v) {
    if (!v)
        return false;
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_TAGGED)
        return true;
    return rep == XR_REP_PTR && xi_own_type_is_rc(v->type);
}

static bool cg_coro_type_needs_boundary_clone(const XrType *type) {
    if (!type)
        return true;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_JSON:
            return true;
        case XR_KIND_INSTANCE:
            return type->instance.class_name &&
                   strcmp(type->instance.class_name, "StringBuilder") == 0;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (cg_coro_type_needs_boundary_clone(type->union_type.members[i]))
                    return true;
            }
            return false;
        default:
            return false;
    }
}

static bool cg_coro_value_needs_boundary_clone(const XiValue *v) {
    const XiValue *origin = cg_coro_release_origin(v);
    return cg_coro_type_needs_boundary_clone(origin ? origin->type : (v ? v->type : NULL));
}

static bool cg_coro_value_has_json_type(const XiValue *v) {
    const XiValue *origin = cg_coro_release_origin(v);
    const XrType *type = origin ? origin->type : (v ? v->type : NULL);
    return type && type->kind == XR_KIND_JSON;
}

static void emit_boxed_vref(FILE *out, const XiValue *v) {
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_I64) {
        if (cg_value_type_is_bool(v))
            fprintf(out, "XR_FROM_BOOL(");
        else
            fprintf(out, "XR_FROM_INT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (rep == XR_REP_PTR && v && v->type && v->type->kind == XR_KIND_STRING) {
        fprintf(out, "xr_str_value_from_ptr(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "xr_mkptr(");
        emit_vref(out, v);
        fprintf(out, "%s", cg_ptr_box_suffix_for_type(v ? v->type : NULL));
    } else {
        emit_vref(out, v);
    }
}

static void emit_coro_boundary_value(FILE *out, const XiValue *v) {
    if (cg_coro_value_needs_boundary_clone(v)) {
        fprintf(out, "%s(",
                cg_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                               : "xrt_value_clone_for_coro");
        emit_boxed_vref(out, v);
        fprintf(out, ")");
        return;
    }
    emit_boxed_vref(out, v);
}

static const XiValue *cg_coro_unboxed_scalar_value(const XiValue *v) {
    if (!v)
        return NULL;
    const XiValue *boxed = cg_coro_box_scalar_source(v);
    if (boxed)
        return boxed;
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_I64 || rep == XR_REP_F64)
        return v;
    return NULL;
}

static const char *cg_coro_typed_send_helper(const char *base, const XiValue *v,
                                             const XiValue **send_arg) {
    const XiValue *arg = cg_coro_unboxed_scalar_value(v);
    if (!arg)
        return base;
    XrRep rep = cg_rep(arg);
    if (rep == XR_REP_I64) {
        if (send_arg)
            *send_arg = arg;
        if (strcmp(base, "xr_aot_chan_send") == 0)
            return "xr_aot_chan_send_i64";
        if (strcmp(base, "xr_aot_chan_send_timeout") == 0)
            return "xr_aot_chan_send_timeout_i64";
        if (strcmp(base, "xr_aot_chan_try_send_ready") == 0)
            return "xr_aot_chan_try_send_ready_i64";
        return "xr_aot_chan_try_send_i64";
    }
    if (rep == XR_REP_F64) {
        if (send_arg)
            *send_arg = arg;
        if (strcmp(base, "xr_aot_chan_send") == 0)
            return "xr_aot_chan_send_f64";
        if (strcmp(base, "xr_aot_chan_send_timeout") == 0)
            return "xr_aot_chan_send_timeout_f64";
        if (strcmp(base, "xr_aot_chan_try_send_ready") == 0)
            return "xr_aot_chan_try_send_ready_f64";
        return "xr_aot_chan_try_send_f64";
    }
    return base;
}

static void emit_coro_send_value(FILE *out, const XiValue *send_value, const XiValue *send_arg) {
    if (send_arg) {
        emit_vref(out, send_arg);
        return;
    }
    emit_coro_boundary_value(out, send_value);
}

static const char *cg_coro_typed_recv_pair_helper(const XiFunc *f, const XiValue *v) {
    const XiValue *slot_value = cg_coro_typed_recv_unbox_user(f, v);
    XrRep rep = cg_rep(slot_value ? slot_value : v);
    if (rep == XR_REP_I64)
        return "xr_aot_chan_recv_pair_i64";
    if (rep == XR_REP_F64)
        return "xr_aot_chan_recv_pair_f64";
    return "xr_aot_chan_recv_pair";
}

static void emit_int64_arg(FILE *out, const XiValue *v) {
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_I64) {
        emit_vref(out, v);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "(int64_t)");
        emit_vref(out, v);
    } else {
        fprintf(out, "XR_TO_INT(");
        emit_vref(out, v);
        fprintf(out, ")");
    }
}

static void emit_assign_from_xrvalue_temp(FILE *out, const XiValue *dst, const char *temp_name) {
    fprintf(out, "    ");
    emit_vref(out, dst);
    fprintf(out, " = ");
    XrRep rep = cg_rep(dst);
    if (rep == XR_REP_I64) {
        fprintf(out, "XR_TO_INT(%s)", temp_name);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_TO_FLOAT(%s)", temp_name);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "%s.ptr", temp_name);
    } else {
        fprintf(out, "%s", temp_name);
    }
    fprintf(out, ";\n");
}

static void emit_bridge_stored_tagged_value(FILE *out, const XiValue *value) {
    if (!value || cg_rep(value) != XR_REP_TAGGED)
        return;
    fprintf(out, "    ");
    emit_vref(out, value);
    fprintf(out, " = xr_aot_bridge_value_to_xrt(");
    emit_vref(out, value);
    fprintf(out, ");\n");
}

static void emit_assign_coro_param_from_xrvalue(FILE *out, const XiFunc *f, uint16_t index) {
    XrRep rep = cg_rep(f->params[index]);
    fprintf(out, "    f->p%u = ", index);
    const char *conv_suffix =
        emit_conversion_prefix(out, f->params[index]->type, XR_REP_TAGGED, rep);
    fprintf(out, "p%u", index);
    emit_conversion_suffix(out, conv_suffix);
    fprintf(out, ";\n");
}

static void emit_coro_slot_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix,
                               const XiValue *v) {
    (void) ctx;
    (void) prefix;
    const XiValue *slot_value = cg_coro_typed_recv_unbox_user(f, v);
    if (!slot_value)
        slot_value = v;
    fprintf(out, "xr_slot_aot_frame_offset(f, (uint32_t)((uint8_t *)&");
    emit_vref(out, slot_value);
    fprintf(out, " - (uint8_t *)f), %u)", (unsigned) cg_rep(slot_value));
}

static void emit_coro_optional_slot_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *prefix, const XiValue *v) {
    if (cg_coro_value_has_storage(f, v)) {
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        return;
    }
    fprintf(out, "xr_slot_none()");
}

static void emit_coro_await_result_slot(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *prefix, const XiValue *await_value,
                                        const XiValue *typed_slot_value) {
    if (typed_slot_value) {
        emit_coro_slot_ref(ctx, out, f, prefix, typed_slot_value);
        return;
    }
    if (cg_coro_value_has_storage(f, await_value)) {
        if (cg_rep(await_value) == XR_REP_TAGGED) {
            fprintf(out, "xr_slot_xvalue_ptr(&");
            emit_vref(out, await_value);
            fprintf(out, ")");
        } else {
            emit_coro_slot_ref(ctx, out, f, prefix, await_value);
        }
        return;
    }
    fprintf(out, "xr_slot_none()");
}

static bool cg_is_channel_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1)
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    if (nargs >= 0 && (int) v->nargs - 1 != nargs)
        return false;
    if (cg_value_type_is_channel(v->args[0]))
        return true;
    return cg_value_type_is_unknown(v->args[0]) &&
           ((strcmp(method, "send") == 0 && v->nargs == 2) ||
            (strcmp(method, "recv") == 0 && v->nargs == 1));
}

static bool cg_is_blocking_channel_method_call(const XiValue *v) {
    return cg_is_channel_method_call(v, "send", 1) ||
           cg_is_channel_method_call(v, "sendTimeout", 2) ||
           cg_is_channel_method_call(v, "recv", 0) ||
           cg_is_channel_method_call(v, "recvTimeout", 1);
}

static bool cg_is_task_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_task(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

static bool cg_is_blocking_task_method_call(const XiValue *v) {
    return cg_is_task_method_call(v, "awaitResult", 0) ||
           cg_is_task_method_call(v, "awaitTimeout", 1);
}

static bool cg_is_work_queue_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_work_queue(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

static bool cg_is_blocking_work_queue_method_call(const XiValue *v) {
    return cg_is_work_queue_method_call(v, "pop", 0) || cg_is_work_queue_method_call(v, "pop", 1);
}

static const XiFunc *cg_coro_direct_suspend_call_target(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v);

static bool cg_coro_value_may_suspend(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    return v &&
           (v->op == XI_YIELD || v->op == XI_GO || v->op == XI_AWAIT || v->op == XI_CHAN_SEND ||
            v->op == XI_CHAN_RECV || cg_is_blocking_channel_method_call(v) ||
            cg_is_blocking_task_method_call(v) || cg_is_blocking_work_queue_method_call(v) ||
            v->op == XI_SELECT_BLOCK || v->op == XI_SCOPE_EXIT ||
            cg_is_time_sleep_call_ctx(ctx, f, v) || cg_coro_direct_suspend_call_target(ctx, f, v));
}

static int count_coro_suspend_states(XiCgenCtx *ctx, const XiFunc *f) {
    int count = 0;
    if (!f)
        return 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_value_may_suspend(ctx, f, v))
                count++;
        }
    }
    return count;
}

static bool cg_coro_value_is_func_param(const XiFunc *f, const XiValue *target) {
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (f->params[i] == target)
            return true;
    }
    return false;
}

static bool cg_coro_block_defines_phi(const XiBlock *blk, const XiValue *target) {
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
        if (&phi->value == target)
            return true;
    }
    return false;
}

static bool cg_coro_block_defines_value(const XiBlock *blk, const XiValue *target) {
    for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
        if (blk->values[vi] == target)
            return true;
    }
    return false;
}

static bool cg_coro_value_uses_target(const XiValue *user, const XiValue *target) {
    if (!user || !target)
        return false;
    for (uint16_t a = 0; a < user->nargs; a++) {
        if (user->args[a] == target)
            return true;
    }
    return false;
}

static bool cg_coro_block_uses_target_after(const XiBlock *blk, uint32_t start,
                                            const XiValue *target) {
    for (uint32_t vi = start; vi < blk->nvalues; vi++) {
        if (cg_coro_value_uses_target(blk->values[vi], target))
            return true;
    }
    return blk->control == target;
}

static bool cg_coro_value_live_across_suspend(XiCgenCtx *ctx, const XiFunc *f,
                                              const XiLiveness *live, const XiValue *target) {
    if (!f || !live || !target)
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        bool defined_in_block = cg_coro_block_defines_value(blk, target);
        bool available = cg_coro_value_is_func_param(f, target) ||
                         (!defined_in_block && xi_is_live_in(live, blk, target)) ||
                         cg_coro_block_defines_phi(blk, target);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v == target) {
                available = true;
                continue;
            }
            if (!available || !cg_coro_value_may_suspend(ctx, f, v))
                continue;
            if (xi_is_live_out(live, blk, target) ||
                cg_coro_block_uses_target_after(blk, vi + 1, target))
                return true;
        }
    }
    return false;
}

static bool cg_coro_value_needs_runtime_slot(const XiValue *v) {
    return v && (v->op == XI_CHAN_RECV || cg_is_channel_method_call(v, "sendTimeout", 2) ||
                 cg_is_channel_method_call(v, "recv", 0) ||
                 cg_is_channel_method_call(v, "recvTimeout", 1) ||
                 cg_is_blocking_work_queue_method_call(v) || cg_is_blocking_task_method_call(v));
}

static bool cg_coro_value_is_aggregate_await_tasks(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_AWAIT || v->nargs < 1 || v->args[0] != target)
                continue;
            if (((int) v->aux_int & 0x7) != 0)
                return true;
        }
    }
    return false;
}

static bool cg_coro_value_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live,
                                      const XiValue *v) {
    if (!cg_coro_value_has_storage(f, v))
        return false;
    if (cg_coro_unbox_from_typed_await(f, v))
        return true;
    if (cg_coro_unbox_from_typed_recv(f, v))
        return true;
    if (cg_coro_is_paired_recv_status(f, v))
        return true;
    if (cg_coro_value_needs_runtime_slot(v))
        return true;
    if (cg_coro_value_is_aggregate_await_tasks(f, v))
        return true;
    if (v->op == XI_GO)
        return true;
    return cg_coro_value_live_across_suspend(ctx, f, live, v);
}

static bool cg_coro_phi_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live,
                                    const XiPhi *phi) {
    return phi && cg_coro_value_live_across_suspend(ctx, f, live, &phi->value);
}

static bool cg_coro_value_may_hold_frame_root(XiCgenCtx *ctx, const XiFunc *f,
                                              const XiLiveness *live, const XiValue *v) {
    if (v && v->op == XI_GO)
        return true;
    return cg_coro_value_live_across_suspend(ctx, f, live, v) ||
           cg_coro_value_needs_runtime_slot(v) || cg_coro_value_is_aggregate_await_tasks(f, v);
}

static size_t cg_coro_align_up(size_t size, size_t align) {
    if (align <= 1)
        return size;
    size_t rem = size % align;
    return rem == 0 ? size : size + (align - rem);
}

static void cg_coro_layout_add(size_t *size, size_t *max_align, size_t field_size,
                               size_t field_align) {
    XR_DCHECK(size != NULL, "coro layout: NULL size");
    XR_DCHECK(max_align != NULL, "coro layout: NULL align");
    XR_DCHECK(field_size > 0, "coro layout: zero field size");
    if (field_align == 0)
        field_align = 1;
    *size = cg_coro_align_up(*size, field_align) + field_size;
    if (*max_align < field_align)
        *max_align = field_align;
}

static void cg_coro_layout_add_rep(size_t *size, size_t *max_align, XrRep rep) {
    if (rep == XR_REP_I64) {
        cg_coro_layout_add(size, max_align, sizeof(int64_t), _Alignof(int64_t));
    } else if (rep == XR_REP_F64) {
        cg_coro_layout_add(size, max_align, sizeof(double), _Alignof(double));
    } else {
        cg_coro_layout_add(size, max_align, sizeof(XrValue), _Alignof(XrValue));
    }
}

static bool cg_func_frame_needs_cl(const XiFunc *f);

static size_t estimate_coro_frame_size(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live) {
    size_t size = 0;
    size_t max_align = 1;
    cg_coro_layout_add(&size, &max_align, sizeof(uint32_t), _Alignof(uint32_t));
    if (cg_func_frame_needs_cl(f))
        cg_coro_layout_add(&size, &max_align, sizeof(void *), _Alignof(void *));
    for (uint16_t i = 0; i < f->nparams; i++)
        cg_coro_layout_add_rep(&size, &max_align, cg_rep(f->params[i]));
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, live, phi))
                cg_coro_layout_add_rep(&size, &max_align, cg_rep(&phi->value));
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_direct_suspend_call_target(ctx, f, v))
                cg_coro_layout_add(&size, &max_align, sizeof(void *), _Alignof(void *));
            if (cg_coro_value_needs_frame(ctx, f, live, v))
                cg_coro_layout_add_rep(&size, &max_align, cg_rep(v));
        }
    }
    return cg_coro_align_up(size, max_align);
}

static void record_coro_frame_stats(XiCgenCtx *ctx, size_t frame_size, uint32_t root_count,
                                    uint32_t release_count) {
    if (!ctx)
        return;
    XiCgenCoroFrameStats *stats = &ctx->coro_frame_stats;
    stats->coroutine_count++;
    stats->total_frame_bytes += frame_size;
    stats->total_roots += root_count;
    stats->total_releases += release_count;
    if (stats->max_frame_bytes < frame_size)
        stats->max_frame_bytes = frame_size;
    if (stats->max_roots < root_count)
        stats->max_roots = root_count;
    if (stats->max_releases < release_count)
        stats->max_releases = release_count;
}

static bool cg_func_needs_aot_coro_ctx_depth(XiCgenCtx *ctx, const XiFunc *f, int depth);

static CgStaticFunctionCall
cg_coro_direct_suspend_call_target_info(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v);

static const XiFunc *cg_coro_direct_suspend_call_target(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    return cg_coro_direct_suspend_call_target_info(ctx, current, v).func;
}

static CgStaticFunctionCall
cg_coro_direct_suspend_call_target_info(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v) {
    if (!v || v->op != XI_CALL || v->nargs < 1)
        return cg_no_static_function_call();
    CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, current, v->args[0]);
    return call.func && cg_func_needs_aot_coro_ctx_depth(ctx, call.func, 0)
               ? call
               : cg_no_static_function_call();
}

static bool cg_func_needs_aot_coro_ctx_depth(XiCgenCtx *ctx, const XiFunc *f, int depth) {
    if (cg_func_needs_aot_coro(f))
        return true;
    if (!ctx || !f || depth >= 8)
        return false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_is_time_sleep_call_ctx(ctx, f, v))
                return true;
            if (!v || v->op != XI_CALL || v->nargs < 1)
                continue;
            const XiFunc *target = cg_resolve_static_function_call(ctx, f, v->args[0]).func;
            if (!target || target == f)
                continue;
            if (cg_func_needs_aot_coro_ctx_depth(ctx, target, depth + 1))
                return true;
        }
    }
    return false;
}

static bool cg_func_needs_aot_coro_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_needs_aot_coro_ctx_depth(ctx, f, 0);
}

static bool cg_func_frame_needs_cl(const XiFunc *f) {
    return f && f->ncaptures > 0;
}

static bool cg_func_can_emit_sync_go_wrapper_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    return f && !cg_func_needs_aot_coro_ctx(ctx, f);
}

static bool cg_sync_go_target_marked(const XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target)
        return false;
    for (int i = 0; i < ctx->nsync_go_targets; i++) {
        if (ctx->sync_go_targets[i] == target)
            return true;
    }
    return false;
}

static bool cg_mark_sync_go_target(XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target)
        return false;
    if (cg_sync_go_target_marked(ctx, target))
        return true;
    if (ctx->nsync_go_targets >= CG_MAX_SYNC_GO_TARGETS) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: too many sync AOT go targets\n");
        return false;
    }
    ctx->sync_go_targets[ctx->nsync_go_targets++] = target;
    return true;
}

static bool cg_func_needs_sync_go_wrapper_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_can_emit_sync_go_wrapper_ctx(ctx, f) && cg_sync_go_target_marked(ctx, f);
}

static void cg_collect_sync_go_targets_from_func(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GO || v->nargs < 1)
                continue;
            CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, f, v->args[0]);
            if (call.func && cg_func_can_emit_sync_go_wrapper_ctx(ctx, call.func))
                (void) cg_mark_sync_go_target(ctx, call.func);
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++)
        cg_collect_sync_go_targets_from_func(ctx, f->children[i]);
}

static void cg_reset_sync_go_targets(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    memset(ctx->sync_go_targets, 0, sizeof(ctx->sync_go_targets));
    ctx->nsync_go_targets = 0;
}

static void cg_prepare_sync_go_targets_for_modules(XiCgenCtx *ctx, XiModule **modules, int n) {
    if (!ctx || !modules || n <= 0)
        return;
    cg_reset_sync_go_targets(ctx);
    for (int i = 0; i < n; i++) {
        XiModule *module = modules[i];
        if (!module || !module->init)
            continue;
        cg_init_from_module(ctx, module);
        cg_register_imported_classes(ctx);
        cg_collect_sync_go_targets_from_func(ctx, module->init);
    }
}

static void emit_aot_frame_new_params(FILE *out, const XiFunc *f, bool typed_params) {
    bool need_comma = false;
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "xrt_closure_t *_cl");
        need_comma = true;
    }
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (need_comma)
            fprintf(out, ", ");
        fprintf(out, "%s p%u", typed_params ? ctype_str(cg_rep(f->params[i])) : "XrValue", i);
        need_comma = true;
    }
}

static void emit_aot_frame_new_cl_arg(FILE *out, const XiFunc *current, const XiValue *callee,
                                      const XiFunc *target) {
    if (!cg_func_frame_needs_cl(target))
        return;

    if (callee && callee->op == XI_CLOSURE_NEW) {
        fprintf(out, "(xrt_closure_t *)");
        emit_vref(out, callee);
        fprintf(out, ".ptr");
        return;
    }
    if (callee && callee->op == XI_BOX && callee->nargs >= 1 &&
        callee->args[0]->op == XI_CLOSURE_NEW) {
        fprintf(out, "(xrt_closure_t *)");
        emit_vref(out, callee);
        fprintf(out, ".ptr");
        return;
    }
    if (callee && callee->op == XI_CONST && current == target && cg_func_frame_needs_cl(current)) {
        fprintf(out, "f->_cl");
        return;
    }
    if (callee && callee->op == XI_BOX && callee->nargs >= 1 && callee->args[0]->op == XI_CONST &&
        current == target && cg_func_frame_needs_cl(current)) {
        fprintf(out, "f->_cl");
        return;
    }
    fprintf(out, "NULL");
}

static bool cg_aot_frame_new_can_supply_cl_arg(const XiFunc *current, const XiValue *callee,
                                               const XiFunc *target) {
    if (!cg_func_frame_needs_cl(target))
        return true;
    if (!callee)
        return false;
    if (callee->op == XI_CLOSURE_NEW)
        return true;
    if (callee->op == XI_BOX && callee->nargs >= 1 && callee->args[0]->op == XI_CLOSURE_NEW)
        return true;
    if (callee->op == XI_CONST && current == target && cg_func_frame_needs_cl(current))
        return true;
    if (callee->op == XI_BOX && callee->nargs >= 1 && callee->args[0]->op == XI_CONST &&
        current == target && cg_func_frame_needs_cl(current))
        return true;
    return false;
}

static void emit_xrvalue_from_native_expr(FILE *out, const XrType *type, XrRep rep,
                                          const char *expr);
static bool cg_sync_go_param_needs_release(const XiFunc *f, uint16_t index);

static void emit_aot_frame_new_call_args(FILE *out, const XiFunc *current, const XiValue *callee,
                                         const XiFunc *target, bool typed_params,
                                         XiValue *const *args, uint16_t arg_start, uint16_t nargs) {
    bool need_comma = false;
    if (cg_func_frame_needs_cl(target)) {
        emit_aot_frame_new_cl_arg(out, current, callee, target);
        need_comma = true;
    }
    for (uint16_t a = arg_start; a < nargs; a++) {
        if (need_comma)
            fprintf(out, ", ");
        if (!typed_params || a - arg_start >= target->nparams) {
            emit_coro_boundary_value(out, args[a]);
        } else {
            XrRep param_rep = cg_rep(target->params[a - arg_start]);
            if (param_rep == XR_REP_I64) {
                emit_int64_arg(out, args[a]);
            } else if (param_rep == XR_REP_F64) {
                if (cg_rep(args[a]) == XR_REP_F64) {
                    emit_vref(out, args[a]);
                } else if (cg_rep(args[a]) == XR_REP_I64) {
                    fprintf(out, "(double)");
                    emit_vref(out, args[a]);
                } else {
                    fprintf(out, "XR_TO_FLOAT(");
                    emit_vref(out, args[a]);
                    fprintf(out, ")");
                }
            } else if (param_rep == XR_REP_PTR) {
                emit_value_as_rep(out, args[a], XR_REP_PTR);
            } else {
                emit_coro_boundary_value(out, args[a]);
            }
        }
        need_comma = true;
    }
}

static void emit_sync_go_frame_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "typedef struct ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " {\n");
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_closure_t *_cl;\n");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "    %s p%u;\n", ctype_str(cg_rep(f->params[i])), i);
    fprintf(out, "} ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, ";\n\n");
}

static void emit_sync_go_frame_factory(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix) {
    fprintf(out, "static void *");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
    fprintf(out, "(");
    emit_aot_frame_new_params(out, f, true);
    fprintf(out, ") {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)xr_aot_frame_alloc(sizeof(*f));\n");
    fprintf(out, "    if (!f)\n        return NULL;\n");
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "    f->_cl = _cl;\n");
        fprintf(out, "    if (_cl)\n        xrt_retain(xr_mkptr(_cl, XR_TAG_CLOSURE));\n");
    }
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_sync_go_param_needs_release(f, i)) {
            XrRep rep = cg_rep(f->params[i]);
            fprintf(out, "    XrValue _pval%u = ", i);
            char expr[32];
            snprintf(expr, sizeof(expr), "p%u", i);
            emit_xrvalue_from_native_expr(out, f->params[i]->type, rep, expr);
            fprintf(out, ";\n");
            fprintf(out, "    XrValue _pclone%u = %s(_pval%u);\n", i,
                    f->params[i]->type && f->params[i]->type->kind == XR_KIND_JSON
                        ? "xrt_json_clone_for_coro"
                        : "xrt_value_clone_for_coro",
                    i);
            fprintf(out, "    f->p%u = ", i);
            if (rep == XR_REP_I64) {
                if (cg_value_type_is_bool(f->params[i]))
                    fprintf(out, "XR_TO_INT(_pclone%u) != 0", i);
                else
                    fprintf(out, "XR_TO_INT(_pclone%u)", i);
            } else if (rep == XR_REP_F64) {
                fprintf(out, "XR_TO_FLOAT(_pclone%u)", i);
            } else if (rep == XR_REP_PTR) {
                fprintf(out, "_pclone%u.ptr", i);
            } else {
                fprintf(out, "_pclone%u", i);
            }
            fprintf(out, ";\n");
            continue;
        }
        fprintf(out, "    f->p%u = p%u;\n", i, i);
    }
    fprintf(out, "    return f;\n");
    fprintf(out, "}\n\n");
}

static bool cg_sync_go_param_needs_release(const XiFunc *f, uint16_t index) {
    return f && index < f->nparams && f->params[index] &&
           cg_coro_type_needs_boundary_clone(f->params[index]->type);
}

static bool cg_sync_go_param_needs_trace(const XiFunc *f, uint16_t index) {
    return f && index < f->nparams && f->params[index] &&
           cg_coro_value_rep_can_trace_root(f->params[index]);
}

static uint32_t count_sync_go_frame_roots(const XiFunc *f) {
    if (!f)
        return 0;
    uint32_t count = 0;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_sync_go_param_needs_trace(f, i))
            count++;
    }
    return count;
}

static uint32_t count_sync_go_frame_releases(const XiFunc *f) {
    if (!f)
        return 0;
    uint32_t count = cg_func_frame_needs_cl(f) ? 1u : 0u;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_sync_go_param_needs_release(f, i))
            count++;
    }
    return count;
}

static size_t estimate_sync_go_frame_size(const XiFunc *f) {
    size_t size = 0;
    size_t max_align = 1;
    if (cg_func_frame_needs_cl(f))
        cg_coro_layout_add(&size, &max_align, sizeof(void *), _Alignof(void *));
    for (uint16_t i = 0; f && i < f->nparams; i++)
        cg_coro_layout_add_rep(&size, &max_align, cg_rep(f->params[i]));
    return cg_coro_align_up(size, max_align);
}

static void emit_sync_go_frame_param_as_xrvalue(FILE *out, const XiFunc *f, uint16_t index) {
    XrRep rep = cg_rep(f->params[index]);
    if (rep == XR_REP_I64) {
        if (cg_value_type_is_bool(f->params[index]))
            fprintf(out, "XR_FROM_BOOL(f->p%u)", index);
        else
            fprintf(out, "XR_FROM_INT(f->p%u)", index);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(f->p%u)", index);
    } else if (rep == XR_REP_PTR && f->params[index]->type &&
               f->params[index]->type->kind == XR_KIND_STRING) {
        fprintf(out, "xr_str_value_from_ptr(f->p%u)", index);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "xr_mkptr(f->p%u%s", index,
                cg_ptr_box_suffix_for_type(f->params[index]->type));
    } else {
        fprintf(out, "f->p%u", index);
    }
}

static void emit_sync_go_frame_param_for_func_abi(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                  uint16_t index) {
    XrRep param_rep = cg_func_param_abi_rep(ctx, f, index);
    if (param_rep == XR_REP_TAGGED) {
        emit_sync_go_frame_param_as_xrvalue(out, f, index);
    } else {
        fprintf(out, "f->p%u", index);
    }
}

static void emit_xrvalue_from_native_expr(FILE *out, const XrType *type, XrRep rep,
                                          const char *expr) {
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "%s", expr);
    } else if (rep == XR_REP_PTR && type && type->kind == XR_KIND_STRING) {
        fprintf(out, "xr_str_value_from_ptr(%s)", expr);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "xr_mkptr(%s%s", expr, cg_ptr_box_suffix_for_type(type));
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(%s)", expr);
    } else if (type && type->kind == XR_KIND_BOOL) {
        fprintf(out, "XR_FROM_BOOL(%s)", expr);
    } else {
        fprintf(out, "XR_FROM_INT(%s)", expr);
    }
}

static void emit_sync_go_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    uint32_t root_count = count_sync_go_frame_roots(f);
    uint32_t release_count = count_sync_go_frame_releases(f);
    record_coro_frame_stats(ctx, estimate_sync_go_frame_size(f), root_count, release_count);

    emit_sync_go_frame_type(ctx, out, f, prefix);
    emit_sync_go_frame_factory(ctx, out, f, prefix);

    fprintf(out, "static XrAotResult ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
    fprintf(out, "(void *raw_frame, const XrAotContext *ctx) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)raw_frame;\n");
    fprintf(out, "    (void)ctx;\n");
    fprintf(out, "    if (!f)\n        return xr_aot_error(XR_NULL_VAL, false);\n");
    XrRep result_rep = cg_func_return_abi_rep(ctx, f);
    fprintf(out, "    %s _raw_result = ", ctype_str(result_rep));
    emit_fname(ctx, out, prefix, f);
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "(f->_cl");
    else
        fprintf(out, "(NULL");
    for (uint16_t i = 0; i < f->nparams; i++) {
        fprintf(out, ", ");
        emit_sync_go_frame_param_for_func_abi(ctx, out, f, i);
    }
    fprintf(out, ");\n");
    fprintf(out, "    XrValue _result = ");
    emit_xrvalue_from_native_expr(out, f->return_type, result_rep, "_raw_result");
    fprintf(out, ";\n");
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (!cg_sync_go_param_needs_release(f, i))
            continue;
        fprintf(out, "    XrValue _pval%u = ", i);
        emit_sync_go_frame_param_as_xrvalue(out, f, i);
        fprintf(out, ";\n");
        fprintf(out,
                "    if (_result.tag == _pval%u.tag && _result.ptr == _pval%u.ptr)\n"
                "        xrt_retain(_result);\n",
                i, i);
    }
    fprintf(out, "    return xr_aot_done(_result);\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static void ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, "(void *frame, void *visitor) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (!cg_sync_go_param_needs_trace(f, i))
            continue;
        fprintf(out, "    xr_aot_trace_frame_value(visitor, ");
        emit_sync_go_frame_param_as_xrvalue(out, f, i);
        fprintf(out, ");\n");
    }
    fprintf(out, "}\n\n");

    fprintf(out, "static void ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, "(void *frame, struct XrCoroGC *gc) {\n");
    fprintf(out, "    (void)gc;\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_sync_go_param_needs_release(f, i)) {
            fprintf(out, "    xrt_release(");
            emit_sync_go_frame_param_as_xrvalue(out, f, i);
            fprintf(out, ");\n");
        }
    }
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_release(xr_mkptr(f->_cl, XR_TAG_CLOSURE));\n");
    fprintf(out, "    xr_aot_frame_free(frame);\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static const XrAotCoroDesc ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
    fprintf(out, " = {\n");
    fprintf(out, "    .name = \"%s\",\n", f->name ? f->name : "aot");
    fprintf(out, "    .frame_size = sizeof(");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, "),\n");
    fprintf(out, "    .root_count = %u,\n", root_count);
    fprintf(out, "    .release_count = %u,\n", release_count);
    fprintf(out, "    .resume = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
    fprintf(out, ",\n");
    fprintf(out, "    .trace_roots = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, ",\n");
    fprintf(out, "    .release_frame = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, ",\n");
    fprintf(out, "};\n\n");
}

static void emit_coro_frame_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiLiveness *live,
                                 const char *prefix) {
    fprintf(out, "typedef struct ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " {\n");
    fprintf(out, "    uint32_t state;\n");
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_closure_t *_cl;\n");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "    %s p%u;\n", ctype_str(cg_rep(f->params[i])), i);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_phi_needs_frame(ctx, f, live, phi))
                continue;
            fprintf(out, "    %s ", ctype_str(cg_rep(&phi->value)));
            emit_phi_ref(out, phi);
            fprintf(out, ";\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_direct_suspend_call_target(ctx, f, v))
                fprintf(out, "    void *call_frame_%u;\n", v->id);
            if (!cg_coro_value_needs_frame(ctx, f, live, v))
                continue;
            fprintf(out, "    %s ", ctype_str(cg_rep(v)));
            emit_vref(out, v);
            fprintf(out, ";\n");
        }
    }
    fprintf(out, "} ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, ";\n\n");
}

static void emit_aot_frame_param_names(FILE *out, const XiFunc *f) {
    bool need_comma = false;
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "_cl");
        need_comma = true;
    }
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (need_comma)
            fprintf(out, ", ");
        fprintf(out, "p%u", i);
        need_comma = true;
    }
}

static void emit_coro_frame_init(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    fprintf(out, "static bool ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_init");
    fprintf(out, "(void *raw_frame");
    if (cg_func_frame_needs_cl(f) || f->nparams > 0)
        fprintf(out, ", ");
    emit_aot_frame_new_params(out, f, false);
    fprintf(out, ") {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)raw_frame;\n");
    fprintf(out, "    if (!f)\n        return false;\n");
    fprintf(out, "    f->state = 0;\n");
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "    f->_cl = _cl;\n");
        fprintf(out, "    if (_cl)\n        xrt_retain(xr_mkptr(_cl, XR_TAG_CLOSURE));\n");
    }
    for (uint16_t i = 0; i < f->nparams; i++)
        emit_assign_coro_param_from_xrvalue(out, f, i);
    fprintf(out, "    return true;\n");
    fprintf(out, "}\n\n");
}

static void emit_coro_local_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiLiveness *live) {
    (void) ctx;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, live, phi))
                continue;
            XrRep rep = cg_rep(&phi->value);
            fprintf(out, "    %s ", ctype_str(rep));
            emit_phi_ref(out, phi);
            if (rep == XR_REP_TAGGED)
                fprintf(out, " = XR_NULL_VAL;\n");
            else
                fprintf(out, " = 0;\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_has_storage(f, v) || cg_coro_value_needs_frame(ctx, f, live, v))
                continue;
            XrRep rep = cg_rep(v);
            fprintf(out, "    %s ", ctype_str(rep));
            emit_vref(out, v);
            if (rep == XR_REP_TAGGED)
                fprintf(out, " = XR_NULL_VAL;\n");
            else
                fprintf(out, " = 0;\n");
        }
    }
}

static void emit_coro_macros(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiLiveness *live,
                             const char *prefix) {
    (void) ctx;
    (void) prefix;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, live, phi))
                fprintf(out, "#define phi%u (f->phi%u)\n", phi->value.id, phi->value.id);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_PARAM && v->aux_int >= 0 && v->aux_int < f->nparams) {
                fprintf(out, "#define v%u (f->p%u)\n", v->id, (unsigned) v->aux_int);
            } else if (cg_coro_value_needs_frame(ctx, f, live, v)) {
                fprintf(out, "#define v%u (f->v%u)\n", v->id, v->id);
            }
        }
    }
}

static void emit_coro_undefs(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiLiveness *live) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, live, phi))
                fprintf(out, "#undef phi%u\n", phi->value.id);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_PARAM && v->aux_int >= 0 && v->aux_int < f->nparams)
                fprintf(out, "#undef v%u\n", v->id);
            else if (cg_coro_value_needs_frame(ctx, f, live, v))
                fprintf(out, "#undef v%u\n", v->id);
        }
    }
}

static void emit_coro_frame_factory(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "static void *");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
    fprintf(out, "(");
    emit_aot_frame_new_params(out, f, false);
    fprintf(out, ") {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)xr_aot_frame_alloc(sizeof(*f));\n");
    fprintf(out, "    if (!f)\n        return NULL;\n");
    fprintf(out, "    memset(f, 0, sizeof(*f));\n");
    fprintf(out, "    if (!");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_init");
    fprintf(out, "(f");
    if (cg_func_frame_needs_cl(f) || f->nparams > 0)
        fprintf(out, ", ");
    emit_aot_frame_param_names(out, f);
    fprintf(out, ")) {\n");
    fprintf(out, "        xr_aot_frame_free(f);\n");
    fprintf(out, "        return NULL;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return f;\n");
    fprintf(out, "}\n\n");
}

static uint32_t count_coro_frame_roots(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live);
static uint32_t count_coro_frame_releases(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live);
static bool cg_coro_direct_call_frame_reusable(XiCgenCtx *ctx, const XiFunc *target);

static void emit_coro_sync_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    fprintf(out, "static XrValue ");
    emit_fname(ctx, out, prefix, f);
    fprintf(out, "(xrt_closure_t *_cl");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, ", XrValue p%u", i);
    fprintf(out, ") {\n");
    fprintf(out, "    (void)_cl;\n");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "    (void)p%u;\n", i);
    fprintf(out, "    return (abort(), XR_NULL_VAL);\n");
    fprintf(out, "}\n\n");
}

static void emit_coro_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix, int *state_id) {
    XR_DCHECK(v != NULL, "emit_coro_value_stmt: NULL value");
    emit_value_source_line(ctx, out, v);

    if (v->op == XI_YIELD) {
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    return xr_aot_yielded();\n");
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_PARAM) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = p%u;\n", (unsigned) v->aux_int);
        }
        return;
    }

    if (cg_coro_box_only_feeds_typed_send(f, v))
        return;

    if (cg_coro_unbox_from_typed_recv(f, v))
        return;

    if (cg_coro_unbox_from_typed_await(f, v))
        return;

    if (v->op == XI_GO) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: GO missing callee\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        CgStaticFunctionCall go_call = cg_resolve_static_function_call(ctx, f, v->args[0]);
        const XiFunc *target = go_call.func;
        const char *go_prefix = go_call.prefix ? go_call.prefix : prefix;
        bool target_is_coro = target && cg_func_needs_aot_coro_ctx(ctx, target);
        bool target_is_sync_go = target && cg_func_needs_sync_go_wrapper_ctx(ctx, target);
        if (!target || (!target_is_coro && !target_is_sync_go)) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT go target\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        if (!cg_aot_frame_new_can_supply_cl_arg(f, v->args[0], target)) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported captured AOT go target\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int link_mode = (int) v->aux_int & 0xff;
        bool fire_and_forget = (v->flags & XI_FLAG_FIRE_AND_FORGET) != 0;
        int sid = ++(*state_id);
        fprintf(out, "    void *_child_frame_%u = ", v->id);
        emit_fname_suffix(ctx, out, go_prefix, target, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_call_args(out, f, v->args[0], target, target_is_sync_go, v->args, 1,
                                     v->nargs);
        fprintf(out, ");\n");
        fprintf(out, "    XrAotSpawnResult _spawn_%u = xr_aot_spawn(ctx, &", v->id);
        emit_fname_suffix(ctx, out, go_prefix, target, "_aot_desc");
        fprintf(out, ", _child_frame_%u, %d, %s, \"%s\");\n", v->id, link_mode,
                fire_and_forget ? "true" : "false", target->name ? target->name : "aot");
        fprintf(out, "    ");
        emit_vref(out, v);
        fprintf(out, " = _spawn_%u.task_value;\n", v->id);
        fprintf(out, "    if (!_spawn_%u.child)\n", v->id);
        fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    return xr_aot_spawn_child(_spawn_%u.child);\n", v->id);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_AWAIT) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: AWAIT missing task\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        const XiValue *await_slot_value = cg_coro_typed_await_unbox_user(f, v);
        int await_flags = (int) v->aux_int;
        bool await_all = (await_flags & XI_AWAIT_AUX_ALL) != 0;
        bool await_any_success = (await_flags & XI_AWAIT_AUX_ANY_SUCCESS) != 0;
        bool await_any = (await_flags & XI_AWAIT_AUX_ANY) != 0 || await_any_success;
        if (await_all || await_any) {
            fprintf(out, "    ");
            emit_vref(out, v->args[0]);
            fprintf(out, " = xrt_value_clone_for_coro(");
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        if (await_all) {
            fprintf(out, "    XrAotResult _await_%u = xr_aot_await_all_tasks(ctx, ", v->id);
        } else if (await_any) {
            fprintf(out, "    XrAotResult _await_%u = xr_aot_await_any_task(ctx, ", v->id);
        } else {
            fprintf(out, "    XrAotResult _await_%u = xr_aot_await_task(ctx, ", v->id);
        }
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
        if (await_any)
            fprintf(out, ", %s);\n", await_any_success ? "true" : "false");
        else if (await_all) {
            fprintf(out, ");\n");
        } else {
            fprintf(out, ", ");
            if (v->nargs >= 2)
                emit_int64_arg(out, v->args[1]);
            else
                fprintf(out, "-1");
            fprintf(out, ", false);\n");
        }
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        if (await_all) {
            fprintf(out, "    _await_%u = xr_aot_await_all_tasks_resume(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ");\n");
        } else if (await_any) {
            fprintf(out, "    _await_%u = xr_aot_await_any_task_resume(ctx, ", v->id);
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ");\n");
        } else {
            fprintf(out, "    _await_%u = xr_aot_await_task_resume(ctx, ", v->id);
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ", false);\n");
        }
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if ((await_all || await_any) && cg_coro_value_has_storage(f, v) &&
            cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = xr_aot_bridge_value_to_xrt(");
            emit_vref(out, v);
            fprintf(out, ");\n");
        }
        if (cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED &&
            cg_coro_value_needs_boundary_clone(v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = %s(",
                    cg_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                                   : "xrt_value_clone_for_coro");
            emit_vref(out, v);
            fprintf(out, ");\n");
        }
        return;
    }

    CgStaticFunctionCall direct_call = cg_coro_direct_suspend_call_target_info(ctx, f, v);
    const XiFunc *direct_call_target = direct_call.func;
    if (direct_call_target) {
        const char *direct_call_prefix = direct_call.prefix ? direct_call.prefix : prefix;
        bool reuse_call_frame = cg_coro_direct_call_frame_reusable(ctx, direct_call_target);
        if (!cg_aot_frame_new_can_supply_cl_arg(f, v->args[0], direct_call_target)) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported captured AOT suspend call target\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrAotResult _call_%u;\n", v->id);
        fprintf(out, "    XrValue _call_value_%u = XR_NULL_VAL;\n", v->id);
        fprintf(out, "    if (!f->call_frame_%u) {\n", v->id);
        fprintf(out, "        f->call_frame_%u = ", v->id);
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_call_args(out, f, v->args[0], direct_call_target, false, v->args, 1,
                                     v->nargs);
        fprintf(out, ");\n");
        fprintf(out, "        if (!f->call_frame_%u)\n", v->id);
        fprintf(out, "            return xr_aot_error(XR_NULL_VAL, false);\n");
        fprintf(out, "    }\n");
        if (reuse_call_frame) {
            fprintf(out, "    else if (!");
            emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_frame_init");
            fprintf(out, "(f->call_frame_%u", v->id);
            if (cg_func_frame_needs_cl(direct_call_target) || v->nargs > 1) {
                fprintf(out, ", ");
                emit_aot_frame_new_call_args(out, f, v->args[0], direct_call_target, false, v->args,
                                             1, v->nargs);
            }
            fprintf(out, ")) {\n");
            fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
            fprintf(out, "    }\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _call_%u = ", v->id);
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_resume");
        fprintf(out, "(f->call_frame_%u, ctx);\n", v->id);
        fprintf(out, "    if (_call_%u.kind == XR_AOT_RUN_DONE) {\n", v->id);
        fprintf(out, "        _call_value_%u = _call_%u.value;\n", v->id, v->id);
        if (cg_rep(v) == XR_REP_TAGGED)
            fprintf(out,
                    "        if (_call_value_%u.tag == XR_TAG_PTR || "
                    "_call_value_%u.tag >= XR_TAG_STR)\n"
                    "            xrt_retain(_call_value_%u);\n",
                    v->id, v->id, v->id);
        if (!reuse_call_frame) {
            fprintf(out, "        ");
            emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_release");
            fprintf(out, "(f->call_frame_%u, NULL);\n", v->id);
            fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
        }
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_call_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        goto S%d_DONE;\n", sid);
        fprintf(out, "    }\n");
        fprintf(out,
                "    if (_call_%u.kind == XR_AOT_RUN_ERROR || "
                "_call_%u.kind == XR_AOT_RUN_CANCELLED) {\n",
                v->id, v->id);
        fprintf(out, "        ");
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_release");
        fprintf(out, "(f->call_frame_%u, NULL);\n", v->id);
        fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _call_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    return _call_%u;\n", v->id);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _call_%u = ", v->id);
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_resume");
        fprintf(out, "(f->call_frame_%u, ctx);\n", v->id);
        fprintf(out, "    if (_call_%u.kind == XR_AOT_RUN_DONE) {\n", v->id);
        fprintf(out, "        _call_value_%u = _call_%u.value;\n", v->id, v->id);
        if (cg_rep(v) == XR_REP_TAGGED)
            fprintf(out,
                    "        if (_call_value_%u.tag == XR_TAG_PTR || "
                    "_call_value_%u.tag >= XR_TAG_STR)\n"
                    "            xrt_retain(_call_value_%u);\n",
                    v->id, v->id, v->id);
        if (!reuse_call_frame) {
            fprintf(out, "        ");
            emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_release");
            fprintf(out, "(f->call_frame_%u, NULL);\n", v->id);
            fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
        }
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_call_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        goto S%d_DONE;\n", sid);
        fprintf(out, "    }\n");
        fprintf(out,
                "    if (_call_%u.kind == XR_AOT_RUN_ERROR || "
                "_call_%u.kind == XR_AOT_RUN_CANCELLED) {\n",
                v->id, v->id);
        fprintf(out, "        ");
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_release");
        fprintf(out, "(f->call_frame_%u, NULL);\n", v->id);
        fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _call_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    return _call_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (v->op == XI_CORO_OP) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT coroutine Xi op %s\n", xi_op_name(v->op));
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (cg_is_time_sleep_call_ctx(ctx, f, v)) {
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _sleep_%u = xr_aot_sleep(ctx, ", v->id);
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        fprintf(out, "    if (_sleep_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _sleep_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_sleep_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _sleep_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    XrValue _sleep_value_%u = XR_NULL_VAL;\n", v->id);
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_sleep_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (cg_is_time_module_call_ctx(ctx, f, v)) {
        const char *time_helper = cg_time_module_helper_ctx(ctx, f, v);
        if (!time_helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT time method '%s'\n",
                    v->aux ? (const char *) v->aux : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _time_method_%u = %s();\n", v->id, time_helper);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_time_method_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_SCOPE_ENTER) {
        fprintf(out, "    XrAotResult _scope_enter_%u = xr_aot_scope_enter(ctx, %u);\n", v->id,
                (unsigned) v->aux_int);
        fprintf(out, "    if (_scope_enter_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _scope_enter_%u;\n", v->id);
        return;
    }

    if (v->op == XI_SCOPE_EXIT) {
        int sid = ++(*state_id);
        fprintf(out, "    XrValue _scope_exit_value_%u = XR_NULL_VAL;\n", v->id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _scope_exit_%u = xr_aot_scope_exit(ctx, %u, ", v->id,
                (unsigned) v->aux_int);
        if (cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "&");
            emit_vref(out, v);
        } else {
            fprintf(out, "&_scope_exit_value_%u", v->id);
        }
        fprintf(out, ");\n");
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _scope_exit_%u = xr_aot_scope_exit(ctx, %u, ", v->id,
                (unsigned) v->aux_int);
        if (cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "&");
            emit_vref(out, v);
        } else {
            fprintf(out, "&_scope_exit_value_%u", v->id);
        }
        fprintf(out, ");\n");
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = xr_aot_bridge_value_to_xrt(");
            emit_vref(out, v);
            fprintf(out, ");\n");
        } else if (cg_coro_value_has_storage(f, v)) {
            fprintf(out,
                    "    _scope_exit_value_%u = "
                    "xr_aot_bridge_value_to_xrt(_scope_exit_value_%u);\n",
                    v->id, v->id);
        }
        if (cg_coro_value_has_storage(f, v) && cg_rep(v) != XR_REP_TAGGED) {
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "_scope_exit_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_TIME_AFTER) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: TIME_AFTER missing timeout\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _timer_ch_%u = xr_aot_time_after(ctx, ", v->id);
        emit_int64_arg(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_timer_ch_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_TIMER_DISPOSE) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TIMER_DISPOSE missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        // Non-suspending: release the select-owned timer channel synchronously.
        fprintf(out, "    xr_aot_chan_timer_dispose(ctx, ");
        emit_boxed_vref(out, v->args[0]);
        fprintf(out, ");\n");
        return;
    }

    if (v->op == XI_SELECT_BLOCK) {
        if (v->nargs == 0) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: SELECT_BLOCK missing channels\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrValue _select_channels_%u[%u];\n", v->id, (unsigned) v->nargs);
        for (uint16_t i = 0; i < v->nargs; i++) {
            fprintf(out, "    _select_channels_%u[%u] = ", v->id, (unsigned) i);
            emit_boxed_vref(out, v->args[i]);
            fprintf(out, ";\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out,
                "    XrAotResult _select_%u = xr_aot_select_block(ctx, _select_channels_%u, "
                "%u, %u);\n",
                v->id, v->id, (unsigned) v->nargs, (unsigned) v->aux_int);
        fprintf(out, "    if (_select_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _select_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_select_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _select_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (v->op == XI_UNBOX && v->nargs >= 1 && cg_rep(v->args[0]) != XR_REP_TAGGED) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
            emit_vref(out, v->args[0]);
            fprintf(out, ";\n");
        }
        return;
    }

    if (v->op == XI_CHAN_NEW) {
        fprintf(out, "    XrValue _chan_%u = xr_aot_channel_new(ctx, ", v->id);
        if (v->nargs >= 1)
            emit_int64_arg(out, v->args[0]);
        else
            fprintf(out, "0");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_TRY_SEND) {
        if (v->nargs < 2) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_SEND missing operands\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        const XiValue *send_arg = NULL;
        const char *helper =
            cg_coro_typed_send_helper("xr_aot_chan_try_send_ready", v->args[1], &send_arg);
        fprintf(out, "    XrValue _chan_try_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_send_value(out, v->args[1], send_arg);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_try_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_TRY_RECV) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_RECV missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _chan_try_%u = xr_aot_chan_try_recv(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        fprintf(out, "    XrValue _chan_try_payload_%u = xr_aot_recv_payload(_chan_try_%u);\n",
                v->id, v->id);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_try_payload_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_RECV_STATUS) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_RECV_STATUS missing recv value\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        if (v->args[0] && v->args[0]->op == XI_CHAN_RECV)
            return;
        if (cg_coro_value_has_storage(f, v)) {
            // CHAN_RECV_STATUS carries an unboxed i64 rep (sr_def_rep), so the C
            // bool from xr_aot_recv_is_value assigns straight into the i64 slot;
            // no XrValue boxing is needed, matching the runtime helper and ISNULL.
            fprintf(out, "    ");
            emit_vref(out, v);
            if (v->args[0] && v->args[0]->op == XI_CHAN_TRY_RECV) {
                fprintf(out, " = xr_aot_recv_is_value(_chan_try_%u);\n", v->args[0]->id);
            } else {
                fprintf(out, " = xr_aot_recv_is_value(");
                emit_vref(out, v->args[0]);
                fprintf(out, ");\n");
            }
        }
        return;
    }

    if (v->op == XI_ISNULL && v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CHAN_TRY_RECV) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = !xr_aot_recv_is_value(_chan_try_%u);\n", v->args[0]->id);
        }
        return;
    }

    if (v->op == XI_CHAN_IS_CLOSED) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_IS_CLOSED missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _chan_closed_%u = xr_aot_chan_is_closed(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_chan_closed_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_TUPLE_GET) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: TUPLE_GET missing tuple\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _tuple_get_%u = xr_aot_tuple_get(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", %u);\n", (unsigned) v->aux_int);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_tuple_get_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1) {
        const XiValue *builtin = cg_coro_builtin_origin(v->args[0]);
        const char *field = (const char *) v->aux;
        if (builtin && field) {
            fprintf(out,
                    "    XrValue _builtin_field_%u = xr_aot_load_builtin_field(ctx, %d, "
                    "\"%s\");\n",
                    v->id, (int) builtin->aux_int, field);
            if (cg_rep(v) == XR_REP_TAGGED &&
                cg_coro_builtin_field_needs_xrt_bridge(builtin, field))
                fprintf(out,
                        "    _builtin_field_%u = xr_aot_bridge_value_to_xrt(_builtin_field_%u);\n",
                        v->id, v->id);
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_builtin_field_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
            }
            return;
        }
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && cg_value_type_is_task(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_task_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Task field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _task_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_task_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && cg_value_type_is_channel(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_channel_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Channel field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _chan_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (cg_is_task_method_call(v, "cancel", 0)) {
        fprintf(out, "    XrValue _task_method_%u = xr_aot_task_cancel(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_task_method_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
            emit_bridge_stored_tagged_value(out, v);
        }
        return;
    }

    if (cg_is_task_method_call(v, "poll", 0)) {
        fprintf(out, "    XrValue _task_method_%u = xr_aot_task_poll(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_task_method_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
            emit_bridge_stored_tagged_value(out, v);
        }
        return;
    }

    if (cg_is_task_method_call(v, "awaitResult", 0) ||
        cg_is_task_method_call(v, "awaitTimeout", 1)) {
        bool timeout_enabled = cg_is_task_method_call(v, "awaitTimeout", 1);
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _task_await_%u = xr_aot_task_await_result(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", ");
        if (timeout_enabled)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "-1");
        fprintf(out, ", %s);\n", timeout_enabled ? "true" : "false");
        fprintf(out, "    if (_task_await_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _task_await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_task_await_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _task_await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _task_await_%u = xr_aot_task_await_result_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", %s);\n", timeout_enabled ? "true" : "false");
        fprintf(out, "    if (_task_await_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _task_await_%u;\n", v->id);
        fprintf(out, "    if (_task_await_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _task_await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && cg_value_type_is_task(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Task method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (cg_is_work_queue_method_call(v, "push", 1) || cg_is_work_queue_method_call(v, "push", 2)) {
        fprintf(out, "    XrValue _wq_push_%u = xr_aot_work_queue_push(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ", ");
        if (v->nargs >= 3)
            emit_int64_arg(out, v->args[2]);
        else
            fprintf(out, "-1");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_wq_push_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (cg_is_work_queue_method_call(v, "close", 0)) {
        fprintf(out, "    XrValue _wq_close_%u = xr_aot_work_queue_close(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_wq_close_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (cg_is_work_queue_method_call(v, "tryPop", 0) ||
        cg_is_work_queue_method_call(v, "tryPop", 1)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT WorkQueue method 'tryPop'\n");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (cg_is_blocking_work_queue_method_call(v)) {
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _wq_pop_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _wq_pop_%u = xr_aot_work_queue_pop(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "-1");
        fprintf(out, ", _wq_pop_slot_%u);\n", v->id);
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _wq_pop_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    _wq_pop_%u = xr_aot_work_queue_pop_resume(ctx, _wq_pop_slot_%u);\n",
                v->id, v->id);
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && cg_value_type_is_work_queue(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT WorkQueue method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (cg_is_channel_method_call(v, "sendTimeout", 2)) {
        const XiValue *send_arg = NULL;
        const char *helper =
            cg_coro_typed_send_helper("xr_aot_chan_send_timeout", v->args[1], &send_arg);
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _chan_send_timeout_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_send_value(out, v->args[1], send_arg);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[2]);
        fprintf(out, ", ");
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ");\n");
        fprintf(out, "    if (_chan_send_timeout_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_send_timeout_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_send_timeout_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_send_timeout_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_send_timeout_%u = xr_aot_chan_send_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", true);\n");
        fprintf(out, "    if (_chan_send_timeout_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_send_timeout_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (cg_is_channel_method_call(v, "recvTimeout", 1)) {
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _chan_recv_timeout_%u = xr_aot_chan_recv_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        fprintf(out, "    if (_chan_recv_timeout_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_recv_timeout_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_recv_timeout_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_recv_timeout_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_recv_timeout_%u = xr_aot_chan_recv_slot_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", true);\n");
        fprintf(out, "    if (_chan_recv_timeout_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_timeout_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        return;
    }

    if (v->op == XI_CHAN_SEND || cg_is_channel_method_call(v, "send", 1)) {
        if (v->nargs < 2) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel send missing operands\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        const XiValue *channel = v->args[0];
        const XiValue *send_value = v->args[1];
        const XiValue *send_arg = NULL;
        const char *helper = cg_coro_typed_send_helper("xr_aot_chan_send", send_value, &send_arg);
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _chan_send_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, channel);
        fprintf(out, ", ");
        emit_coro_send_value(out, send_value, send_arg);
        if (strcmp(helper, "xr_aot_chan_send") == 0)
            fprintf(out, ", xr_slot_none(), -1");
        fprintf(out, ");\n");
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_send_%u = xr_aot_chan_send_resume(ctx, xr_slot_none(), false);\n",
                v->id);
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    XrValue _chan_send_value_%u = XR_NULL_VAL;\n", v->id);
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_chan_send_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_RECV) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recv missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        const XiValue *status = cg_coro_recv_status_user(f, v);
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _chan_recv_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    XrSlotRef _chan_recv_ok_slot_%u = ", v->id);
        if (status) {
            emit_coro_slot_ref(ctx, out, f, prefix, status);
        } else {
            fprintf(out, "xr_slot_none()");
        }
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        const char *helper = cg_coro_typed_recv_pair_helper(f, v);
        fprintf(out, "    XrAotResult _chan_recv_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_slot_%u, _chan_recv_ok_slot_%u", v->id, v->id);
        if (strcmp(helper, "xr_aot_chan_recv_pair") == 0)
            fprintf(out, ", -1");
        fprintf(out, ");\n");
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_recv_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    _chan_recv_ok_slot_%u = ", v->id);
        if (status) {
            emit_coro_slot_ref(ctx, out, f, prefix, status);
        } else {
            fprintf(out, "xr_slot_none()");
        }
        fprintf(out, ";\n");
        fprintf(out,
                "    _chan_recv_%u = xr_aot_chan_recv_pair_resume(ctx, _chan_recv_slot_%u, "
                "_chan_recv_ok_slot_%u);\n",
                v->id, v->id, v->id);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (cg_is_channel_method_call(v, "recv", 0)) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recv missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _chan_recv_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    XrAotResult _chan_recv_%u = xr_aot_chan_recv_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_slot_%u, -1);\n", v->id);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out,
                "    _chan_recv_%u = xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), true);\n",
                v->id);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && cg_value_type_is_channel(v->args[0])) {
        if (cg_is_channel_method_call(v, "trySend", 1)) {
            const XiValue *send_arg = NULL;
            const char *helper =
                cg_coro_typed_send_helper("xr_aot_chan_try_send", v->args[1], &send_arg);
            fprintf(out, "    XrValue _chan_method_%u = %s(ctx, ", v->id, helper);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_coro_send_value(out, v->args[1], send_arg);
            fprintf(out, ");\n");
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
            }
            return;
        }
        if (cg_is_channel_method_call(v, "tryRecv", 0)) {
            fprintf(out, "    XrValue _chan_method_%u = xr_aot_chan_try_recv(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
                emit_bridge_stored_tagged_value(out, v);
            }
            return;
        }
        if (cg_is_channel_method_call(v, "close", 0)) {
            fprintf(out, "    XrValue _chan_method_%u = xr_aot_chan_close(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
            }
            return;
        }
        if (cg_is_channel_method_call(v, "isClosed", 0)) {
            fprintf(out, "    XrValue _chan_method_%u = xr_aot_chan_is_closed(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
            }
            return;
        }
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Channel method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_ERR_CHECK) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            if (cg_rep(v) == XR_REP_I64)
                fprintf(out, " = 0;\n");
            else if (cg_rep(v) == XR_REP_F64)
                fprintf(out, " = 0.0;\n");
            else
                fprintf(out, " = XR_NULL_VAL;\n");
        }
        return;
    }

    if (v->op == XI_CHAN_SEND || v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_SEND ||
        v->op == XI_CHAN_TRY_RECV || v->op == XI_CHAN_IS_CLOSED || v->op == XI_CHAN_NEW ||
        v->op == XI_CORO_OP) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT coroutine Xi op %s\n", xi_op_name(v->op));
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_TRY || v->op == XI_END_TRY) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: exceptions inside AOT coroutine are unsupported\n");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: inlined struct inside AOT coroutine is unsupported\n");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_GET_BUILTIN) {
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "_builtin_value_%u", v->id);
            fprintf(out, "    XrValue %s = xr_aot_get_builtin(ctx, %d);\n", tmp, (int) v->aux_int);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        } else {
            fprintf(out, "    (void) xr_aot_get_builtin(ctx, %d);\n", (int) v->aux_int);
        }
        return;
    }

    if (cg_ownership_op_is_noop(v) || cg_shared_static_function_ownership_is_noop(ctx, f, v))
        return;

    if (cg_is_void_like(v)) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        return;
    }

    fprintf(out, "    ");
    emit_vref(out, v);
    fprintf(out, " = ");
    emit_value_rhs(ctx, out, f, v, prefix);
    fprintf(out, ";\n");
}

static void emit_coro_block(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *blk,
                            const char *prefix, int *state_id) {
    XR_DCHECK(blk != NULL, "emit_coro_block: NULL block");
    fprintf(out, "L%u:;\n", blk->id);

    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v)
            emit_coro_value_stmt(ctx, out, f, v, prefix, state_id);
    }

    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            emit_value_source_line(ctx, out, blk->control);
            if (blk->control) {
                fprintf(out, "    return xr_aot_done(");
                emit_boxed_vref(out, blk->control);
                fprintf(out, ");\n");
            } else {
                fprintf(out, "    return xr_aot_done(XR_NULL_VAL);\n");
            }
            break;
        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;
        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "AOT coro IF block missing control");
            emit_value_source_line(ctx, out, blk->control);
            fprintf(out, "    if (");
            emit_condition_expr(out, blk->control);
            fprintf(out, ") {\n");
            emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(ctx, out, f, blk->succs[1], find_pred_idx(blk->succs[1], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[1]->id);
            fprintf(out, "    }\n");
            break;
        case XI_BLOCK_UNREACHABLE:
            fprintf(out, "    __builtin_unreachable();\n");
            break;
        default:
            fprintf(out, "    return xr_aot_error(XR_NULL_VAL, false);\n");
            break;
    }
}

static void emit_coro_frame_slot_as_xrvalue(FILE *out, const XrType *type, XrRep rep,
                                            const char *slot_prefix, uint32_t slot_id) {
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "%s%u", slot_prefix, slot_id);
    } else if (rep == XR_REP_PTR && type && type->kind == XR_KIND_STRING) {
        fprintf(out, "xr_str_value_from_ptr(%s%u)", slot_prefix, slot_id);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "xr_mkptr(%s%u%s", slot_prefix, slot_id, cg_ptr_box_suffix_for_type(type));
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(%s%u)", slot_prefix, slot_id);
    } else if (type && type->kind == XR_KIND_BOOL) {
        fprintf(out, "XR_FROM_BOOL(%s%u)", slot_prefix, slot_id);
    } else {
        fprintf(out, "XR_FROM_INT(%s%u)", slot_prefix, slot_id);
    }
}

static void emit_coro_frame_value_visit(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiLiveness *live, const char *helper,
                                        bool with_visitor) {
    (void) ctx;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (!cg_coro_value_rep_can_trace_root(f->params[i]) ||
            !cg_coro_value_live_across_suspend(ctx, f, live, f->params[i]))
            continue;
        fprintf(out, "    %s(", helper);
        if (with_visitor)
            fprintf(out, "visitor, ");
        emit_coro_frame_slot_as_xrvalue(out, f->params[i]->type, cg_rep(f->params[i]), "f->p", i);
        fprintf(out, ");\n");
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_value_rep_can_trace_root(&phi->value) ||
                !cg_coro_value_live_across_suspend(ctx, f, live, &phi->value))
                continue;
            fprintf(out, "    %s(", helper);
            if (with_visitor)
                fprintf(out, "visitor, ");
            emit_coro_frame_slot_as_xrvalue(out, phi->value.type, cg_rep(&phi->value), "f->phi",
                                            phi->value.id);
            fprintf(out, ");\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_needs_frame(ctx, f, live, v) ||
                !cg_coro_value_rep_can_trace_root(v) ||
                !cg_coro_value_may_hold_frame_root(ctx, f, live, v))
                continue;
            fprintf(out, "    %s(", helper);
            if (with_visitor)
                fprintf(out, "visitor, ");
            emit_coro_frame_slot_as_xrvalue(out, v->type, cg_rep(v), "f->v", v->id);
            fprintf(out, ");\n");
        }
    }
}

static void emit_coro_direct_call_frame_trace(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                              const char *prefix) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            CgStaticFunctionCall call = cg_coro_direct_suspend_call_target_info(ctx, f, v);
            if (!call.func)
                continue;
            const char *target_prefix = call.prefix ? call.prefix : prefix;
            fprintf(out, "    if (f->call_frame_%u)\n        ", v->id);
            emit_fname_suffix(ctx, out, target_prefix, call.func, "_aot_trace");
            fprintf(out, "(f->call_frame_%u, visitor);\n", v->id);
        }
    }
}

static void emit_coro_direct_call_frame_release(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                const char *prefix) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            CgStaticFunctionCall call = cg_coro_direct_suspend_call_target_info(ctx, f, v);
            if (!call.func)
                continue;
            const char *target_prefix = call.prefix ? call.prefix : prefix;
            fprintf(out, "    if (f->call_frame_%u) {\n        ", v->id);
            emit_fname_suffix(ctx, out, target_prefix, call.func, "_aot_release");
            fprintf(out, "(f->call_frame_%u, NULL);\n", v->id);
            fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
            fprintf(out, "    }\n");
        }
    }
}

static uint32_t count_coro_frame_roots(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live) {
    uint32_t count = 0;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_coro_value_rep_can_trace_root(f->params[i]) &&
            cg_coro_value_live_across_suspend(ctx, f, live, f->params[i]))
            count++;
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_value_rep_can_trace_root(&phi->value) &&
                cg_coro_value_live_across_suspend(ctx, f, live, &phi->value))
                count++;
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_direct_suspend_call_target(ctx, f, v))
                count++;
            if (cg_coro_value_needs_frame(ctx, f, live, v) && cg_coro_value_rep_can_trace_root(v) &&
                cg_coro_value_may_hold_frame_root(ctx, f, live, v))
                count++;
        }
    }
    return count;
}

static void emit_coro_frame_arc_release(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiLiveness *live) {
    (void) ctx;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (!cg_coro_value_needs_arc_release(f->params[i]) ||
            !cg_coro_value_live_across_suspend(ctx, f, live, f->params[i]))
            continue;
        fprintf(out, "    xrt_release(");
        emit_coro_frame_slot_as_xrvalue(out, f->params[i]->type, cg_rep(f->params[i]), "f->p", i);
        fprintf(out, ");\n");
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_value_needs_arc_release(&phi->value) ||
                !cg_coro_value_live_across_suspend(ctx, f, live, &phi->value))
                continue;
            fprintf(out, "    xrt_release(");
            emit_coro_frame_slot_as_xrvalue(out, phi->value.type, cg_rep(&phi->value), "f->phi",
                                            phi->value.id);
            fprintf(out, ");\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_needs_frame(ctx, f, live, v) ||
                !cg_coro_value_needs_arc_release(v) ||
                !cg_coro_value_live_across_suspend(ctx, f, live, v))
                continue;
            fprintf(out, "    xrt_release(");
            emit_coro_frame_slot_as_xrvalue(out, v->type, cg_rep(v), "f->v", v->id);
            fprintf(out, ");\n");
        }
    }
}

static uint32_t count_coro_frame_releases(XiCgenCtx *ctx, const XiFunc *f, const XiLiveness *live) {
    uint32_t count = cg_func_frame_needs_cl(f) ? 1u : 0u;
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (cg_coro_value_needs_arc_release(f->params[i]) &&
            cg_coro_value_live_across_suspend(ctx, f, live, f->params[i]))
            count++;
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_value_needs_arc_release(&phi->value) &&
                cg_coro_value_live_across_suspend(ctx, f, live, &phi->value))
                count++;
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_direct_suspend_call_target(ctx, f, v))
                count++;
            if (cg_coro_value_needs_frame(ctx, f, live, v) && cg_coro_value_needs_arc_release(v) &&
                cg_coro_value_live_across_suspend(ctx, f, live, v))
                count++;
        }
    }
    return count;
}

static bool cg_coro_direct_call_frame_reusable(XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target || cg_func_frame_needs_cl(target))
        return false;

    XiFunc *mutable_target = (XiFunc *) target;
    xi_ensure_rpo(mutable_target);
    XiLiveness *live = xi_compute_liveness(mutable_target);
    if (!live)
        return false;

    bool reusable = count_coro_frame_roots(ctx, target, live) == 0 &&
                    count_coro_frame_releases(ctx, target, live) == 0;
    xi_liveness_free(live);
    return reusable;
}

static void xi_cgen_coro_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    xi_ensure_rpo(f);
    XiLiveness *live = xi_compute_liveness(f);
    if (!live) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: AOT coroutine liveness failed for '%s'\n",
                f->name ? f->name : "?");
        return;
    }
    size_t frame_size = estimate_coro_frame_size(ctx, f, live);
    uint32_t root_count = count_coro_frame_roots(ctx, f, live);
    uint32_t release_count = count_coro_frame_releases(ctx, f, live);
    record_coro_frame_stats(ctx, frame_size, root_count, release_count);

    emit_coro_frame_type(ctx, out, f, live, prefix);
    emit_coro_sync_wrapper(ctx, out, f, prefix);
    emit_coro_frame_init(ctx, out, f, prefix);
    emit_coro_frame_factory(ctx, out, f, prefix);

    fprintf(out, "static XrAotResult ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
    fprintf(out, "(void *raw_frame, const XrAotContext *ctx) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)raw_frame;\n");
    fprintf(out, "    if (!f)\n        return xr_aot_error(XR_NULL_VAL, false);\n");
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "    xrt_closure_t *_cl = f->_cl;\n");
        fprintf(out, "    if (!_cl)\n        return xr_aot_error(XR_NULL_VAL, false);\n");
    }

    emit_coro_local_declarations(ctx, out, f, live);
    emit_coro_macros(ctx, out, f, live, prefix);

    int state_count = count_coro_suspend_states(ctx, f);
    if (state_count > 0) {
        fprintf(out, "    switch (f->state) {\n");
        fprintf(out, "        case 0: break;\n");
        for (int sid = 1; sid <= state_count; sid++)
            fprintf(out, "        case %d: goto S%d;\n", sid, sid);
        fprintf(out, "        default: return xr_aot_error(XR_NULL_VAL, false);\n");
        fprintf(out, "    }\n");
    }
    if (f->entry)
        fprintf(out, "    goto L%u;\n", f->entry->id);

    int state_id = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            emit_coro_block(ctx, out, f, f->blocks[bi], prefix, &state_id);
    }
    fprintf(out, "}\n");

    emit_coro_undefs(ctx, out, f, live);
    fprintf(out, "\n");

    fprintf(out, "static void ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, "(void *frame, void *visitor) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    emit_coro_frame_value_visit(ctx, out, f, live, "xr_aot_trace_frame_value", true);
    emit_coro_direct_call_frame_trace(ctx, out, f, prefix);
    fprintf(out, "}\n\n");

    fprintf(out, "static void ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, "(void *frame, struct XrCoroGC *gc) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    (void)gc;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    emit_coro_direct_call_frame_release(ctx, out, f, prefix);
    emit_coro_frame_arc_release(ctx, out, f, live);
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_release(xr_mkptr(f->_cl, XR_TAG_CLOSURE));\n");
    fprintf(out, "    xr_aot_frame_free(frame);\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static const XrAotCoroDesc ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
    fprintf(out, " = {\n");
    fprintf(out, "    .name = \"%s\",\n", f->name ? f->name : "aot");
    fprintf(out, "    .frame_size = sizeof(");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, "),\n");
    fprintf(out, "    .root_count = %u,\n", root_count);
    fprintf(out, "    .release_count = %u,\n", release_count);
    fprintf(out, "    .resume = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_resume");
    fprintf(out, ",\n");
    fprintf(out, "    .trace_roots = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, ",\n");
    fprintf(out, "    .release_frame = ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, ",\n");
    fprintf(out, "};\n\n");
    xi_liveness_free(live);
}
