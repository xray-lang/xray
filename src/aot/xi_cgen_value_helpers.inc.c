/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_value_helpers.inc.c - AOT scalar value emission helpers
 */

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
    switch (v->type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static void emit_boxed_value_ref(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (v && v->type && v->type->kind == XR_KIND_NULL) {
        fprintf(out, "XR_NULL_VAL");
        return;
    }

    XrRep rep = cg_value_plan_storage_rep(ctx, v);
    if (rep == XR_REP_TAGGED) {
        emit_vref(out, v);
    } else if (rep == XR_REP_PTR || rep == XR_REP_RAWPTR) {
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

/* Read a non-cell upvalue stored as a tagged XrValue and unbox it to the
 * declared storage rep of the consuming local.
 * Callers pass the storage rep (cg_value_decl_storage_rep), not cg_rep: a native
 * class instance has cg_rep == TAGGED but a PTR storage local, so without this a
 * captured instance is read as a raw XrValue into a `void *` slot — a C type
 * error. */
static void emit_upval_get_for_rep(FILE *out, XrRep rep, const char *up_expr) {
    if (rep == XR_REP_TAGGED) {
        fprintf(out, "%s", up_expr);
    } else if (rep == XR_REP_PTR) {
        fprintf(out, "%s.ptr", up_expr);
    } else if (rep == XR_REP_RAWPTR) {
        fprintf(out, "(void *)(uintptr_t)XR_TO_INT(%s)", up_expr);
    } else if (rep == XR_REP_F64) {
        fprintf(out, "%s.f", up_expr);
    } else {
        fprintf(out, "%s.i", up_expr);
    }
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
    (void) ctx;
    if (!v || cg_value_has_structural_use(f, v))
        return false;
    if (v->flags &
        (XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND))
        return false;
    switch ((XiOp) v->op) {
        case XI_CONST:
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
            return true;
        default:
            return false;
    }
}

static void emit_c_string_literal_bytes(FILE *out, const char *s, size_t len) {
    fputc('"', out);
    if (s) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char) s[i];
            if (c == '"')
                fprintf(out, "\\\"");
            else if (c == '\\')
                fprintf(out, "\\\\");
            else if (c == '\n')
                fprintf(out, "\\n");
            else if (c == '\r')
                fprintf(out, "\\r");
            else if (c == '\t')
                fprintf(out, "\\t");
            else if (c >= 0x20 && c <= 0x7e)
                fputc((int) c, out);
            else
                fprintf(out, "\\%03o", (unsigned) c);
        }
    }
    fputc('"', out);
}

static void emit_c_string_literal(FILE *out, const char *s) {
    emit_c_string_literal_bytes(out, s, s ? strlen(s) : 0);
}

/* ========== Static no-payload enum member boxes ==========
 * A no-payload member value is a compile-time constant, so it never justifies
 * a heap object: body emission interns each referenced member here and the
 * static-data phase writes one immortal file-scope XrAotEnumBox per symbol,
 * mirroring the scalar-layout sidecar. Payload-carrying constructions are the
 * only enum boxes that still allocate. */

static const char *cg_intern_enum_member_box(XiCgenCtx *ctx, const char *symbol,
                                             const char *enum_name, const char *member_name,
                                             uint32_t member_index, uint32_t layout_id) {
    if (!ctx || !symbol)
        return NULL;
    for (int i = 0; i < ctx->nenum_member_boxes; i++) {
        if (strcmp(ctx->enum_member_boxes[i].symbol, symbol) == 0)
            return ctx->enum_member_boxes[i].symbol;
    }
    if (ctx->nenum_member_boxes == ctx->enum_member_box_cap) {
        int cap = ctx->enum_member_box_cap ? ctx->enum_member_box_cap * 2 : 16;
        CgEnumMemberBox *grown = (CgEnumMemberBox *) xr_realloc(
            ctx->enum_member_boxes, (size_t) cap * sizeof(CgEnumMemberBox));
        if (!grown) {
            ctx->error = true;
            return NULL;
        }
        ctx->enum_member_boxes = grown;
        ctx->enum_member_box_cap = cap;
    }
    CgEnumMemberBox *e = &ctx->enum_member_boxes[ctx->nenum_member_boxes];
    e->symbol = xr_strdup(symbol);
    e->enum_name = xr_strdup(enum_name ? enum_name : "");
    e->member_name = xr_strdup(member_name ? member_name : "");
    if (!e->symbol || !e->enum_name || !e->member_name) {
        xr_free(e->symbol);
        xr_free(e->enum_name);
        xr_free(e->member_name);
        ctx->error = true;
        return NULL;
    }
    e->member_index = member_index;
    e->layout_id = layout_id;
    ctx->nenum_member_boxes++;
    return e->symbol;
}

static char *cg_enum_member_box_symbol(XiCgenCtx *ctx, const char *family, uint32_t domain_id,
                                       uint32_t member_index) {
    if (!ctx || !family) {
        if (ctx)
            ctx->error = true;
        return NULL;
    }
    const char *module_prefix =
        ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
    size_t cap = strlen(module_prefix) + strlen(family) + 64;
    char *symbol = (char *) xr_malloc(cap);
    if (!symbol) {
        ctx->error = true;
        return NULL;
    }
    int written = snprintf(symbol, cap, "_xenum_box_%s_%s_%u_%u", module_prefix, family,
                           (unsigned) domain_id, (unsigned) member_index);
    if (written < 0 || (size_t) written >= cap) {
        xr_free(symbol);
        ctx->error = true;
        return NULL;
    }
    return symbol;
}

static void emit_enum_member_box_value_expr(XiCgenCtx *ctx, FILE *out, const char *symbol_family,
                                            uint32_t symbol_domain_id, const char *enum_name,
                                            const char *member_name, uint32_t member_index,
                                            uint32_t layout_id) {
    char *symbol = cg_enum_member_box_symbol(ctx, symbol_family, symbol_domain_id, member_index);
    if (!symbol ||
        !cg_intern_enum_member_box(ctx, symbol, enum_name, member_name, member_index, layout_id)) {
        xr_free(symbol);
        fprintf(out, "XR_NULL_VAL");
        return;
    }
    fprintf(out, "xrt_enum_box_from_static(&%s)", symbol);
    xr_free(symbol);
}

static void emit_enum_member_box_defs(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !out || ctx->nenum_member_boxes == 0)
        return;
    for (int i = 0; i < ctx->nenum_member_boxes; i++) {
        const CgEnumMemberBox *e = &ctx->enum_member_boxes[i];
        fprintf(out,
                "static const XrAotEnumBox %s = "
                "{{XR_TENUM_BOX, XR_OBJ_IMMORTAL, XR_RC_STICKY, 0, 0}, NULL, ",
                e->symbol);
        emit_c_string_literal(out, e->enum_name);
        fprintf(out, ", ");
        emit_c_string_literal(out, e->member_name);
        fprintf(out, ", %uu, 0u, %uu};\n", (unsigned) e->member_index, (unsigned) e->layout_id);
    }
    fprintf(out, "\n");
}

static void cg_reset_enum_member_boxes(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->nenum_member_boxes; i++) {
        xr_free(ctx->enum_member_boxes[i].symbol);
        xr_free(ctx->enum_member_boxes[i].enum_name);
        xr_free(ctx->enum_member_boxes[i].member_name);
    }
    xr_free(ctx->enum_member_boxes);
    ctx->enum_member_boxes = NULL;
    ctx->nenum_member_boxes = 0;
    ctx->enum_member_box_cap = 0;
}

