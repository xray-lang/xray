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

static bool cg_coro_is_typed_send_use(const XiValue *user, const XiValue *target,
                                      uint16_t arg_idx) {
    if (!user || !target || !cg_coro_box_scalar_source(target) || arg_idx != 1)
        return false;
    if ((user->op == XI_CHAN_SEND || user->op == XI_CHAN_TRY_SEND) && user->nargs >= 2)
        return true;
    if (user->op != XI_CALL_METHOD || user->nargs < 2 || !xi_value_type_is_channel(user->args[0]))
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
    if (cg_value_is_elided_i64_optional_blocking_result(f, v))
        return false;
    if (v->op == XI_PARAM)
        return false;
    if (xi_coro_typed_await_unbox_user(f, v))
        return false;
    if (xi_coro_typed_recv_unbox_user(f, v))
        return false;
    if (cg_coro_box_only_feeds_typed_send(f, v))
        return false;
    if (v->op == XI_YIELD || v->op == XI_GEN_YIELD || v->op == XI_TRY || v->op == XI_END_TRY)
        return false;
    if (cg_is_void_like(v))
        return false;
    if (v->op == XI_AGG_NEW && cg_struct_can_inline(f, v))
        return false;
    if (v->op == XI_COPY && v->nargs >= 1) {
        const XiValue *origin = cg_trace_struct_new(v);
        if (origin && cg_struct_can_inline(f, origin))
            return false;
    }
    return true;
}

/* Await always receives a tagged cross-coroutine value from XrSlotRef.  Most
 * tagged await results can use their final local directly, and scalar results
 * reuse a typed UNBOX consumer.  A native class/record result is different:
 * its final AOT storage is a pointer even though the boundary value is tagged.
 * Give that boundary its own XrValue temporary, then bridge and convert it once
 * the await completes.  This keeps the runtime slot ABI and the typed local ABI
 * explicit instead of assigning two physical representations to one C local. */
static bool cg_coro_await_needs_tagged_boundary_temp(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v) {
    return v && v->op == XI_AWAIT && cg_coro_value_has_storage(f, v) &&
           cg_rep(v) == XR_REP_TAGGED && cg_value_plan_storage_rep(ctx, v) != XR_REP_TAGGED;
}

static void emit_coro_await_boundary_temp_ref(FILE *out, const XiValue *v) {
    fprintf(out, "_xr_await_boundary_v%u", v->id);
}

static void emit_coro_clear_inline_await_all_task_handles(FILE *out, const XiFunc *f,
                                                          const CgInlineAwaitAllLiteral *literal) {
    if (!out || !literal)
        return;
    for (uint32_t i = 0; i < literal->count; i++) {
        const XiValue *cur = literal->tasks[i];
        for (uint8_t depth = 0; cur && depth < 8; depth++) {
            if (cg_coro_value_has_storage(f, cur) && cg_rep(cur) == XR_REP_TAGGED) {
                fprintf(out, "    ");
                emit_vref(out, cur);
                fprintf(out, " = XR_NULL_VAL;\n");
            }
            if (cur->nargs == 1 &&
                (xi_copy_is_identity_alias(cur) || xi_op_is_identity_forward(cur->op))) {
                cur = cur->args[0];
                continue;
            }
            break;
        }
    }
}

static bool cg_coro_value_result_observed(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return true;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == target)
                    return true;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user || user == target)
                continue;
            if (user->op == XI_RETAIN || user->op == XI_RELEASE || user->op == XI_ERR_CHECK)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] == target)
                    return true;
            }
        }
    }
    return false;
}

static bool cg_coro_builtin_field_needs_xrt_bridge(const XiValue *builtin, const char *field) {
    if (!builtin || builtin->op != XI_GET_BUILTIN || !field)
        return true;
    switch ((int) builtin->aux_int) {
        case XR_GLOBAL_VAR_ORDERING:
        case XR_GLOBAL_VAR_ENDIAN:
        case XR_GLOBAL_VAR_SEND_RESULT:
        case XR_GLOBAL_VAR_TASK_STATUS:
        case XR_GLOBAL_VAR_UTF8_ERROR:
        case XR_GLOBAL_VAR_STRING_SLICE_ERROR:
        case XR_GLOBAL_VAR_COMPRESSION_ERROR:
        case XR_GLOBAL_VAR_CRYPTO_ERROR:
            return false;
        case XR_GLOBAL_VAR_RECV:
            return strcmp(field, "Value") == 0;
        case XR_GLOBAL_VAR_TASK_RESULT:
            return strcmp(field, "Success") == 0 || strcmp(field, "Failed") == 0;
        default:
            return true;
    }
}

static bool cg_transfer_plan_site_is_spawn_arg(const XiValue *site) {
    return site && (site->op == XI_GO || site->op == XI_THREAD_SPAWN);
}

static bool cg_transfer_plan_has_boundary_clone(const XaotTransferPlan *plan) {
    return plan && (plan->evidence & XAOT_TRANSFER_EV_BOUNDARY_CLONE) != 0;
}

static const XaotTransferPlan *cg_required_transfer_plan(XiCgenCtx *ctx, const XiValue *site,
                                                         uint16_t transfer_index,
                                                         const XiValue *expected_value,
                                                         const char *context) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotTransferPlan *plan = xaot_bundle_find_transfer_plan(bundle, site, transfer_index);
    const char *op = site ? xi_op_name(site->op) : "?";
    const char *label = context ? context : "transfer";
    if (!bundle || !plan) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: missing AOT transfer plan for %s site=v%u/%s index=%u\n",
                label, site ? site->id : 0, op, (unsigned) transfer_index);
        return NULL;
    }
    if (expected_value && plan->value != expected_value) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: AOT transfer plan value mismatch for %s site=v%u/%s "
                "index=%u expected=v%u actual=v%u\n",
                label, site ? site->id : 0, op, (unsigned) transfer_index, expected_value->id,
                plan->value ? plan->value->id : 0);
        return NULL;
    }
    if (plan->action == XR_TRANSFER_REJECT) {
        if (ctx)
            ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: rejected AOT transfer plan for %s site=v%u/%s index=%u "
                "reason=%u\n",
                label, site ? site->id : 0, op, (unsigned) transfer_index,
                (unsigned) plan->unproven_reason);
        return NULL;
    }
    return plan;
}

static void emit_coro_transfer_plan_xrvalue(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                            const XaotTransferPlan *plan) {
    if (!plan) {
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        return;
    }
    if (plan->action == XR_TRANSFER_EXPLICIT_COPY && cg_transfer_plan_has_boundary_clone(plan)) {
        fprintf(out, "%s(",
                xi_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                               : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")");
        return;
    }
    if ((plan->action == XR_TRANSFER_CONST_SHARE || plan->action == XR_TRANSFER_SYNC_SHARE) &&
        cg_transfer_plan_has_boundary_clone(plan)) {
        fprintf(out, "((void)xrt_retain(");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, "), ");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
}

static void emit_coro_transfer_plan_as_rep(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep rep,
                                           const XaotTransferPlan *plan) {
    if (rep == XR_REP_TAGGED) {
        emit_coro_transfer_plan_xrvalue(ctx, out, v, plan);
        return;
    }
    if (!plan) {
        emit_value_as_rep_ctx(ctx, out, v, rep);
        return;
    }
    if (plan->action == XR_TRANSFER_EXPLICIT_COPY && cg_transfer_plan_has_boundary_clone(plan)) {
        fprintf(out, "(");
        fprintf(out, "%s(",
                xi_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                               : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")).ptr");
        return;
    }
    if ((plan->action == XR_TRANSFER_CONST_SHARE || plan->action == XR_TRANSFER_SYNC_SHARE) &&
        cg_transfer_plan_has_boundary_clone(plan)) {
        fprintf(out, "((void)xrt_retain(");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, "), ");
        emit_value_as_rep_ctx(ctx, out, v, rep);
        fprintf(out, ")");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v, rep);
}

static void emit_coro_transfer_xrvalue(XiCgenCtx *ctx, FILE *out, const XiValue *v, uint8_t mode) {
    if (mode == XR_TRANSFER_COPY && xi_coro_value_needs_boundary_clone(v)) {
        fprintf(out, "%s(",
                xi_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                               : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")");
        return;
    }
    if (mode == XR_TRANSFER_SHARE && xi_coro_value_needs_boundary_clone(v)) {
        fprintf(out, "((void)xrt_retain(");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, "), ");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
}

static void emit_coro_transfer_as_rep(XiCgenCtx *ctx, FILE *out, const XiValue *v, XrRep rep,
                                      uint8_t mode) {
    if (rep == XR_REP_TAGGED) {
        emit_coro_transfer_xrvalue(ctx, out, v, mode);
        return;
    }
    if (mode == XR_TRANSFER_COPY && xi_coro_value_needs_boundary_clone(v)) {
        fprintf(out, "(");
        fprintf(out, "%s(",
                xi_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                               : "xrt_value_clone_for_coro");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, ")).ptr");
        return;
    }
    if (mode == XR_TRANSFER_SHARE && xi_coro_value_needs_boundary_clone(v)) {
        fprintf(out, "((void)xrt_retain(");
        emit_value_as_rep_ctx(ctx, out, v, XR_REP_TAGGED);
        fprintf(out, "), ");
        emit_value_as_rep_ctx(ctx, out, v, rep);
        fprintf(out, ")");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v, rep);
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

static void emit_coro_send_value(XiCgenCtx *ctx, FILE *out, const XiValue *send_value,
                                 const XiValue *send_arg) {
    if (send_arg) {
        emit_vref(out, send_arg);
        return;
    }
    emit_value_as_rep_ctx(ctx, out, send_value, XR_REP_TAGGED);
}

static void emit_coro_runtime_channel_bridge_temp(XiCgenCtx *ctx, FILE *out,
                                                  const XiValue *send_value, uint32_t id,
                                                  const char *name_prefix,
                                                  const XaotTransferPlan *transfer_plan,
                                                  char *value_name, size_t value_name_size,
                                                  char *mode_name, size_t mode_name_size) {
    uint8_t transfer_mode = transfer_plan ? transfer_plan->mode : XR_TRANSFER_SHARE;
    snprintf(value_name, value_name_size, "_%s_send_value_%u", name_prefix, id);
    snprintf(mode_name, mode_name_size, "_%s_transfer_mode_%u", name_prefix, id);

    fprintf(out, "    XrValue %s = ", value_name);
    emit_value_as_rep_ctx(ctx, out, send_value, XR_REP_TAGGED);
    fprintf(out, ";\n");
    fprintf(out, "    uint8_t %s = %u;\n", mode_name, (unsigned) transfer_mode);
    fprintf(out, "    if (XR_IS_STR(%s)) {\n", value_name);
    fprintf(out, "        %s = xr_aot_bridge_xrt_to_runtime(ctx, %s);\n", value_name, value_name);
    fprintf(out, "        %s = XR_TRANSFER_MOVE;\n", mode_name);
    fprintf(out, "    }\n");
}

static const char *cg_coro_typed_recv_pair_helper(const XiFunc *f, const XiValue *v) {
    const XiValue *slot_value = xi_coro_typed_recv_unbox_user(f, v);
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
    } else if (rep == XR_REP_RAWPTR) {
        fprintf(out, "(void *)(uintptr_t)XR_TO_INT(%s)", temp_name);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "%s.ptr", temp_name);
    } else {
        fprintf(out, "%s", temp_name);
    }
    fprintf(out, ";\n");
}

/* Suspend-call results cross the runtime boundary as XrValue even when the
 * verified destination representation is an inline enum aggregate.  Keep that
 * boundary conversion here, next to the other coroutine result conversions,
 * rather than letting the statement emitter assign incompatible C types. */
static void emit_assign_from_xrvalue_temp_ctx(XiCgenCtx *ctx, FILE *out, const XiValue *dst,
                                              const char *temp_name) {
    const XaotValuePlan *plan = cg_value_plan(ctx, dst);
    if (!plan || !cg_value_rep_is_adt_aggregate(plan->rep)) {
        emit_assign_from_xrvalue_temp(out, dst, temp_name);
        return;
    }
    fprintf(out, "    ");
    emit_vref(out, dst);
    fprintf(out, " = ");
    if (cg_value_rep_is_typed_adt_aggregate(plan->rep))
        fprintf(out, "%s_from_base(", plan->rep.c_type);
    fprintf(out, "xrt_enum_aggregate_from_boxed(%s)", temp_name);
    if (cg_value_rep_is_typed_adt_aggregate(plan->rep))
        fprintf(out, ")");
    fprintf(out, ";\n");
}

static void emit_assign_from_bool_temp(FILE *out, const XiValue *dst, const char *temp_name) {
    fprintf(out, "    ");
    emit_vref(out, dst);
    fprintf(out, " = ");
    if (cg_rep(dst) == XR_REP_TAGGED)
        fprintf(out, "XR_FROM_BOOL(%s)", temp_name);
    else
        fprintf(out, "%s", temp_name);
    fprintf(out, ";\n");
}

static void emit_assign_from_i64_temp(FILE *out, const XiValue *dst, const char *temp_name) {
    fprintf(out, "    ");
    emit_vref(out, dst);
    fprintf(out, " = ");
    if (cg_rep(dst) == XR_REP_TAGGED)
        fprintf(out, "XR_FROM_INT(%s)", temp_name);
    else
        fprintf(out, "%s", temp_name);
    fprintf(out, ";\n");
}

static XrRep cg_coro_param_rep(XiCgenCtx *ctx, const XiFunc *f, uint16_t index);

static uint16_t cg_coro_param_count(const XiFunc *f) {
    return f ? (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0)) : 0;
}

/* An error crossing a coroutine boundary keeps its channel (task 248): a value
 * error lands in the pending-error slot for the following ERR_CHECK to route to
 * `catch (e)` or propagate by value, a panic keeps unwinding out of the frame.
 * Shared by every re-raising boundary in a generated coroutine. */
static void emit_coro_boundary_error_check(FILE *out, const char *temp, uint32_t id) {
    fprintf(out, "    if (%s%u.kind == XR_AOT_RUN_ERROR) {\n", temp, id);
    fprintf(out, "        f->state = 0;\n");
    fprintf(out, "        if (%s%u.error_is_value) {\n", temp, id);
    fprintf(out, "            xrt_pending_error = %s%u.error;\n", temp, id);
    fprintf(out, "        } else {\n");
    fprintf(out, "            return %s%u;\n", temp, id);
    fprintf(out, "        }\n");
    fprintf(out, "    }\n");
}

static void emit_coro_scope_exit_error_check(FILE *out, uint32_t id) {
    emit_coro_boundary_error_check(out, "_scope_exit_", id);
}

/* Tail of a coroutine-to-coroutine call's terminal-failure branch.  The callee
 * ended on the value channel: hand the error to this frame's pending-error slot
 * and rejoin the normal path, where the call's ERR_CHECK routes it exactly as it
 * would a same-frame throw.  A panic, or a cancellation, still ends this frame.
 * Emitted inside the branch, after the caller's own frame cleanup. */
static void emit_coro_call_error_channel_dispatch(FILE *out, uint32_t id, int sid) {
    fprintf(out, "        f->state = 0;\n");
    fprintf(out, "        if (_call_%u.kind == XR_AOT_RUN_ERROR && _call_%u.error_is_value) {\n",
            id, id);
    fprintf(out, "            xrt_pending_error = _call_%u.error;\n", id);
    fprintf(out, "            goto S%d_DONE;\n", sid);
    fprintf(out, "        }\n");
    fprintf(out, "        return _call_%u;\n", id);
}

static void emit_aot_coro_op_stmt(FILE *out, const XiFunc *f, const XiValue *v) {
    char args_name[64];
    char tmp_name[64];
    snprintf(args_name, sizeof(args_name), "_coro_args_%u", v->id);
    snprintf(tmp_name, sizeof(tmp_name), "_coro_value_%u", v->id);

    if (v->nargs > 0) {
        fprintf(out, "    XrValue %s[%u] = {", args_name, (unsigned) v->nargs);
        for (uint16_t i = 0; i < v->nargs; i++) {
            if (i > 0)
                fprintf(out, ", ");
            emit_boxed_value_ref(out, v->args[i]);
        }
        fprintf(out, "};\n");
    }

    fprintf(out, "    XrValue %s = xr_aot_coro_op(ctx, %d, %s, %u);\n", tmp_name, (int) v->aux_int,
            v->nargs > 0 ? args_name : "NULL", (unsigned) v->nargs);
    if (cg_coro_value_has_storage(f, v))
        emit_assign_from_xrvalue_temp(out, v, tmp_name);
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

static bool cg_coro_value_terminates_c_path(const XiValue *v) {
    return v && (v->op == XI_ERR_RETURN || v->op == XI_THROW);
}

static void emit_assign_coro_param_from_xrvalue(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                uint16_t index) {
    XrRep rep = cg_coro_param_rep(ctx, f, index);
    fprintf(out, "    f->p%u = ", index);
    const char *conv_suffix =
        emit_conversion_prefix(out, f->params[index]->type, XR_REP_TAGGED, rep);
    fprintf(out, "p%u", index);
    emit_conversion_suffix(out, conv_suffix);
    fprintf(out, ";\n");
}

static bool cg_coro_value_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);

static void emit_coro_slot_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix,
                               const XiValue *v) {
    (void) prefix;
    const XiValue *slot_value = xi_coro_typed_recv_unbox_user(f, v);
    if (!slot_value)
        slot_value = v;
    /* Frame members live inside the heap-allocated coroutine frame 'f', so their
     * address is a small frame-relative offset that stays valid across
     * suspend/resume. Non-frame values are plain C locals: their address is only
     * valid within the current resume execution and MUST NOT be encoded as a
     * frame offset. (uint8_t *)&local - (uint8_t *)f is a full 64-bit distance
     * truncated to uint32_t; it round-trips only when the heap frame and the C
     * stack share their top 32 bits (true in a normal build, but NOT under ASan,
     * where the heap sits far from the stack, so f + offset dereferences an
     * unmapped high address -> BUS/WRITE). Every slot store to a non-frame value
     * is synchronous within the same resume (the await/recv helpers only touch
     * the slot on the READY path), so a raw pointer to the C local is valid. */
    if (cg_coro_value_needs_frame(ctx, f, slot_value)) {
        fprintf(out, "xr_slot_aot_frame_offset(f, (uint32_t)((uint8_t *)&");
        emit_vref(out, slot_value);
        fprintf(out, " - (uint8_t *)f), %u)", (unsigned) cg_rep(slot_value));
        return;
    }
    XrRep rep = cg_rep(slot_value);
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "xr_slot_xvalue_ptr(&");
        emit_vref(out, slot_value);
        fprintf(out, ")");
    } else {
        fprintf(out, "xr_slot_native_ptr(&");
        emit_vref(out, slot_value);
        fprintf(out, ", %u)", (unsigned) rep);
    }
}

static void emit_coro_optional_slot_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *prefix, const XiValue *v) {
    if (cg_coro_value_has_storage(f, v)) {
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        return;
    }
    fprintf(out, "xr_slot_none()");
}

static bool cg_coro_can_use_xvalue_result_ptr(const XiFunc *f, const XiValue *v) {
    return cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED;
}

static const XiCoroPlan *cg_coro_plan(XiCgenCtx *ctx, const XiFunc *f);

static bool cg_coro_i64_optional_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiValue *root) {
    return root && cg_value_is_i64_optional_blocking_result_root(root) &&
           cg_value_is_elided_i64_optional_blocking_result(f, root) &&
           xi_coro_plan_is_logical_member(cg_coro_plan(ctx, f), root);
}

