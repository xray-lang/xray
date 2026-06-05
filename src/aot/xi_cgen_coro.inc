/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_coro.inc - AOT coroutine frame emission helpers
 */

/* ========== AOT Coroutine Frame Emission ========== */

static bool cg_coro_value_has_storage(const XiFunc *f, const XiValue *v) {
    if (!v)
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

static void emit_boxed_vref(FILE *out, const XiValue *v) {
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_I64) {
        fprintf(out, "XR_FROM_INT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else {
        emit_vref(out, v);
    }
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
    } else {
        fprintf(out, "%s", temp_name);
    }
    fprintf(out, ";\n");
}

static void emit_coro_slot_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix,
                               const XiValue *v) {
    (void) ctx;
    (void) f;
    (void) prefix;
    fprintf(out, "xr_slot_aot_frame_offset(f, (uint32_t)((uint8_t *)&");
    emit_vref(out, v);
    fprintf(out, " - (uint8_t *)f), %u)", (unsigned) cg_rep(v));
}

static bool cg_is_channel_method_call(const XiValue *v, const char *method, int nargs) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !cg_value_type_is_channel(v->args[0]))
        return false;
    const char *actual = (const char *) v->aux;
    if (!actual || strcmp(actual, method) != 0)
        return false;
    return nargs < 0 || (int) v->nargs - 1 == nargs;
}

static bool cg_is_blocking_channel_method_call(const XiValue *v) {
    return cg_is_channel_method_call(v, "send", 1) || cg_is_channel_method_call(v, "recv", 0);
}

static int count_coro_suspend_states(const XiFunc *f) {
    int count = 0;
    if (!f)
        return 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_YIELD || v->op == XI_GO || v->op == XI_AWAIT || v->op == XI_CHAN_SEND ||
                v->op == XI_CHAN_RECV || cg_is_blocking_channel_method_call(v) ||
                cg_is_time_sleep_call(f, v))
                count++;
        }
    }
    return count;
}

static const XiFunc *cg_resolve_go_target(XiCgenCtx *ctx, const XiFunc *current,
                                          const XiValue *callee) {
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (const XiFunc *) callee->aux;
    if (callee->op == XI_BOX && callee->nargs >= 1 && callee->args[0]->op == XI_CLOSURE_NEW &&
        callee->args[0]->aux)
        return (const XiFunc *) callee->args[0]->aux;
    if (callee->op == XI_CONST && callee->type && callee->type->kind == XR_KIND_NULL &&
        current->name)
        return current;
    if (callee->op == XI_GET_SHARED) {
        int slot = (int) callee->aux_int;
        if (slot >= 0 && slot < ctx->nshared)
            return ctx->shared_funcs[slot];
    }
    if ((callee->op == XI_BOX || callee->op == XI_UNBOX) && callee->nargs >= 1 &&
        callee->args[0]->op == XI_GET_SHARED) {
        int slot = (int) callee->args[0]->aux_int;
        if (slot >= 0 && slot < ctx->nshared)
            return ctx->shared_funcs[slot];
    }
    return NULL;
}

static void emit_coro_frame_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    fprintf(out, "typedef struct ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " {\n");
    fprintf(out, "    uint32_t state;\n");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "    XrValue p%u;\n", i);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            fprintf(out, "    %s ", ctype_str(cg_rep(&phi->value)));
            emit_phi_ref(out, phi);
            fprintf(out, ";\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_has_storage(f, v))
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

static void emit_coro_macros(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    (void) ctx;
    (void) prefix;
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "#define p%u (f->p%u)\n", i, i);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
            fprintf(out, "#define phi%u (f->phi%u)\n", phi->value.id, phi->value.id);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_value_has_storage(f, v))
                fprintf(out, "#define v%u (f->v%u)\n", v->id, v->id);
        }
    }
}

static void emit_coro_undefs(FILE *out, const XiFunc *f) {
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "#undef p%u\n", i);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
            fprintf(out, "#undef phi%u\n", phi->value.id);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_value_has_storage(f, v))
                fprintf(out, "#undef v%u\n", v->id);
        }
    }
}