static const XiEnumData *cg_enum_for_runtime_type(const XiCgenCtx *ctx, const void *runtime_type);

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
            emit_enum_member_box_value_expr(ctx, out, "unit", ed->layout_id,
                                            ed->name ? ed->name : "", name, i, ed->layout_id);
        }
        fprintf(out, "); ");
    }
    fprintf(out, "_e; })");
}

static bool emit_enum_namespace_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *v, bool storage_predeclared) {
    if (!ctx || !out || !v || v->op != XI_CONST || !v->type)
        return false;

    const XiEnumData *ed = NULL;
    if (v->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE && v->aux)
        ed = (const XiEnumData *) v->aux;
    else
        return false;

    if (ctx->freestanding_profile)
        return false;
    if (!storage_predeclared && !ctx->pre_decl_all) {
        fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
        emit_vref(out, v);
        fprintf(out, ";\n");
    }
    fprintf(out, "    ");
    emit_vref(out, v);
    fprintf(out, " = xrt_map_new(%u);\n", (unsigned) (ed ? ed->member_count : 0));
    for (uint32_t i = 0; ed && i < ed->member_count; i++) {
        const XiEnumMemberData *member = &ed->members[i];
        const char *name = member->name ? member->name : "";
        fprintf(out, "    xrt_map_set((xrt_map_t*)");
        emit_vref(out, v);
        fprintf(out, ".ptr, ");
        cg_emit_str_value(ctx, out, name);
        fprintf(out, ", ");
        if (ed->is_adt) {
            fprintf(out, "XR_FROM_INT(%u)", (unsigned) i);
        } else {
            emit_enum_member_box_value_expr(ctx, out, "unit", ed->layout_id,
                                            ed->name ? ed->name : "", name, i, ed->layout_id);
        }
        fprintf(out, ");\n");
    }
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
    return true;
}

typedef struct CgPreludeEnumMember {
    const char *name; /* owned: static prelude enum member literal (compiler-owned descriptor) */
    bool has_payload;
} CgPreludeEnumMember;

typedef struct CgPreludeEnumData {
    int builtin_index;
    const char *enum_name; /* owned: static prelude enum name literal */
    const CgPreludeEnumMember *members;
    uint32_t member_count;
} CgPreludeEnumData;

static const int cg_prelude_enum_builtin_indices[] = {
    XR_GLOBAL_VAR_ORDERING,
    XR_GLOBAL_VAR_ENDIAN,
    XR_GLOBAL_VAR_RECV,
    XR_GLOBAL_VAR_SEND_RESULT,
    XR_GLOBAL_VAR_TASK_RESULT,
    XR_GLOBAL_VAR_TASK_STATUS,
    XR_GLOBAL_VAR_UTF8_ERROR,
    XR_GLOBAL_VAR_STRING_SLICE_ERROR,
    XR_GLOBAL_VAR_COMPRESSION_ERROR,
    XR_GLOBAL_VAR_CRYPTO_ERROR,
};

static const CgPreludeEnumData *cg_prelude_enum_data(int builtin_index) {
    static const CgPreludeEnumMember ordering[] = {
        {"Relaxed", false},        {"Acquire", false}, {"Release", false},
        {"AcquireRelease", false}, {"SeqCst", false},
    };
    static const CgPreludeEnumMember endian[] = {
        {"Native", false},
        {"LE", false},
        {"BE", false},
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
    static const CgPreludeEnumMember task_status[] = {
        {"Pending", false}, {"Running", false},   {"Success", false},
        {"Failed", false},  {"Cancelled", false},
    };
    static const CgPreludeEnumMember utf8_error[] = {
        {"InvalidUtf8", false},
    };
    static const CgPreludeEnumMember string_slice_error[] = {
        {"InvalidByteRange", false},
    };
    static const CgPreludeEnumMember compression_error[] = {
        {"InvalidData", false},
    };
    static const CgPreludeEnumMember crypto_error[] = {
        {"InvalidLength", false},
    };
    static const CgPreludeEnumData enums[] = {
        {XR_GLOBAL_VAR_ORDERING, "Ordering", ordering, 5},
        {XR_GLOBAL_VAR_ENDIAN, "Endian", endian, 3},
        {XR_GLOBAL_VAR_RECV, "Recv", recv, 4},
        {XR_GLOBAL_VAR_SEND_RESULT, "SendResult", send_result, 4},
        {XR_GLOBAL_VAR_TASK_RESULT, "TaskResult", task_result, 5},
        {XR_GLOBAL_VAR_TASK_STATUS, "TaskStatus", task_status, 5},
        {XR_GLOBAL_VAR_UTF8_ERROR, "Utf8Error", utf8_error, 1},
        {XR_GLOBAL_VAR_STRING_SLICE_ERROR, "StringSliceError", string_slice_error, 1},
        {XR_GLOBAL_VAR_COMPRESSION_ERROR, "CompressionError", compression_error, 1},
        {XR_GLOBAL_VAR_CRYPTO_ERROR, "CryptoError", crypto_error, 1},
    };
    for (uint32_t i = 0; i < (uint32_t) (sizeof(enums) / sizeof(enums[0])); i++) {
        if (enums[i].builtin_index == builtin_index)
            return &enums[i];
    }
    return NULL;
}

static int cg_prelude_enum_member_index(const CgPreludeEnumData *ed, const char *name) {
    if (!ed || !name)
        return -1;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members[i].name && strcmp(ed->members[i].name, name) == 0)
            return (int) i;
    }
    return -1;
}

static bool cg_prelude_enum_has_payload_member(const CgPreludeEnumData *ed) {
    if (!ed)
        return false;
    for (uint32_t i = 0; i < ed->member_count; i++) {
        if (ed->members[i].has_payload)
            return true;
    }
    return false;
}

static int cg_prelude_enum_scalar_sidecar_slot(const CgPreludeEnumData *ed) {
    if (!ed)
        return -1;
    for (uint32_t i = 0; i < (uint32_t) (sizeof(cg_prelude_enum_builtin_indices) /
                                         sizeof(cg_prelude_enum_builtin_indices[0]));
         i++) {
        if (cg_prelude_enum_builtin_indices[i] == ed->builtin_index)
            return (int) i;
    }
    return -1;
}

static bool cg_mark_prelude_enum_scalar_sidecar(XiCgenCtx *ctx, const CgPreludeEnumData *ed,
                                                uint32_t *out_slot) {
    int slot = cg_prelude_enum_scalar_sidecar_slot(ed);
    if (!ctx || slot < 0 || slot >= 32 || cg_prelude_enum_has_payload_member(ed)) {
        if (ctx)
            ctx->error = true;
        return false;
    }
    ctx->prelude_enum_scalar_sidecar_mask |= UINT32_C(1) << (uint32_t) slot;
    if (out_slot)
        *out_slot = (uint32_t) slot;
    return true;
}

static void cg_reset_prelude_enum_scalar_sidecars(XiCgenCtx *ctx) {
    if (ctx)
        ctx->prelude_enum_scalar_sidecar_mask = 0;
}