static void emit_coro_i64_optional_has_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *root) {
    if (cg_coro_i64_optional_needs_frame(ctx, f, root))
        fprintf(out, "f->v%u_opt_has", root->id);
    else
        fprintf(out, "v%u_opt_has", root->id);
}

static void emit_coro_i64_optional_value_ref(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const XiValue *root) {
    if (cg_coro_i64_optional_needs_frame(ctx, f, root))
        fprintf(out, "f->v%u_opt_value", root->id);
    else
        fprintf(out, "v%u_opt_value", root->id);
}

static void emit_coro_await_result_slot(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *prefix, const XiValue *await_value,
                                        const XiValue *typed_slot_value) {
    if (typed_slot_value) {
        emit_coro_slot_ref(ctx, out, f, prefix, typed_slot_value);
        return;
    }
    if (cg_coro_await_needs_tagged_boundary_temp(ctx, f, await_value)) {
        fprintf(out, "xr_slot_xvalue_ptr(&");
        emit_coro_await_boundary_temp_ref(out, await_value);
        fprintf(out, ")");
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

static void emit_coro_await_all_result_slots(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const char *prefix,
                                             const CgAwaitAllScalarResult *scalar,
                                             uint32_t await_id, const char *name_prefix) {
    XR_DCHECK(scalar != NULL, "emit_coro_await_all_result_slots: NULL scalar result");
    fprintf(out, "    XrSlotRef %s%u[%u] = {", name_prefix, await_id, scalar->count);
    for (uint32_t i = 0; i < scalar->count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        emit_coro_slot_ref(ctx, out, f, prefix, scalar->index_values[i]);
    }
    fprintf(out, "};\n");
}

static const char *cg_coro_await_all_result_elem_name(XiCgenCtx *ctx, const XiValue *await_value) {
    CgArrayElemInfo info;
    if (cg_array_elem_info_from_type_ctx(ctx, await_value ? await_value->type : NULL, &info) ||
        cg_array_elem_info_from_type(await_value ? await_value->type : NULL, &info)) {
        return info.elem_name ? info.elem_name : "XR_ELEM_ANY";
    }
    return "XR_ELEM_ANY";
}

static const char *cg_coro_array_value_elem_name(XiCgenCtx *ctx, const XiValue *array_value) {
    CgArrayElemInfo info;
    if (cg_array_elem_info_from_type_ctx(ctx, array_value ? array_value->type : NULL, &info) ||
        cg_array_elem_info_from_type(array_value ? array_value->type : NULL, &info)) {
        return info.elem_name ? info.elem_name : "XR_ELEM_ANY";
    }
    return "XR_ELEM_ANY";
}

static bool cg_coro_await_all_result_needs_boundary_clone(const char *elem_name) {
    return !elem_name || strcmp(elem_name, "XR_ELEM_ANY") == 0;
}

static bool cg_coro_aggregate_await_tasks_need_heap_clone(const XiValue *tasks) {
    const XiValue *cur = tasks;
    while (cur && (cur->op == XI_BOX || cur->op == XI_UNBOX || xi_copy_is_identity_alias(cur)) &&
           cur->nargs >= 1) {
        cur = cur->args[0];
    }
    return cur && cur->op == XI_STACK_ALLOC && cur->aux_int == XI_ARRAY_NEW;
}

static const XiValue *cg_coro_await_task_index_array(const XiValue *task_value) {
    const XiValue *cur = task_value;
    for (uint8_t depth = 0; cur && depth < 8; depth++) {
        if (cur->op == XI_INDEX_GET && cur->nargs >= 2)
            return cur->args[0];
        if ((cur->op == XI_BOX || cur->op == XI_UNBOX || xi_copy_is_identity_alias(cur)) &&
            cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }
    return NULL;
}

static const XiValue *cg_coro_await_task_index_value(const XiValue *task_value) {
    const XiValue *cur = task_value;
    for (uint8_t depth = 0; cur && depth < 8; depth++) {
        if (cur->op == XI_INDEX_GET && cur->nargs >= 2)
            return cur->args[1];
        if ((cur->op == XI_BOX || cur->op == XI_UNBOX || xi_copy_is_identity_alias(cur)) &&
            cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }
    return NULL;
}

static void emit_coro_debug_result_source_var_sync(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v) {
    if (!v)
        return;

    const XiValue *slot_value = NULL;
    if (v->op == XI_AWAIT)
        slot_value = xi_coro_typed_await_unbox_user(f, v);
    if (!slot_value)
        slot_value = xi_coro_typed_recv_unbox_user(f, v);

    if (slot_value && slot_value != v)
        emit_debug_source_var_sync(ctx, out, f, slot_value);
    emit_debug_source_var_sync(ctx, out, f, v);
}

static const XiFunc *cg_coro_direct_suspend_call_target(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v);
static const XaotCallableInvokePlan *cg_coro_callable_target_switch_plan(XiCgenCtx *ctx,
                                                                         const XiValue *v);
static bool cg_coro_call_needs_child_frame(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v);
static bool cg_coro_test_yield_add_call(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v);

/* The shared coroutine plan for 'f', wired to the ctx-level resolver.  Cached
 * on f->coro_plan by xi_coro_analyze, so repeated lookups are cheap. */
static const XiCoroPlan *cg_coro_plan(XiCgenCtx *ctx, const XiFunc *f) {
    const XiCoroResolver resolver = cg_coro_resolver_ctx(ctx);
    return xi_coro_analyze((XiFunc *) f, &resolver);
}

/* Cross-suspend liveness is shared IR analysis: read it from the plan slot
 * instead of recomputing a backend-local XiLiveness. */
static bool cg_coro_value_live_across_suspend(XiCgenCtx *ctx, const XiFunc *f,
                                              const XiValue *target) {
    const XiCoroSlot *slot = xi_coro_plan_find_slot(cg_coro_plan(ctx, f), target);
    return slot && slot->live_across;
}

/* Physical frame membership: a value occupies a frame slot iff it has backend
 * storage and the shared IR plan lists it as a logical member. */
static bool cg_coro_value_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    return cg_coro_value_has_storage(f, v) &&
           xi_coro_plan_is_logical_member(cg_coro_plan(ctx, f), v);
}

static bool cg_coro_phi_needs_frame(XiCgenCtx *ctx, const XiFunc *f, const XiPhi *phi) {
    return cg_phi_has_storage(phi) && cg_coro_value_live_across_suspend(ctx, f, &phi->value);
}

static bool cg_coro_value_may_hold_frame_root(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (v && v->op == XI_GO)
        return true;
    return cg_coro_value_live_across_suspend(ctx, f, v) || xi_coro_value_needs_runtime_slot(v) ||
           xi_coro_value_is_aggregate_await_tasks(f, v);
}

static bool cg_coro_value_is_borrowed_unbox_alias(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v) {
    if (!v || v->op != XI_UNBOX || v->nargs < 1 || !v->args[0] || cg_rep(v) != XR_REP_PTR ||
        cg_rep(v->args[0]) != XR_REP_TAGGED || !xi_coro_value_rep_can_trace_root(v))
        return false;

    const XiValue *source = v->args[0];
    if (source->op == XI_PARAM)
        return true;
    return cg_coro_value_needs_frame(ctx, f, source);
}

static const XiValue *cg_coro_unwrap_borrowed_identity_alias(const XiValue *v) {
    const XiValue *cur = v;
    while (cur && (cur->op == XI_BOX || cur->op == XI_UNBOX || xi_copy_is_identity_alias(cur)) &&
           cur->nargs >= 1) {
        cur = cur->args[0];
    }
    return cur;
}

static bool cg_coro_value_is_borrowed_identity_alias(XiCgenCtx *ctx, const XiFunc *f,
                                                     const XiValue *v) {
    if (!v || !(v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v)) ||
        v->nargs < 1 || !xi_coro_value_rep_can_trace_root(v))
        return false;

    const XiValue *origin = cg_coro_unwrap_borrowed_identity_alias(v);
    if (!origin || origin == v)
        return false;

    if (origin->op == XI_PARAM || origin->op == XI_GET_SHARED || origin->op == XI_IMPORT_REF)
        return true;
    return cg_coro_value_needs_frame(ctx, f, origin);
}

static const XaotValuePlan *cg_coro_optional_value_plan(XiCgenCtx *ctx, const XiValue *v) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    return bundle && v ? xaot_bundle_find_value_plan(bundle, v) : NULL;
}

static const XaotEnumPlan *cg_coro_value_enum_plan(XiCgenCtx *ctx, const XiValue *v) {
    const XaotValuePlan *value_plan = cg_coro_optional_value_plan(ctx, v);
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!value_plan || !bundle || !cg_value_rep_is_adt_aggregate(value_plan->rep))
        return NULL;
    if (value_plan->rep.c_type) {
        uint32_t index = 0;
        if (cg_enum_plan_index_for_ctype(ctx, value_plan->rep.c_type, &index) &&
            index < bundle->nenum_plans)
            return &bundle->enum_plans[index];
    }
    return xaot_bundle_find_enum_plan_for_type(bundle, value_plan->rep.type ? value_plan->rep.type
                                                                            : (v ? v->type : NULL));
}

static uint16_t cg_coro_value_frame_root_width(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    (void) f;
    const XaotValuePlan *value_plan = cg_coro_optional_value_plan(ctx, v);
    if (value_plan && value_plan->rep.kind == XAOT_VALUE_AGGREGATE) {
        const XaotEnumPlan *enum_plan = cg_coro_value_enum_plan(ctx, v);
        return enum_plan ? enum_plan->max_payload : 0;
    }
    return xi_coro_value_rep_can_trace_root(v) ? 1u : 0u;
}

static bool cg_coro_value_can_trace_frame_slot(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    return cg_coro_value_frame_root_width(ctx, f, v) > 0 &&
           !cg_coro_value_is_borrowed_unbox_alias(ctx, f, v) &&
           !cg_coro_value_is_borrowed_identity_alias(ctx, f, v);
}

static bool cg_coro_value_needs_frame_arc_release(XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v) {
    const XiValue *origin = cg_unwrap_identity_value(v);
    if (origin && origin->op == XI_GET_SHARED) {
        int slot = (int) origin->aux_int;
        if (cg_shared_function_slot_target(ctx, f, slot) ||
            (origin->type && XR_TYPE_IS_FUNCTION(origin->type)))
            return false;
    }
    const XaotValuePlan *value_plan = cg_coro_optional_value_plan(ctx, v);
    bool owns_runtime_refs = value_plan && value_plan->rep.kind == XAOT_VALUE_AGGREGATE
                                 ? cg_coro_value_frame_root_width(ctx, f, v) > 0
                                 : xi_coro_value_needs_arc_release(v);
    return owns_runtime_refs && !cg_coro_value_is_borrowed_unbox_alias(ctx, f, v) &&
           !cg_coro_value_is_borrowed_identity_alias(ctx, f, v);
}

static bool cg_coro_return_value_needs_retain(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_PARAM)
        return xi_coro_value_needs_arc_release(v);
    if (cg_coro_value_needs_frame(ctx, f, v) && cg_coro_value_may_hold_frame_root(ctx, f, v) &&
        cg_coro_value_needs_frame_arc_release(ctx, f, v))
        return true;

    const XiValue *origin = cg_unwrap_identity_value(v);
    if (!origin || origin == v)
        return false;
    if (origin->op == XI_PARAM)
        return xi_coro_value_needs_arc_release(origin);
    return cg_coro_value_needs_frame(ctx, f, origin) &&
           cg_coro_value_may_hold_frame_root(ctx, f, origin) &&
           cg_coro_value_needs_frame_arc_release(ctx, f, origin);
}

static void emit_coro_aot_done_value(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                     const XiValue *value) {
    if (!value) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    if (cg_coro_return_value_needs_retain(ctx, f, value)) {
        fprintf(out, "({ XrValue _xrt_ret = ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, "; xrt_retain(_xrt_ret); _xrt_ret; })");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
}

static bool cg_coro_param_is_native_receiver(XiCgenCtx *ctx, const XiFunc *f, uint16_t index) {
    return index == 0 && cg_class_func_uses_native_receiver(ctx, f);
}

static XrRep cg_coro_param_rep(XiCgenCtx *ctx, const XiFunc *f, uint16_t index) {
    if (cg_coro_param_is_native_receiver(ctx, f, index))
        return XR_REP_PTR;
    return cg_rep(f->params[index]);
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

static size_t estimate_coro_frame_size(XiCgenCtx *ctx, const XiFunc *f) {
    size_t size = 0;
    size_t max_align = 1;
    cg_coro_layout_add(&size, &max_align, sizeof(uint32_t), _Alignof(uint32_t));
    if (cg_func_frame_needs_cl(f))
        cg_coro_layout_add(&size, &max_align, sizeof(void *), _Alignof(void *));
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++)
        cg_coro_layout_add_rep(&size, &max_align, cg_coro_param_rep(ctx, f, i));
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, phi))
                cg_coro_layout_add_rep(&size, &max_align, cg_coro_decl_rep(ctx, f, &phi->value));
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_test_yield_add_call(ctx, f, v))
                cg_coro_layout_add(&size, &max_align, sizeof(int64_t), _Alignof(int64_t));
            if (cg_coro_call_needs_child_frame(ctx, f, v)) {
                cg_coro_layout_add(&size, &max_align, sizeof(void *), _Alignof(void *));
                if (cg_coro_callable_target_switch_plan(ctx, v))
                    cg_coro_layout_add(&size, &max_align, sizeof(uint32_t), _Alignof(uint32_t));
            }
            if (cg_coro_value_needs_frame(ctx, f, v))
                cg_coro_layout_add_rep(&size, &max_align, cg_coro_decl_rep(ctx, f, v));
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

typedef struct CgCoroSuspendCallSite {
    CgStaticFunctionCall call;
    const XiValue *cl_source;
    uint16_t arg_start;
} CgCoroSuspendCallSite;

static CgCoroSuspendCallSite cg_no_coro_suspend_call_site(void) {
    CgCoroSuspendCallSite site;
    site.call = cg_no_static_function_call();
    site.cl_source = NULL;
    site.arg_start = 0;
    return site;
}

static CgCoroSuspendCallSite
cg_coro_direct_suspend_call_site_info(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v);

static CgStaticFunctionCall
cg_coro_direct_suspend_call_target_info(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v) {
    return cg_coro_direct_suspend_call_site_info(ctx, current, v).call;
}

static const XiFunc *cg_coro_direct_suspend_call_target(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *v) {
    return cg_coro_direct_suspend_call_target_info(ctx, current, v).func;
}

static const XaotCallableInvokePlan *cg_coro_callable_target_switch_plan(XiCgenCtx *ctx,
                                                                         const XiValue *v) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotCallableInvokePlan *plan = xaot_bundle_find_callable_invoke_plan(bundle, v);
    if (!plan || plan->action != XAOT_CALLABLE_TARGET_SWITCH || plan->target_count < 2 ||
        (plan->effect_bits & XG_BODY_MAY_SUSPEND) == 0)
        return NULL;
    return plan;
}

static bool cg_coro_call_needs_child_frame(XiCgenCtx *ctx, const XiFunc *current,
                                           const XiValue *v) {
    return cg_coro_direct_suspend_call_target(ctx, current, v) != NULL ||
           cg_coro_callable_target_switch_plan(ctx, v) != NULL;
}

static CgStaticFunctionCall cg_coro_verified_direct_method_call(XiCgenCtx *ctx, const XiValue *v) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    const XaotMethodDispatchPlan *plan =
        xaot_bundle_find_method_dispatch_plan_for_xi_call(bundle, v);
    if (!bundle || !plan || plan->kind != XAOT_DISPATCH_DIRECT || plan->target_count != 1 ||
        plan->target_start == 0 || plan->target_start - 1 >= bundle->ndispatch_target_cases)
        return cg_no_static_function_call();

    const XaotDispatchTargetCase *target = &bundle->dispatch_target_cases[plan->target_start - 1];
    const char *target_prefix = NULL;
    const XiFunc *target_func =
        xaot_bundle_find_dispatch_target_func(bundle, target, &target_prefix);
    if (!target_func)
        return cg_no_static_function_call();
    return cg_static_function_call(target_func, target_prefix);
}

static CgCoroSuspendCallSite cg_coro_direct_call_site_info(XiCgenCtx *ctx, const XiFunc *current,
                                                           const XiValue *v) {
    if (!v || v->nargs < 1)
        return cg_no_coro_suspend_call_site();

    CgCoroSuspendCallSite site = cg_no_coro_suspend_call_site();
    if (v->op == XI_CALL) {
        site.call = cg_resolve_static_function_call(ctx, current, v->args[0]);
        if (!site.call.func) {
            const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
            const XaotCallableInvokePlan *plan = xaot_bundle_find_callable_invoke_plan(bundle, v);
            const XaotCallableTargetCase *target =
                xaot_bundle_callable_target_case(bundle, plan, 0);
            if (plan && plan->target_count == 1 && target && target->target_func)
                site.call = cg_static_function_call(
                    target->target_func, cg_module_prefix_for_func(ctx, target->target_func));
        }
        site.cl_source = v->args[0];
        site.arg_start = 1;
    } else if (v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) {
        const char *method = (const char *) v->aux;
        bool is_super = v->op == XI_CALL_METHOD && (v->aux_int & 1) != 0;
        const XiFunc *ctor = NULL;
        const char *ctor_prefix = NULL;
        const XiClassData *ctor_class =
            cg_class_native_ctor_call_data(ctx, current, v, &ctor, &ctor_prefix);
        if (ctor && ctor_class)
            site.call = cg_static_class_constructor_data_call(ctor, ctor_prefix, ctor_class);
        if (!is_super && method && !site.call.func) {
            site.call = cg_resolve_module_member_call(ctx, current, v, method);
            if (site.call.func)
                site.arg_start = 1;
        }
        if (!site.call.func)
            site.call = cg_coro_verified_direct_method_call(ctx, v);
        if (!site.call.func) {
            const char *method_prefix = NULL;
            const XiFunc *mfunc =
                cg_class_native_resolve_method_call(ctx, current, v, &method_prefix);
            if (mfunc)
                site.call = cg_static_function_call(mfunc, method_prefix);
        }
        site.cl_source = NULL;
    }

    return site;
}

static CgCoroSuspendCallSite
cg_coro_direct_suspend_call_site_info(XiCgenCtx *ctx, const XiFunc *current, const XiValue *v) {
    CgCoroSuspendCallSite site = cg_coro_direct_call_site_info(ctx, current, v);
    if (!site.call.func || !cg_func_needs_aot_coro_ctx(ctx, site.call.func))
        return cg_no_coro_suspend_call_site();
    return site;
}

static bool cg_func_frame_needs_cl(const XiFunc *f) {
    return f && f->ncaptures > 0;
}

static bool cg_func_can_emit_sync_go_wrapper_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    return f && !cg_func_needs_aot_coro_ctx(ctx, f);
}

static bool cg_func_can_emit_sync_backedge_heartbeat_ctx(XiCgenCtx *ctx, const XiFunc *f) {
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

static bool cg_sync_heartbeat_target_marked(const XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target)
        return false;
    for (int i = 0; i < ctx->nsync_heartbeat_targets; i++) {
        if (ctx->sync_heartbeat_targets[i] == target)
            return true;
    }
    return false;
}

static bool cg_mark_sync_heartbeat_target(XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target)
        return false;
    if (cg_sync_heartbeat_target_marked(ctx, target))
        return true;
    if (ctx->nsync_heartbeat_targets >= CG_MAX_SYNC_HEARTBEAT_TARGETS) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: too many sync AOT heartbeat targets\n");
        return false;
    }
    ctx->sync_heartbeat_targets[ctx->nsync_heartbeat_targets++] = target;
    return true;
}

