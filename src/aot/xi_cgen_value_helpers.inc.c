/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_value_helpers.inc.c - AOT scalar value emission helpers
 */

static void emit_cell_ref(FILE *out, XiVarId var_id) {
    fprintf(out, "cell_%u", (unsigned) var_id);
}

static void emit_c_float_literal(FILE *out, double value) {
    if (isnan(value)) {
        fprintf(out, "NAN");
        return;
    }
    if (isinf(value)) {
        fprintf(out, signbit(value) ? "(-INFINITY)" : "INFINITY");
        return;
    }
    fprintf(out, "%a", value);
}

static bool cg_value_type_is_unsigned_int(const XiValue *v) {
    if (!v || !v->type || v->type->kind != XR_KIND_INT || v->type->is_nullable)
        return false;
    switch (v->type->native_width) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            return true;
        default:
            return false;
    }
}

static void emit_boxed_value_ref(FILE *out, const XiValue *v) {
    if (v && v->type && v->type->kind == XR_KIND_NULL) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }

    XrRep rep = cg_rep(v);
    if (rep == XR_REP_TAGGED) {
        emit_vref(out, v);
    } else if (rep == XR_REP_PTR) {
        const char *conv_suffix =
            emit_conversion_prefix(out, v ? v->type : NULL, rep, XR_REP_TAGGED);
        emit_vref(out, v);
        emit_conversion_suffix(out, conv_suffix);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "XR_FROM_FLOAT(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else if (cg_value_type_is_bool(v)) {
        fprintf(out, "XR_FROM_BOOL(");
        emit_vref(out, v);
        fprintf(out, ")");
    } else {
        fprintf(out, "XR_FROM_INT(");
        emit_vref(out, v);
        fprintf(out, ")");
    }
}

static bool xicgen_defer_mark(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) f;
    (void) prefix;
    XrRep rep = ctx ? cg_value_plan_storage_rep(ctx, v) : cg_rep(v);
    if (rep == XR_REP_TAGGED)
        fprintf(out, "XR_FROM_INT(xrt_defer_mark(&_xrt_ds))");
    else
        fprintf(out, "(int64_t)xrt_defer_mark(&_xrt_ds)");
    return true;
}

static void emit_cell_get_for_rep(FILE *out, const XiValue *v, const char *cell_expr) {
    XrRep rep = cg_rep(v);
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "xrt_cell_get(%s)", cell_expr);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "xrt_cell_get(%s).ptr", cell_expr);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "xrt_cell_get(%s).f", cell_expr);
    } else {
        fprintf(out, "xrt_cell_get(%s).i", cell_expr);
    }
}

/* Read a non-cell upvalue stored as a tagged XrValue and unbox it to the
 * declared storage rep of the consuming local (mirrors emit_cell_get_for_rep).
 * Callers pass the storage rep (cg_value_decl_storage_rep), not cg_rep: a native
 * class instance has cg_rep == TAGGED but a PTR storage local, so without this a
 * captured instance is read as a raw XrValue into a `void *` slot — a C type
 * error. */
static void emit_upval_get_for_rep(FILE *out, XrRep rep, const char *up_expr) {
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "%s", up_expr);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "%s.ptr", up_expr);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "%s.f", up_expr);
    } else {
        fprintf(out, "%s.i", up_expr);
    }
}

static bool cg_value_has_cell(const XiCgenCtx *ctx, const XiValue *v) {
    return ctx && v && xi_var_id_is_valid(v->var_id) && v->var_id < ctx->cell_var_count &&
           ctx->cell_vars[v->var_id];
}

static bool cg_value_is_cell_origin(const XiCgenCtx *ctx, const XiValue *v) {
    return cg_value_has_cell(ctx, v) && ctx->cell_origins[v->var_id] == v;
}

static bool cg_value_has_structural_use(const XiFunc *f, const XiValue *target) {
    if (!f || !target)
        return target && target->uses > 0;
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
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == target)
                    return true;
            }
        }
    }
    return false;
}