static void emit_prelude_enum_scalar_sidecar_defs(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !out || ctx->prelude_enum_scalar_sidecar_mask == 0)
        return;
    const char *module_prefix =
        ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
    for (uint32_t slot = 0; slot < (uint32_t) (sizeof(cg_prelude_enum_builtin_indices) /
                                               sizeof(cg_prelude_enum_builtin_indices[0]));
         slot++) {
        if ((ctx->prelude_enum_scalar_sidecar_mask & (UINT32_C(1) << slot)) == 0)
            continue;
        const CgPreludeEnumData *ed = cg_prelude_enum_data(cg_prelude_enum_builtin_indices[slot]);
        if (!ed || cg_prelude_enum_has_payload_member(ed)) {
            ctx->error = true;
            return;
        }
        fprintf(out, "static const char *const _xprelude_enum_scalar_names_%s_%u[%u] = {",
                module_prefix, (unsigned) slot, (unsigned) ed->member_count);
        for (uint32_t i = 0; i < ed->member_count; i++) {
            if (i > 0)
                fprintf(out, ",");
            emit_c_string_literal(out, ed->members[i].name ? ed->members[i].name : "");
        }
        fprintf(out, "};\n");
        fprintf(out,
                "static const XrAotEnumScalarLayout "
                "_xprelude_enum_scalar_layout_%s_%u = "
                "{{XR_TENUM_SCALAR_LAYOUT, XR_OBJ_IMMORTAL, XR_RC_STICKY, 0, 0}, ",
                module_prefix, (unsigned) slot);
        emit_c_string_literal(out, ed->enum_name ? ed->enum_name : "");
        fprintf(out, ", _xprelude_enum_scalar_names_%s_%u, %u, 0};\n\n", module_prefix,
                (unsigned) slot, (unsigned) ed->member_count);
    }
}