static bool cg_func_needs_sync_backedge_heartbeat_ctx(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_can_emit_sync_backedge_heartbeat_ctx(ctx, f) &&
           cg_sync_heartbeat_target_marked(ctx, f);
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
            if (!v || (v->op != XI_GO && v->op != XI_THREAD_SPAWN) || v->nargs < 1)
                continue;
            CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, f, v->args[0]);
            if (call.func && cg_func_can_emit_sync_go_wrapper_ctx(ctx, call.func)) {
                (void) cg_mark_sync_go_target(ctx, call.func);
                (void) cg_mark_sync_heartbeat_target(ctx, call.func);
            }
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++)
        cg_collect_sync_go_targets_from_func(ctx, f->children[i]);
}

static void cg_collect_sync_heartbeat_targets_from_func(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return;

    if (cg_func_needs_aot_coro_ctx(ctx, f)) {
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            const XiBlock *blk = f->blocks[bi];
            if (!blk)
                continue;
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                const XiValue *v = blk->values[vi];
                if (!v)
                    continue;
                CgCoroSuspendCallSite site = cg_coro_direct_call_site_info(ctx, f, v);
                if (site.call.func &&
                    cg_func_can_emit_sync_backedge_heartbeat_ctx(ctx, site.call.func))
                    (void) cg_mark_sync_heartbeat_target(ctx, site.call.func);
            }
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++)
        cg_collect_sync_heartbeat_targets_from_func(ctx, f->children[i]);
}

static void cg_reset_sync_go_targets(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    memset(ctx->sync_go_targets, 0, sizeof(ctx->sync_go_targets));
    ctx->nsync_go_targets = 0;
    memset(ctx->sync_heartbeat_targets, 0, sizeof(ctx->sync_heartbeat_targets));
    ctx->nsync_heartbeat_targets = 0;
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
        cg_collect_sync_heartbeat_targets_from_func(ctx, module->init);
    }
}

static void emit_aot_frame_new_params(FILE *out, const XiFunc *f, bool typed_params) {
    bool need_comma = false;
    uint16_t total_params = (uint16_t) (f->nparams + (f->is_vararg ? 1 : 0));
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "xrt_closure_t *_cl, bool _take_cl");
        need_comma = true;
    }
    for (uint16_t i = 0; i < total_params; i++) {
        if (need_comma)
            fprintf(out, ", ");
        fprintf(out, "%s p%u", typed_params ? ctype_str(cg_rep(f->params[i])) : "XrValue", i);
        need_comma = true;
    }
}

static void emit_aot_frame_raw_cl_arg(FILE *out, const XiFunc *current, const XiValue *callee,
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
    if (callee) {
        fprintf(out, "(xrt_closure_t *)");
        emit_vref(out, callee);
        if (cg_rep(callee) == XR_REP_TAGGED)
            fprintf(out, ".ptr");
        return;
    }
    fprintf(out, "NULL");
}

static bool emit_aot_frame_transfer_cl_arg(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                           const XiValue *callee, const XiFunc *target) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!bundle || !target || target->ncaptures == 0)
        return false;
    for (uint16_t ci = 0; ci < target->ncaptures; ci++) {
        const XaotCapturePlan *plan = xaot_capture_plan_find(bundle, target, ci);
        if (!plan || plan->action == XR_TRANSFER_REJECT ||
            plan->action == XR_TRANSFER_MOVE_UNIQUE) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: rejected AOT cross-execution capture target=%s index=%u\n",
                    target->name ? target->name : "?", (unsigned) ci);
            fprintf(out, "NULL");
            return true;
        }
    }
    fprintf(out, "({ xrt_closure_t *_src = ");
    emit_aot_frame_raw_cl_arg(out, current, callee, target);
    fprintf(out,
            "; xrt_closure_t *_dst = NULL; if (_src) { XrValue _dstv = "
            "xrt_closure_new(_src->callable, %u); _dst = (xrt_closure_t *)_dstv.ptr; ",
            (unsigned) target->ncaptures);
    for (uint16_t ci = 0; ci < target->ncaptures; ci++) {
        const XaotCapturePlan *plan = xaot_capture_plan_find(bundle, target, ci);
        fprintf(out, "_dst->upvals[%u] = ", (unsigned) ci);
        if (plan->action == XR_TRANSFER_EXPLICIT_COPY)
            fprintf(out, "xrt_value_clone_for_coro(_src->upvals[%u]); ", (unsigned) ci);
        else {
            fprintf(out, "_src->upvals[%u]; xrt_retain(_dst->upvals[%u]); ", (unsigned) ci,
                    (unsigned) ci);
        }
    }
    fprintf(out, "} _dst; })");
    return true;
}

static bool cg_aot_frame_new_can_supply_cl_arg(const XiFunc *current, const XiValue *callee,
                                               const XiFunc *target) {
    (void) current;
    if (!cg_func_frame_needs_cl(target))
        return true;
    return callee != NULL;
}

static void emit_xrvalue_from_native_expr(FILE *out, const XrType *type, XrRep rep,
                                          const char *expr);
static bool cg_sync_go_param_needs_release(const XiFunc *f, uint16_t index);

static void emit_aot_frame_new_call_args(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                         const XiValue *callee, const XiFunc *target,
                                         bool typed_params, XiValue *const *args,
                                         uint16_t arg_start, uint16_t nargs,
                                         const XiValue *transfer_owner) {
    bool need_comma = false;
    bool transfer_owner_has_plans = cg_transfer_plan_site_is_spawn_arg(transfer_owner);
    uint16_t arg_end = nargs;
    if (target->is_vararg) {
        uint32_t fixed_end = (uint32_t) arg_start + target->nparams;
        if (fixed_end < arg_end)
            arg_end = (uint16_t) fixed_end;
    }
    if (cg_func_frame_needs_cl(target)) {
        if (!transfer_owner_has_plans ||
            !emit_aot_frame_transfer_cl_arg(ctx, out, current, callee, target))
            emit_aot_frame_raw_cl_arg(out, current, callee, target);
        fprintf(out, ", %s", transfer_owner_has_plans ? "true" : "false");
        need_comma = true;
    }
    for (uint16_t a = arg_start; a < arg_end; a++) {
        uint16_t transfer_slot = (uint16_t) (a - arg_start);
        const XaotTransferPlan *transfer_plan = NULL;
        uint8_t transfer_mode = transfer_owner ? XR_TRANSFER_SHARE : XR_TRANSFER_COPY;
        if (transfer_owner_has_plans) {
            transfer_plan = cg_required_transfer_plan(ctx, transfer_owner, transfer_slot, args[a],
                                                      "frame argument");
        }
        if (need_comma)
            fprintf(out, ", ");
        if (!typed_params || a - arg_start >= target->nparams) {
            if (transfer_owner_has_plans)
                emit_coro_transfer_plan_xrvalue(ctx, out, args[a], transfer_plan);
            else
                emit_coro_transfer_xrvalue(ctx, out, args[a], transfer_mode);
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
            } else if (param_rep == XR_REP_PTR || param_rep == XR_REP_RAWPTR) {
                if (transfer_owner_has_plans)
                    emit_coro_transfer_plan_as_rep(ctx, out, args[a], param_rep, transfer_plan);
                else
                    emit_coro_transfer_as_rep(ctx, out, args[a], param_rep, transfer_mode);
            } else {
                if (transfer_owner_has_plans)
                    emit_coro_transfer_plan_xrvalue(ctx, out, args[a], transfer_plan);
                else
                    emit_coro_transfer_xrvalue(ctx, out, args[a], transfer_mode);
            }
        }
        need_comma = true;
    }
    if (target->is_vararg) {
        if (need_comma)
            fprintf(out, ", ");
        /* The frame ABI has exactly one trailing rest-Array slot, matching the
         * ordinary direct-call ABI.  Never pass expanded varargs to a frame
         * factory: doing so loses the callee's rest parameter identity across
         * translation units. */
        emit_vararg_rest_expr_values(ctx, out, transfer_owner ? transfer_owner->id : 0, args, nargs,
                                     target, arg_start);
    }
}

static void emit_sync_go_frame_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "typedef struct ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " {\n");
    bool has_field = false;
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "    xrt_closure_t *_cl;\n");
        has_field = true;
    }
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        fprintf(out, "    %s p%u;\n", ctype_str(cg_rep(f->params[i])), i);
        has_field = true;
    }
    if (!has_field)
        fprintf(out, "    uint8_t _empty;\n");
    fprintf(out, "} ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, ";\n\n");
}

static void emit_sync_go_frame_factory(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                       const char *prefix) {
    fprintf(out, "%svoid *", cg_linkage(ctx));
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
        fprintf(out,
                "    if (_cl && !_take_cl)\n        xrt_retain(xr_mkptr(_cl, XR_TAG_CLOSURE));\n");
    }
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        fprintf(out, "    f->p%u = p%u;\n", i, i);
    }
    fprintf(out, "    return f;\n");
    fprintf(out, "}\n\n");
}

static void emit_thread_spawn_abort_stmt(FILE *out, bool in_coro, const XiValue *v,
                                         bool declare_local, XiCgenCtx *ctx, const XiFunc *f) {
    if (in_coro) {
        emit_codegen_abort_aot_result(out);
        return;
    }
    fprintf(out, "    ");
    if (declare_local)
        fprintf(out, "%s ", local_ctype_str_ctx(ctx, f, v));
    emit_vref(out, v);
    fprintf(out, " = ");
    emit_codegen_abort_expr(out);
    fprintf(out, ";\n");
}

static bool emit_thread_spawn_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                         const XiValue *v, const char *prefix, bool in_coro) {
    if (!v || v->op != XI_THREAD_SPAWN)
        return false;

    bool declare_local = !in_coro && !ctx->pre_decl_all;
    emit_value_generated_line_reset(ctx, out, v);
    if (v->nargs < 1) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: THREAD_SPAWN missing callee\n");
        emit_thread_spawn_abort_stmt(out, in_coro, v, declare_local, ctx, f);
        return true;
    }

    CgStaticFunctionCall call = cg_resolve_static_function_call(ctx, f, v->args[0]);
    const XiFunc *target = call.func;
    const char *thread_prefix = call.prefix ? call.prefix : prefix;
    if (!target || !cg_func_needs_sync_go_wrapper_ctx(ctx, target)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT sys.Thread.spawn target\n");
        emit_thread_spawn_abort_stmt(out, in_coro, v, declare_local, ctx, f);
        return true;
    }
    if (!cg_aot_frame_new_can_supply_cl_arg(f, v->args[0], target)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported captured AOT sys.Thread.spawn target\n");
        emit_thread_spawn_abort_stmt(out, in_coro, v, declare_local, ctx, f);
        return true;
    }

    emit_value_source_line(ctx, out, v);
    const uint32_t *affinity_cpus = xi_thread_spawn_affinity_cpus(v);
    uint16_t affinity_count = xi_thread_spawn_affinity_count(v);
    if (affinity_count > 0 && affinity_cpus) {
        fprintf(out, "    static const uint32_t _thread_affinity_%u[%u] = {", v->id,
                (unsigned) affinity_count);
        for (uint16_t i = 0; i < affinity_count; i++) {
            if (i > 0)
                fprintf(out, ", ");
            fprintf(out, "%uu", (unsigned) affinity_cpus[i]);
        }
        fprintf(out, "};\n");
    }
    fprintf(out, "    void *_thread_frame_%u = ", v->id);
    emit_fname_suffix(ctx, out, thread_prefix, target, "_aot_frame_new");
    fprintf(out, "(");
    emit_aot_frame_new_call_args(ctx, out, f, v->args[0], target, true, v->args, 1, v->nargs, v);
    fprintf(out, ");\n");
    fprintf(out, "    ");
    if (declare_local)
        fprintf(out, "%s ", local_ctype_str_ctx(ctx, f, v));
    emit_vref(out, v);
    fprintf(out, " = xrt_thread_spawn_aot(&");
    emit_fname_suffix(ctx, out, thread_prefix, target, "_aot_desc");
    fprintf(out, ", _thread_frame_%u, %llu, ", v->id,
            (unsigned long long) xi_thread_spawn_stack_size(v));
    const char *thread_name = xi_thread_spawn_name(v);
    if (thread_name)
        xicgen_emit_c_string_literal(out, thread_name);
    else
        fprintf(out, "NULL");
    if (affinity_count > 0 && affinity_cpus) {
        fprintf(out, ", _thread_affinity_%u, %u);\n", v->id, (unsigned) affinity_count);
    } else {
        fprintf(out, ", NULL, 0);\n");
    }
    if (in_coro) {
        fprintf(out, "    if (XR_UNLIKELY(XR_IS_NULL(");
        emit_vref(out, v);
        fprintf(out, "))) {\n");
        fprintf(out, "        XrValue _thread_error_%u = xrt_pending_error;\n", v->id);
        fprintf(out, "        xrt_pending_error = XR_NULL_VAL;\n");
        fprintf(out, "        return xr_aot_error(_thread_error_%u, true);\n", v->id);
        fprintf(out, "    }\n");
        emit_value_generated_line_reset(ctx, out, v);
        return true;
    }

    emit_typed_array_data_cache_decl(ctx, out, v);
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
    return true;
}

static bool cg_sync_go_param_needs_release(const XiFunc *f, uint16_t index) {
    return f && index < cg_coro_param_count(f) && f->params[index] &&
           xi_coro_type_needs_boundary_clone(f->params[index]->type);
}

static bool cg_sync_go_param_needs_trace(const XiFunc *f, uint16_t index) {
    return f && index < cg_coro_param_count(f) && f->params[index] &&
           xi_coro_value_rep_can_trace_root(f->params[index]);
}

static uint32_t count_sync_go_frame_roots(const XiFunc *f) {
    if (!f)
        return 0;
    uint32_t count = 0;
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (cg_sync_go_param_needs_trace(f, i))
            count++;
    }
    return count;
}

static uint32_t count_sync_go_frame_releases(const XiFunc *f) {
    if (!f)
        return 0;
    uint32_t count = cg_func_frame_needs_cl(f) ? 1u : 0u;
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
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
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++)
        cg_coro_layout_add_rep(&size, &max_align, cg_rep(f->params[i]));
    if (size == 0)
        cg_coro_layout_add(&size, &max_align, sizeof(uint8_t), _Alignof(uint8_t));
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
    } else if (rep == XR_REP_PTR && cg_type_is_class_instance_ptr(f->params[index]->type)) {
        fprintf(out, "xrt_box_obj(f->p%u)", index);
    } else if (rep == XR_REP_RAWPTR) {
        fprintf(out, "XR_FROM_INT((int64_t)(uintptr_t)(f->p%u))", index);
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
    XrRep frame_rep = cg_rep(f->params[index]);
    if (param_rep == XR_REP_TAGGED) {
        emit_sync_go_frame_param_as_xrvalue(out, f, index);
    } else {
        const char *suffix =
            emit_conversion_prefix(out, f->params[index]->type, frame_rep, param_rep);
        fprintf(out, "f->p%u", index);
        emit_conversion_suffix(out, suffix);
    }
}

static void emit_xrvalue_from_native_expr(FILE *out, const XrType *type, XrRep rep,
                                          const char *expr) {
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "%s", expr);
    } else if (rep == XR_REP_PTR && type && type->kind == XR_KIND_STRING) {
        fprintf(out, "xr_str_value_from_ptr(%s)", expr);
    } else if (rep == XR_REP_PTR && cg_type_is_class_instance_ptr(type)) {
        fprintf(out, "xrt_box_obj(%s)", expr);
    } else if (rep == XR_REP_RAWPTR) {
        fprintf(out, "XR_FROM_INT((int64_t)(uintptr_t)(%s))", expr);
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

static bool cg_sync_go_uses_class_param_boxed_adapter(XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return false;
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (cg_rep(f->params[i]) == XR_REP_TAGGED && cg_func_param_native_class_data(ctx, f, i))
            return true;
    }
    return false;
}

static void emit_sync_go_wrapper(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    uint32_t root_count = count_sync_go_frame_roots(f);
    uint32_t release_count = count_sync_go_frame_releases(f);
    record_coro_frame_stats(ctx, estimate_sync_go_frame_size(f), root_count, release_count);

    emit_sync_go_frame_type(ctx, out, f, prefix);
    emit_sync_go_frame_factory(ctx, out, f, prefix);

    fprintf(out, "%sXrAotResult ", cg_linkage(ctx));
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
    bool use_boxed_adapter = cg_sync_go_uses_class_param_boxed_adapter(ctx, f);
    if (use_boxed_adapter) {
        fprintf(out, "    XrValue _result = ");
        emit_typed_abi_fname(ctx, out, prefix, f);
        if (cg_func_frame_needs_cl(f))
            fprintf(out, "(f->_cl");
        else
            fprintf(out, "(NULL");
        for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
            fprintf(out, ", ");
            emit_sync_go_frame_param_as_xrvalue(out, f, i);
        }
        fprintf(out, ");\n");
    } else if (result_rep == XR_REP_VOID) {
        fprintf(out, "    ");
        emit_fname(ctx, out, prefix, f);
        if (cg_func_frame_needs_cl(f))
            fprintf(out, "(f->_cl");
        else
            fprintf(out, "(NULL");
        for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
            fprintf(out, ", ");
            emit_sync_go_frame_param_for_func_abi(ctx, out, f, i);
        }
        fprintf(out, ");\n");
        fprintf(out, "    XrValue _result = XR_NULL_VAL;\n");
    } else {
        fprintf(out, "    %s _raw_result = ", ctype_str(result_rep));
        emit_fname(ctx, out, prefix, f);
        if (cg_func_frame_needs_cl(f))
            fprintf(out, "(f->_cl");
        else
            fprintf(out, "(NULL");
        for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
            fprintf(out, ", ");
            emit_sync_go_frame_param_for_func_abi(ctx, out, f, i);
        }
        fprintf(out, ");\n");
        fprintf(out, "    XrValue _result = ");
        emit_xrvalue_from_native_expr(out, f->return_type, result_rep, "_raw_result");
        fprintf(out, ";\n");
    }
    fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
    fprintf(out, "        XrValue _error = xrt_pending_error;\n");
    fprintf(out, "        xrt_pending_error = XR_NULL_VAL;\n");
    fprintf(out, "        return xr_aot_error(_error, true);\n");
    fprintf(out, "    }\n");
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
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

    fprintf(out, "%svoid ", cg_linkage(ctx));
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, "(void *frame, void *visitor) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (!cg_sync_go_param_needs_trace(f, i))
            continue;
        fprintf(out, "    xr_aot_trace_frame_value(visitor, ");
        emit_sync_go_frame_param_as_xrvalue(out, f, i);
        fprintf(out, ");\n");
    }
    fprintf(out, "}\n\n");

    fprintf(out, "%svoid ", cg_linkage(ctx));
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, "(void *frame, struct XrCoroHeap *heap) {\n");
    fprintf(out, "    (void)heap;\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
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

    fprintf(out, "%sconst XrAotCoroDesc ", cg_linkage(ctx));
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

static void emit_coro_frame_type(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    fprintf(out, "typedef struct ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " {\n");
    fprintf(out, "    uint32_t state;\n");
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_closure_t *_cl;\n");
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++)
        fprintf(out, "    %s p%u;\n", ctype_str(cg_coro_param_rep(ctx, f, i)), i);
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_phi_needs_frame(ctx, f, phi))
                continue;
            /* Frame fields hold values across a suspension: a TAGGED-rep value is
             * spilled boxed (XrValue), a native-rep value keeps its rep. The slot
             * type therefore follows cg_rep, matching the spill/reload in
             * emit_coro_value_stmt (which keys on cg_rep == XR_REP_TAGGED). */
            fprintf(out, "    %s ", cg_coro_decl_ctype(ctx, f, &phi->value));
            emit_phi_ref(ctx, out, phi);
            fprintf(out, ";\n");
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_test_yield_add_call(ctx, f, v))
                fprintf(out, "    int64_t test_yield_value_%u;\n", v->id);
            if (cg_coro_call_needs_child_frame(ctx, f, v)) {
                fprintf(out, "    void *call_frame_%u;\n", v->id);
                if (cg_coro_callable_target_switch_plan(ctx, v))
                    fprintf(out, "    uint32_t call_target_id_%u;\n", v->id);
            }
            if (cg_coro_i64_optional_needs_frame(ctx, f, v)) {
                fprintf(out, "    bool v%u_opt_has;\n", v->id);
                fprintf(out, "    int64_t v%u_opt_value;\n", v->id);
                continue;
            }
            if (!cg_coro_value_needs_frame(ctx, f, v))
                continue;
            fprintf(out, "    %s ", cg_coro_decl_ctype(ctx, f, v));
            emit_vref(out, v);
            fprintf(out, ";\n");
        }
    }
    /* Function-scoped defers live in the frame so they survive suspensions; the
     * scope is run at the coroutine's exit (xr_aot_done) and on release. */
    if (cg_func_has_defer_stmt(f))
        fprintf(out, "    XrtDeferScope _xrt_ds;\n");
    fprintf(out, "} ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, ";\n\n");
}