static bool cg_value_is_dead_aot_marker(const XiCgenCtx *ctx, const XiFunc *f, const XiValue *v) {
    if (!v || cg_value_has_cell(ctx, v) || cg_value_has_structural_use(f, v))
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;
    switch ((XiOp) v->op) {
        case XI_CONST:
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_MOVE:
            return true;
        default:
            return false;
    }
}

static void cg_note_var_id(const XiValue *v, uint32_t *max_var_id, bool *has_var_id) {
    if (!v || !xi_var_id_is_valid(v->var_id))
        return;
    if (!*has_var_id || v->var_id > *max_var_id)
        *max_var_id = v->var_id;
    *has_var_id = true;
}

static bool cg_value_is_closure_alloc(const XiValue *v) {
    return v &&
           (v->op == XI_CLOSURE_NEW || (v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW)) &&
           v->aux;
}

static uint32_t cg_cell_var_count_for_func(const XiFunc *f) {
    uint32_t max_var_id = 0;
    bool has_var_id = false;
    if (!f)
        return 0;

    for (uint16_t i = 0; i < f->nparams; i++)
        cg_note_var_id(f->params[i], &max_var_id, &has_var_id);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control)
            cg_note_var_id(blk->control, &max_var_id, &has_var_id);
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
            cg_note_var_id(&phi->value, &max_var_id, &has_var_id);
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            cg_note_var_id(v, &max_var_id, &has_var_id);
            if (!cg_value_is_closure_alloc(v))
                continue;
            const XiFunc *child = (const XiFunc *) v->aux;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                const XiCapture *cap = &child->captures[ci];
                cg_note_var_id(cap->value, &max_var_id, &has_var_id);
                if (ci < v->nargs)
                    cg_note_var_id(v->args[ci], &max_var_id, &has_var_id);
            }
        }
    }

    return has_var_id ? max_var_id + 1u : 0u;
}

static bool cg_reset_cell_var_tables(XiCgenCtx *ctx, const XiFunc *f) {
    uint32_t need = cg_cell_var_count_for_func(f);
    if (need == 0) {
        ctx->cell_var_count = 0;
        return true;
    }
    if (need > ctx->cell_var_count) {
        bool *new_vars = (bool *) xr_realloc(ctx->cell_vars, (size_t) need * sizeof(*new_vars));
        bool *new_release_vars =
            (bool *) xr_realloc(ctx->cell_release_vars, (size_t) need * sizeof(*new_release_vars));
        bool *new_heap_capture_vars = (bool *) xr_realloc(
            ctx->cell_heap_capture_vars, (size_t) need * sizeof(*new_heap_capture_vars));
        const XiValue **new_origins =
            (const XiValue **) xr_realloc(ctx->cell_origins, (size_t) need * sizeof(*new_origins));
        if (!new_vars || !new_release_vars || !new_heap_capture_vars || !new_origins) {
            ctx->cell_vars = new_vars ? new_vars : ctx->cell_vars;
            ctx->cell_release_vars = new_release_vars ? new_release_vars : ctx->cell_release_vars;
            ctx->cell_heap_capture_vars =
                new_heap_capture_vars ? new_heap_capture_vars : ctx->cell_heap_capture_vars;
            ctx->cell_origins = new_origins ? new_origins : ctx->cell_origins;
            ctx->cell_var_count = 0;
            ctx->error = true;
            return false;
        }
        ctx->cell_vars = new_vars;
        ctx->cell_release_vars = new_release_vars;
        ctx->cell_heap_capture_vars = new_heap_capture_vars;
        ctx->cell_origins = new_origins;
        ctx->cell_var_count = need;
    }
    memset(ctx->cell_vars, 0, (size_t) ctx->cell_var_count * sizeof(*ctx->cell_vars));
    memset(ctx->cell_release_vars, 0,
           (size_t) ctx->cell_var_count * sizeof(*ctx->cell_release_vars));
    memset(ctx->cell_heap_capture_vars, 0,
           (size_t) ctx->cell_var_count * sizeof(*ctx->cell_heap_capture_vars));
    memset(ctx->cell_origins, 0, (size_t) ctx->cell_var_count * sizeof(*ctx->cell_origins));
    return true;
}