static const XaotEnumPlan *cg_unique_unit_enum_scalar_plan_named(XiCgenCtx *ctx,
                                                                 const char *enum_name) {
    if (!ctx || ctx->freestanding_profile || !ctx->aot_bundle || !enum_name || !enum_name[0])
        return NULL;
    const XaotEnumPlan *match = NULL;
    for (uint32_t i = 0; i < ctx->aot_bundle->nenum_plans; i++) {
        const XaotEnumPlan *candidate = &ctx->aot_bundle->enum_plans[i];
        if (!candidate->enum_data || !candidate->enum_data->name || !candidate->members ||
            candidate->member_count == 0 || candidate->max_payload != 0 ||
            strcmp(candidate->enum_data->name, enum_name) != 0)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool emit_portable_scalar_enum_member_value_expr(XiCgenCtx *ctx, FILE *out,
                                                        const XiValue *value, const char *enum_name,
                                                        const CgPreludeEnumData *prelude_enum,
                                                        uint32_t member_index) {
    const XaotEnumPlan *plan = cg_unit_enum_scalar_plan(ctx, value ? value->type : NULL);
    if (!plan)
        plan = cg_unique_unit_enum_scalar_plan_named(ctx, enum_name);
    if (!plan && prelude_enum) {
        uint32_t slot = 0;
        if (!cg_mark_prelude_enum_scalar_sidecar(ctx, prelude_enum, &slot))
            return false;
        const char *module_prefix = ctx && ctx->module && ctx->module->name && ctx->module->name[0]
                                        ? ctx->module->name
                                        : "mod";
        fprintf(out, "xrt_enum_scalar_box(&_xprelude_enum_scalar_layout_%s_%u, INT64_C(%u))",
                module_prefix, (unsigned) slot, (unsigned) member_index);
        return true;
    }
    uint32_t sidecar_index = 0;
    if (!plan || !cg_mark_enum_scalar_sidecar(ctx, plan, &sidecar_index))
        return false;
    const char *module_prefix =
        ctx && ctx->module && ctx->module->name && ctx->module->name[0] ? ctx->module->name : "mod";
    fprintf(out, "xrt_enum_scalar_box(&_xenum_scalar_layout_%s_%u, INT64_C(%u))", module_prefix,
            (unsigned) sidecar_index, (unsigned) member_index);
    return true;
}

static void emit_prelude_enum_member_value_expr(XiCgenCtx *ctx, FILE *out,
                                                const CgPreludeEnumData *ed,
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
    emit_enum_member_box_value_expr(ctx, out, "prelude", (uint32_t) ed->builtin_index,
                                    ed->enum_name ? ed->enum_name : "",
                                    member->name ? member->name : "", member_index, 0);
}

static bool emit_prelude_enum_type_expr(XiCgenCtx *ctx, FILE *out, int builtin_index) {
    const CgPreludeEnumData *ed = cg_prelude_enum_data(builtin_index);
    if (!ed)
        return false;
    fprintf(out, "({ XrValue _e = xrt_map_new(%u); ", (unsigned) ed->member_count);
    for (uint32_t i = 0; i < ed->member_count; i++) {
        fprintf(out, "xrt_map_set((xrt_map_t*)_e.ptr, xr_box_str(\"%s\"), ", ed->members[i].name);
        emit_prelude_enum_member_value_expr(ctx, out, ed, i);
        fprintf(out, "); ");
    }
    fprintf(out, "_e; })");
    return true;
}

static bool emit_static_prelude_enum_member_value_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                       int builtin_index, const char *member_name) {
    const CgPreludeEnumData *ed = cg_prelude_enum_data(builtin_index);
    int member_index = cg_prelude_enum_member_index(ed, member_name);
    if (!ed || member_index < 0 || (uint32_t) member_index >= ed->member_count)
        return false;
    if (ed->members[member_index].has_payload || cg_prelude_enum_has_payload_member(ed))
        return false;
    XrRep source_rep =
        ctx && cg_value_plan_storage_rep(ctx, v) == XR_REP_I64 ? XR_REP_I64 : XR_REP_TAGGED;
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, source_rep,
                                                     cg_value_plan_storage_rep(ctx, v));
    if (source_rep == XR_REP_I64)
        fprintf(out, "INT64_C(%d)", member_index);
    else if (!emit_portable_scalar_enum_member_value_expr(ctx, out, v, ed->enum_name, ed,
                                                          (uint32_t) member_index))
        emit_prelude_enum_member_value_expr(ctx, out, ed, (uint32_t) member_index);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_static_enum_member_value_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                               const XiEnumData *ed, uint32_t member_index) {
    if (!out || !ed || !ed->members || member_index >= ed->member_count)
        return false;
    const XiEnumMemberData *member = &ed->members[member_index];
    if (member->payload_count != 0)
        return false;
    XrRep source_rep =
        ctx && cg_value_plan_storage_rep(ctx, v) == XR_REP_I64 ? XR_REP_I64 : XR_REP_TAGGED;
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, source_rep,
                                                     cg_value_plan_storage_rep(ctx, v));
    if (source_rep == XR_REP_I64) {
        fprintf(out, "INT64_C(%u)", (unsigned) member_index);
    } else if (!emit_portable_scalar_enum_member_value_expr(ctx, out, v, ed->name, NULL,
                                                            member_index)) {
        /* Reaching a tagged value is an erased representation boundary.
         * Preserve nominal identity in the static immortal box; fully typed
         * scalar enum values still take the allocation-free sidecar path
         * above. */
        emit_enum_member_box_value_expr(ctx, out, "unit", ed->layout_id, ed->name ? ed->name : "",
                                        member->name ? member->name : "", member_index,
                                        ed->layout_id);
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static int cg_builtin_enum_member_index(const XaBuiltinEnum *decl, const char *member_name) {
    if (!decl || !member_name)
        return -1;
    for (int i = 0; i < decl->variant_count; i++) {
        if (decl->variants[i].name && strcmp(decl->variants[i].name, member_name) == 0)
            return i;
    }
    return -1;
}

static bool emit_static_builtin_enum_member_value_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v,
                                                       const XaBuiltinEnum *decl,
                                                       uint32_t member_index) {
    if (!out || !decl || !decl->variants || member_index >= (uint32_t) decl->variant_count)
        return false;
    const XaBuiltinEnumVariant *member = &decl->variants[member_index];
    if (member->payload_count != 0)
        return false;
    XrRep source_rep =
        (ctx && ctx->freestanding_profile && cg_value_plan_storage_rep(ctx, v) == XR_REP_I64)
            ? XR_REP_I64
            : XR_REP_TAGGED;
    const char *conv_suffix = emit_conversion_prefix(out, v ? v->type : NULL, source_rep,
                                                     cg_value_plan_storage_rep(ctx, v));
    if (source_rep == XR_REP_I64) {
        fprintf(out, "INT64_C(%u)", (unsigned) member_index);
    } else if (!emit_portable_scalar_enum_member_value_expr(ctx, out, v, decl->name, NULL,
                                                            member_index)) {
        emit_enum_member_box_value_expr(
            ctx, out, "builtin", decl->layout_id, decl->name ? decl->name : "",
            member->name ? member->name : "", member_index, decl->layout_id);
    }
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static void emit_enum_variant_c_field_name(FILE *out, const XiEnumMemberData *member,
                                           uint32_t index) {
    fprintf(out, "v%u_", (unsigned) index);
    const char *name = member && member->name ? member->name : "Variant";
    char first = name[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
        fputc('_', out);
    for (const char *p = name; *p; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            fputc(c, out);
        else
            fputc('_', out);
    }
}

static bool emit_enum_payload_type_predicate(XiCgenCtx *ctx, FILE *out, const XrType *type,
                                             const char *value_expr);

static bool enum_payload_type_predicate_supported(XiCgenCtx *ctx, const XrType *type) {
    if (!type)
        return false;
    if (type->is_nullable && type->kind != XR_KIND_NULL) {
        XrType tmp = *type;
        tmp.is_nullable = false;
        return enum_payload_type_predicate_supported(ctx, &tmp);
    }
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
        case XR_KIND_POINTER:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_STRING:
        case XR_KIND_NULL:
        case XR_KIND_UNIT:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_ENUM:
            return true;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return cg_class_native_data_for_abi_type(ctx, type) != NULL;
        case XR_KIND_UNION:
            if (type->union_type.member_count == 0 || !type->union_type.members)
                return false;
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (!enum_payload_type_predicate_supported(ctx, type->union_type.members[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

static uint32_t cg_enum_layout_id_for_type(XiCgenCtx *ctx, const XrType *type) {
    if (!type || type->kind != XR_KIND_ENUM)
        return 0;
    const XaotEnumPlan *plan =
        ctx && ctx->aot_bundle ? xaot_bundle_find_enum_plan_for_type(ctx->aot_bundle, type) : NULL;
    if (plan && plan->layout_id != 0)
        return plan->layout_id;
    /* Zero-payload enums have no XaotEnumPlan.  Imported analyzer types may
     * carry a sibling layout from an earlier analysis instance, so prefer the
     * unique provider definition in the lowered module graph.  If two modules
     * really declare the same bare enum name, keep the type's own layout and
     * preserve the ambiguity boundary. */
    if (ctx && ctx->aot_bundle && type->enum_type.enum_name) {
        uint32_t provider_layout_id = 0;
        bool ambiguous = false;
        for (uint32_t mi = 0; mi < ctx->aot_bundle->nmodules && !ambiguous; mi++) {
            const XiModule *mod = ctx->aot_bundle->modules ? ctx->aot_bundle->modules[mi] : NULL;
            if (!mod || !mod->slot_enums)
                continue;
            for (uint16_t si = 0; si < mod->nslots; si++) {
                const XiEnumData *ed = mod->slot_enums[si];
                if (!ed || !ed->name || strcmp(ed->name, type->enum_type.enum_name) != 0 ||
                    ed->layout_id == 0)
                    continue;
                if (provider_layout_id == 0)
                    provider_layout_id = ed->layout_id;
                else if (provider_layout_id != ed->layout_id) {
                    ambiguous = true;
                    break;
                }
            }
        }
        if (!ambiguous && provider_layout_id != 0)
            return provider_layout_id;
    }
    if (type->enum_type.layout && type->enum_type.layout->layout_id != 0)
        return type->enum_type.layout->layout_id;
    return type->enum_type.layout_id;
}

static bool emit_enum_payload_type_predicate_nonnull(XiCgenCtx *ctx, FILE *out, const XrType *type,
                                                     const char *value_expr) {
    if (!out || !type || !value_expr)
        return false;
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
        case XR_KIND_POINTER:
            fprintf(out, "XR_IS_INT(%s)", value_expr);
            return true;
        case XR_KIND_FLOAT:
            fprintf(out, "XR_IS_FLOAT(%s)", value_expr);
            return true;
        case XR_KIND_BOOL:
            fprintf(out, "XR_IS_BOOL(%s)", value_expr);
            return true;
        case XR_KIND_RUNE:
            fprintf(out, "XR_IS_RUNE(%s)", value_expr);
            return true;
        case XR_KIND_STRING:
            fprintf(out, "XR_IS_STR(%s)", value_expr);
            return true;
        case XR_KIND_NULL:
        case XR_KIND_UNIT:
            fprintf(out, "XR_IS_NULL(%s)", value_expr);
            return true;
        case XR_KIND_ARRAY:
            fprintf(out, "XR_IS_ARRAY(%s)", value_expr);
            return true;
        case XR_KIND_MAP:
            fprintf(out, "XR_IS_MAP(%s)", value_expr);
            return true;
        case XR_KIND_SET:
            fprintf(out, "XR_IS_SET(%s)", value_expr);
            return true;
        case XR_KIND_ENUM:
            fprintf(out, "((%s).tag == XR_TAG_ENUM", value_expr);
            uint32_t layout_id = cg_enum_layout_id_for_type(ctx, type);
            if (layout_id != 0) {
                fprintf(out,
                        " && (xrt_enum_value_layout_id(%s) == 0 || "
                        "xrt_enum_value_layout_id(%s) == %u)",
                        value_expr, value_expr, layout_id);
            }
            fprintf(out, ")");
            return true;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE: {
            const XiClassData *cd = cg_class_native_data_for_abi_type(ctx, type);
            if (!cd)
                return false;
            fprintf(out, "xrt_instance_exact_type(%s, (uint16_t)", value_expr);
            if (!emit_class_native_type_id_expr(ctx, out, cd))
                return false;
            fprintf(out, ")");
            return true;
        }
        case XR_KIND_UNION: {
            if (type->union_type.member_count == 0 || !type->union_type.members)
                return false;
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                if (!emit_enum_payload_type_predicate(ctx, out, type->union_type.members[i],
                                                      value_expr))
                    return false;
                if (i + 1 < type->union_type.member_count)
                    fprintf(out, " || ");
            }
            return true;
        }
        default:
            return false;
    }
}

static bool emit_enum_payload_type_predicate(XiCgenCtx *ctx, FILE *out, const XrType *type,
                                             const char *value_expr) {
    if (!out || !type || !value_expr)
        return false;
    if (!enum_payload_type_predicate_supported(ctx, type))
        return false;
    if (type->is_nullable && type->kind != XR_KIND_NULL) {
        fprintf(out, "(XR_IS_NULL(%s) || ", value_expr);
        if (!emit_enum_payload_type_predicate_nonnull(ctx, out, type, value_expr))
            return false;
        fprintf(out, ")");
        return true;
    }
    return emit_enum_payload_type_predicate_nonnull(ctx, out, type, value_expr);
}

static void emit_enum_payload_type_checks(XiCgenCtx *ctx, FILE *out, const XiEnumMemberData *member,
                                          const char *enum_name) {
    if (!out || !member || member->payload_count <= 0 || !member->payload_types)
        return;
    for (uint16_t pi = 0; pi < (uint16_t) member->payload_count; pi++) {
        const XrType *payload_type = member->payload_types[pi];
        char value_expr[64];
        snprintf(value_expr, sizeof(value_expr), "value.payloads[%u]", (unsigned) pi);
        fprintf(out, "xrt_enum_aggregate_check_payload_type(value.layout_id, ");
        if (!enum_payload_type_predicate_supported(ctx, payload_type) ||
            !emit_enum_payload_type_predicate(ctx, out, payload_type, value_expr)) {
            fprintf(out, "1");
        }
        fprintf(out, ", ");
        emit_c_string_literal(out, enum_name ? enum_name : "");
        fprintf(out, "); ");
    }
}

static const XiModule *cg_enum_plan_owner_module(XiCgenCtx *ctx, const XaotEnumPlan *plan) {
    if (!ctx || !ctx->aot_bundle || !plan || plan->module_index >= ctx->aot_bundle->nmodules)
        return NULL;
    return ctx->aot_bundle->modules ? ctx->aot_bundle->modules[plan->module_index] : NULL;
}

static void emit_one_enum_native_typedef(XiCgenCtx *ctx, FILE *out, const XaotEnumPlan *plan) {
    if (!ctx || !out || !plan || !plan->c_type)
        return;
    uint16_t payload_cap = plan->max_payload > 0 ? plan->max_payload : 1;
    const XiEnumData *ed = plan->enum_data;
    if (plan->type_arg_count == 0 && ed && ed->type_param_count > 0)
        return;
    const XiEnumMemberData *members = plan->members ? plan->members : (ed ? ed->members : NULL);
    /* `--c-only` concatenates module units into one translation unit.  An
     * imported compact enum is emitted in each unit that needs its native ABI,
     * so guard the typedef and converters just like native struct typedefs. */
    fprintf(out, "#ifndef XRT_TYPEDEF_%s\n#define XRT_TYPEDEF_%s 1\n", plan->c_type, plan->c_type);
    fprintf(out, "typedef struct %s { int64_t tag; union { ", plan->c_type);
    for (uint32_t mi = 0; ed && mi < ed->member_count; mi++) {
        const XiEnumMemberData *member = members ? &members[mi] : NULL;
        uint16_t pc = member && member->payload_count > 0 ? member->payload_count : 0;
        fprintf(out, "struct { ");
        if (pc == 0) {
            fprintf(out, "uint8_t _empty; ");
        } else {
            for (uint16_t pi = 0; pi < pc; pi++)
                fprintf(out, "XrValue f%u; ", (unsigned) pi);
        }
        fprintf(out, "} ");
        emit_enum_variant_c_field_name(out, member, mi);
        fprintf(out, "; ");
    }
    fprintf(out, "XrValue raw[%u]; } payload; } %s;\n", (unsigned) payload_cap, plan->c_type);
    fprintf(out, "static inline %s %s_from_base(XrAotEnumAggregate value) { %s out = {0}; ",
            plan->c_type, plan->c_type, plan->c_type);
    fprintf(out, "xrt_enum_aggregate_check_layout(value.layout_id, %u, ", plan->layout_id);
    emit_c_string_literal(out, ed && ed->name ? ed->name : "");
    fprintf(out, "); out.tag = value.tag; switch (value.tag) { ");
    for (uint32_t mi = 0; ed && mi < ed->member_count; mi++) {
        const XiEnumMemberData *member = members ? &members[mi] : NULL;
        uint16_t pc = member && member->payload_count > 0 ? member->payload_count : 0;
        fprintf(out,
                "case %u: xrt_enum_aggregate_check_payload_count(value.layout_id, "
                "value.payload_count, %u, ",
                (unsigned) mi, (unsigned) pc);
        emit_c_string_literal(out, ed && ed->name ? ed->name : "");
        fprintf(out, "); ");
        emit_enum_payload_type_checks(ctx, out, member, ed && ed->name ? ed->name : "");
        fprintf(out,
                "for (uint32_t i = 0; i < value.payload_count && i < %u; i++) "
                "out.payload.raw[i] = value.payloads[i]; break; ",
                (unsigned) pc);
    }
    fprintf(out, "default: xrt_enum_aggregate_check_known_tag(value.layout_id, ");
    emit_c_string_literal(out, ed && ed->name ? ed->name : "");
    fprintf(out, "); break; } return out; }\n");
    fprintf(out,
            "static inline XrAotEnumAggregate %s_to_base(%s value) { "
            "XrAotEnumAggregate out = xrt_enum_aggregate_zero(); ",
            plan->c_type, plan->c_type);
    fprintf(out, "out.layout_id = %u; out.enum_name = ", plan->layout_id);
    emit_c_string_literal(out, ed && ed->name ? ed->name : "");
    fprintf(out, "; out.tag = value.tag; switch (value.tag) { ");
    for (uint32_t mi = 0; ed && mi < ed->member_count; mi++) {
        const XiEnumMemberData *member = members ? &members[mi] : NULL;
        uint16_t pc = member && member->payload_count > 0 ? member->payload_count : 0;
        fprintf(out, "case %u: out.member_name = ", (unsigned) mi);
        emit_c_string_literal(out, member && member->name ? member->name : "");
        fprintf(out, "; out.payload_count = %u; ", (unsigned) pc);
        for (uint16_t pi = 0; pi < pc; pi++)
            fprintf(out, "out.payloads[%u] = value.payload.raw[%u]; ", (unsigned) pi,
                    (unsigned) pi);
        fprintf(out, "break; ");
    }
    fprintf(out,
            "default: out.member_name = NULL; out.payload_count = 0; break; } return out; }\n");
    fprintf(out, "#endif\n");
}

static bool cg_enum_plan_rep_matches(XaotValueRep rep, const XaotEnumPlan *plan) {
    return plan && plan->c_type && cg_value_rep_is_typed_adt_aggregate(rep) && rep.c_type &&
           strcmp(rep.c_type, plan->c_type) == 0;
}

static bool cg_enum_native_typedef_is_reachable(XiCgenCtx *ctx, const XaotEnumPlan *plan,
                                                uint32_t module_index) {
    if (!ctx || !ctx->aot_bundle || !plan ||
        plan->scalar_action != XAOT_ENUM_SCALAR_COMPACT_AGGREGATE)
        return false;
    for (uint32_t i = 0; i < ctx->aot_bundle->nfunc_plans; i++) {
        const XaotFuncPlan *func = &ctx->aot_bundle->func_plans[i];
        if (func->module_index != module_index)
            continue;
        if (cg_enum_plan_rep_matches(xaot_abi_slot_value_rep(&func->abi.ret), plan))
            return true;
        for (uint16_t p = 0; p < func->abi.nparams; p++) {
            if (cg_enum_plan_rep_matches(xaot_abi_slot_value_rep(&func->abi.params[p]), plan))
                return true;
        }
    }
    for (uint32_t i = 0; i < ctx->aot_bundle->nvalue_plans; i++) {
        const XaotValuePlan *value = &ctx->aot_bundle->value_plans[i];
        const XaotFuncPlan *func =
            value->func ? xaot_bundle_find_func_plan(ctx->aot_bundle, value->func) : NULL;
        if (func && func->module_index == module_index &&
            cg_enum_plan_rep_matches(value->rep, plan))
            return true;
    }
    return false;
}

static void emit_enum_native_typedefs(XiCgenCtx *ctx, FILE *out, const XiModule *module) {
    if (!ctx || !ctx->aot_bundle || !out || !module)
        return;
    for (uint32_t i = 0; i < ctx->aot_bundle->nenum_plans; i++) {
        const XaotEnumPlan *plan = &ctx->aot_bundle->enum_plans[i];
        const XiModule *owner = cg_enum_plan_owner_module(ctx, plan);
        if (owner != module || !plan->c_type ||
            !cg_enum_native_typedef_is_reachable(ctx, plan, plan->module_index))
            continue;
        emit_one_enum_native_typedef(ctx, out, plan);
    }
}

static bool cg_enum_plan_index_for_ctype(XiCgenCtx *ctx, const char *c_type, uint32_t *out_idx) {
    if (!ctx || !ctx->aot_bundle || !c_type)
        return false;
    for (uint32_t i = 0; i < ctx->aot_bundle->nenum_plans; i++) {
        const XaotEnumPlan *plan = &ctx->aot_bundle->enum_plans[i];
        if (plan->c_type && strcmp(plan->c_type, c_type) == 0) {
            if (out_idx)
                *out_idx = i;
            return true;
        }
    }
    return false;
}

static uint32_t cg_current_module_index(XiCgenCtx *ctx) {
    if (!ctx || !ctx->aot_bundle || !ctx->module)
        return UINT32_MAX;
    for (uint32_t i = 0; i < ctx->aot_bundle->nmodules; i++) {
        if (ctx->aot_bundle->modules && ctx->aot_bundle->modules[i] == ctx->module)
            return i;
    }
    return UINT32_MAX;
}

static void cg_emit_imported_enum_native_typedef_for_rep(XiCgenCtx *ctx, FILE *out,
                                                         XaotValueRep rep, uint32_t module_index,
                                                         bool *emitted) {
    uint32_t enum_index = 0;
    if (!ctx || !out || !emitted || !cg_value_rep_is_typed_adt_aggregate(rep))
        return;
    if (!cg_enum_plan_index_for_ctype(ctx, rep.c_type, &enum_index))
        return;
    if (enum_index >= ctx->aot_bundle->nenum_plans || emitted[enum_index])
        return;
    const XaotEnumPlan *plan = &ctx->aot_bundle->enum_plans[enum_index];
    if (plan->module_index == module_index)
        return;
    emit_one_enum_native_typedef(ctx, out, plan);
    emitted[enum_index] = true;
}

static void cg_emit_imported_enum_native_typedefs_for_func(XiCgenCtx *ctx, FILE *out,
                                                           const XiFunc *func,
                                                           uint32_t module_index, bool *emitted) {
    const XaotFuncPlan *func_plan;
    if (!ctx || !out || !func || !emitted)
        return;
    func_plan = xaot_bundle_find_func_plan(ctx->aot_bundle, func);
    if (!func_plan)
        return;
    cg_emit_imported_enum_native_typedef_for_rep(
        ctx, out, xaot_abi_slot_value_rep(&func_plan->abi.ret), module_index, emitted);
    for (uint16_t p = 0; p < func_plan->abi.nparams; p++)
        cg_emit_imported_enum_native_typedef_for_rep(
            ctx, out, xaot_abi_slot_value_rep(&func_plan->abi.params[p]), module_index, emitted);
}

static void emit_imported_enum_native_typedefs(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !ctx->aot_bundle || !out || ctx->aot_bundle->nenum_plans == 0)
        return;
    uint32_t module_index = cg_current_module_index(ctx);
    if (module_index == UINT32_MAX)
        return;
    bool *emitted = (bool *) xr_calloc(ctx->aot_bundle->nenum_plans, sizeof(bool));
    if (!emitted) {
        ctx->error = true;
        return;
    }

    for (int i = 0; i < ctx->n_xmod_refs; i++)
        cg_emit_imported_enum_native_typedefs_for_func(ctx, out, ctx->xmod_ref_funcs[i],
                                                       module_index, emitted);

    for (uint32_t i = 0; i < ctx->aot_bundle->nvalue_plans; i++) {
        const XaotValuePlan *value_plan = &ctx->aot_bundle->value_plans[i];
        const XaotFuncPlan *func_plan =
            value_plan->func ? xaot_bundle_find_func_plan(ctx->aot_bundle, value_plan->func) : NULL;
        if (!func_plan || func_plan->module_index != module_index)
            continue;
        cg_emit_imported_enum_native_typedef_for_rep(ctx, out, value_plan->rep, module_index,
                                                     emitted);
    }
    xr_free(emitted);
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

static const XiEnumData *cg_enum_for_shared_slot_in_func(const XiCgenCtx *ctx, const XiFunc *f,
                                                         int slot) {
    if (!ctx)
        return NULL;
    if (f && f->module && f->module->slot_enums && slot >= 0 && slot < (int) f->module->nslots)
        return f->module->slot_enums[slot];
    if (slot < 0 || slot >= ctx->shared_cap)
        return NULL;
    return ctx->shared_enum[slot];
}

static const XiEnumData *cg_enum_for_shared_value_in_func(const XiCgenCtx *ctx, const XiFunc *f,
                                                          const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    if (!ctx || !v || v->op != XI_GET_SHARED)
        return NULL;
    return cg_enum_for_shared_slot_in_func(ctx, f, (int) v->aux_int);
}

static const XiEnumData *cg_enum_for_namespace_value(const XiValue *v) {
    v = cg_unwrap_identity_value(v);
    return v && v->op == XI_CONST && v->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE
               ? (const XiEnumData *) v->aux
               : NULL;
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

static void emit_adt_enum_make_expr(XiCgenCtx *ctx, FILE *out, const XiEnumData *ed, int member_idx,
                                    const XiValue *v, uint16_t payload_count, const char *enum_name,
                                    const char *member_name) {
    fprintf(out, "%s(%u, %d, %u, ",
            payload_count > 0 ? "XRT_ENUM_AGGREGATE_MAKE" : "xrt_enum_aggregate_make",
            ed ? ed->layout_id : 0u, member_idx, (unsigned) payload_count);
    emit_c_string_literal(out, enum_name);
    fprintf(out, ", ");
    emit_c_string_literal(out, member_name);
    fprintf(out, ", ");
    if (payload_count == 0 || !v || v->nargs <= 1) {
        fprintf(out, "NULL)");
        return;
    }
    for (uint16_t a = 1; a < v->nargs; a++) {
        if (a > 1)
            fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, v->args[a], XR_REP_TAGGED);
    }
    fprintf(out, ")");
}

static void emit_adt_enum_construct_expr(XiCgenCtx *ctx, FILE *out, const XiEnumData *ed,
                                         int member_idx, const XiValue *v) {
    uint16_t payload_count = v->nargs > 0 ? (uint16_t) (v->nargs - 1) : 0;
    const char *enum_name = ed && ed->name ? ed->name : "";
    const char *member_name =
        (ed && ed->members && member_idx >= 0 && (uint32_t) member_idx < ed->member_count &&
         ed->members[member_idx].name)
            ? ed->members[member_idx].name
            : "";
    if (cg_value_plan_is_aggregate(ctx, v)) {
        const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
        if (plan)
            emit_adt_base_to_value_rep_prefix(out, plan->rep);
        emit_adt_enum_make_expr(ctx, out, ed, member_idx, v, payload_count, enum_name, member_name);
        if (plan)
            emit_adt_base_to_value_rep_suffix(out, plan->rep);
        return;
    }
    fprintf(out, "xrt_enum_aggregate_box(");
    emit_adt_enum_make_expr(ctx, out, ed, member_idx, v, payload_count, enum_name, member_name);
    fprintf(out, ")");
}

static void emit_adt_enum_constructor_recipe_expr(XiCgenCtx *ctx, FILE *out, const XiValue *value,
                                                  const XrCValueEmissionView *recipe) {
    if (!ctx || !out || !value || !recipe || !recipe->recipe_symbol || !recipe->recipe_type_name ||
        !recipe->recipe_member_name) {
        emit_codegen_abort_expr(out);
        return;
    }
    fprintf(out, "%s(XRT_ENUM_AGGREGATE_MAKE(%u, %u, %u, ", recipe->recipe_symbol,
            recipe->recipe_layout_id, recipe->recipe_discriminant,
            (unsigned) recipe->recipe_argument_count);
    emit_c_string_literal(out, recipe->recipe_type_name);
    fprintf(out, ", ");
    emit_c_string_literal(out, recipe->recipe_member_name);
    for (uint16_t i = 0; i < recipe->recipe_argument_count; i++) {
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, value->args[i + 1u], XR_REP_TAGGED);
    }
    fprintf(out, "))");
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

static void emit_str_concat_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *function,
                                 const XiValue *v) {
    XrCValueEmissionView recipe = {0};
    if (!cg_string_concat_emission_view(ctx, function, v, &recipe)) {
        emit_codegen_abort_expr(out);
        return;
    }

    fprintf(out, "({ xrt_strpart_t _scp_%u[%u]; ", v->id, (unsigned) v->nargs);
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XrCRecipeArgumentView *argument = &recipe.recipe_arguments[i];
        if (argument->kind == XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64) {
            const XiValue *source =
                cg_string_concat_direct_u64_source(ctx, function, v->args[i], argument);
            fprintf(out, "xrt_strpart_init_u64(&_scp_%u[%u], (uint64_t)", v->id, (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, source, XR_REP_I64);
        } else {
            fprintf(out, "xrt_strpart_init(&_scp_%u[%u], ", v->id, (unsigned) i);
            emit_value_as_display_tagged(ctx, out, v->args[i]);
        }
        fprintf(out, "); ");
    }
    fprintf(out, "%s(%u, _scp_%u); })", recipe.recipe_symbol, (unsigned) v->nargs, v->id);
}

static bool emit_str_concat_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                       bool storage_predeclared) {
    if (!ctx || !out || !f || !v)
        return false;
    XrCValueEmissionView recipe = {0};
    if (!cg_string_concat_emission_view(ctx, f, v, &recipe))
        return false;

    /* A coroutine body declares every value in the frame prologue, so an
     * inline declaration here would redefine the C local. */
    if (!storage_predeclared && !ctx->pre_decl_all) {
        fprintf(out, "    %s ", local_ctype_str_ctx(ctx, f, v));
        emit_vref(out, v);
        fprintf(out, ";\n");
    }
    fprintf(out, "    {\n        xrt_strpart_t _scp_%u[%u];\n", v->id, (unsigned) v->nargs);
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XrCRecipeArgumentView *argument = &recipe.recipe_arguments[i];
        if (argument->kind == XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64) {
            const XiValue *source =
                cg_string_concat_direct_u64_source(ctx, f, v->args[i], argument);
            fprintf(out, "        xrt_strpart_init_u64(&_scp_%u[%u], (uint64_t)", v->id,
                    (unsigned) i);
            emit_value_as_rep_ctx(ctx, out, source, XR_REP_I64);
        } else {
            fprintf(out, "        xrt_strpart_init(&_scp_%u[%u], ", v->id, (unsigned) i);
            emit_value_as_display_tagged(ctx, out, v->args[i]);
        }
        fprintf(out, ");\n");
    }
    fprintf(out, "        ");
    emit_vref(out, v);
    fprintf(out, " = ");
    const char *suffix =
        emit_conversion_prefix(out, v->type, XR_REP_TAGGED, cg_value_decl_storage_rep(ctx, f, v));
    fprintf(out, "%s(%u, _scp_%u)", recipe.recipe_symbol, (unsigned) v->nargs, v->id);
    emit_conversion_suffix(out, suffix);
    fprintf(out, ";\n    }\n");
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, f, v);
    return true;
}