static void emit_aot_frame_param_names(FILE *out, const XiFunc *f) {
    bool need_comma = false;
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "_cl, _take_cl");
        need_comma = true;
    }
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (need_comma)
            fprintf(out, ", ");
        fprintf(out, "p%u", i);
        need_comma = true;
    }
}

static void emit_coro_frame_init(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    fprintf(out, "%sbool ", cg_linkage(ctx));
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame_init");
    fprintf(out, "(void *raw_frame");
    if (cg_func_frame_needs_cl(f) || cg_coro_param_count(f) > 0)
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
    if (cg_func_has_defer_stmt(f))
        fprintf(out, "    xrt_defer_init(&f->_xrt_ds);\n");
    if (cg_func_frame_needs_cl(f)) {
        fprintf(out, "    f->_cl = _cl;\n");
        fprintf(out,
                "    if (_cl && !_take_cl)\n        xrt_retain(xr_mkptr(_cl, XR_TAG_CLOSURE));\n");
    }
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++)
        emit_assign_coro_param_from_xrvalue(ctx, out, f, i);
    fprintf(out, "    return true;\n");
    fprintf(out, "}\n\n");
}

static void emit_coro_local_declarations(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_phi_has_storage(phi))
                continue;
            if (cg_coro_phi_needs_frame(ctx, f, phi))
                continue;
            /* Declare with the storage rep (matches the value emission and the
             * sync path); cg_rep would type a native class instance as XrValue
             * while its value is a PTR. */
            XrRep rep = cg_coro_decl_rep(ctx, f, &phi->value);
            const char *ctype = cg_coro_decl_ctype(ctx, f, &phi->value);
            fprintf(out, "    %s ", ctype);
            emit_phi_ref(ctx, out, phi);
            if (cg_value_plan_is_aggregate(ctx, &phi->value)) {
                fprintf(out, " = ");
                emit_value_plan_zero_expr(ctx, out, &phi->value);
                fprintf(out, ";\n");
            } else
                fprintf(out, rep == XR_REP_TAGGED ? " = XR_NULL_VAL;\n" : " = 0;\n");
        }

        /* Each predecessor captures all incoming PHI values before publishing
         * any of them. Those edge temporaries need function scope here too: a
         * resume body reaches every block through goto — including the state
         * dispatch at entry — so an inline declaration would be jumped over.
         * A frame-resident phi still copies through a plain local temp, so this
         * covers frame and non-frame phis alike; the slot type must match the
         * phi's own declaration, hence cg_coro_decl_ctype. */
        for (uint16_t pred_idx = 0; pred_idx < blk->npreds; pred_idx++) {
            for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
                if (!cg_phi_copy_should_emit(ctx, f, phi, pred_idx))
                    continue;
                fprintf(out, "    %s ", cg_coro_decl_ctype(ctx, f, &phi->value));
                emit_phi_tmp_ref(out, blk, phi, pred_idx);
                fprintf(out, ";\n");
            }
        }

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_await_needs_tagged_boundary_temp(ctx, f, v)) {
                fprintf(out, "    XrValue ");
                emit_coro_await_boundary_temp_ref(out, v);
                fprintf(out, " = XR_NULL_VAL;\n");
            }
            if (cg_value_is_i64_optional_blocking_result_root(v) &&
                cg_value_is_elided_i64_optional_blocking_result(f, v) &&
                !cg_coro_i64_optional_needs_frame(ctx, f, v)) {
                fprintf(out, "    bool v%u_opt_has = false;\n", v->id);
                fprintf(out, "    int64_t v%u_opt_value = 0;\n", v->id);
                continue;
            }
            if (!cg_coro_value_has_storage(f, v) || cg_coro_value_needs_frame(ctx, f, v))
                continue;
            XrRep rep = cg_coro_decl_rep(ctx, f, v);
            const char *ctype = cg_coro_decl_ctype(ctx, f, v);
            fprintf(out, "    %s ", ctype);
            emit_vref(out, v);
            if (cg_value_plan_is_aggregate(ctx, v) || cg_value_plan_is_vector(ctx, v)) {
                fprintf(out, " = ");
                emit_value_plan_zero_expr(ctx, out, v);
                fprintf(out, ";\n");
            } else
                fprintf(out, rep == XR_REP_TAGGED ? " = XR_NULL_VAL;\n" : " = 0;\n");
        }
    }
}

static void emit_coro_debug_frame_source_var_syncs(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (f->params && f->params[i])
            emit_debug_source_var_sync(ctx, out, f, f->params[i]);
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, phi))
                emit_debug_source_var_sync(ctx, out, f, &phi->value);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (cg_coro_value_needs_frame(ctx, f, v))
                emit_debug_source_var_sync(ctx, out, f, v);
        }
    }
}

static void emit_coro_macros(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const char *prefix) {
    (void) ctx;
    (void) prefix;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, phi))
                fprintf(out, "#define phi%u (f->phi%u)\n", phi->value.id, phi->value.id);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_PARAM && v->aux_int >= 0 && v->aux_int < cg_coro_param_count(f)) {
                fprintf(out, "#define v%u (f->p%u)\n", v->id, (unsigned) v->aux_int);
            } else if (cg_coro_value_needs_frame(ctx, f, v)) {
                fprintf(out, "#define v%u (f->v%u)\n", v->id, v->id);
            }
        }
    }
}

static void emit_coro_undefs(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (cg_coro_phi_needs_frame(ctx, f, phi))
                fprintf(out, "#undef phi%u\n", phi->value.id);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_PARAM && v->aux_int >= 0 && v->aux_int < cg_coro_param_count(f))
                fprintf(out, "#undef v%u\n", v->id);
            else if (cg_coro_value_needs_frame(ctx, f, v))
                fprintf(out, "#undef v%u\n", v->id);
        }
    }
}

static void emit_coro_frame_factory(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                    const char *prefix) {
    fprintf(out, "%svoid *", cg_linkage(ctx));
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
    if (cg_func_frame_needs_cl(f) || cg_coro_param_count(f) > 0)
        fprintf(out, ", ");
    emit_aot_frame_param_names(out, f);
    fprintf(out, ")) {\n");
    fprintf(out, "        xr_aot_frame_free(f);\n");
    fprintf(out, "        return NULL;\n");
    fprintf(out, "    }\n");
    fprintf(out, "    return f;\n");
    fprintf(out, "}\n\n");
}

static bool cg_coro_direct_call_frame_reusable(XiCgenCtx *ctx, const XiFunc *target);

static const XiValue *cg_coro_i64_optional_null_compare_root(const XiValue *v) {
    if (!v || (v->op != XI_EQ && v->op != XI_NE) || v->nargs < 2)
        return NULL;
    if (cg_value_is_null_const(v->args[0]))
        return cg_i64_optional_blocking_result_root(v->args[1]);
    if (cg_value_is_null_const(v->args[1]))
        return cg_i64_optional_blocking_result_root(v->args[0]);
    return NULL;
}

static bool emit_coro_i64_optional_native_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *v) {
    if (!ctx || !out || !f || !v)
        return false;
    const XiValue *root = cg_i64_optional_blocking_result_root(v);
    if (root && root != v && cg_value_is_elided_i64_optional_blocking_result(f, root) &&
        ((xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)) || v->op == XI_UNBOX) &&
        cg_rep(v) == XR_REP_TAGGED) {
        return true;
    }

    root = NULL;
    if (v->op == XI_ISNULL && v->nargs >= 1)
        root = cg_i64_optional_blocking_result_root(v->args[0]);
    if (root && cg_value_is_elided_i64_optional_blocking_result(f, root)) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = !");
            emit_coro_i64_optional_has_ref(ctx, out, f, root);
            fprintf(out, ";\n");
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        }
        return true;
    }

    root = cg_coro_i64_optional_null_compare_root(v);
    if (root && cg_value_is_elided_i64_optional_blocking_result(f, root)) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
            if (v->op == XI_EQ)
                fprintf(out, "!");
            emit_coro_i64_optional_has_ref(ctx, out, f, root);
            fprintf(out, ";\n");
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        }
        return true;
    }

    if (v->op == XI_UNBOX && v->nargs >= 1) {
        root = cg_i64_optional_blocking_result_root(v->args[0]);
        if (root && cg_value_is_elided_i64_optional_blocking_result(f, root)) {
            if (cg_coro_value_has_storage(f, v)) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                emit_coro_i64_optional_value_ref(ctx, out, f, root);
                fprintf(out, ";\n");
                emit_coro_debug_result_source_var_sync(ctx, out, f, v);
            }
            return true;
        }
    }

    if ((xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)) && v->nargs >= 1 &&
        cg_rep(v) != XR_REP_TAGGED) {
        root = cg_i64_optional_blocking_result_root(v->args[0]);
        if (root && cg_value_is_elided_i64_optional_blocking_result(f, root)) {
            if (cg_coro_value_has_storage(f, v)) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                emit_coro_i64_optional_value_ref(ctx, out, f, root);
                fprintf(out, ";\n");
                emit_coro_debug_result_source_var_sync(ctx, out, f, v);
            }
            return true;
        }
    }

    return false;
}

static bool cg_coro_edge_is_backedge(const XiBlock *from, const XiBlock *to) {
    return from && to && to->rpo <= from->rpo;
}

static bool cg_coro_block_has_backedge(const XiBlock *blk) {
    if (!blk)
        return false;
    switch (blk->kind) {
        case XI_BLOCK_PLAIN:
            return cg_coro_edge_is_backedge(blk, blk->succs[0]);
        case XI_BLOCK_IF:
            return cg_coro_edge_is_backedge(blk, blk->succs[0]) ||
                   cg_coro_edge_is_backedge(blk, blk->succs[1]);
        default:
            return false;
    }
}

static bool cg_coro_block_ends_with_yield(const XiBlock *blk) {
    if (!blk || blk->nvalues == 0)
        return false;
    const XiValue *last = blk->values[blk->nvalues - 1];
    return last && last->op == XI_YIELD;
}

static const XiValue *cg_coro_block_first_runtime_value(const XiBlock *blk) {
    if (!blk)
        return NULL;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v || v->op == XI_PHI)
            continue;
        return v;
    }
    return NULL;
}

static bool cg_coro_plain_backedge_targets_suspend_point(const XiFunc *f, const XiBlock *blk,
                                                         const XiCoroResolver *resolver) {
    if (!f || !blk || blk->kind != XI_BLOCK_PLAIN)
        return false;
    XiBlock *target = blk->succs[0];
    if (!cg_coro_edge_is_backedge(blk, target))
        return false;
    const XiValue *head = cg_coro_block_first_runtime_value(target);
    return head && xi_coro_is_suspend_point(f, head, resolver);
}

static bool cg_coro_value_is_deferred_batch_go(const XiValue *v) {
    return v && v->op == XI_GO && (v->aux_int & XI_GO_AUX_DEFER_BATCH) != 0;
}

static bool cg_coro_block_is_deferred_spawn_registration_latch(const XiFunc *f, const XiBlock *blk,
                                                               const XiCoroResolver *resolver) {
    if (!f || !blk)
        return false;
    bool saw_deferred_go = false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v || v->op == XI_PHI)
            continue;
        if (cg_coro_value_is_deferred_batch_go(v)) {
            saw_deferred_go = true;
            continue;
        }
        if (xi_coro_is_suspend_point(f, v, resolver))
            return false;
    }
    return saw_deferred_go;
}

static void cg_coro_insert_loop_poll_safepoints(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return;
    xi_ensure_rpo(f);
    xr_type_global_init();
    XrType *unit_type = xr_type_new_unit(NULL);
    bool changed = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!cg_coro_block_has_backedge(blk) || cg_coro_block_ends_with_yield(blk))
            continue;
        if (cg_coro_plain_backedge_targets_suspend_point(f, blk, resolver))
            continue;
        if (cg_coro_block_is_deferred_spawn_registration_latch(f, blk, resolver))
            continue;
        XiValue *poll = xi_value_new(f, blk, XI_YIELD, unit_type, 0);
        if (!poll)
            continue;
        poll->aux_int = XI_YIELD_AUX_POLL;
        poll->line = blk->line;
        changed = true;
    }
    if (changed) {
        f->coro_plan = NULL;
        xi_func_compute_effects(f);
    }
}

static bool cg_coro_func_emits_loop_poll(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->op == XI_YIELD && v->aux_int == XI_YIELD_AUX_POLL)
                return true;
        }
    }
    return false;
}

static void emit_coro_callable_closure_ptr(FILE *out, const XiValue *callee) {
    fprintf(out, "(xrt_closure_t *)");
    emit_vref(out, callee);
    if (cg_rep(callee) == XR_REP_TAGGED)
        fprintf(out, ".ptr");
}

static void emit_coro_callable_switch_resume(XiCgenCtx *ctx, FILE *out,
                                             const XaotCallableInvokePlan *plan,
                                             const XiValue *call) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    fprintf(out, "    switch (f->call_target_id_%u) {\n", call->id);
    for (uint16_t ti = 0; ti < plan->target_count; ti++) {
        const XaotCallableTargetCase *target = xaot_bundle_callable_target_case(bundle, plan, ti);
        if (!target || !target->target_func || (target->effect_bits & XG_BODY_MAY_SUSPEND) == 0)
            continue;
        const char *target_prefix = cg_module_prefix_for_func(ctx, target->target_func);
        fprintf(out, "        case %uu: _call_%u = ", target->target_id, call->id);
        emit_fname_suffix(ctx, out, target_prefix, target->target_func, "_aot_resume");
        fprintf(out, "(f->call_frame_%u, ctx); break;\n", call->id);
    }
    fprintf(out, "        default: return xr_aot_error(XR_NULL_VAL, false);\n");
    fprintf(out, "    }\n");
}

static void emit_coro_callable_switch_release(XiCgenCtx *ctx, FILE *out,
                                              const XaotCallableInvokePlan *plan,
                                              const XiValue *call, const char *indent) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    fprintf(out, "%sswitch (f->call_target_id_%u) {\n", indent, call->id);
    for (uint16_t ti = 0; ti < plan->target_count; ti++) {
        const XaotCallableTargetCase *target = xaot_bundle_callable_target_case(bundle, plan, ti);
        if (!target || !target->target_func || (target->effect_bits & XG_BODY_MAY_SUSPEND) == 0)
            continue;
        const char *target_prefix = cg_module_prefix_for_func(ctx, target->target_func);
        fprintf(out, "%s    case %uu: ", indent, target->target_id);
        emit_fname_suffix(ctx, out, target_prefix, target->target_func, "_aot_release");
        fprintf(out, "(f->call_frame_%u, NULL); break;\n", call->id);
    }
    fprintf(out, "%s    default: break;\n", indent);
    fprintf(out, "%s}\n", indent);
}

static bool emit_coro_callable_target_switch(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const XiValue *v, int *state_id) {
    const XaotCallableInvokePlan *plan = cg_coro_callable_target_switch_plan(ctx, v);
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    if (!plan)
        return false;

    int sid = ++(*state_id);
    fprintf(out, "    XrAotResult _call_%u;\n", v->id);
    fprintf(out, "    XrValue _call_value_%u = XR_NULL_VAL;\n", v->id);
    fprintf(out, "    xrt_closure_t *_call_cl_%u = ", v->id);
    emit_coro_callable_closure_ptr(out, v->args[0]);
    fprintf(out, ";\n");
    fprintf(out,
            "    if (!_call_cl_%u || !_call_cl_%u->callable)\n"
            "        return xr_aot_error(XR_NULL_VAL, false);\n",
            v->id, v->id);
    fprintf(out, "    f->call_target_id_%u = _call_cl_%u->callable->target_id;\n", v->id, v->id);
    fprintf(out, "    switch (f->call_target_id_%u) {\n", v->id);
    for (uint16_t ti = 0; ti < plan->target_count; ti++) {
        const XaotCallableTargetCase *target = xaot_bundle_callable_target_case(bundle, plan, ti);
        if (!target || !target->target_func) {
            ctx->error = true;
            continue;
        }
        const char *target_prefix = cg_module_prefix_for_func(ctx, target->target_func);
        fprintf(out, "        case %uu:\n", target->target_id);
        if ((target->effect_bits & XG_BODY_MAY_SUSPEND) == 0) {
            fprintf(out, "            if (!_call_cl_%u->callable->sync_entry)\n", v->id);
            fprintf(out, "                return xr_aot_error(XR_NULL_VAL, false);\n");
            fprintf(out, "            _call_value_%u = ((XrValue (*)(xrt_closure_t *", v->id);
            for (uint16_t ai = 1; ai < v->nargs; ai++)
                fprintf(out, ", XrValue");
            fprintf(out, ")) _call_cl_%u->callable->sync_entry)(_call_cl_%u", v->id, v->id);
            for (uint16_t ai = 1; ai < v->nargs; ai++) {
                fprintf(out, ", ");
                emit_value_as_rep_ctx(ctx, out, v->args[ai], XR_REP_TAGGED);
            }
            fprintf(out, ");\n");
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[40];
                snprintf(tmp, sizeof(tmp), "_call_value_%u", v->id);
                emit_assign_from_xrvalue_temp_ctx(ctx, out, v, tmp);
            }
            fprintf(out, "            f->call_target_id_%u = 0;\n", v->id);
            fprintf(out, "            goto S%d_DONE;\n", sid);
            continue;
        }
        fprintf(out, "            f->call_frame_%u = ", v->id);
        emit_fname_suffix(ctx, out, target_prefix, target->target_func, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_call_args(ctx, out, f, v->args[0], target->target_func, false, v->args,
                                     1, v->nargs, NULL);
        fprintf(out, ");\n");
        fprintf(out,
                "            if (!f->call_frame_%u)\n"
                "                return xr_aot_error(XR_NULL_VAL, false);\n"
                "            break;\n",
                v->id);
    }
    fprintf(out, "        default: return xr_aot_error(XR_NULL_VAL, false);\n");
    fprintf(out, "    }\n");
    fprintf(out, "    f->state = %d;\n", sid);
    emit_coro_callable_switch_resume(ctx, out, plan, v);
    fprintf(out, "    goto S%d_RESULT;\n", sid);
    fprintf(out, "S%d:;\n", sid);
    fprintf(out, "    f->state = %d;\n", sid);
    emit_coro_callable_switch_resume(ctx, out, plan, v);
    fprintf(out, "S%d_RESULT:;\n", sid);
    fprintf(out, "    if (_call_%u.kind == XR_AOT_RUN_DONE) {\n", v->id);
    fprintf(out, "        _call_value_%u = _call_%u.value;\n", v->id, v->id);
    if (cg_rep(v) == XR_REP_TAGGED)
        fprintf(out,
                "        if (_call_value_%u.tag == XR_TAG_PTR || _call_value_%u.tag >= "
                "XR_TAG_STR)\n"
                "            xrt_retain(_call_value_%u);\n",
                v->id, v->id, v->id);
    emit_coro_callable_switch_release(ctx, out, plan, v, "        ");
    fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
    fprintf(out, "        f->call_target_id_%u = 0;\n", v->id);
    if (cg_coro_value_has_storage(f, v)) {
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "_call_value_%u", v->id);
        emit_assign_from_xrvalue_temp_ctx(ctx, out, v, tmp);
    }
    fprintf(out, "        f->state = 0;\n");
    fprintf(out, "        goto S%d_DONE;\n", sid);
    fprintf(out, "    }\n");
    fprintf(out,
            "    if (_call_%u.kind == XR_AOT_RUN_ERROR || "
            "_call_%u.kind == XR_AOT_RUN_CANCELLED) {\n",
            v->id, v->id);
    emit_coro_callable_switch_release(ctx, out, plan, v, "        ");
    fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
    fprintf(out, "        f->call_target_id_%u = 0;\n", v->id);
    emit_coro_call_error_channel_dispatch(out, v->id, sid);
    fprintf(out, "    }\n");
    fprintf(out, "    return _call_%u;\n", v->id);
    fprintf(out, "S%d_DONE:;\n", sid);
    return true;
}