static void emit_coro_frame_factory(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "static void *");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_new");
    fprintf(out, "(");
    for (uint16_t i = 0; i < f->nparams; i++) {
        if (i > 0)
            fprintf(out, ", ");
        fprintf(out, "XrValue p%u", i);
    }
    fprintf(out, ") {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)xr_aot_frame_alloc(sizeof(*f));\n");
    fprintf(out, "    if (!f)\n        return NULL;\n");
    for (uint16_t i = 0; i < f->nparams; i++)
        fprintf(out, "    f->p%u = p%u;\n", i, i);
    fprintf(out, "    return f;\n");
    fprintf(out, "}\n\n");
}

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

    if (v->op == XI_YIELD) {
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    return xr_aot_yielded();\n");
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_GO) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: GO missing callee\n");
            return;
        }
        const XiFunc *target = cg_resolve_go_target(ctx, f, v->args[0]);
        if (!target || !cg_func_needs_aot_coro(target)) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT go target\n");
            return;
        }
        int priority = (((int) v->aux_int >> 8) & 0xff) - 1;
        if (priority < 0)
            priority = 1;
        int link_mode = (int) v->aux_int & 0xff;
        int sid = ++(*state_id);
        fprintf(out, "    void *_child_frame_%u = ", v->id);
        emit_fname_suffix(ctx, out, prefix, target, "_aot_frame_new");
        fprintf(out, "(");
        for (uint16_t a = 1; a < v->nargs; a++) {
            if (a > 1)
                fprintf(out, ", ");
            XrRep rep = cg_rep(v->args[a]);
            if (rep == XR_REP_TAGGED) {
                emit_vref(out, v->args[a]);
            } else if (rep == XR_REP_I64) {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[a]);
                fprintf(out, ")");
            } else {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[a]);
                fprintf(out, ")");
            }
        }
        fprintf(out, ");\n");
        fprintf(out, "    XrAotSpawnResult _spawn_%u = xr_aot_spawn(ctx, &", v->id);
        emit_fname_suffix(ctx, out, prefix, target, "_aot_desc");
        fprintf(out, ", _child_frame_%u, %d, %d, false, \"%s\");\n", v->id, priority, link_mode,
                target->name ? target->name : "aot");
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
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrAotResult _await_%u = xr_aot_await_task(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", &");
        emit_vref(out, v);
        fprintf(out, ", false);\n");
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        f->state = %d;\n", sid);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _await_%u = xr_aot_await_task_resume(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", &");
        emit_vref(out, v);
        fprintf(out, ", false);\n");
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (cg_is_time_sleep_call(f, v)) {
        int sid = ++(*state_id);
        fprintf(out, "    XrAotResult _sleep_%u = xr_aot_sleep(ctx, ", v->id);
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        fprintf(out, "    if (_sleep_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        f->state = %d;\n", sid);
        fprintf(out, "        return _sleep_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_sleep_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _sleep_%u;\n", v->id);
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
            return;
        }
        fprintf(out, "    XrValue _chan_try_%u = xr_aot_chan_try_send(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_boxed_vref(out, v->args[1]);
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
            return;
        }
        fprintf(out, "    XrValue _chan_try_%u = xr_aot_chan_try_recv(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_try_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_SEND || cg_is_channel_method_call(v, "send", 1)) {
        if (v->nargs < 2) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel send missing operands\n");
            return;
        }
        const XiValue *channel = v->args[0];
        const XiValue *send_value = v->args[1];
        int sid = ++(*state_id);
        fprintf(out, "    XrAotResult _chan_send_%u = xr_aot_chan_send(ctx, ", v->id);
        emit_vref(out, channel);
        fprintf(out, ", ");
        emit_boxed_vref(out, send_value);
        fprintf(out, ");\n");
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        f->state = %d;\n", sid);
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_send_%u = xr_aot_chan_send_resume(ctx);\n", v->id);
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

    if (v->op == XI_CHAN_RECV || cg_is_channel_method_call(v, "recv", 0)) {
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recv missing channel\n");
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _chan_recv_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    XrAotResult _chan_recv_%u = xr_aot_chan_recv_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_slot_%u);\n", v->id);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        f->state = %d;\n", sid);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_recv_%u = xr_aot_chan_recv_slot_resume(ctx, _chan_recv_slot_%u);\n",
                v->id, v->id);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        return;
    }

    if (v->op == XI_CALL_METHOD && cg_value_type_is_channel(v->args[0])) {
        if (cg_is_channel_method_call(v, "trySend", 1)) {
            fprintf(out, "    XrValue _chan_method_%u = xr_aot_chan_try_send(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_boxed_vref(out, v->args[1]);
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
        v->op == XI_CHAN_TRY_RECV || v->op == XI_SELECT_BLOCK || v->op == XI_CHAN_NEW ||
        v->op == XI_SCOPE_ENTER || v->op == XI_SCOPE_EXIT || v->op == XI_CORO_OP) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT coroutine Xi op %s\n", xi_op_name(v->op));
        return;
    }

    if (v->op == XI_TRY || v->op == XI_END_TRY) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: exceptions inside AOT coroutine are unsupported\n");
        return;
    }

    if (v->op == XI_STRUCT_NEW && cg_struct_can_inline(f, v)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: inlined struct inside AOT coroutine is unsupported\n");
        return;
    }

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
    if (blk->id != 0)
        fprintf(out, "L%u:;\n", blk->id);

    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v)
            emit_coro_value_stmt(ctx, out, f, v, prefix, state_id);
    }

    switch (blk->kind) {
        case XI_BLOCK_RETURN:
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
                emit_phi_copies(out, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;
        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "AOT coro IF block missing control");
            fprintf(out, "    if (");
            emit_vref(out, blk->control);
            fprintf(out, ") {\n");
            emit_phi_copies(out, blk->succs[0], find_pred_idx(blk->succs[0], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(out, blk->succs[1], find_pred_idx(blk->succs[1], blk));
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

static void emit_coro_frame_value_visit(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *helper, bool with_visitor) {
    (void) ctx;
    for (uint16_t i = 0; i < f->nparams; i++) {
        fprintf(out, "    %s(", helper);
        if (with_visitor)
            fprintf(out, "visitor, ");
        fprintf(out, "f->p%u);\n", i);
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_rep(&phi->value) != XR_REP_TAGGED)
                continue;
            fprintf(out, "    %s(", helper);
            if (with_visitor)
                fprintf(out, "visitor, ");
            fprintf(out, "f->phi%u);\n", phi->value.id);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_has_storage(f, v) || cg_rep(v) != XR_REP_TAGGED)
                continue;
            fprintf(out, "    %s(", helper);
            if (with_visitor)
                fprintf(out, "visitor, ");
            fprintf(out, "f->v%u);\n", v->id);
        }
    }
}