static void emit_closure_entry_pointer(XiCgenCtx *ctx, FILE *out, const char *prefix,
                                       const XiFunc *child) {
    if (cg_func_is_par_for_native_callback(child))
        emit_fname(ctx, out, prefix, child);
    else if (cg_func_uses_typed_abi(ctx, child) && !cg_func_needs_aot_coro_ctx(ctx, child))
        emit_typed_abi_fname(ctx, out, prefix, child);
    else
        emit_fname(ctx, out, prefix, child);
}

static uint32_t cg_callable_target_effects(XiCgenCtx *ctx, const XiFunc *target) {
    const XaotBundle *bundle = cg_ctx_aot_bundle(ctx);
    uint32_t effects = 0;
    if (bundle) {
        for (uint32_t i = 0; i < bundle->ncallable_target_cases; i++) {
            if (bundle->callable_target_cases[i].target_func == target)
                effects |= bundle->callable_target_cases[i].effect_bits;
        }
        const XgGlobalEvidence *ev = bundle->global_evidence_plan.evidence;
        for (uint32_t i = 0; ev && target && i < ev->nbodies; i++) {
            if (ev->bodies[i].func_id == target->xg_body_func_id)
                effects |= ev->bodies[i].effect_bits;
        }
    }
    if (cg_func_needs_aot_coro_ctx(ctx, target))
        effects |= XG_BODY_MAY_SUSPEND;
    return effects;
}