static void cg_prepare_cell_vars(XiCgenCtx *ctx, const XiFunc *f) {
    if (!cg_reset_cell_var_tables(ctx, f))
        return;
    if (!f)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!cg_value_is_closure_alloc(v))
                continue;
            const XiFunc *child = (const XiFunc *) v->aux;
            if (!child)
                continue;
            bool stack_closure = v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW;
            for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
                const XiCapture *cap = &child->captures[ci];
                if (!cap->needs_cell || cap->source != XI_CAPTURE_SRC_REG)
                    continue;
                const XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
                if (!cap_val || !xi_var_id_is_valid(cap_val->var_id) ||
                    cap_val->var_id >= ctx->cell_var_count)
                    continue;
                XiVarId var_id = cap_val->var_id;
                ctx->cell_vars[var_id] = true;
                if (stack_closure) {
                    if (!ctx->cell_heap_capture_vars[var_id])
                        ctx->cell_release_vars[var_id] = true;
                } else {
                    ctx->cell_heap_capture_vars[var_id] = true;
                    ctx->cell_release_vars[var_id] = false;
                }
                if (!ctx->cell_origins[var_id])
                    ctx->cell_origins[var_id] = cap_val;
            }
        }
    }
}

static void emit_c_string_literal(FILE *out, const char *s) {
    fputc('"', out);
    if (s) {
        for (const char *p = s; *p; p++) {
            if (*p == '"')
                fprintf(out, "\\\"");
            else if (*p == '\\')
                fprintf(out, "\\\\");
            else if (*p == '\n')
                fprintf(out, "\\n");
            else if (*p == '\t')
                fprintf(out, "\\t");
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void emit_enum_member_value_expr(XiCgenCtx *ctx, FILE *out, const XiEnumMemberData *member) {
    if (!member) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    switch (member->value_kind) {
        case XI_ENUM_LITERAL_INT:
            fprintf(out, "XR_FROM_INT(%" PRId64 ")", member->int_value);
            break;
        case XI_ENUM_LITERAL_FLOAT:
            fprintf(out, "XR_FROM_FLOAT(");
            emit_c_float_literal(out, member->float_value);
            fprintf(out, ")");
            break;
        case XI_ENUM_LITERAL_BOOL:
            fprintf(out, "XR_FROM_BOOL(%d)", member->bool_value ? 1 : 0);
            break;
        case XI_ENUM_LITERAL_STRING:
            cg_emit_str_value(ctx, out, member->string_value);
            break;
        case XI_ENUM_LITERAL_NULL:
        default:
            fprintf(out, "XR_NULL_VAL");
            break;
    }
}

static void emit_enum_type_expr(XiCgenCtx *ctx, FILE *out, const XiEnumData *ed) {
    if (!ed) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    fprintf(out, "({ XrValue _e = xrt_map_new(%u); ", (unsigned) ed->member_count);
    for (uint32_t i = 0; i < ed->member_count; i++) {
        const XiEnumMemberData *member = &ed->members[i];
        const char *name = member->name ? member->name : "";
        fprintf(out, "xrt_map_set((xrt_map_t*)_e.ptr, ");
        cg_emit_str_value(ctx, out, name);
        fprintf(out, ", ");
        if (ed->is_adt)
            fprintf(out, "XR_FROM_INT(%u)", (unsigned) i);
        else {
            fprintf(out, "xrt_enum_value_new(");
            emit_c_string_literal(out, ed->name ? ed->name : "");
            fprintf(out, ", ");
            emit_c_string_literal(out, name);
            fprintf(out, ", ");
            emit_enum_member_value_expr(ctx, out, member);
            fprintf(out, ", %u)", (unsigned) i);
        }
        fprintf(out, "); ");
    }
    fprintf(out, "_e; })");
}

typedef struct CgPreludeEnumMember {
    const char *name;
    bool has_payload;
} CgPreludeEnumMember;

typedef struct CgPreludeEnumData {
    int builtin_index;
    const char *enum_name;
    const CgPreludeEnumMember *members;
    uint32_t member_count;
} CgPreludeEnumData;

static const CgPreludeEnumData *cg_prelude_enum_data(int builtin_index) {
    static const CgPreludeEnumMember ordering[] = {
        {"Relaxed", false},        {"Acquire", false}, {"Release", false},
        {"AcquireRelease", false}, {"SeqCst", false},
    };
    static const CgPreludeEnumMember recv[] = {
        {"Value", true},
        {"Empty", false},
        {"Timeout", false},
        {"Closed", false},
    };
    static const CgPreludeEnumMember send_result[] = {
        {"Sent", false},
        {"Full", false},
        {"Timeout", false},
        {"Closed", false},
    };
    static const CgPreludeEnumMember task_result[] = {
        {"Success", true},  {"Failed", true},   {"Cancelled", false},
        {"Timeout", false}, {"Pending", false},
    };
    static const CgPreludeEnumMember task_outcome[] = {
        {"Success", true},
        {"Failed", true},
        {"Cancelled", false},
    };
    static const CgPreludeEnumMember task_status[] = {
        {"Pending", false}, {"Running", false},   {"Success", false},
        {"Failed", false},  {"Cancelled", false},
    };
    static const CgPreludeEnumData enums[] = {
        {XR_GLOBAL_VAR_ORDERING, "Ordering", ordering, 5},
        {XR_GLOBAL_VAR_RECV, "Recv", recv, 4},
        {XR_GLOBAL_VAR_SEND_RESULT, "SendResult", send_result, 4},
        {XR_GLOBAL_VAR_TASK_RESULT, "TaskResult", task_result, 5},
        {XR_GLOBAL_VAR_TASK_OUTCOME, "TaskOutcome", task_outcome, 3},
        {XR_GLOBAL_VAR_TASK_STATUS, "TaskStatus", task_status, 5},
    };
    for (uint32_t i = 0; i < (uint32_t) (sizeof(enums) / sizeof(enums[0])); i++) {
        if (enums[i].builtin_index == builtin_index)
            return &enums[i];
    }
    return NULL;
}

static void emit_prelude_enum_member_value_expr(FILE *out, const CgPreludeEnumData *ed,
                                                uint32_t member_index) {
    const CgPreludeEnumMember *member =
        ed && member_index < ed->member_count ? &ed->members[member_index] : NULL;
    if (!ed || !member) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    if (member->has_payload) {
        fprintf(out, "XR_FROM_INT(%u)", (unsigned) member_index);
        return;
    }
    fprintf(out,
            "({ static const XrAotEnumValueView _ev_%s_%s = {{0, 0}, NULL, \"%s\", "
            "\"%s\", {.tag = XR_TAG_I64, .i = %u}, %u}; XrValue _v = {0}; "
            "_v.tag = XR_TAG_ENUM; _v.ext = %u; "
            "_v.ptr = (void *)&_ev_%s_%s; _v; })",
            ed->enum_name, member->name, ed->enum_name, member->name, (unsigned) member_index,
            (unsigned) member_index, (unsigned) member_index, ed->enum_name, member->name);
}

static bool emit_prelude_enum_type_expr(FILE *out, int builtin_index) {
    const CgPreludeEnumData *ed = cg_prelude_enum_data(builtin_index);
    if (!ed)
        return false;
    fprintf(out, "({ XrValue _e = xrt_map_new(%u); ", (unsigned) ed->member_count);
    for (uint32_t i = 0; i < ed->member_count; i++) {
        fprintf(out, "xrt_map_set((xrt_map_t*)_e.ptr, xr_box_str(\"%s\"), ", ed->members[i].name);
        emit_prelude_enum_member_value_expr(out, ed, i);
        fprintf(out, "); ");
    }
    fprintf(out, "_e; })");
    return true;
}

static int cg_enum_member_index(const XiEnumData *ed, const char *member_name) {
    if (!ed || !member_name)
        return -1;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members[i].name && strcmp(ed->members[i].name, member_name) == 0)
            return (int) i;
    }
    return -1;
}

static const XiEnumData *cg_enum_for_shared_value(const XiCgenCtx *ctx, const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    if (!ctx || !v || v->op != XI_GET_SHARED)
        return NULL;
    int slot = (int) v->aux_int;
    if (slot < 0 || slot >= ctx->shared_cap)
        return NULL;
    return ctx->shared_enum[slot];
}

static const XiEnumData *cg_enum_for_runtime_type(const XiCgenCtx *ctx, const void *runtime_type) {
    if (!ctx || !runtime_type)
        return NULL;
    for (int i = 0; i < ctx->nshared; i++) {
        const XiEnumData *ed = ctx->shared_enum[i];
        if (ed && ed->runtime_type == runtime_type)
            return ed;
    }
    return NULL;
}

static void emit_adt_enum_construct_expr(FILE *out, int member_idx, const XiValue *v) {
    uint16_t payload_count = v->nargs > 0 ? (uint16_t) (v->nargs - 1) : 0;
    fprintf(out, "({ XrValue _adt = xrt_array_new(%u); ", (unsigned) payload_count + 1);
    fprintf(out, "xrt_array_push(_adt, XR_FROM_INT(%d)); ", member_idx);
    for (uint16_t a = 1; a < v->nargs; a++) {
        fprintf(out, "xrt_array_push(_adt, ");
        emit_value_as_rep(out, v->args[a], XR_REP_TAGGED);
        fprintf(out, "); ");
    }
    fprintf(out, "_adt; })");
}

static void emit_call_hidden_closure(FILE *out, const XiFunc *current, const XiFunc *target,
                                     const XiValue *callee) {
    if (!target || target->ncaptures == 0) {
        fprintf(out, "NULL");
        return;
    }
    if (current == target && callee && callee->op == XI_CONST && callee->type &&
        callee->type->kind == XR_KIND_NULL) {
        fprintf(out, "_cl");
        return;
    }
    fprintf(out, "(xrt_closure_t*)");
    emit_vref(out, callee);
    fprintf(out, ".ptr");
}

static void emit_str_concat_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->nargs == 0) {
        cg_emit_str_value(ctx, out, "");
        return;
    }
    if (v->nargs == 1 && v->args[0] && v->args[0]->type &&
        v->args[0]->type->kind == XR_KIND_STRING) {
        emit_vref(out, v->args[0]);
        return;
    }
    if (v->nargs == 1) {
        if (cg_value_type_is_unsigned_int(v->args[0])) {
            fprintf(out, "xrt_uint64_to_string((uint64_t)");
            emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
            fprintf(out, ")");
        } else {
            fprintf(out, "xrt_to_string(");
            emit_value_as_rep(out, v->args[0], XR_REP_TAGGED);
            fprintf(out, ")");
        }
        return;
    }

    fprintf(out, "({ xrt_strpart_t _scp_%u[%u]; ", v->id, (unsigned) v->nargs);
    for (uint16_t i = 0; i < v->nargs; i++) {
        if (cg_value_type_is_unsigned_int(v->args[i])) {
            fprintf(out, "xrt_strpart_init_u64(&_scp_%u[%u], (uint64_t)", v->id, (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, v->args[i], XR_REP_I64);
            fprintf(out, "); ");
        } else {
            fprintf(out, "xrt_strpart_init(&_scp_%u[%u], ", v->id, (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, v->args[i], XR_REP_TAGGED);
            fprintf(out, "); ");
        }
    }
    fprintf(out, "xrt_str_concat_parts(%u, _scp_%u); })", (unsigned) v->nargs, v->id);
}

static bool cg_aot_coro_closure_has_only_supported_uses(XiCgenCtx *ctx, const XiFunc *current,
                                                        const XiValue *value, const XiFunc *child,
                                                        int depth) {
    if (!ctx || !current || !value || !child || depth > 8)
        return false;

    bool saw_use = false;
    for (uint32_t bi = 0; bi < current->nblocks; bi++) {
        const XiBlock *blk = current->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *user = blk->values[vi];
            if (!user)
                continue;
            for (uint16_t ai = 0; ai < user->nargs; ai++) {
                if (user->args[ai] != value)
                    continue;
                saw_use = true;
                switch ((XiOp) user->op) {
                    case XI_RETAIN:
                    case XI_RELEASE:
                    case XI_SET_SHARED:
                        break;
                    case XI_BOX:
                    case XI_COPY:
                    case XI_MOVE:
                        if (!cg_aot_coro_closure_has_only_supported_uses(ctx, current, user, child,
                                                                         depth + 1))
                            return false;
                        break;
                    case XI_GO:
                        if (ai != 0)
                            return false;
                        break;
                    case XI_CALL: {
                        if (ai != 0 || !cg_func_needs_aot_coro_ctx(ctx, current))
                            return false;
                        CgStaticFunctionCall call =
                            cg_resolve_static_function_call(ctx, current, user->args[0]);
                        if (call.func != child)
                            return false;
                        break;
                    }
                    default:
                        return false;
                }
            }
        }
    }
    return saw_use;
}