static const char *cg_coro_test_yield_call_name(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v,
                                                uint16_t *arg_base_out) {
    if (!ctx || !f || !v || v->nargs < 1)
        return NULL;
    if ((v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT) &&
        cg_value_is_module_import_ctx(ctx, f, v->args[0], "test_yield")) {
        if (arg_base_out)
            *arg_base_out = 1;
        return (const char *) v->aux;
    }
    if (v->op != XI_CALL)
        return NULL;

    const XiValue *callee = cg_unwrap_identity_value(v->args[0]);
    const XiImportRef *ref = (callee && callee->op == XI_IMPORT_REF && callee->aux)
                                 ? (const XiImportRef *) callee->aux
                                 : cg_import_ref_for_value(ctx, f, callee);
    if (!ref || !ref->module_path || strcmp(ref->module_path, "test_yield") != 0)
        return NULL;
    if (arg_base_out)
        *arg_base_out = 1;
    return ref->member_name;
}

static bool cg_coro_test_yield_add_call(XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    const char *method = v ? cg_coro_test_yield_call_name(ctx, f, v, NULL) : NULL;
    return method && strcmp(method, "add") == 0;
}

static bool emit_coro_test_yield_call_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, int *state_id) {
    uint16_t arg_base = 0;
    const char *method = cg_coro_test_yield_call_name(ctx, f, v, &arg_base);
    if (!method || (strcmp(method, "simple") != 0 && strcmp(method, "add") != 0 &&
                    strcmp(method, "counter_inc") != 0))
        return false;

    uint16_t argc = v->nargs >= arg_base ? (uint16_t) (v->nargs - arg_base) : 0;
    uint16_t expected_argc = strcmp(method, "add") == 0 ? 2 : 0;
    if (argc != expected_argc) {
        ctx->error = true;
        fprintf(stderr,
                "[xi_cgen] ERROR: unsupported yieldable AOT test_yield call '%s' with "
                "%u args\n",
                method, (unsigned) argc);
        emit_codegen_abort_aot_result(out);
        return true;
    }

    int sid = ++(*state_id);
    emit_value_source_line(ctx, out, v);
    if (strcmp(method, "add") == 0) {
        fprintf(out, "    f->test_yield_value_%u = xr_aot_test_yield_add(", v->id);
        emit_value_as_rep_ctx(ctx, out, v->args[arg_base], XR_REP_I64);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[arg_base + 1], XR_REP_I64);
        fprintf(out, ");\n");
    } else if (strcmp(method, "counter_inc") == 0) {
        fprintf(out, "    xr_aot_test_yield_counter_inc();\n");
    }
    fprintf(out, "    f->state = %d;\n", sid);
    fprintf(out, "    return xr_aot_yielded();\n");
    emit_value_generated_line_reset(ctx, out, v);
    fprintf(out, "S%d:;\n", sid);
    fprintf(out, "    f->state = 0;\n");
    if (cg_coro_value_has_storage(f, v) && strcmp(method, "add") == 0) {
        char temp[48];
        snprintf(temp, sizeof(temp), "f->test_yield_value_%u", v->id);
        emit_assign_from_i64_temp(out, v, temp);
    } else if (cg_coro_value_has_storage(f, v) && strcmp(method, "simple") == 0) {
        fprintf(out, "    int64_t _test_yield_value_%u = xr_aot_test_yield_simple();\n", v->id);
        char temp[48];
        snprintf(temp, sizeof(temp), "_test_yield_value_%u", v->id);
        emit_assign_from_i64_temp(out, v, temp);
    } else if (cg_coro_value_has_storage(f, v) && strcmp(method, "counter_inc") == 0) {
        fprintf(out, "    int64_t _test_yield_value_%u = xr_aot_test_yield_counter_get();\n",
                v->id);
        char temp[48];
        snprintf(temp, sizeof(temp), "_test_yield_value_%u", v->id);
        emit_assign_from_i64_temp(out, v, temp);
    }
    emit_coro_debug_result_source_var_sync(ctx, out, f, v);
    return true;
}