static void emit_callable_descriptor(XiCgenCtx *ctx, FILE *out, const char *prefix,
                                     uint32_t descriptor_id, const XiValue *closure,
                                     const XiFunc *target, uint32_t target_id_override,
                                     uint64_t signature_override, const char *sync_entry_override) {
    uint32_t target_id = target_id_override;
    uint64_t signature = signature_override;
    uint32_t effects = target ? cg_callable_target_effects(ctx, target) : XG_BODY_MAY_CALL_NATIVE;
    if (target_id == 0 && target)
        target_id = target->xg_body_func_id;
    if (target_id == 0)
        target_id = closure ? closure->id + 1 : 1;
    if (signature == 0 && closure)
        signature = xaot_type_fingerprint(closure->type);
    fprintf(out,
            "static const XrAotCallableDesc _xr_callable_%u = {.target_id=%uu, "
            ".effect_bits=0x%xu, .signature_key=UINT64_C(0x%016" PRIx64 "), "
            ".sync_entry=",
            descriptor_id, target_id, effects, signature);
    if (sync_entry_override) {
        fprintf(out, "(void (*)(void))%s", sync_entry_override);
    } else if (target && !cg_func_needs_aot_coro_ctx(ctx, target)) {
        fprintf(out, "(void (*)(void))");
        emit_closure_entry_pointer(ctx, out, prefix, target);
    } else {
        fprintf(out, "NULL");
    }
    fprintf(out, "}; ");
}