static void emit_closure_new_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                  const char *prefix, const XiValue *v) {
    if (v->aux) {
        XiFunc *child = (XiFunc *) v->aux;
        /* FFI: an @extern function has no Xray closure entry (its body is the
         * foreign C symbol). Calls to it resolve statically to a direct C call,
         * so the closure value itself is never invoked — emit a NULL so the
         * shared-slot store is well-formed without referencing a missing
         * boxed entry. */
        if (child->is_extern) {
            fprintf(out, "XR_NULL_VAL");
            return;
        }
        if (cg_func_needs_aot_coro_ctx(ctx, child) &&
            !cg_aot_coro_closure_has_only_supported_uses(ctx, current, v, child, 0)) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: unsupported AOT sync call to suspendable function '%s'\n",
                    child->name ? child->name : "?");
            fprintf(out, "XR_NULL_VAL /* unsupported suspendable closure */");
            return;
        }
        uint16_t ncap = child->ncaptures;
        bool stack_closure = v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW;
        const char *alloc_fn = stack_closure ? "xrt_closure_stack_new" : "xrt_closure_new";
        fprintf(out, "({ xrt_closure_t *_c = (xrt_closure_t*)%s((void*)", alloc_fn);
        if (cg_func_uses_typed_abi(ctx, child) && !cg_func_needs_aot_coro_ctx(ctx, child))
            emit_typed_abi_fname(ctx, out, prefix, child);
        else
            emit_fname(ctx, out, prefix, child);
        fprintf(out, ", %u).ptr; ", ncap);
        for (uint16_t ci = 0; ci < ncap; ci++) {
            XiCapture *cap = &child->captures[ci];
            if (cap->needs_cell && cap->source == XI_CAPTURE_SRC_REG) {
                const XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
                fprintf(out, "_c->upvals[%u] = ", ci);
                if (cap_val && cg_value_has_cell(ctx, cap_val))
                    emit_cell_ref(out, cap_val->var_id);
                else if (cap_val)
                    emit_vref(out, cap_val);
                else
                    fprintf(out, "XR_NULL_VAL");
                fprintf(out, "; ");
            } else if (ci < v->nargs && v->args[ci]) {
                /* Upvals are stored as tagged XrValues; convert the capture from
                 * its declared storage rep to TAGGED (e.g. box an all-scalar
                 * native class instance via xrt_box_obj) so the assignment is
                 * well-typed and the load can unbox it back. */
                fprintf(out, "_c->upvals[%u] = ", ci);
                emit_value_as_rep_ctx(ctx, out, v->args[ci], XR_REP_TAGGED);
                fprintf(out, "; ");
            } else if (cap->source == XI_CAPTURE_SRC_UPVAL) {
                fprintf(out, "_c->upvals[%u] = _cl ? _cl->upvals[%u] : XR_NULL_VAL; ", ci,
                        (unsigned) cap->index);
            } else {
                ctx->error = true;
                fprintf(stderr, "[xi_cgen] ERROR: missing AOT closure capture '%s'\n",
                        cap->name ? cap->name : "?");
                fprintf(out, "_c->upvals[%u] = XR_NULL_VAL; ", ci);
            }
            if (!stack_closure)
                fprintf(out, "xrt_retain(_c->upvals[%u]); ", ci);
        }
        fprintf(out, "xr_mkptr(_c, XR_TAG_CLOSURE); })");
    } else {
        fprintf(out, "XR_NULL_VAL /* closure: unknown */");
    }
}