static void xi_cgen_coro_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    if (f->ncaptures > 0) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: captured AOT coroutine '%s' is unsupported\n",
                f->name ? f->name : "?");
        return;
    }

    emit_coro_frame_type(ctx, out, f, prefix);
    emit_coro_sync_wrapper(ctx, out, f, prefix);
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

    emit_coro_macros(ctx, out, f, prefix);

    int state_count = count_coro_suspend_states(f);
    if (state_count > 0) {
        fprintf(out, "    switch (f->state) {\n");
        fprintf(out, "        case 0: break;\n");
        for (int sid = 1; sid <= state_count; sid++)
            fprintf(out, "        case %d: goto S%d;\n", sid, sid);
        fprintf(out, "        default: return xr_aot_error(XR_NULL_VAL, false);\n");
        fprintf(out, "    }\n");
    }

    int state_id = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            emit_coro_block(ctx, out, f, f->blocks[bi], prefix, &state_id);
    }
    fprintf(out, "}\n");

    emit_coro_undefs(out, f);
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
    emit_coro_frame_value_visit(ctx, out, f, "xr_aot_trace_frame_value", true);
    fprintf(out, "}\n\n");

    fprintf(out, "static void ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, "(void *frame) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    emit_coro_frame_value_visit(ctx, out, f, "xr_aot_release_frame_value", false);
    fprintf(out, "    xr_aot_frame_free(frame);\n");
    fprintf(out, "}\n\n");

    fprintf(out, "static const XrAotCoroDesc ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_desc");
    fprintf(out, " = {\n");
    fprintf(out, "    .name = \"%s\",\n", f->name ? f->name : "aot");
    fprintf(out, "    .frame_size = sizeof(");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, "),\n");
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