/* Whether an upval needs its own +1 depends on WHERE THE VALUE CAME FROM, not
 * on where the closure was placed.
 *
 * xi.closure.new is `:own-use consume`, so a capture that arrived through the
 * IR args was MOVED in — ARC dup'd the source first if it is used again, and
 * gives each capture site exactly one +1. Retaining it again creates a second
 * reference nobody owns, and the destructor releases only one.
 *
 * A capture read out of the ENCLOSING closure's upvals never passed through
 * ARC at all: the outer closure still holds it, so this one has to take its
 * own reference.
 *
 * The old rule was `retain everything unless this is a stack closure`, which
 * was wrong in both directions — a heap closure leaked every arg-sourced
 * capture, and a stack closure under-retained every upval-sourced one.
 *
 * owns_upvals is the separate question of whether ANYTHING will release them.
 * An ordinary closure is destroyed and releases each upval. A synthesized
 * parallel-body closure is not: it is a stack view that lives exactly as long
 * as the par_for/par_map call it belongs to, is never destroyed, and therefore
 * borrows everything and retains nothing. */
static void emit_closure_upval_initializers(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                            const XiValue *v, bool owns_upvals) {
    (void) current;
    XiFunc *child = v && v->aux ? (XiFunc *) v->aux : NULL;
    uint16_t ncap = child ? child->ncaptures : 0;
    for (uint16_t ci = 0; ci < ncap; ci++) {
        XiCapture *cap = &child->captures[ci];
        if (cap->needs_cell &&
            (cap->source != XI_CAPTURE_SRC_REG || ci >= v->nargs || !v->args[ci])) {
            ctx->error = true;
            fprintf(stderr,
                    "[xi_cgen] ERROR: mutable closure capture '%s' has no explicit cell XiValue\n",
                    cap->name ? cap->name : "?");
            fprintf(out, "_c->upvals[%u] = XR_NULL_VAL; ", ci);
            continue;
        }
        if (ci < v->nargs && v->args[ci]) {
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
            /* Borrowed from the enclosing closure: take our own reference,
             * unless this closure never releases anything. */
            if (owns_upvals)
                fprintf(out, "xrt_retain(_c->upvals[%u]); ", ci);
        } else {
            ctx->error = true;
            fprintf(stderr, "[xi_cgen] ERROR: missing AOT closure capture '%s'\n",
                    cap->name ? cap->name : "?");
            fprintf(out, "_c->upvals[%u] = XR_NULL_VAL; ", ci);
        }
    }
}