static void emit_coro_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                 const char *prefix, int *state_id) {
    XR_DCHECK(v != NULL, "emit_coro_value_stmt: NULL value");
    emit_value_source_line(ctx, out, v);

    if (xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v))
        return;
    if (cg_await_all_inline_literal_value_is_elided(f, v))
        return;
    if (cg_await_all_scalar_result_value_is_elided(f, v))
        return;
    if (cg_shared_static_function_value_is_elided(ctx, f, v))
        return;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1 &&
        (cg_value_is_borrowed_array_slot_alias(ctx, f, v->args[0]) ||
         xicgen_slice_value_only_used_by_stack_slice_direct_call(ctx, f, v->args[0])))
        return;
    if (emit_coro_i64_optional_native_stmt(ctx, out, f, v))
        return;

    if (v->op == XI_YIELD) {
        int sid = ++(*state_id);
        emit_value_generated_line_reset(ctx, out, v);
        if (v->aux_int == XI_YIELD_AUX_POLL) {
            emit_value_source_line(ctx, out, v);
            fprintf(out, "    if (XR_UNLIKELY(++_xr_aot_coro_poll_count >= "
                         "XR_AOT_LOOP_POLL_INTERVAL)) {\n");
            fprintf(out, "        _xr_aot_coro_poll_count = 0;\n");
            fprintf(out,
                    "        XrAotRunKind _yield_poll_%u = "
                    "xr_aot_poll_yield_kind_cost(ctx, XR_AOT_LOOP_POLL_INTERVAL);\n",
                    v->id);
            fprintf(out, "        if (_yield_poll_%u != XR_AOT_RUN_DONE) {\n", v->id);
            fprintf(out, "            f->state = %d;\n", sid);
            fprintf(out, "            return xr_aot_result(_yield_poll_%u);\n", v->id);
            fprintf(out, "        }\n");
            fprintf(out, "    }\n");
        } else if (v->aux_int > XI_YIELD_AUX_IMMEDIATE) {
            emit_value_source_line(ctx, out, v);
            fprintf(out, "    XrAotRunKind _yield_poll_%u = xr_aot_poll_yield_kind(ctx);\n", v->id);
            fprintf(out, "    if (_yield_poll_%u != XR_AOT_RUN_DONE) {\n", v->id);
            fprintf(out, "        f->state = %d;\n", sid);
            fprintf(out, "        return xr_aot_result(_yield_poll_%u);\n", v->id);
            fprintf(out, "    }\n");
        } else {
            fprintf(out, "    f->state = %d;\n", sid);
            emit_value_source_line(ctx, out, v);
            fprintf(out, "    return xr_aot_yielded();\n");
            emit_value_generated_line_reset(ctx, out, v);
        }
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_GEN_YIELD) {
        int sid = ++(*state_id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    return xr_aot_gen_yielded(");
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_DEFER) {
        /* Register the deferred closure onto the frame-stored defer scope; it
         * runs LIFO at the coroutine's exit (see emit_coro_block RETURN) and on
         * release. The scope lives in the frame so it survives suspensions. */
        if (v->nargs >= 1) {
            fprintf(out, "    xrt_defer_push(&f->_xrt_ds, ");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ");\n");
        }
        return;
    }

    if (v->op == XI_DEFER_MARK) {
        fprintf(out, "    ");
        emit_vref(out, v);
        XrRep rep = cg_value_plan_storage_rep(ctx, v);
        if (rep == XR_REP_TAGGED)
            fprintf(out, " = XR_FROM_INT(xrt_defer_mark(&f->_xrt_ds));\n");
        else
            fprintf(out, " = (int64_t)xrt_defer_mark(&f->_xrt_ds);\n");
        return;
    }

    if (v->op == XI_DEFER_RUN_TO) {
        if (v->nargs >= 1) {
            fprintf(out, "    xrt_defer_run_to(&f->_xrt_ds, (int)");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ");\n");
        }
        return;
    }

    if (v->op == XI_ERR_RETURN || v->op == XI_THROW) {
        if (v->nargs < 1) {
            fprintf(out, "    return xr_aot_error(XR_NULL_VAL, %s);\n",
                    v->op == XI_ERR_RETURN ? "true" : "false");
            return;
        }
        if (cg_func_has_defer_stmt(f)) {
            fprintf(out, "    XrValue _xrt_err_%u = ", v->id);
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ";\n");
            fprintf(out, "    xrt_defer_run(&f->_xrt_ds);\n");
            fprintf(out, "    return xr_aot_error(_xrt_err_%u, %s);\n", v->id,
                    v->op == XI_ERR_RETURN ? "true" : "false");
        } else {
            fprintf(out, "    return xr_aot_error(");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ", %s);\n", v->op == XI_ERR_RETURN ? "true" : "false");
        }
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

    if (xi_coro_unbox_from_typed_recv(f, v))
        return;

    if (xi_coro_unbox_from_typed_await(f, v))
        return;

    if (emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, true))
        return;

    if (v->op == XI_GO) {
        emit_value_generated_line_reset(ctx, out, v);
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
        bool one_shot_await = (v->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) != 0;
        bool result_copy_shared = (v->aux_int & XI_GO_AUX_RESULT_COPY_SHARED) != 0;
        bool defer_batch = (v->aux_int & XI_GO_AUX_DEFER_BATCH) != 0;
        bool fire_and_forget = (v->flags & XI_FLAG_FIRE_AND_FORGET) != 0;
        int sid = ++(*state_id);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    void *_child_frame_%u = ", v->id);
        emit_fname_suffix(ctx, out, go_prefix, target, "_aot_frame_new");
        fprintf(out, "(");
        emit_aot_frame_new_call_args(ctx, out, f, v->args[0], target, target_is_sync_go, v->args, 1,
                                     v->nargs, v);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    XrAotSpawnResult _spawn_%u = %s(ctx, &", v->id,
                defer_batch ? "xr_aot_spawn_deferred" : "xr_aot_spawn");
        emit_fname_suffix(ctx, out, go_prefix, target, "_aot_desc");
        fprintf(out, ", _child_frame_%u, %d, %s, %s, %s, \"%s\");\n", v->id, link_mode,
                fire_and_forget ? "true" : "false", one_shot_await ? "true" : "false",
                result_copy_shared ? "true" : "false", target->name ? target->name : "aot");
        fprintf(out, "    ");
        emit_vref(out, v);
        fprintf(out, " = _spawn_%u.task_value;\n", v->id);
        fprintf(out, "    if (!_spawn_%u.child)\n", v->id);
        fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
        if (!defer_batch) {
            fprintf(out, "    f->state = %d;\n", sid);
            fprintf(out, "    return xr_aot_spawn_child(_spawn_%u.child);\n", v->id);
        }
        fprintf(out, "S%d:;\n", sid);
        if (!defer_batch)
            fprintf(out, "    f->state = 0;\n");
        return;
    }

    if (v->op == XI_AWAIT) {
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: AWAIT missing task\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        const XiValue *await_slot_value = xi_coro_typed_await_unbox_user(f, v);
        int await_flags = (int) v->aux_int;
        bool await_all = (await_flags & XI_AWAIT_AUX_ALL) != 0;
        bool await_any_success = (await_flags & XI_AWAIT_AUX_ANY_SUCCESS) != 0;
        bool await_any = (await_flags & XI_AWAIT_AUX_ANY) != 0 || await_any_success;
        bool one_shot_await = (await_flags & XI_AWAIT_AUX_CONSUME_TASK) != 0;
        bool aggregate_one_shot = (await_flags & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) != 0;
        bool submit_deferred_batch = (await_flags & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) != 0;
        bool await_into_result = (await_flags & XI_AWAIT_AUX_INTO_RESULT) != 0;
        const XiValue *await_into_value = (await_into_result && v->nargs >= 2) ? v->args[1] : NULL;
        const char *await_all_elem_name =
            await_all ? (await_into_result ? cg_coro_array_value_elem_name(ctx, await_into_value)
                                           : cg_coro_await_all_result_elem_name(ctx, v))
                      : "XR_ELEM_ANY";
        CgInlineAwaitAllLiteral inline_await_all_literal;
        memset(&inline_await_all_literal, 0, sizeof(inline_await_all_literal));
        bool inline_await_all_tasks =
            await_all && !await_into_result &&
            cg_await_all_inline_literal_collect(f, v, &inline_await_all_literal);
        CgAwaitAllScalarResult scalar_await_all_result;
        memset(&scalar_await_all_result, 0, sizeof(scalar_await_all_result));
        bool scalarize_await_all_result =
            inline_await_all_tasks &&
            cg_await_all_inline_scalar_result_collect(f, v, &scalar_await_all_result);
        bool dynamic_await_all_xrt_result =
            await_all && !await_into_result && !inline_await_all_tasks &&
            cg_coro_value_has_storage(f, v) && cg_rep(v) == XR_REP_TAGGED &&
            !cg_coro_await_all_result_needs_boundary_clone(await_all_elem_name);
        if ((await_all || await_any) && !inline_await_all_tasks &&
            cg_coro_aggregate_await_tasks_need_heap_clone(v->args[0])) {
            fprintf(out, "    ");
            emit_vref(out, v->args[0]);
            fprintf(out, " = xrt_value_clone_for_coro(");
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
        }
        const XiValue *deferred_task_array =
            (submit_deferred_batch && !await_all && !await_any && v->nargs >= 1)
                ? cg_coro_await_task_index_array(v->args[0])
                : NULL;
        const XiValue *deferred_task_index =
            deferred_task_array ? cg_coro_await_task_index_value(v->args[0]) : NULL;
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        if (await_all) {
            if (inline_await_all_tasks) {
                emit_await_all_inline_task_vector(ctx, out, &inline_await_all_literal, v->id,
                                                  "_await_tasks_");
                if (scalarize_await_all_result) {
                    emit_coro_await_all_result_slots(ctx, out, f, prefix, &scalar_await_all_result,
                                                     v->id, "_await_result_slots_");
                    fprintf(out,
                            "    XrAotResult _await_%u = "
                            "xr_aot_await_all_task_values_to_slots(ctx, "
                            "_await_tasks_%u, %u, _await_result_slots_%u, %s, %s);\n",
                            v->id, v->id, inline_await_all_literal.count, v->id,
                            await_all_elem_name, aggregate_one_shot ? "true" : "false");
                } else {
                    fprintf(out,
                            "    XrAotResult _await_%u = xr_aot_await_all_task_values(ctx, "
                            "_await_tasks_%u, %u, ",
                            v->id, v->id, inline_await_all_literal.count);
                    emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                }
            } else {
                fprintf(out, "    XrAotResult _await_%u = ", v->id);
                if (await_into_result) {
                    if (!await_into_value) {
                        ctx->error = true;
                        fprintf(stderr, "[xi_cgen] ERROR: await all into missing result buffer\n");
                        emit_codegen_abort_aot_result(out);
                        return;
                    }
                    fprintf(out, "xr_aot_await_all_tasks_into_array(ctx, ");
                } else if (dynamic_await_all_xrt_result) {
                    fprintf(out, "xr_aot_await_all_tasks_wait(ctx, ");
                } else {
                    fprintf(out, "xr_aot_await_all_tasks(ctx, ");
                }
                emit_vref(out, v->args[0]);
                if (await_into_result) {
                    fprintf(out, ", ");
                    emit_value_as_rep_ctx(ctx, out, await_into_value, XR_REP_TAGGED);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                } else if (dynamic_await_all_xrt_result) {
                    fprintf(out, ");\n");
                } else {
                    fprintf(out, ", ");
                    emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                }
            }
        } else if (await_any) {
            fprintf(out, "    XrAotResult _await_%u = xr_aot_await_any_task(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ", %s);\n", await_any_success ? "true" : "false");
        } else {
            fprintf(out, "    XrAotResult _await_%u = ", v->id);
            if (deferred_task_array) {
                fprintf(out, "xr_aot_await_deferred_task_from_array(ctx, ");
                emit_vref(out, deferred_task_array);
                fprintf(out, ", ");
                if (deferred_task_index)
                    emit_int64_arg(out, deferred_task_index);
                else
                    fprintf(out, "-1");
                fprintf(out, ", ");
            } else {
                fprintf(out, "xr_aot_await_task(ctx, ");
            }
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ", ");
            if (v->nargs >= 2)
                emit_int64_arg(out, v->args[1]);
            else
                fprintf(out, "-1");
            fprintf(out, ", false, %s);\n", one_shot_await ? "true" : "false");
        }
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        fprintf(out, "    }\n");
        emit_coro_boundary_error_check(out, "_await_", v->id);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        if (await_all) {
            if (inline_await_all_tasks) {
                emit_await_all_inline_task_vector(ctx, out, &inline_await_all_literal, v->id,
                                                  "_await_resume_tasks_");
                if (scalarize_await_all_result) {
                    emit_coro_await_all_result_slots(ctx, out, f, prefix, &scalar_await_all_result,
                                                     v->id, "_await_resume_result_slots_");
                    fprintf(out,
                            "    _await_%u = "
                            "xr_aot_await_all_task_values_to_slots_resume(ctx, "
                            "_await_resume_tasks_%u, %u, _await_resume_result_slots_%u, %s, "
                            "%s);\n",
                            v->id, v->id, inline_await_all_literal.count, v->id,
                            await_all_elem_name, aggregate_one_shot ? "true" : "false");
                } else {
                    fprintf(out,
                            "    _await_%u = xr_aot_await_all_task_values_resume(ctx, "
                            "_await_resume_tasks_%u, %u, ",
                            v->id, v->id, inline_await_all_literal.count);
                    emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                }
            } else {
                fprintf(out, "    _await_%u = ", v->id);
                if (await_into_result) {
                    if (!await_into_value) {
                        ctx->error = true;
                        fprintf(stderr, "[xi_cgen] ERROR: await all into missing result buffer\n");
                        emit_codegen_abort_aot_result(out);
                        return;
                    }
                    fprintf(out, "xr_aot_await_all_tasks_into_array_resume(ctx, ");
                } else if (dynamic_await_all_xrt_result) {
                    fprintf(out, "xr_aot_await_all_tasks_wait_resume(ctx, ");
                } else {
                    fprintf(out, "xr_aot_await_all_tasks_resume(ctx, ");
                }
                emit_vref(out, v->args[0]);
                if (await_into_result) {
                    fprintf(out, ", ");
                    emit_value_as_rep_ctx(ctx, out, await_into_value, XR_REP_TAGGED);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                } else if (dynamic_await_all_xrt_result) {
                    fprintf(out, ");\n");
                } else {
                    fprintf(out, ", ");
                    emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
                    fprintf(out, ", %s, %s);\n", await_all_elem_name,
                            aggregate_one_shot ? "true" : "false");
                }
            }
        } else if (await_any) {
            fprintf(out, "    _await_%u = xr_aot_await_any_task_resume(ctx, ", v->id);
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ");\n");
        } else {
            if (deferred_task_array) {
                fprintf(out, "    _await_%u = xr_aot_await_deferred_task_from_array_resume(ctx, ",
                        v->id);
                emit_vref(out, deferred_task_array);
                fprintf(out, ", ");
                if (deferred_task_index)
                    emit_int64_arg(out, deferred_task_index);
                else
                    fprintf(out, "-1");
                fprintf(out, ", ");
            } else {
                fprintf(out, "    _await_%u = xr_aot_await_task_resume(ctx, ", v->id);
            }
            emit_coro_await_result_slot(ctx, out, f, prefix, v, await_slot_value);
            fprintf(out, ", false, %s);\n", one_shot_await ? "true" : "false");
        }
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_await_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _await_%u;\n", v->id);
        emit_coro_boundary_error_check(out, "_await_", v->id);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if (one_shot_await && !await_all && !await_any &&
            cg_coro_value_has_storage(f, v->args[0]) && cg_rep(v->args[0]) == XR_REP_TAGGED) {
            fprintf(out, "    ");
            emit_vref(out, v->args[0]);
            fprintf(out, " = XR_NULL_VAL;\n");
        }
        if (inline_await_all_tasks && aggregate_one_shot)
            emit_coro_clear_inline_await_all_task_handles(out, f, &inline_await_all_literal);
        if (dynamic_await_all_xrt_result) {
            fprintf(out, "    int64_t _await_count_%u = xr_aot_await_all_tasks_count(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
            fprintf(out, "    if (_await_count_%u < 0)\n", v->id);
            fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = xrt_array_new_typed(_await_count_%u, %s);\n", v->id,
                    await_all_elem_name);
            fprintf(out, "    if (!xr_aot_await_all_tasks_collect_into_array(ctx, ");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v);
            fprintf(out, ", %s, %s))\n", await_all_elem_name,
                    aggregate_one_shot ? "true" : "false");
            fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
        }
        if (!scalarize_await_all_result && !dynamic_await_all_xrt_result && !await_into_result &&
            cg_coro_await_needs_tagged_boundary_temp(ctx, f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = ");
            const char *suffix = emit_load_conversion_prefix(ctx, out, v, XR_REP_TAGGED);
            fprintf(out, "xr_aot_bridge_value_to_xrt(");
            emit_coro_await_boundary_temp_ref(out, v);
            fprintf(out, ")");
            emit_conversion_suffix(out, suffix);
            fprintf(out, ";\n");
        } else if (!scalarize_await_all_result && !dynamic_await_all_xrt_result &&
                   !await_into_result && cg_coro_value_has_storage(f, v) &&
                   cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = xr_aot_bridge_value_to_xrt(");
            emit_vref(out, v);
            fprintf(out, ");\n");
        }
        bool await_all_skip_scalar_result_clone =
            await_all && !cg_coro_await_all_result_needs_boundary_clone(await_all_elem_name);
        if (!scalarize_await_all_result && cg_coro_value_has_storage(f, v) &&
            cg_rep(v) == XR_REP_TAGGED && xi_coro_value_needs_boundary_clone(v) &&
            !await_all_skip_scalar_result_clone) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = %s(",
                    xi_coro_value_has_json_type(v) ? "xrt_json_clone_for_coro"
                                                   : "xrt_value_clone_for_coro");
            emit_vref(out, v);
            fprintf(out, ");\n");
        }
        if (!scalarize_await_all_result)
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (emit_coro_test_yield_call_stmt(ctx, out, f, v, state_id))
        return;

    if (emit_coro_callable_target_switch(ctx, out, f, v, state_id))
        return;

    CgCoroSuspendCallSite direct_site = cg_coro_direct_suspend_call_site_info(ctx, f, v);
    CgStaticFunctionCall direct_call = direct_site.call;
    const XiFunc *direct_call_target = direct_call.func;
    if (direct_call_target) {
        emit_value_generated_line_reset(ctx, out, v);
        const char *direct_call_prefix = direct_call.prefix ? direct_call.prefix : prefix;
        bool reuse_call_frame = cg_coro_direct_call_frame_reusable(ctx, direct_call_target);
        if (!cg_aot_frame_new_can_supply_cl_arg(f, direct_site.cl_source, direct_call_target)) {
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
        emit_aot_frame_new_call_args(ctx, out, f, direct_site.cl_source, direct_call_target, false,
                                     v->args, direct_site.arg_start, v->nargs, NULL);
        fprintf(out, ");\n");
        fprintf(out, "        if (!f->call_frame_%u)\n", v->id);
        fprintf(out, "            return xr_aot_error(XR_NULL_VAL, false);\n");
        fprintf(out, "    }\n");
        if (reuse_call_frame) {
            fprintf(out, "    else if (!");
            emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_frame_init");
            fprintf(out, "(f->call_frame_%u", v->id);
            if (cg_func_frame_needs_cl(direct_call_target) || v->nargs > direct_site.arg_start) {
                fprintf(out, ", ");
                emit_aot_frame_new_call_args(ctx, out, f, direct_site.cl_source, direct_call_target,
                                             false, v->args, direct_site.arg_start, v->nargs, NULL);
            }
            fprintf(out, ")) {\n");
            fprintf(out, "        return xr_aot_error(XR_NULL_VAL, false);\n");
            fprintf(out, "    }\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _call_%u = ", v->id);
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_resume");
        fprintf(out, "(f->call_frame_%u, ctx);\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
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
            emit_assign_from_xrvalue_temp_ctx(ctx, out, v, tmp);
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
        emit_coro_call_error_channel_dispatch(out, v->id, sid);
        fprintf(out, "    }\n");
        fprintf(out, "    return _call_%u;\n", v->id);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _call_%u = ", v->id);
        emit_fname_suffix(ctx, out, direct_call_prefix, direct_call_target, "_aot_resume");
        fprintf(out, "(f->call_frame_%u, ctx);\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
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
            emit_assign_from_xrvalue_temp_ctx(ctx, out, v, tmp);
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
        emit_coro_call_error_channel_dispatch(out, v->id, sid);
        fprintf(out, "    }\n");
        fprintf(out, "    return _call_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CORO_OP) {
        emit_aot_coro_op_stmt(out, f, v);
        return;
    }

    if (cg_is_time_sleep_call_ctx(ctx, f, v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _sleep_%u = xr_aot_sleep(ctx, ", v->id);
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
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
        fprintf(out, "    XrValue _time_method_%u = %s(", v->id, time_helper);
        if (cg_time_module_helper_has_tagged_arg(time_helper))
            emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_time_method_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_SCOPE_ENTER) {
        emit_value_generated_line_reset(ctx, out, v);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _scope_enter_%u = xr_aot_scope_enter(ctx, %u);\n", v->id,
                (unsigned) v->aux_int);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_scope_enter_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _scope_enter_%u;\n", v->id);
        return;
    }

    if (v->op == XI_SCOPE_EXIT) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        /* A scope block is a statement — scope exit yields no value.  When the
         * frame still reserves a slot for the unit result, park a null in it;
         * nothing ever reads it. */
        bool has_storage = cg_coro_value_has_storage(f, v);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _scope_exit_%u = xr_aot_scope_exit(ctx, %u);\n", v->id,
                (unsigned) v->aux_int);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        emit_coro_scope_exit_error_check(out, v->id);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _scope_exit_%u = xr_aot_scope_exit(ctx, %u);\n", v->id,
                (unsigned) v->aux_int);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_scope_exit_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _scope_exit_%u;\n", v->id);
        fprintf(out, "    }\n");
        emit_coro_scope_exit_error_check(out, v->id);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        if (has_storage && cg_rep(v) == XR_REP_TAGGED) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = xr_aot_bridge_value_to_xrt(XR_NULL_VAL);\n");
        } else if (has_storage) {
            fprintf(out,
                    "    XrValue _scope_exit_value_%u = "
                    "xr_aot_bridge_value_to_xrt(XR_NULL_VAL);\n",
                    v->id);
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "_scope_exit_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
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
        emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ");\n");
        return;
    }

    if (v->op == XI_SELECT_BLOCK) {
        emit_value_generated_line_reset(ctx, out, v);
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
            emit_value_as_rep_ctx(ctx, out, v->args[i], XR_REP_TAGGED);
            fprintf(out, ";\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    XrAotResult _select_%u = xr_aot_select_block(ctx, _select_channels_%u, "
                "%u, %u);\n",
                v->id, v->id, (unsigned) v->nargs, (unsigned) v->aux_int);
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 2) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_SEND missing operands\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        const XiValue *send_arg = NULL;
        const char *helper =
            cg_coro_typed_send_helper("xr_aot_chan_try_send_ready", v->args[1], &send_arg);
        bool transfer_helper =
            send_arg == NULL && strcmp(helper, "xr_aot_chan_try_send_ready") == 0;
        if (transfer_helper)
            helper = "xr_aot_chan_try_send_ready_transfer";
        char bridge_value[64] = {0};
        char bridge_mode[64] = {0};
        if (transfer_helper) {
            const XaotTransferPlan *transfer_plan =
                cg_required_transfer_plan(ctx, v, 0, v->args[1], "channel trySend");
            emit_coro_runtime_channel_bridge_temp(ctx, out, v->args[1], v->id, "chan_try",
                                                  transfer_plan, bridge_value, sizeof(bridge_value),
                                                  bridge_mode, sizeof(bridge_mode));
        }
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrValue _chan_try_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (transfer_helper)
            fprintf(out, "%s", bridge_value);
        else
            emit_coro_send_value(ctx, out, v->args[1], send_arg);
        if (transfer_helper)
            fprintf(out, ", %s", bridge_mode);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_try_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_TRY_RECV) {
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: CHAN_TRY_RECV missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrValue _chan_try_%u = xr_aot_chan_try_recv(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out,
                "    XrValue _chan_try_payload_%u = "
                "xr_aot_bridge_value_to_xrt(xr_aot_recv_payload(_chan_try_%u));\n",
                v->id, v->id);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_chan_try_payload_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
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
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        }
        return;
    }

    if (v->op == XI_ISNULL && v->nargs >= 1 && v->args[0] && v->args[0]->op == XI_CHAN_TRY_RECV) {
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    ");
            emit_vref(out, v);
            fprintf(out, " = !xr_aot_recv_is_value(_chan_try_%u);\n", v->args[0]->id);
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
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
        fprintf(out, "    XrValue _tuple_get_%u = xrt_tuple_get(", v->id);
        emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
        fprintf(out, ", %u);\n", (unsigned) v->aux_int);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_tuple_get_%u", v->id);
            emit_assign_from_xrvalue_temp_ctx(ctx, out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1) {
        const XiValue *builtin = xi_coro_builtin_origin(v->args[0]);
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

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_task(v->args[0])) {
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

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_channel(v->args[0])) {
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

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_work_queue(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_work_queue_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT WorkQueue field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _wq_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_wq_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_result_group(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_result_group_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT ResultGroup field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _rg_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_rg_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_countdown_latch(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_countdown_latch_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT CountdownLatch field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _latch_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_latch_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_semaphore(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_semaphore_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Semaphore field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _sem_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_sem_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_LOAD_FIELD && v->nargs >= 1 && xi_value_type_is_event_count(v->args[0])) {
        const char *field = (const char *) v->aux;
        const char *helper = cg_event_count_field_helper(field);
        if (!helper) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT EventCount field '%s'\n",
                    field ? field : "?");
            emit_codegen_abort_aot_result(out);
            return;
        }
        fprintf(out, "    XrValue _event_count_field_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[56];
            snprintf(tmp, sizeof(tmp), "_event_count_field_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_task_method_call(v, "cancel", 0)) {
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

    if (xi_value_is_task_method_call(v, "poll", 0)) {
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

    if (xi_value_is_task_method_call(v, "awaitResult", 0) ||
        xi_value_is_task_method_call(v, "awaitTimeout", 1)) {
        emit_value_generated_line_reset(ctx, out, v);
        bool timeout_enabled = xi_value_is_task_method_call(v, "awaitTimeout", 1);
        int sid = ++(*state_id);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
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
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _task_await_%u = xr_aot_task_await_result_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", %s);\n", timeout_enabled ? "true" : "false");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_task(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Task method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_work_queue_method_call(v, "push", 1) ||
        xi_value_is_work_queue_method_call(v, "push", 2)) {
        fprintf(out, "    bool _wq_push_%u = xr_aot_work_queue_push_bool(ctx, ", v->id);
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
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_work_queue_method_call(v, "pushRange", 2) ||
        xi_value_is_work_queue_method_call(v, "pushRange", 3)) {
        fprintf(out, "    int64_t _wq_push_range_%u = xr_aot_work_queue_push_range_i64(ctx, ",
                v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[2]);
        fprintf(out, ", ");
        if (v->nargs >= 4)
            emit_int64_arg(out, v->args[3]);
        else
            fprintf(out, "-1");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_wq_push_range_%u", v->id);
            emit_assign_from_i64_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_work_queue_method_call(v, "close", 0)) {
        fprintf(out, "    xr_aot_work_queue_close_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_work_queue_method_call(v, "tryPop", 0) ||
        xi_value_is_work_queue_method_call(v, "tryPop", 1)) {
        /* The runtime returns the popped value via out-param + an ok flag; the
         * (value, ok) pair is packed here into an AOT-native tuple so downstream
         * XI_TUPLE_GET reads it through xrt_tuple_get like every other AOT tuple.
         * The popped value is bridged in case its element type is a heap object. */
        fprintf(out, "    XrValue _wq_try_pop_val_%u = XR_NULL_VAL;\n", v->id);
        fprintf(out, "    bool _wq_try_pop_ok_%u = xr_aot_work_queue_try_pop(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "-1");
        fprintf(out, ", &_wq_try_pop_val_%u);\n", v->id);
        fprintf(
            out,
            "    XrValue _wq_try_pop_%u = xrt_tuple_make(2, (XrValue[]){"
            "xr_aot_bridge_value_to_xrt(_wq_try_pop_val_%u), XR_FROM_BOOL(_wq_try_pop_ok_%u)});\n",
            v->id, v->id, v->id);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_wq_try_pop_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_blocking_work_queue_method_call(v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        bool direct_i64_optional = cg_value_is_i64_optional_blocking_result_root(v) &&
                                   cg_value_is_elided_i64_optional_blocking_result(f, v);
        bool direct_xvalue = !direct_i64_optional && cg_coro_can_use_xvalue_result_ptr(f, v);
        if (!direct_i64_optional && !direct_xvalue) {
            fprintf(out, "    XrSlotRef _wq_pop_slot_%u = ", v->id);
            emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
            fprintf(out, ";\n");
        }
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        const char *helper = "xr_aot_work_queue_pop";
        if (direct_i64_optional)
            helper = "xr_aot_work_queue_pop_i64_optional";
        else if (direct_xvalue)
            helper = "xr_aot_work_queue_pop_value";
        fprintf(out, "    XrAotResult _wq_pop_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "-1");
        if (direct_i64_optional) {
            fprintf(out, ", &");
            emit_coro_i64_optional_value_ref(ctx, out, f, v);
            fprintf(out, ", &");
            emit_coro_i64_optional_has_ref(ctx, out, f, v);
            fprintf(out, ");\n");
        } else if (direct_xvalue) {
            fprintf(out, ", &");
            emit_vref(out, v);
            fprintf(out, ");\n");
        } else {
            fprintf(out, ", _wq_pop_slot_%u);\n", v->id);
        }
        emit_value_generated_line_reset(ctx, out, v);
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
        if (!direct_i64_optional && !direct_xvalue) {
            fprintf(out, "    _wq_pop_slot_%u = ", v->id);
            emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
            fprintf(out, ";\n");
        }
        emit_value_source_line(ctx, out, v);
        if (direct_i64_optional) {
            fprintf(out, "    _wq_pop_%u = xr_aot_work_queue_pop_i64_optional_resume(ctx, &",
                    v->id);
            emit_coro_i64_optional_value_ref(ctx, out, f, v);
            fprintf(out, ", &");
            emit_coro_i64_optional_has_ref(ctx, out, f, v);
            fprintf(out, ");\n");
        } else if (direct_xvalue) {
            fprintf(out, "    _wq_pop_%u = xr_aot_work_queue_pop_value_resume(ctx, &", v->id);
            emit_vref(out, v);
            fprintf(out, ");\n");
        } else {
            fprintf(out, "    _wq_pop_%u = xr_aot_work_queue_pop_resume(ctx, _wq_pop_slot_%u);\n",
                    v->id, v->id);
        }
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    if (_wq_pop_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _wq_pop_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_work_queue(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT WorkQueue method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_result_group_method_call(v, "add", 1)) {
        fprintf(out, "    bool _rg_add_%u = xr_aot_result_group_add_bool(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_rg_add_%u", v->id);
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_result_group_method_call(v, "flush", 0)) {
        fprintf(out, "    xr_aot_result_group_flush_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_result_group_method_call(v, "reset", 1)) {
        fprintf(out, "    bool _rg_reset_%u = xr_aot_result_group_reset_bool(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_rg_reset_%u", v->id);
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_result_group_method_call(v, "tryRecv", 0)) {
        fprintf(out, "    XrValue _rg_try_recv_val_%u = XR_NULL_VAL;\n", v->id);
        fprintf(out, "    bool _rg_try_recv_ok_%u = xr_aot_result_group_try_recv(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", &_rg_try_recv_val_%u);\n", v->id);
        fprintf(out,
                "    XrValue _rg_try_recv_%u = xrt_tuple_make(2, (XrValue[]){_rg_try_recv_val_%u, "
                "XR_FROM_BOOL(_rg_try_recv_ok_%u)});\n",
                v->id, v->id, v->id);
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_rg_try_recv_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_result_group_method_call(v, "close", 0)) {
        fprintf(out, "    xr_aot_result_group_close_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_blocking_result_group_method_call(v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        bool direct_i64_optional = cg_value_is_i64_optional_blocking_result_root(v) &&
                                   cg_value_is_elided_i64_optional_blocking_result(f, v);
        bool direct_xvalue = !direct_i64_optional && cg_coro_can_use_xvalue_result_ptr(f, v);
        if (!direct_i64_optional && !direct_xvalue) {
            fprintf(out, "    XrSlotRef _rg_recv_slot_%u = ", v->id);
            emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
            fprintf(out, ";\n");
        }
        fprintf(out, "    xr_aot_submit_deferred_spawns(ctx);\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        const char *helper = "xr_aot_result_group_recv";
        if (direct_i64_optional)
            helper = "xr_aot_result_group_recv_i64_optional";
        else if (direct_xvalue)
            helper = "xr_aot_result_group_recv_value";
        fprintf(out, "    XrAotResult _rg_recv_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        if (direct_i64_optional) {
            fprintf(out, ", &");
            emit_coro_i64_optional_value_ref(ctx, out, f, v);
            fprintf(out, ", &");
            emit_coro_i64_optional_has_ref(ctx, out, f, v);
            fprintf(out, ");\n");
        } else if (direct_xvalue) {
            fprintf(out, ", &");
            emit_vref(out, v);
            fprintf(out, ");\n");
        } else {
            fprintf(out, ", _rg_recv_slot_%u);\n", v->id);
        }
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_rg_recv_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _rg_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_rg_recv_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _rg_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        if (!direct_i64_optional && !direct_xvalue) {
            fprintf(out, "    _rg_recv_slot_%u = ", v->id);
            emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
            fprintf(out, ";\n");
        }
        emit_value_source_line(ctx, out, v);
        if (direct_i64_optional) {
            fprintf(out, "    _rg_recv_%u = xr_aot_result_group_recv_i64_optional_resume(ctx, &",
                    v->id);
            emit_coro_i64_optional_value_ref(ctx, out, f, v);
            fprintf(out, ", &");
            emit_coro_i64_optional_has_ref(ctx, out, f, v);
            fprintf(out, ");\n");
        } else if (direct_xvalue) {
            fprintf(out, "    _rg_recv_%u = xr_aot_result_group_recv_value_resume(ctx, &", v->id);
            emit_vref(out, v);
            fprintf(out, ");\n");
        } else {
            fprintf(out,
                    "    _rg_recv_%u = xr_aot_result_group_recv_resume(ctx, _rg_recv_slot_%u);\n",
                    v->id, v->id);
        }
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_rg_recv_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _rg_recv_%u;\n", v->id);
        fprintf(out, "    if (_rg_recv_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _rg_recv_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_result_group(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT ResultGroup method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_countdown_latch_method_call(v, "reset", 1)) {
        fprintf(out, "    bool _latch_reset_%u = xr_aot_countdown_latch_reset_bool(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_latch_reset_%u", v->id);
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_countdown_latch_method_call(v, "done", 0) ||
        xi_value_is_countdown_latch_method_call(v, "done", 1)) {
        fprintf(out, "    int64_t _latch_done_%u = xr_aot_countdown_latch_done_i64(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "1");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_latch_done_%u", v->id);
            emit_assign_from_i64_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_countdown_latch_method_call(v, "tryWait", 0)) {
        fprintf(out, "    bool _latch_try_wait_%u = xr_aot_countdown_latch_try_wait_bool(ctx, ",
                v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "_latch_try_wait_%u", v->id);
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_countdown_latch_method_call(v, "close", 0)) {
        fprintf(out, "    xr_aot_countdown_latch_close_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_blocking_countdown_latch_method_call(v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _latch_wait_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _latch_wait_%u = xr_aot_countdown_latch_wait(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _latch_wait_slot_%u);\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_latch_wait_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _latch_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_latch_wait_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _latch_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _latch_wait_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    _latch_wait_%u = xr_aot_countdown_latch_wait_resume(ctx, "
                "_latch_wait_slot_%u);\n",
                v->id, v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_latch_wait_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _latch_wait_%u;\n", v->id);
        fprintf(out, "    if (_latch_wait_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _latch_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_countdown_latch(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT CountdownLatch method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_semaphore_method_call(v, "release", 0) ||
        xi_value_is_semaphore_method_call(v, "release", 1)) {
        fprintf(out, "    int64_t _sem_release_%u = xr_aot_semaphore_release_i64(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "1");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_sem_release_%u", v->id);
            emit_assign_from_i64_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_semaphore_method_call(v, "tryAcquire", 0)) {
        fprintf(out, "    bool _sem_try_acquire_%u = xr_aot_semaphore_try_acquire_bool(ctx, ",
                v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "_sem_try_acquire_%u", v->id);
            emit_assign_from_bool_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_semaphore_method_call(v, "close", 0)) {
        fprintf(out, "    xr_aot_semaphore_close_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_blocking_semaphore_method_call(v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _sem_acquire_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _sem_acquire_%u = xr_aot_semaphore_acquire(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _sem_acquire_slot_%u);\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_sem_acquire_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _sem_acquire_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_sem_acquire_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _sem_acquire_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _sem_acquire_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    _sem_acquire_%u = xr_aot_semaphore_acquire_resume(ctx, "
                "_sem_acquire_slot_%u);\n",
                v->id, v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_sem_acquire_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _sem_acquire_%u;\n", v->id);
        fprintf(out, "    if (_sem_acquire_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _sem_acquire_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_semaphore(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT Semaphore method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_event_count_method_call(v, "advance", 0) ||
        xi_value_is_event_count_method_call(v, "advance", 1)) {
        fprintf(out, "    int64_t _event_count_advance_%u = xr_aot_event_count_advance_i64(ctx, ",
                v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (v->nargs >= 2)
            emit_int64_arg(out, v->args[1]);
        else
            fprintf(out, "1");
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[56];
            snprintf(tmp, sizeof(tmp), "_event_count_advance_%u", v->id);
            emit_assign_from_i64_temp(out, v, tmp);
        }
        return;
    }

    if (xi_value_is_event_count_method_call(v, "close", 0)) {
        fprintf(out, "    xr_aot_event_count_close_void(ctx, ");
        emit_vref(out, v->args[0]);
        fprintf(out, ");\n");
        if (cg_coro_value_has_storage(f, v)) {
            XrRep rep = cg_rep(v);
            if (rep != XR_REP_VOID) {
                fprintf(out, "    ");
                emit_vref(out, v);
                fprintf(out, " = ");
                fprintf(out, rep == XR_REP_TAGGED ? "XR_NULL_VAL" : "0");
                fprintf(out, ";\n");
            }
        }
        return;
    }

    if (xi_value_is_blocking_event_count_method_call(v)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _event_count_wait_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _event_count_wait_%u = xr_aot_event_count_wait(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ", ");
        if (v->nargs >= 3)
            emit_int64_arg(out, v->args[2]);
        else
            fprintf(out, "-1");
        fprintf(out, ", _event_count_wait_slot_%u);\n", v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_event_count_wait_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _event_count_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_event_count_wait_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _event_count_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = %d;\n", sid);
        fprintf(out, "    _event_count_wait_slot_%u = ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    _event_count_wait_%u = xr_aot_event_count_wait_resume(ctx, "
                "_event_count_wait_slot_%u);\n",
                v->id, v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_event_count_wait_%u.kind == XR_AOT_RUN_BLOCKED)\n", v->id);
        fprintf(out, "        return _event_count_wait_%u;\n", v->id);
        fprintf(out, "    if (_event_count_wait_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _event_count_wait_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && v->nargs >= 1 && xi_value_type_is_event_count(v->args[0])) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT EventCount method '%s'\n",
                v->aux ? (const char *) v->aux : "?");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (xi_value_is_channel_method_call(v, "sendTimeout", 2)) {
        emit_value_generated_line_reset(ctx, out, v);
        const XiValue *send_arg = NULL;
        const char *helper =
            cg_coro_typed_send_helper("xr_aot_chan_send_timeout", v->args[1], &send_arg);
        bool transfer_helper = send_arg == NULL && strcmp(helper, "xr_aot_chan_send_timeout") == 0;
        if (transfer_helper)
            helper = "xr_aot_chan_send_timeout_transfer";
        int sid = ++(*state_id);
        char bridge_value[64] = {0};
        char bridge_mode[64] = {0};
        if (transfer_helper) {
            const XaotTransferPlan *transfer_plan =
                cg_required_transfer_plan(ctx, v, 0, v->args[1], "channel sendTimeout");
            emit_coro_runtime_channel_bridge_temp(ctx, out, v->args[1], v->id, "chan_send_timeout",
                                                  transfer_plan, bridge_value, sizeof(bridge_value),
                                                  bridge_mode, sizeof(bridge_mode));
        }
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_send_timeout_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        if (transfer_helper)
            fprintf(out, "%s", bridge_value);
        else
            emit_coro_send_value(ctx, out, v->args[1], send_arg);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[2]);
        fprintf(out, ", ");
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        if (transfer_helper)
            fprintf(out, ", %s", bridge_mode);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _chan_send_timeout_%u = xr_aot_chan_send_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", true);\n");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_send_timeout_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_send_timeout_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (xi_value_is_channel_method_call(v, "recvTimeout", 1)) {
        emit_value_generated_line_reset(ctx, out, v);
        int sid = ++(*state_id);
        bool result_observed = cg_coro_value_result_observed(f, v);
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_recv_timeout_%u = xr_aot_chan_recv_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", ");
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", ");
        emit_int64_arg(out, v->args[1]);
        fprintf(out, ", %s);\n", result_observed ? "true" : "false");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _chan_recv_timeout_%u = xr_aot_chan_recv_slot_resume(ctx, ", v->id);
        emit_coro_optional_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ", %s);\n", result_observed ? "true" : "false");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_recv_timeout_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_timeout_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (result_observed && cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        if (result_observed)
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CHAN_SEND || xi_value_is_channel_method_call(v, "send", 1)) {
        emit_value_generated_line_reset(ctx, out, v);
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
        bool transfer_helper = send_arg == NULL && strcmp(helper, "xr_aot_chan_send") == 0;
        if (transfer_helper)
            helper = "xr_aot_chan_send_transfer";
        int sid = ++(*state_id);
        char bridge_value[64] = {0};
        char bridge_mode[64] = {0};
        if (transfer_helper) {
            const XaotTransferPlan *transfer_plan =
                cg_required_transfer_plan(ctx, v, 0, send_value, "channel send");
            emit_coro_runtime_channel_bridge_temp(ctx, out, send_value, v->id, "chan_send",
                                                  transfer_plan, bridge_value, sizeof(bridge_value),
                                                  bridge_mode, sizeof(bridge_mode));
        }
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_send_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, channel);
        fprintf(out, ", ");
        if (transfer_helper)
            fprintf(out, "%s", bridge_value);
        else
            emit_coro_send_value(ctx, out, send_value, send_arg);
        if (transfer_helper)
            fprintf(out, ", xr_slot_none(), -1, %s", bridge_mode);
        else if (strcmp(helper, "xr_aot_chan_send") == 0)
            fprintf(out, ", xr_slot_none(), -1");
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _chan_send_%u = xr_aot_chan_send_resume(ctx, xr_slot_none(), false);\n",
                v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_send_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_send_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v)) {
            fprintf(out, "    XrValue _chan_send_value_%u = XR_NULL_VAL;\n", v->id);
            char tmp[40];
            snprintf(tmp, sizeof(tmp), "_chan_send_value_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CHAN_RECV) {
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recv missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        const XiValue *status = xi_coro_recv_status_user(f, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_recv_%u = %s(ctx, ", v->id, helper);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_slot_%u, _chan_recv_ok_slot_%u", v->id, v->id);
        if (strcmp(helper, "xr_aot_chan_recv_pair") == 0)
            fprintf(out, ", -1");
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    _chan_recv_%u = xr_aot_chan_recv_pair_resume(ctx, _chan_recv_slot_%u, "
                "_chan_recv_ok_slot_%u);\n",
                v->id, v->id, v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        if (status)
            emit_coro_debug_result_source_var_sync(ctx, out, f, status);
        return;
    }

    if (xi_value_is_channel_method_call(v, "recv", 0)) {
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 1) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recv missing channel\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        bool result_observed = cg_coro_value_result_observed(f, v);
        fprintf(out, "    XrSlotRef _chan_recv_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_recv_%u = xr_aot_chan_recv_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_slot_%u, -1, %s);\n", v->id, result_observed ? "true" : "false");
        emit_value_generated_line_reset(ctx, out, v);
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
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    _chan_recv_%u = xr_aot_chan_recv_slot_resume(ctx, xr_slot_none(), %s);\n",
                v->id, result_observed ? "true" : "false");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_recv_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (result_observed && cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        if (result_observed)
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (xi_value_is_channel_method_call(v, "recvOr", 1)) {
        emit_value_generated_line_reset(ctx, out, v);
        if (v->nargs < 2) {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: channel recvOr missing operands\n");
            emit_codegen_abort_aot_result(out);
            return;
        }
        int sid = ++(*state_id);
        fprintf(out, "    XrSlotRef _chan_recv_or_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        fprintf(out, "    f->state = %d;\n", sid);
        emit_value_source_line(ctx, out, v);
        fprintf(out, "    XrAotResult _chan_recv_or_%u = xr_aot_chan_recv_or_slot(ctx, ", v->id);
        emit_vref(out, v->args[0]);
        fprintf(out, ", _chan_recv_or_slot_%u, ", v->id);
        emit_value_as_rep(out, v->args[1], XR_REP_TAGGED);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_recv_or_%u.kind == XR_AOT_RUN_BLOCKED) {\n", v->id);
        fprintf(out, "        return _chan_recv_or_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    if (_chan_recv_or_%u.kind == XR_AOT_RUN_ERROR) {\n", v->id);
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return _chan_recv_or_%u;\n", v->id);
        fprintf(out, "    }\n");
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    goto S%d_DONE;\n", sid);
        fprintf(out, "S%d:;\n", sid);
        fprintf(out, "    f->state = 0;\n");
        fprintf(out, "    _chan_recv_or_slot_%u = ", v->id);
        emit_coro_slot_ref(ctx, out, f, prefix, v);
        fprintf(out, ";\n");
        emit_value_source_line(ctx, out, v);
        fprintf(out,
                "    _chan_recv_or_%u = xr_aot_chan_recv_or_slot_resume(ctx, "
                "_chan_recv_or_slot_%u);\n",
                v->id, v->id);
        emit_value_generated_line_reset(ctx, out, v);
        fprintf(out, "    if (_chan_recv_or_%u.kind == XR_AOT_RUN_ERROR)\n", v->id);
        fprintf(out, "        return _chan_recv_or_%u;\n", v->id);
        fprintf(out, "S%d_DONE:;\n", sid);
        if (cg_coro_value_has_storage(f, v))
            emit_bridge_stored_tagged_value(out, v);
        emit_coro_debug_result_source_var_sync(ctx, out, f, v);
        return;
    }

    if (v->op == XI_CALL_METHOD && xi_value_type_is_channel(v->args[0])) {
        if (xi_value_is_channel_method_call(v, "trySend", 1)) {
            emit_value_generated_line_reset(ctx, out, v);
            const XiValue *send_arg = NULL;
            const char *helper =
                cg_coro_typed_send_helper("xr_aot_chan_try_send", v->args[1], &send_arg);
            bool transfer_helper = send_arg == NULL && strcmp(helper, "xr_aot_chan_try_send") == 0;
            if (transfer_helper)
                helper = "xr_aot_chan_try_send_transfer";
            char bridge_value[64] = {0};
            char bridge_mode[64] = {0};
            if (transfer_helper) {
                const XaotTransferPlan *transfer_plan =
                    cg_required_transfer_plan(ctx, v, 0, v->args[1], "channel method");
                emit_coro_runtime_channel_bridge_temp(
                    ctx, out, v->args[1], v->id, "chan_method", transfer_plan, bridge_value,
                    sizeof(bridge_value), bridge_mode, sizeof(bridge_mode));
            }
            emit_value_source_line(ctx, out, v);
            fprintf(out, "    XrValue _chan_method_%u = %s(ctx, ", v->id, helper);
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            if (transfer_helper)
                fprintf(out, "%s", bridge_value);
            else
                emit_coro_send_value(ctx, out, v->args[1], send_arg);
            if (transfer_helper)
                fprintf(out, ", %s", bridge_mode);
            fprintf(out, ");\n");
            emit_value_generated_line_reset(ctx, out, v);
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
            }
            return;
        }
        if (xi_value_is_channel_method_call(v, "tryRecv", 0)) {
            emit_value_generated_line_reset(ctx, out, v);
            emit_value_source_line(ctx, out, v);
            fprintf(out, "    XrValue _chan_method_%u = xr_aot_chan_try_recv(ctx, ", v->id);
            emit_vref(out, v->args[0]);
            fprintf(out, ");\n");
            emit_value_generated_line_reset(ctx, out, v);
            if (cg_coro_value_has_storage(f, v)) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "_chan_method_%u", v->id);
                emit_assign_from_xrvalue_temp(out, v, tmp);
                emit_bridge_stored_tagged_value(out, v);
            }
            emit_coro_debug_result_source_var_sync(ctx, out, f, v);
            return;
        }
        if (xi_value_is_channel_method_call(v, "close", 0)) {
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
        if (xi_value_is_channel_method_call(v, "isClosed", 0)) {
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
        /* Bool form: inside a try, the CFG branches to the catch on the result.
         * Unit form: outside one, an error propagates by ending this frame --
         * for a coroutine that means a value-channel XR_AOT_RUN_ERROR, which
         * the worker records on the task and the awaiter re-raises by value.
         * Generating nothing for the unit form is how a value error raised
         * across a coroutine boundary used to vanish (task 248). */
        if (cg_value_type_is_bool(v)) {
            if (cg_coro_value_has_storage(f, v)) {
                fprintf(out, "    ");
                emit_vref(out, v);
                if (cg_rep(v) == XR_REP_TAGGED)
                    fprintf(out, " = XR_FROM_BOOL(xrt_has_pending_error());\n");
                else
                    fprintf(out, " = xrt_has_pending_error();\n");
            }
            return;
        }
        fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
        fprintf(out, "        XrValue _err_check_%u = xrt_pending_error;\n", v->id);
        fprintf(out, "        xrt_pending_error = XR_NULL_VAL;\n");
        fprintf(out, "        f->state = 0;\n");
        fprintf(out, "        return xr_aot_error(_err_check_%u, true);\n", v->id);
        fprintf(out, "    }\n");
        return;
    }

    if (v->op == XI_ERR_CATCH) {
        fprintf(out, "    XrValue _err_catch_%u = xrt_pending_error;\n", v->id);
        fprintf(out, "    xrt_pending_error = XR_NULL_VAL;\n");
        if (cg_coro_value_has_storage(f, v)) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "_err_catch_%u", v->id);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        }
        return;
    }

    if (v->op == XI_CHAN_SEND || v->op == XI_CHAN_RECV || v->op == XI_CHAN_TRY_SEND ||
        v->op == XI_CHAN_TRY_RECV || v->op == XI_CHAN_IS_CLOSED || v->op == XI_CHAN_NEW) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported AOT coroutine Xi op %s in '%s'\n",
                xi_op_name(v->op), f && f->name ? f->name : "<anonymous>");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_TRY || v->op == XI_END_TRY) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: exceptions inside AOT coroutine are unsupported\n");
        emit_codegen_abort_aot_result(out);
        return;
    }

    if (v->op == XI_AGG_NEW && cg_struct_can_inline(f, v)) {
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
            if (cg_rep(v) == XR_REP_TAGGED &&
                (v->aux_int == XR_GLOBAL_VAR_FILE || v->aux_int == XR_GLOBAL_VAR_DIR))
                fprintf(out, "    %s = xr_aot_bridge_value_to_xrt(%s);\n", tmp, tmp);
            emit_assign_from_xrvalue_temp(out, v, tmp);
        } else {
            fprintf(out, "    (void) xr_aot_get_builtin(ctx, %d);\n", (int) v->aux_int);
        }
        return;
    }

    if (cg_ownership_op_is_noop(ctx && ctx->freestanding_profile, v) ||
        cg_shared_static_function_ownership_is_noop(ctx, f, v))
        return;

    if (xicgen_par_for_stack_closure_value_is_elided(ctx, f, v))
        return;

    if (v->op == XI_RELEASE && v->nargs >= 1 &&
        (cg_coro_value_is_borrowed_unbox_alias(ctx, f, v->args[0]) ||
         cg_coro_value_is_borrowed_identity_alias(ctx, f, v->args[0]) ||
         cg_value_is_borrowed_array_slot_alias(ctx, f, v->args[0])))
        return;

    if (v->op == XI_RELEASE && v->nargs >= 1 && v->args[0] &&
        xi_coro_value_needs_arc_release(v->args[0]) &&
        cg_coro_value_needs_frame(ctx, f, v->args[0]) &&
        cg_coro_value_live_across_suspend(ctx, f, v->args[0])) {
        fprintf(out, "    ");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ";\n");
        fprintf(out, "    ");
        emit_vref(out, v->args[0]);
        fprintf(out, " = %s;\n", cg_rep(v->args[0]) == XR_REP_PTR ? "NULL" : "XR_NULL_VAL");
        return;
    }

    if (cg_is_void_like(v)) {
        fprintf(out, "    (void)(");
        emit_value_rhs(ctx, out, f, v, prefix);
        fprintf(out, ");\n");
        emit_value_generated_line_reset(ctx, out, v);
        return;
    }

    fprintf(out, "    ");
    emit_vref(out, v);
    fprintf(out, " = ");
    emit_value_rhs(ctx, out, f, v, prefix);
    fprintf(out, ";\n");
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
}

static void emit_coro_block(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiBlock *blk,
                            const char *prefix, int *state_id) {
    XR_DCHECK(blk != NULL, "emit_coro_block: NULL block");
    fprintf(out, "L%u:;\n", blk->id);

    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v) {
            emit_coro_value_stmt(ctx, out, f, v, prefix, state_id);
            if (cg_coro_value_terminates_c_path(v))
                return;
        }
    }

    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            emit_block_terminator_source_line(ctx, out, blk);
            if (cg_func_has_defer_stmt(f)) {
                /* Function-scope exit: capture the result, run pending defers
                 * LIFO, then complete. Defers run before the awaiting caller
                 * observes the result, matching the VM. */
                fprintf(out, "    { XrValue _xrt_dret = ");
                emit_coro_aot_done_value(ctx, out, f, blk->control);
                fprintf(out, ";\n");
                fprintf(out, "      xrt_defer_run(&f->_xrt_ds);\n");
                fprintf(out, "      return xr_aot_done(_xrt_dret); }\n");
            } else if (blk->control) {
                fprintf(out, "    return xr_aot_done(");
                emit_coro_aot_done_value(ctx, out, f, blk->control);
                fprintf(out, ");\n");
            } else {
                fprintf(out, "    return xr_aot_done(XR_NULL_VAL);\n");
            }
            emit_block_terminator_generated_line_reset(ctx, out, blk);
            break;
        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                emit_phi_copies(ctx, out, f, blk->succs[0], find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;
        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "AOT coro IF block missing control");
            emit_block_terminator_source_line(ctx, out, blk);
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
    } else if (rep == XR_REP_PTR && cg_type_is_class_instance_ptr(type)) {
        fprintf(out, "xrt_box_obj(%s%u)", slot_prefix, slot_id);
    } else if (rep == XR_REP_RAWPTR) {
        fprintf(out, "XR_FROM_INT((int64_t)(uintptr_t)(%s%u))", slot_prefix, slot_id);
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

static uint16_t emit_coro_frame_slot_visits(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XiValue *value, XrRep rep,
                                            const char *slot_prefix, uint32_t slot_id,
                                            const char *helper, bool with_visitor) {
    const XaotValuePlan *value_plan = cg_coro_optional_value_plan(ctx, value);
    if (value_plan && cg_value_rep_is_adt_aggregate(value_plan->rep)) {
        const XaotEnumPlan *enum_plan = cg_coro_value_enum_plan(ctx, value);
        uint16_t width = enum_plan ? enum_plan->max_payload : 0;
        for (uint16_t payload = 0; payload < width; payload++) {
            fprintf(out, "    %s(", helper);
            if (with_visitor)
                fprintf(out, "visitor, ");
            fprintf(out, "%s%u", slot_prefix, slot_id);
            if (cg_value_rep_is_typed_adt_aggregate(value_plan->rep))
                fprintf(out, ".payload.raw[%u]", (unsigned) payload);
            else
                fprintf(out, ".payloads[%u]", (unsigned) payload);
            fprintf(out, ");\n");
        }
        return width;
    }
    if (!cg_coro_value_can_trace_frame_slot(ctx, f, value))
        return 0;
    fprintf(out, "    %s(", helper);
    if (with_visitor)
        fprintf(out, "visitor, ");
    emit_coro_frame_slot_as_xrvalue(out, value ? value->type : NULL, rep, slot_prefix, slot_id);
    fprintf(out, ");\n");
    return 1;
}

static void emit_coro_frame_value_visit(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const char *helper, bool with_visitor) {
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (!cg_sync_go_param_needs_trace(f, i))
            continue;
        (void) emit_coro_frame_slot_visits(ctx, out, f, f->params[i], cg_coro_param_rep(ctx, f, i),
                                           "f->p", i, helper, with_visitor);
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_value_can_trace_frame_slot(ctx, f, &phi->value) ||
                !cg_coro_value_live_across_suspend(ctx, f, &phi->value))
                continue;
            (void) emit_coro_frame_slot_visits(ctx, out, f, &phi->value,
                                               cg_coro_decl_rep(ctx, f, &phi->value), "f->phi",
                                               phi->value.id, helper, with_visitor);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_needs_frame(ctx, f, v) ||
                !cg_coro_value_can_trace_frame_slot(ctx, f, v) ||
                !cg_coro_value_may_hold_frame_root(ctx, f, v))
                continue;
            (void) emit_coro_frame_slot_visits(ctx, out, f, v, cg_coro_decl_rep(ctx, f, v), "f->v",
                                               v->id, helper, with_visitor);
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
            const XaotCallableInvokePlan *switch_plan = cg_coro_callable_target_switch_plan(ctx, v);
            if (switch_plan) {
                const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
                fprintf(out, "    if (f->call_frame_%u) {\n", v->id);
                fprintf(out, "        switch (f->call_target_id_%u) {\n", v->id);
                for (uint16_t ti = 0; ti < switch_plan->target_count; ti++) {
                    const XaotCallableTargetCase *target =
                        xaot_bundle_callable_target_case(bundle, switch_plan, ti);
                    if (!target || !target->target_func ||
                        (target->effect_bits & XG_BODY_MAY_SUSPEND) == 0)
                        continue;
                    const char *target_prefix = cg_module_prefix_for_func(ctx, target->target_func);
                    fprintf(out, "            case %uu: ", target->target_id);
                    emit_fname_suffix(ctx, out, target_prefix, target->target_func, "_aot_trace");
                    fprintf(out, "(f->call_frame_%u, visitor); break;\n", v->id);
                }
                fprintf(out, "            default: break;\n");
                fprintf(out, "        }\n    }\n");
                continue;
            }
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
            const XaotCallableInvokePlan *switch_plan = cg_coro_callable_target_switch_plan(ctx, v);
            if (switch_plan) {
                const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
                fprintf(out, "    if (f->call_frame_%u) {\n", v->id);
                fprintf(out, "        switch (f->call_target_id_%u) {\n", v->id);
                for (uint16_t ti = 0; ti < switch_plan->target_count; ti++) {
                    const XaotCallableTargetCase *target =
                        xaot_bundle_callable_target_case(bundle, switch_plan, ti);
                    if (!target || !target->target_func ||
                        (target->effect_bits & XG_BODY_MAY_SUSPEND) == 0)
                        continue;
                    const char *target_prefix = cg_module_prefix_for_func(ctx, target->target_func);
                    fprintf(out, "            case %uu: ", target->target_id);
                    emit_fname_suffix(ctx, out, target_prefix, target->target_func, "_aot_release");
                    fprintf(out, "(f->call_frame_%u, NULL); break;\n", v->id);
                }
                fprintf(out, "            default: break;\n");
                fprintf(out, "        }\n");
                fprintf(out, "        f->call_frame_%u = NULL;\n", v->id);
                fprintf(out, "        f->call_target_id_%u = 0;\n", v->id);
                fprintf(out, "    }\n");
                continue;
            }
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

/* One child-frame pointer slot per direct suspend-call site; each such pointer
 * is both a GC root and an ARC release in the parent frame. */
static uint32_t cg_coro_direct_call_frame_count(XiCgenCtx *ctx, const XiFunc *f) {
    uint32_t count = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (cg_coro_call_needs_child_frame(ctx, f, blk->values[vi]))
                count++;
        }
    }
    return count;
}

/* A logical frame slot occupies a physical AOT slot iff it is a parameter/phi
 * or a value that passes the storage test; this mirrors the slot set the frame
 * type / trace / release emitters walk. */
static bool cg_coro_plan_slot_is_physical(const XiFunc *f, const XiCoroSlot *s) {
    return s->kind != XI_CORO_SLOT_VALUE || cg_coro_value_has_storage(f, s->value);
}

static uint16_t cg_coro_plan_slot_physical_root_width(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiCoroSlot *s) {
    if (s && s->kind == XI_CORO_SLOT_PARAM)
        return 0;
    return s && s->frame_root && cg_coro_plan_slot_is_physical(f, s) &&
                   cg_coro_value_can_trace_frame_slot(ctx, f, s->value)
               ? cg_coro_value_frame_root_width(ctx, f, s->value)
               : 0;
}

static uint16_t cg_coro_plan_slot_physical_release_width(XiCgenCtx *ctx, const XiFunc *f,
                                                         const XiCoroSlot *s) {
    if (s && s->kind == XI_CORO_SLOT_PARAM)
        return 0;
    return s && s->frame_release && cg_coro_plan_slot_is_physical(f, s) &&
                   cg_coro_value_needs_frame_arc_release(ctx, f, s->value)
               ? cg_coro_value_frame_root_width(ctx, f, s->value)
               : 0;
}

/* Physical GC-root count for the coroutine frame: each plan slot whose logical
 * frame_root survives the storage filter, plus the direct suspend-call pointers.
 * Matches what emit_coro_frame_value_visit traces by construction. */
static uint32_t cg_coro_plan_frame_roots(XiCgenCtx *ctx, const XiFunc *f, const XiCoroPlan *plan) {
    uint32_t count = cg_coro_direct_call_frame_count(ctx, f);
    if (!plan)
        return count;
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (cg_sync_go_param_needs_trace(f, i))
            count += cg_coro_value_frame_root_width(ctx, f, f->params[i]);
    }
    for (uint32_t i = 0; i < plan->nslots; i++) {
        const XiCoroSlot *s = &plan->slots[i];
        count += cg_coro_plan_slot_physical_root_width(ctx, f, s);
    }
    return count;
}

static void emit_coro_frame_arc_release(XiCgenCtx *ctx, FILE *out, const XiFunc *f) {
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (!cg_sync_go_param_needs_release(f, i))
            continue;
        (void) emit_coro_frame_slot_visits(ctx, out, f, f->params[i], cg_coro_param_rep(ctx, f, i),
                                           "f->p", i, "xrt_release", false);
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cg_coro_value_needs_frame_arc_release(ctx, f, &phi->value) ||
                !cg_coro_value_live_across_suspend(ctx, f, &phi->value))
                continue;
            (void) emit_coro_frame_slot_visits(ctx, out, f, &phi->value,
                                               cg_coro_decl_rep(ctx, f, &phi->value), "f->phi",
                                               phi->value.id, "xrt_release", false);
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_coro_value_needs_frame(ctx, f, v) ||
                !cg_coro_value_needs_frame_arc_release(ctx, f, v) ||
                !cg_coro_value_live_across_suspend(ctx, f, v))
                continue;
            (void) emit_coro_frame_slot_visits(ctx, out, f, v, cg_coro_decl_rep(ctx, f, v), "f->v",
                                               v->id, "xrt_release", false);
        }
    }
}

/* Physical ARC-release count: the closure environment (when present), each plan
 * slot whose logical frame_release survives the storage filter, plus the direct
 * suspend-call pointers.  Matches emit_coro_frame_arc_release by construction. */
static uint32_t cg_coro_plan_frame_releases(XiCgenCtx *ctx, const XiFunc *f,
                                            const XiCoroPlan *plan) {
    uint32_t count =
        (cg_func_frame_needs_cl(f) ? 1u : 0u) + cg_coro_direct_call_frame_count(ctx, f);
    if (!plan)
        return count;
    for (uint16_t i = 0; i < cg_coro_param_count(f); i++) {
        if (cg_sync_go_param_needs_release(f, i))
            count += cg_coro_value_frame_root_width(ctx, f, f->params[i]);
    }
    for (uint32_t i = 0; i < plan->nslots; i++) {
        const XiCoroSlot *s = &plan->slots[i];
        count += cg_coro_plan_slot_physical_release_width(ctx, f, s);
    }
    return count;
}

static bool cg_coro_direct_call_frame_reusable(XiCgenCtx *ctx, const XiFunc *target) {
    if (!ctx || !target || cg_func_frame_needs_cl(target))
        return false;
    const XiCoroPlan *plan = cg_coro_plan(ctx, target);
    return cg_coro_plan_frame_roots(ctx, target, plan) == 0 &&
           cg_coro_plan_frame_releases(ctx, target, plan) == 0;
}

static void xi_cgen_coro_func(XiCgenCtx *ctx, FILE *out, XiFunc *f, const char *prefix) {
    xi_ensure_rpo(f);
    const XiCoroResolver resolver = cg_coro_resolver_ctx(ctx);
    cg_coro_insert_loop_poll_safepoints(f, &resolver);
    const XiCoroPlan *plan = xi_coro_analyze(f, &resolver);
    if (!plan) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: AOT coroutine plan failed for '%s'\n",
                f->name ? f->name : "?");
        return;
    }
    size_t frame_size = estimate_coro_frame_size(ctx, f);
    uint32_t root_count = cg_coro_plan_frame_roots(ctx, f, plan);
    uint32_t release_count = cg_coro_plan_frame_releases(ctx, f, plan);
    record_coro_frame_stats(ctx, frame_size, root_count, release_count);

    emit_coro_frame_type(ctx, out, f, prefix);
    emit_coro_frame_init(ctx, out, f, prefix);
    emit_coro_frame_factory(ctx, out, f, prefix);

    fprintf(out, "%sXrAotResult ", cg_linkage(ctx));
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

    emit_coro_local_declarations(ctx, out, f);
    if (cg_coro_func_emits_loop_poll(f))
        fprintf(out, "    uint32_t _xr_aot_coro_poll_count = 0;\n");
    emit_debug_source_var_declarations(ctx, out, f);
    emit_coro_macros(ctx, out, f, prefix);
    emit_coro_debug_frame_source_var_syncs(ctx, out, f);

    int state_count = (int) plan->nstates;
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

    emit_coro_undefs(ctx, out, f);
    fprintf(out, "\n");

    fprintf(out, "%svoid ", cg_linkage(ctx));
    emit_fname_suffix(ctx, out, prefix, f, "_aot_trace");
    fprintf(out, "(void *frame, void *visitor) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    emit_coro_frame_value_visit(ctx, out, f, "xr_aot_trace_frame_value", true);
    emit_coro_direct_call_frame_trace(ctx, out, f, prefix);
    fprintf(out, "}\n\n");

    if (cg_func_has_defer_stmt(f)) {
        /* Cancellation stops the coroutine between statements, so no return
         * edge drains the defer scope. This entry runs it in place, before the
         * task completes, without touching the frame's other resources. */
        fprintf(out, "%svoid ", cg_linkage(ctx));
        emit_fname_suffix(ctx, out, prefix, f, "_aot_run_defers");
        fprintf(out, "(void *frame) {\n");
        fprintf(out, "    ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
        fprintf(out, " *f = (");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
        fprintf(out, " *)frame;\n");
        fprintf(out, "    if (!f)\n        return;\n");
        fprintf(out, "    xrt_defer_run(&f->_xrt_ds);\n");
        fprintf(out, "}\n\n");
    }

    fprintf(out, "%svoid ", cg_linkage(ctx));
    emit_fname_suffix(ctx, out, prefix, f, "_aot_release");
    fprintf(out, "(void *frame, struct XrCoroHeap *heap) {\n");
    fprintf(out, "    ");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *f = (");
    emit_fname_suffix(ctx, out, prefix, f, "_aot_frame");
    fprintf(out, " *)frame;\n");
    fprintf(out, "    (void)heap;\n");
    fprintf(out, "    if (!f)\n        return;\n");
    /* Run any defers still pending when the frame is released (e.g. a coroutine
     * cancelled mid-flight before reaching its return). Idempotent: a normal
     * completion already drained the scope (count==0), so this is a no-op then. */
    if (cg_func_has_defer_stmt(f))
        fprintf(out, "    xrt_defer_run(&f->_xrt_ds);\n");
    emit_coro_direct_call_frame_release(ctx, out, f, prefix);
    emit_coro_frame_arc_release(ctx, out, f);
    if (cg_func_frame_needs_cl(f))
        fprintf(out, "    xrt_release(xr_mkptr(f->_cl, XR_TAG_CLOSURE));\n");
    fprintf(out, "    xr_aot_frame_free(frame);\n");
    fprintf(out, "}\n\n");

    fprintf(out, "%sconst XrAotCoroDesc ", cg_linkage(ctx));
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
    if (cg_func_has_defer_stmt(f)) {
        fprintf(out, "    .run_pending_defers = ");
        emit_fname_suffix(ctx, out, prefix, f, "_aot_run_defers");
        fprintf(out, ",\n");
    }
    fprintf(out, "};\n\n");
}

/* Generator call: allocate a producer coroutine frame and wrap it in a pull-driven
 * xrt_iterator (never scheduled — driven synchronously by hasNext()/next()). */
static void xicgen_gen_call(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    if (!v || v->nargs < 1) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: GEN_CALL missing callee\n");
        emit_codegen_abort_expr(out);
        return;
    }
    CgStaticFunctionCall gen_call = cg_resolve_static_function_call(ctx, f, v->args[0]);
    const XiFunc *target = gen_call.func;
    const char *gen_prefix = gen_call.prefix ? gen_call.prefix : prefix;
    if (!target || target->entry_type != 2 /* XR_ENTRY_GENERATOR */) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: GEN_CALL target is not a generator\n");
        emit_codegen_abort_expr(out);
        return;
    }
    if (!cg_aot_frame_new_can_supply_cl_arg(f, v->args[0], target)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: unsupported captured AOT generator call\n");
        emit_codegen_abort_expr(out);
        return;
    }
    const char *aot_ctx = cg_aot_context_expr(ctx, f);
    fprintf(out, "({ void *_gen_frame_%u = ", v->id);
    emit_fname_suffix(ctx, out, gen_prefix, target, "_aot_frame_new");
    fprintf(out, "(");
    emit_aot_frame_new_call_args(ctx, out, f, v->args[0], target, false, v->args, 1, v->nargs, v);
    fprintf(out, ");\n");
    fprintf(out, "    xr_aot_gen_iterator_new(%s, &", aot_ctx);
    emit_fname_suffix(ctx, out, gen_prefix, target, "_aot_desc");
    fprintf(out, ", _gen_frame_%u); })", v->id);
}