static void emit_closure_new_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                  const char *prefix, const XiValue *v) {
    if (v->aux) {
        XiFunc *child = (XiFunc *) v->aux;
        /* Foreign functions remain first-class Xray function values.  The
         * boxed entry is derived from the canonical declaration registry;
         * statically resolved calls still use the zero-overhead C ABI path. */
        if (child->is_extern) {
            const XaotExternDecl *decl = NULL;
            if (!cg_mark_extern_decl_adapter_used(ctx, child, &decl)) {
                emit_codegen_abort_expr(out);
                return;
            }
            fprintf(out, "({ ");
            char adapter[64];
            snprintf(adapter, sizeof(adapter), "xr_ffi_closure_%u", decl->stable_id);
            emit_callable_descriptor(ctx, out, prefix, v->id, v, NULL, decl->stable_id,
                                     decl->signature_hash, adapter);
            fprintf(out,
                    "xrt_closure_t *_c = (xrt_closure_t*)xrt_closure_new("
                    "&_xr_callable_%u, 0).ptr; xr_mkptr(_c, XR_TAG_CLOSURE); })",
                    v->id);
            return;
        }
        if (cg_closure_new_value_can_emit_null_for_unreachable_body(ctx, current, v, child, 0)) {
            fprintf(out, "XR_NULL_VAL /* unreachable closure: %s */",
                    child->name ? child->name : "?");
            return;
        }
        uint16_t ncap = child->ncaptures;
        bool stack_closure = v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW;
        const char *alloc_fn = stack_closure ? "xrt_closure_stack_new" : "xrt_closure_new";
        fprintf(out, "({ ");
        emit_callable_descriptor(ctx, out, prefix, v->id, v, child, 0, 0, NULL);
        fprintf(out, "xrt_closure_t *_c = (xrt_closure_t*)%s(&_xr_callable_%u, %u).ptr; ", alloc_fn,
                v->id, ncap);
        emit_closure_upval_initializers(ctx, out, current, v, true);
        fprintf(out, "xr_mkptr(_c, XR_TAG_CLOSURE); })");
    } else {
        fprintf(out, "XR_NULL_VAL /* closure: unknown */");
    }
}

/* Closure construction needs a descriptor declaration, allocation, capture
 * initialization, and a result assignment. Emit those as ordinary C11
 * statements so the generated path is accepted by every supported provider. */
static bool emit_closure_new_value_stmt(XiCgenCtx *ctx, FILE *out, const XiFunc *current,
                                        const char *prefix, const XiValue *v,
                                        bool storage_predeclared) {
    if (!ctx || !out || !v ||
        (v->op != XI_CLOSURE_NEW && !(v->op == XI_STACK_ALLOC && v->aux_int == XI_CLOSURE_NEW)) ||
        !v->aux)
        return false;

    XiFunc *child = (XiFunc *) v->aux;
    if (cg_closure_new_value_can_emit_null_for_unreachable_body(ctx, current, v, child, 0))
        return false;

    if (child->is_extern) {
        const XaotExternDecl *decl = NULL;
        if (!cg_mark_extern_decl_adapter_used(ctx, child, &decl)) {
            ctx->error = true;
            return true;
        }
        char adapter[64];
        snprintf(adapter, sizeof(adapter), "xr_ffi_closure_%u", decl->stable_id);
        fprintf(out, "    ");
        emit_callable_descriptor(ctx, out, prefix, v->id, v, NULL, decl->stable_id,
                                 decl->signature_hash, adapter);
    } else {
        fprintf(out, "    ");
        emit_callable_descriptor(ctx, out, prefix, v->id, v, child, 0, 0, NULL);
    }
    fprintf(out, "\n");

    if (!storage_predeclared && !ctx->pre_decl_all) {
        fprintf(out, "    %s ", local_ctype_str_ctx(ctx, current, v));
        emit_vref(out, v);
        fprintf(out, ";\n");
    }
    fprintf(out, "    {\n");
    bool stack_closure = v->op == XI_STACK_ALLOC;
    const char *alloc_fn = stack_closure ? "xrt_closure_stack_new" : "xrt_closure_new";
    fprintf(out, "        xrt_closure_t *_c = (xrt_closure_t*)%s(&_xr_callable_%u, %u).ptr; ",
            alloc_fn, v->id, (unsigned) child->ncaptures);
    emit_closure_upval_initializers(ctx, out, current, v, true);
    fprintf(out, "\n        ");
    emit_vref(out, v);
    fprintf(out, " = xr_mkptr(_c, XR_TAG_CLOSURE);\n    }\n");
    emit_value_generated_line_reset(ctx, out, v);
    emit_debug_source_var_sync(ctx, out, current, v);
    return true;
}
