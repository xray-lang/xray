/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_struct_helpers.inc.c - AOT native struct emission helpers
 */

static const char *cg_struct_native_c_type(uint8_t native_type) {
    const char *c_type = xaot_layout_c_type_for_native_type(native_type);
    return c_type ? c_type : "XrValue";
}

static XrRep cg_struct_native_rep(uint8_t native_type) {
    return xaot_layout_storage_rep_for_native_type(native_type);
}

static bool cg_struct_native_heap_supported_depth(const XrAggregateLayout *sl, int depth) {
    if (!sl || sl->field_count == 0 || sl->field_count > XR_MAX_AGG_FIELDS)
        return false;
    if (depth > 8)
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        uint8_t native_type = sl->fields[i].native_type;
        if (xaot_layout_native_field_direct_heap_supported(native_type))
            continue;
        if (xaot_layout_native_field_uses_elem_layout(native_type)) {
            if (sl->fields[i].elem_count == 0 ||
                cg_struct_native_rep(sl->fields[i].elem_native_type) == XR_REP_TAGGED)
                return false;
            continue;
        }
        if (xaot_layout_native_field_uses_nested_layout(native_type)) {
            if (!cg_struct_native_heap_supported_depth(sl->fields[i].sub_layout, depth + 1))
                return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool cg_struct_native_heap_supported(const XrAggregateLayout *sl) {
    return cg_struct_native_heap_supported_depth(sl, 0);
}

static bool cg_struct_c_identifier_is_valid(const char *s) {
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return false;
    for (const char *p = s + 1; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    }
    return true;
}

static bool cg_struct_c_identifier_is_reserved(const char *s) {
    static const char *const reserved[] = {
        "alignas",       "alignof",  "auto",           "bool",          "break",    "case",
        "char",          "const",    "continue",       "default",       "do",       "double",
        "else",          "enum",     "extern",         "false",         "float",    "for",
        "goto",          "if",       "inline",         "int",           "long",     "register",
        "restrict",      "return",   "short",          "signed",        "sizeof",   "static",
        "static_assert", "struct",   "switch",         "thread_local",  "true",     "typedef",
        "union",         "unsigned", "void",           "volatile",      "while",    "_Alignas",
        "_Alignof",      "_Atomic",  "_Bool",          "_Complex",      "_Generic", "_Imaginary",
        "_Noreturn",     "_Pragma",  "_Static_assert", "_Thread_local",
    };
    if (!s)
        return true;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(s, reserved[i]) == 0)
            return true;
    }
    return false;
}

static bool cg_struct_source_field_name_is_usable(const XrAggregateLayout *sl, int64_t idx) {
    if (!sl || idx < 0 || idx >= sl->field_count || !sl->field_names)
        return false;
    const char *name = sl->field_names[idx];
    if (!cg_struct_c_identifier_is_valid(name) || cg_struct_c_identifier_is_reserved(name))
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if ((int64_t) i == idx)
            continue;
        const char *other = sl->field_names[i];
        if (other && cg_struct_c_identifier_is_valid(other) &&
            !cg_struct_c_identifier_is_reserved(other) && strcmp(name, other) == 0)
            return false;
    }
    return true;
}

static void cg_struct_field_c_name(const XrAggregateLayout *sl, int64_t idx, char *buf,
                                   size_t buflen) {
    if (!buf || buflen == 0)
        return;
    if (cg_struct_source_field_name_is_usable(sl, idx)) {
        const char *name = sl->field_names[idx];
        if (strlen(name) < buflen) {
            snprintf(buf, buflen, "%s", name);
            return;
        }
    }
    snprintf(buf, buflen, "f%d", (int) idx);
}

static uint64_t cg_struct_hash_string(uint64_t h, const char *s) {
    if (!s)
        return h;
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    h ^= UINT64_C(0xff);
    h *= UINT64_C(1099511628211);
    return h;
}

static uint64_t cg_struct_layout_hash_depth(const XrAggregateLayout *sl, int depth) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!sl)
        return h;
    if (depth > 8)
        return h ^ UINT64_C(0x9e3779b97f4a7c15);
    h ^= sl->field_count;
    h *= UINT64_C(1099511628211);
    h ^= sl->kind;
    h *= UINT64_C(1099511628211);
    h ^= sl->explicit_align;
    h *= UINT64_C(1099511628211);
    for (uint16_t i = 0; i < sl->field_count; i++) {
        char fname[128];
        cg_struct_field_c_name(sl, i, fname, sizeof(fname));
        h = cg_struct_hash_string(h, fname);
        h ^= sl->fields[i].native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_native_type;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].elem_count;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].size;
        h *= UINT64_C(1099511628211);
        h ^= sl->fields[i].sub_layout_id;
        h *= UINT64_C(1099511628211);
        if (sl->fields[i].native_type == XR_NATIVE_STRUCT) {
            h ^= cg_struct_layout_hash_depth(sl->fields[i].sub_layout, depth + 1);
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static uint64_t cg_struct_layout_hash(const XrAggregateLayout *sl) {
    return xaot_struct_layout_hash(sl);
}

static bool cg_struct_layout_same_shape_depth(const XrAggregateLayout *a,
                                              const XrAggregateLayout *b, int depth) {
    if (a == b)
        return true;
    if (!a || !b || a->field_count != b->field_count || a->kind != b->kind ||
        a->explicit_align != b->explicit_align || depth > 8)
        return false;
    for (uint16_t i = 0; i < a->field_count; i++) {
        char aname[128];
        char bname[128];
        cg_struct_field_c_name(a, i, aname, sizeof(aname));
        cg_struct_field_c_name(b, i, bname, sizeof(bname));
        if (strcmp(aname, bname) != 0)
            return false;
        if (a->fields[i].native_type != b->fields[i].native_type ||
            a->fields[i].elem_native_type != b->fields[i].elem_native_type ||
            a->fields[i].elem_count != b->fields[i].elem_count ||
            a->fields[i].size != b->fields[i].size)
            return false;
        if (a->fields[i].native_type == XR_NATIVE_STRUCT &&
            !cg_struct_layout_same_shape_depth(a->fields[i].sub_layout, b->fields[i].sub_layout,
                                               depth + 1))
            return false;
    }
    return true;
}

static bool cg_struct_layout_same_shape(const XrAggregateLayout *a, const XrAggregateLayout *b) {
    return cg_struct_layout_same_shape_depth(a, b, 0);
}

static void cg_struct_heap_type_name(char *buf, size_t buflen, const char *prefix,
                                     const XrAggregateLayout *sl) {
    xaot_struct_c_type_name(buf, buflen, prefix, sl);
}

static const XrAggregateFieldLayout *cg_struct_field(const XrAggregateLayout *sl, int64_t idx);
static const char *cg_struct_field_c_type(const XrAggregateLayout *sl, int64_t idx);
static const XrAggregateLayout *cg_type_struct_layout(const XrType *type);

static void emit_struct_field_decl(FILE *out, const XrAggregateLayout *sl, int64_t idx,
                                   const char *name, const char *prefix) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
        char tname[128];
        cg_struct_heap_type_name(tname, sizeof(tname), prefix, field->sub_layout);
        fprintf(out, "%s %s", tname, name);
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "%s %s[%u]", cg_struct_native_c_type(field->elem_native_type), name,
                (unsigned) field->elem_count);
        return;
    }
    fprintf(out, "%s %s", cg_struct_field_c_type(sl, idx), name);
}

static void emit_aggregate_layout_c_attributes(FILE *out, const XrAggregateLayout *sl);

static void emit_struct_native_typedef(FILE *out, const XrAggregateLayout *sl, const char *prefix) {
    char tname[128];
    cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
    bool is_union = sl && sl->kind == XR_AGG_LAYOUT_UNION;
    fprintf(out, "typedef %s", is_union ? "union" : "struct");
    emit_aggregate_layout_c_attributes(out, sl);
    fprintf(out, " %s { ", tname);
    if (xr_aggregate_layout_header_size(sl) != 0)
        fprintf(out, "uint32_t _size; uint32_t _layout; ");
    for (uint16_t i = 0; i < sl->field_count; i++) {
        char fname[128];
        cg_struct_field_c_name(sl, i, fname, sizeof(fname));
        emit_struct_field_decl(out, sl, i, fname, prefix);
        fprintf(out, "; ");
    }
    fprintf(out, "} %s;\n", tname);
}

static void emit_aggregate_layout_c_attributes(FILE *out, const XrAggregateLayout *sl) {
    bool is_union = sl && sl->kind == XR_AGG_LAYOUT_UNION;
    if (sl && (sl->kind == XR_AGG_LAYOUT_PACKED_STRUCT || sl->explicit_align != 0)) {
        fprintf(out, " __attribute__((");
        bool need_comma = false;
        if (!is_union && sl->kind == XR_AGG_LAYOUT_PACKED_STRUCT) {
            fprintf(out, "packed");
            need_comma = true;
        }
        if (sl->explicit_align != 0) {
            if (need_comma)
                fprintf(out, ", ");
            fprintf(out, "aligned(%u)", (unsigned) sl->explicit_align);
        }
        fprintf(out, "))");
    }
}

static void emit_static_aggregate_decl_head(FILE *out, const XrAggregateLayout *sl) {
    fprintf(out, "%s", sl && sl->kind == XR_AGG_LAYOUT_UNION ? "union" : "struct");
    emit_aggregate_layout_c_attributes(out, sl);
    fprintf(out, " { ");
}

#define CG_STRUCT_TYPEDEF_MAX 128

static void cg_collect_struct_layout(const XrAggregateLayout *sl, const XrAggregateLayout **layouts,
                                     uint64_t *hashes, int *count) {
    if (!cg_struct_native_heap_supported(sl) || !layouts || !hashes || !count ||
        *count >= CG_STRUCT_TYPEDEF_MAX)
        return;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if (sl->fields[i].native_type == XR_NATIVE_STRUCT)
            cg_collect_struct_layout(sl->fields[i].sub_layout, layouts, hashes, count);
    }
    uint64_t hash = cg_struct_layout_hash(sl);
    for (int i = 0; i < *count; i++) {
        if (hashes[i] == hash)
            return;
    }
    layouts[*count] = sl;
    hashes[*count] = hash;
    (*count)++;
}

static void cg_collect_struct_layouts_from_func(const XiFunc *f, const XrAggregateLayout **layouts,
                                                uint64_t *hashes, int *count) {
    if (!f)
        return;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_AGG_NEW || v->op == XI_AGG_GET || v->op == XI_AGG_SET)
                cg_collect_struct_layout((const XrAggregateLayout *) v->aux, layouts, hashes,
                                         count);
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++)
        cg_collect_struct_layouts_from_func(f->children[ci], layouts, hashes, count);
}

static void emit_struct_native_typedefs(FILE *out, const XiFunc *f, const char *prefix) {
    const XrAggregateLayout *layouts[CG_STRUCT_TYPEDEF_MAX];
    uint64_t hashes[CG_STRUCT_TYPEDEF_MAX];
    int count = 0;
    cg_collect_struct_layouts_from_func(f, layouts, hashes, &count);
    for (int i = 0; i < count; i++)
        emit_struct_native_typedef(out, layouts[i], prefix);
}

static const XrAggregateFieldLayout *cg_struct_field(const XrAggregateLayout *sl, int64_t idx) {
    if (!sl || idx < 0 || idx >= sl->field_count)
        return NULL;
    return &sl->fields[idx];
}

static const char *cg_struct_field_c_type(const XrAggregateLayout *sl, int64_t idx) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    return field ? cg_struct_native_c_type(field->native_type) : "XrValue";
}

static XrRep cg_struct_field_rep(const XrAggregateLayout *sl, int64_t idx) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    return field ? cg_struct_native_rep(field->native_type) : XR_REP_TAGGED;
}

static bool cg_static_struct_native_scalar_supported(uint8_t native_type) {
    switch ((XrNativeType) native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_F64:
        case XR_NATIVE_BOOL:
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
        case XR_NATIVE_F32:
            return true;
        default:
            return false;
    }
}

static bool cg_static_struct_ct_scalar_supported(const XrCtValue *value, uint8_t native_type) {
    if (!value || !cg_static_struct_native_scalar_supported(native_type))
        return false;
    switch ((XrNativeType) native_type) {
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            return value->kind == XR_CT_FLOAT || value->kind == XR_CT_INT ||
                   value->kind == XR_CT_BOOL || value->kind == XR_CT_CHAR;
        case XR_NATIVE_BOOL:
            return value->kind == XR_CT_BOOL;
        case XR_NATIVE_I64:
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return value->kind == XR_CT_INT || value->kind == XR_CT_BOOL ||
                   value->kind == XR_CT_CHAR;
        default:
            return false;
    }
}

static bool cg_static_struct_ct_string_supported(const XrCtValue *value, uint8_t native_type) {
    return native_type == XR_NATIVE_STRING && value && value->kind == XR_CT_STRING;
}

static bool cg_static_struct_native_array_elem_supported(uint8_t native_type) {
    return native_type == XR_NATIVE_STRING || native_type == XR_NATIVE_VALUE ||
           cg_static_struct_native_scalar_supported(native_type);
}

static bool cg_static_struct_native_fixed_array_supported(const XrAggregateFieldLayout *field) {
    return field && field->native_type == XR_NATIVE_ARRAY && field->elem_count > 0 &&
           cg_static_struct_native_array_elem_supported(field->elem_native_type);
}

static uint8_t cg_static_struct_array_ref_elem_native_type(const XrAggregateFieldLayout *field) {
    return field && field->elem_native_type == XR_NATIVE_STRING ? XR_NATIVE_VALUE
                                                                : field->elem_native_type;
}

static bool cg_static_struct_array_elem_ct_string_lane(uint8_t native_type,
                                                       const XrCtValue *value) {
    return (native_type == XR_NATIVE_STRING || native_type == XR_NATIVE_VALUE) && value &&
           value->kind == XR_CT_STRING;
}

static bool cg_static_struct_ct_layout_supported_depth(const XrAggregateLayout *sl,
                                                       const XrCtStructValue *st, int depth);

static bool cg_static_struct_ct_fixed_array_supported(const XrCtValue *value,
                                                      const XrAggregateFieldLayout *field) {
    if (!value || value->kind != XR_CT_FIXED_ARRAY ||
        !cg_static_struct_native_fixed_array_supported(field))
        return false;
    const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
    if (array->count != (int) field->elem_count || array->count <= 0 || !array->elements)
        return false;
    for (int i = 0; i < array->count; i++) {
        if (field->elem_native_type == XR_NATIVE_STRING ||
            field->elem_native_type == XR_NATIVE_VALUE) {
            if (!cg_static_struct_array_elem_ct_string_lane(field->elem_native_type,
                                                            &array->elements[i]))
                return false;
            continue;
        }
        if (!cg_static_struct_ct_scalar_supported(&array->elements[i], field->elem_native_type))
            return false;
    }
    return true;
}

static const XrCtValue *cg_static_struct_ct_field_value(const XrCtStructValue *st,
                                                        const XrAggregateLayout *sl,
                                                        uint16_t field_idx) {
    if (!st || !sl || field_idx >= sl->field_count || st->field_count < 0)
        return NULL;
    const char *field_name = sl->field_names ? sl->field_names[field_idx] : NULL;
    if (field_name && st->field_names) {
        for (int i = 0; i < st->field_count; i++) {
            if (st->field_names[i] && strcmp(st->field_names[i], field_name) == 0)
                return st->field_values ? &st->field_values[i] : NULL;
        }
        return NULL;
    }
    return field_idx < (uint16_t) st->field_count && st->field_values ? &st->field_values[field_idx]
                                                                      : NULL;
}

static int cg_static_struct_field_index_by_name(const XrAggregateLayout *sl, const char *name) {
    if (!sl || !sl->field_names || !name)
        return -1;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if (sl->field_names[i] && strcmp(sl->field_names[i], name) == 0)
            return (int) i;
    }
    return -1;
}

static bool cg_static_struct_field_access_index(const XiValue *v, const XrAggregateLayout *sl,
                                                int64_t *out_idx) {
    if (!v || !sl)
        return false;
    int64_t idx = -1;
    if (v->op == XI_AGG_GET) {
        idx = v->aux_int;
    } else if (v->op == XI_LOAD_FIELD) {
        idx = cg_static_struct_field_index_by_name(sl, (const char *) v->aux);
    } else {
        return false;
    }
    if (idx < 0 || idx >= sl->field_count)
        return false;
    if (out_idx)
        *out_idx = idx;
    return true;
}

static int cg_static_union_ct_active_field_index(const XrAggregateLayout *sl,
                                                 const XrCtStructValue *st) {
    if (!sl || sl->kind != XR_AGG_LAYOUT_UNION || !st || st->field_count != 1 || !st->field_names ||
        !st->field_names[0])
        return -1;
    return cg_static_struct_field_index_by_name(sl, st->field_names[0]);
}

static bool cg_static_struct_ct_nested_struct_supported(const XrCtValue *value,
                                                        const XrAggregateFieldLayout *field,
                                                        int depth) {
    return value && value->kind == XR_CT_STRUCT_VALUE && field &&
           field->native_type == XR_NATIVE_STRUCT && field->sub_layout &&
           cg_static_struct_ct_layout_supported_depth(field->sub_layout, &value->as.struct_val,
                                                      depth + 1);
}

static bool cg_static_struct_ct_field_supported(const XrAggregateLayout *sl,
                                                const XrCtStructValue *st, uint16_t field_idx,
                                                int depth) {
    if (!sl || !st || field_idx >= sl->field_count)
        return false;
    const XrAggregateFieldLayout *field = &sl->fields[field_idx];
    const XrCtValue *field_value = cg_static_struct_ct_field_value(st, sl, field_idx);
    if (cg_static_struct_ct_string_supported(field_value, field->native_type))
        return true;
    if (cg_static_struct_ct_scalar_supported(field_value, field->native_type))
        return true;
    if (cg_static_struct_ct_fixed_array_supported(field_value, field))
        return true;
    if (cg_static_struct_ct_nested_struct_supported(field_value, field, depth))
        return true;
    return false;
}

static bool cg_static_struct_ct_layout_supported_depth(const XrAggregateLayout *sl,
                                                       const XrCtStructValue *st, int depth) {
    if (!sl || !st || depth > 8 || sl->field_count == 0 || sl->field_count > XR_MAX_AGG_FIELDS)
        return false;
    if (sl->kind == XR_AGG_LAYOUT_UNION) {
        int active_idx = cg_static_union_ct_active_field_index(sl, st);
        return active_idx >= 0 &&
               cg_static_struct_ct_field_supported(sl, st, (uint16_t) active_idx, depth);
    }
    if (sl->kind != XR_AGG_LAYOUT_STRUCT && sl->kind != XR_AGG_LAYOUT_PACKED_STRUCT)
        return false;
    for (uint16_t i = 0; i < sl->field_count; i++) {
        if (!cg_static_struct_ct_field_supported(sl, st, i, depth))
            return false;
    }
    return true;
}

static bool cg_freestanding_static_struct_literal_in_module(XiCgenCtx *ctx, const XiModule *module,
                                                            int64_t slot,
                                                            const XrAggregateLayout **out_layout,
                                                            const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !module || !module->slot_const_literals || slot < 0 ||
        slot >= module->nslots)
        return false;
    const XiConstLiteral *lit = &module->slot_const_literals[slot];
    if (lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_STRUCT_VALUE || !lit->type)
        return false;
    const XrAggregateLayout *sl = cg_type_struct_layout(lit->type);
    const XrCtStructValue *st = &lit->ct_value->as.struct_val;
    if (!cg_static_struct_ct_layout_supported_depth(sl, st, 0))
        return false;
    if (out_layout)
        *out_layout = sl;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_struct_value_ex(XiCgenCtx *ctx, const XiValue *value,
                                                   const XrAggregateLayout **out_layout,
                                                   int64_t *out_slot, const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (out_module)
        *out_module = NULL;
    if (!v)
        return false;

    const XiModule *module = ctx ? ctx->module : NULL;
    int64_t slot = -1;
    if (v->op == XI_GET_SHARED) {
        slot = v->aux_int;
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit =
            cg_import_slot_const_literal(ctx, NULL, (int) slot, &import_module, &import_slot);
        if (import_lit && import_lit->kind == XI_CONST_LITERAL_COMPTIME_AGGREGATE) {
            module = import_module;
            slot = import_slot;
        }
    } else if (v->op == XI_IMPORT_REF && v->aux) {
        const XiModule *import_module = NULL;
        int64_t import_slot = -1;
        const XiConstLiteral *import_lit = cg_import_ref_target_const_literal(
            ctx, (const XiImportRef *) v->aux, &import_module, &import_slot);
        if (!import_lit || import_lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE)
            return false;
        module = import_module;
        slot = import_slot;
    } else {
        return false;
    }

    if (!cg_freestanding_static_struct_literal_in_module(ctx, module, slot, out_layout, NULL))
        return false;
    const XiConstLiteral *lit = cg_module_const_literal(module, slot);
    if (cg_imported_static_const_needs_weak_symbol(ctx, module, lit)) {
        cg_report_imported_static_const_requires_weak(ctx, module, slot);
        ctx->error = true;
        return false;
    }
    if (out_slot)
        *out_slot = slot;
    if (out_module)
        *out_module = module;
    return true;
}

static bool cg_freestanding_static_struct_value(XiCgenCtx *ctx, const XiValue *value,
                                                const XrAggregateLayout **out_layout,
                                                int64_t *out_slot) {
    return cg_freestanding_static_struct_value_ex(ctx, value, out_layout, out_slot, NULL);
}

static bool cg_static_tuple_native_for_type(const XrType *type, uint8_t *out_native) {
    if (!type || type->is_nullable)
        return false;
    int native = -1;
    if (type->kind == XR_KIND_STRING)
        native = XR_NATIVE_STRING;
    else if (type->kind == XR_KIND_CHAR)
        native = XR_NATIVE_U32;
    else
        native = xr_type_kind_to_native(type->kind, type->native_width);
    if (native < 0 ||
        (native != XR_NATIVE_STRING && !cg_static_struct_native_scalar_supported((uint8_t) native)))
        return false;
    if (out_native)
        *out_native = (uint8_t) native;
    return true;
}

static bool cg_static_tuple_type_supported_depth(const XrType *type, const XrCtTupleValue *tuple,
                                                 int depth);

static bool cg_static_tuple_ct_value_supported(const XrCtValue *value, uint8_t native_type) {
    if (native_type == XR_NATIVE_STRING)
        return value && value->kind == XR_CT_STRING;
    return cg_static_struct_ct_scalar_supported(value, native_type);
}

static int cg_static_tuple_type_count(const XrType *type) {
    return type && type->kind == XR_KIND_TUPLE ? type->tuple.element_count : 0;
}

static XrType *cg_static_tuple_type_element(const XrType *type, int index) {
    int count = cg_static_tuple_type_count(type);
    if (index < 0 || index >= count || !type->tuple.element_types)
        return NULL;
    return type->tuple.element_types[index];
}

static bool cg_static_tuple_element_supported(const XrType *type, const XrCtValue *value,
                                              int depth) {
    if (!type || !value || type->is_nullable || depth > 8)
        return false;
    if (type->kind == XR_KIND_TUPLE) {
        return value->kind == XR_CT_TUPLE &&
               cg_static_tuple_type_supported_depth(type, &value->as.tuple_val, depth + 1);
    }
    uint8_t native_type = XR_NATIVE_VALUE;
    return cg_static_tuple_native_for_type(type, &native_type) &&
           cg_static_tuple_ct_value_supported(value, native_type);
}

static bool cg_static_tuple_type_supported_depth(const XrType *type, const XrCtTupleValue *tuple,
                                                 int depth) {
    int count = cg_static_tuple_type_count(type);
    if (!tuple || depth > 8 || count <= 0 || count > XR_MAX_AGG_FIELDS || tuple->count != count ||
        !tuple->elements)
        return false;
    for (int i = 0; i < count; i++) {
        if (!cg_static_tuple_element_supported(cg_static_tuple_type_element(type, i),
                                               &tuple->elements[i], depth))
            return false;
    }
    return true;
}

static bool cg_freestanding_static_tuple_literal(XiCgenCtx *ctx, int64_t slot, XrType **out_type,
                                                 const XrCtValue **out_value) {
    if (!ctx || !ctx->freestanding_profile || !ctx->module || !ctx->module->slot_const_literals ||
        slot < 0 || slot >= ctx->module->nslots)
        return false;
    const XiConstLiteral *lit = &ctx->module->slot_const_literals[slot];
    if (!lit || lit->kind != XI_CONST_LITERAL_COMPTIME_AGGREGATE || !lit->ct_value ||
        lit->ct_value->kind != XR_CT_TUPLE || !lit->type || lit->type->kind != XR_KIND_TUPLE)
        return false;
    if (!cg_static_tuple_type_supported_depth(lit->type, &lit->ct_value->as.tuple_val, 0))
        return false;
    if (out_type)
        *out_type = lit->type;
    if (out_value)
        *out_value = lit->ct_value;
    return true;
}

static bool cg_freestanding_static_tuple_value(XiCgenCtx *ctx, const XiValue *value,
                                               XrType **out_type, int64_t *out_slot) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_GET_SHARED)
        return false;
    if (!cg_freestanding_static_tuple_literal(ctx, v->aux_int, out_type, NULL))
        return false;
    if (out_slot)
        *out_slot = v->aux_int;
    return true;
}

static void cg_emit_static_struct_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                       int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xctstruct");
}

static void cg_emit_static_tuple_name(XiCgenCtx *ctx, FILE *out, const XiModule *module,
                                      int64_t slot) {
    cg_emit_static_const_data_name(ctx, out, module, slot, "_xcttuple");
}

static void cg_emit_static_struct_i64(FILE *out, int64_t value) {
    if (value == INT64_MIN)
        fprintf(out, "INT64_MIN");
    else
        fprintf(out, "INT64_C(%" PRId64 ")", value);
}

static void cg_emit_static_struct_scalar(FILE *out, uint8_t native_type, const XrCtValue *value) {
    fprintf(out, "(%s)", cg_struct_native_c_type(native_type));
    switch (value ? value->kind : XR_CT_NONE) {
        case XR_CT_FLOAT:
            emit_c_float_literal(out, value->as.float_val);
            return;
        case XR_CT_BOOL:
            fprintf(out, "%d", value->as.bool_val ? 1 : 0);
            return;
        case XR_CT_CHAR:
            fprintf(out, "0x%X", (unsigned) value->as.char_val);
            return;
        case XR_CT_INT:
        default:
            cg_emit_static_struct_i64(out, value ? value->as.int_val : 0);
            return;
    }
}

static void cg_emit_static_tuple_element(XiCgenCtx *ctx, FILE *out, uint8_t native_type,
                                         const XrCtValue *value) {
    if (native_type == XR_NATIVE_STRING) {
        cg_emit_static_str_value_initializer(
            ctx, out,
            value && value->kind == XR_CT_STRING && value->as.string_val ? value->as.string_val
                                                                         : "");
        return;
    }
    cg_emit_static_struct_scalar(out, native_type, value);
}

static void cg_emit_static_tuple_field_decl(FILE *out, XrType *tuple_type, uint16_t idx) {
    XrType *elem_type = cg_static_tuple_type_element(tuple_type, idx);
    if (elem_type && elem_type->kind == XR_KIND_TUPLE) {
        fprintf(out, "struct { ");
        int count = cg_static_tuple_type_count(elem_type);
        for (uint16_t i = 0; i < (uint16_t) count; i++)
            cg_emit_static_tuple_field_decl(out, elem_type, i);
        fprintf(out, "} f%u; ", (unsigned) idx);
        return;
    }
    uint8_t native_type = XR_NATIVE_VALUE;
    if (!cg_static_tuple_native_for_type(elem_type, &native_type))
        native_type = XR_NATIVE_VALUE;
    fprintf(out, "%s f%u; ", cg_struct_native_c_type(native_type), (unsigned) idx);
}

static void cg_emit_static_tuple_initializer(XiCgenCtx *ctx, FILE *out, XrType *tuple_type,
                                             const XrCtTupleValue *tuple) {
    int count = cg_static_tuple_type_count(tuple_type);
    fprintf(out, "{");
    for (uint16_t i = 0; i < (uint16_t) count; i++) {
        XrType *elem_type = cg_static_tuple_type_element(tuple_type, i);
        const XrCtValue *elem =
            tuple && tuple->elements && i < (uint16_t) tuple->count ? &tuple->elements[i] : NULL;
        if (i > 0)
            fprintf(out, ", ");
        fprintf(out, ".f%u = ", (unsigned) i);
        if (elem_type && elem_type->kind == XR_KIND_TUPLE && elem && elem->kind == XR_CT_TUPLE) {
            cg_emit_static_tuple_initializer(ctx, out, elem_type, &elem->as.tuple_val);
            continue;
        }
        uint8_t native_type = XR_NATIVE_VALUE;
        if (!cg_static_tuple_native_for_type(elem_type, &native_type))
            native_type = XR_NATIVE_VALUE;
        cg_emit_static_tuple_element(ctx, out, native_type, elem);
    }
    fprintf(out, "}");
}

static void cg_emit_static_struct_fixed_array_initializer(XiCgenCtx *ctx, FILE *out,
                                                          const XrAggregateFieldLayout *field,
                                                          const XrCtValue *value) {
    const XrCtFixedArrayValue *array =
        value && value->kind == XR_CT_FIXED_ARRAY ? &value->as.fixed_array_val : NULL;
    fprintf(out, "{");
    for (uint16_t i = 0; i < field->elem_count; i++) {
        if (i > 0)
            fprintf(out, ", ");
        const XrCtValue *elem =
            (array && array->elements && i < (uint16_t) array->count) ? &array->elements[i] : NULL;
        if (cg_static_struct_array_elem_ct_string_lane(field->elem_native_type, elem))
            cg_emit_static_str_value_initializer(
                ctx, out,
                elem && elem->kind == XR_CT_STRING && elem->as.string_val ? elem->as.string_val
                                                                          : "");
        else
            cg_emit_static_struct_scalar(out, field->elem_native_type, elem);
    }
    fprintf(out, "}");
}

static void cg_emit_static_struct_initializer(XiCgenCtx *ctx, FILE *out,
                                              const XrAggregateLayout *sl,
                                              const XrCtStructValue *st);

static void cg_emit_static_struct_field_initializer(XiCgenCtx *ctx, FILE *out,
                                                    const XrAggregateLayout *sl, uint16_t field_idx,
                                                    const XrCtValue *field_value) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, field_idx);
    if (cg_static_struct_native_fixed_array_supported(field)) {
        cg_emit_static_struct_fixed_array_initializer(ctx, out, field, field_value);
        return;
    }
    if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout && field_value &&
        field_value->kind == XR_CT_STRUCT_VALUE) {
        cg_emit_static_struct_initializer(ctx, out, field->sub_layout, &field_value->as.struct_val);
        return;
    }
    if (field && cg_static_struct_ct_string_supported(field_value, field->native_type)) {
        cg_emit_static_str_value_initializer(ctx, out, field_value->as.string_val);
        return;
    }
    cg_emit_static_struct_scalar(out, field ? field->native_type : XR_NATIVE_VALUE, field_value);
}

static void emit_static_struct_field_decl(FILE *out, const XrAggregateLayout *sl, int64_t idx,
                                          const char *name, const char *prefix) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
        emit_static_aggregate_decl_head(out, field->sub_layout);
        for (uint16_t i = 0; i < field->sub_layout->field_count; i++) {
            char fname[128];
            cg_struct_field_c_name(field->sub_layout, i, fname, sizeof(fname));
            emit_static_struct_field_decl(out, field->sub_layout, i, fname, prefix);
            fprintf(out, "; ");
        }
        fprintf(out, "} %s", name);
        return;
    }
    emit_struct_field_decl(out, sl, idx, name, prefix);
}

static void cg_emit_static_struct_initializer(XiCgenCtx *ctx, FILE *out,
                                              const XrAggregateLayout *sl,
                                              const XrCtStructValue *st) {
    if (sl && sl->kind == XR_AGG_LAYOUT_UNION) {
        int active_idx = cg_static_union_ct_active_field_index(sl, st);
        fprintf(out, "{");
        if (active_idx >= 0) {
            char fname[128];
            const XrCtValue *field_value =
                cg_static_struct_ct_field_value(st, sl, (uint16_t) active_idx);
            cg_struct_field_c_name(sl, active_idx, fname, sizeof(fname));
            fprintf(out, ".%s = ", fname);
            cg_emit_static_struct_field_initializer(ctx, out, sl, (uint16_t) active_idx,
                                                    field_value);
        }
        fprintf(out, "}");
        return;
    }
    fprintf(out, "{");
    for (uint16_t i = 0; sl && i < sl->field_count; i++) {
        char fname[128];
        const XrCtValue *field_value = cg_static_struct_ct_field_value(st, sl, i);
        if (i > 0)
            fprintf(out, ", ");
        cg_struct_field_c_name(sl, i, fname, sizeof(fname));
        fprintf(out, ".%s = ", fname);
        cg_emit_static_struct_field_initializer(ctx, out, sl, i, field_value);
    }
    fprintf(out, "}");
}

static bool cg_emit_freestanding_static_struct_defs(XiCgenCtx *ctx, FILE *out,
                                                    const XiModule *module, const char *prefix) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        const XrAggregateLayout *sl = NULL;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_struct_literal_in_module(ctx, module, slot, &sl, &value))
            continue;
        const XrCtStructValue *st = &value->as.struct_val;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        cg_emit_static_const_storage(out, lit);
        emit_static_aggregate_decl_head(out, sl);
        for (uint16_t i = 0; i < sl->field_count; i++) {
            char fname[128];
            cg_struct_field_c_name(sl, i, fname, sizeof(fname));
            emit_static_struct_field_decl(out, sl, i, fname, prefix);
            fprintf(out, "; ");
        }
        fprintf(out, "} ");
        cg_emit_static_struct_name(ctx, out, module, slot);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = ");
        cg_emit_static_struct_initializer(ctx, out, sl, st);
        fprintf(out, ";\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static bool cg_emit_freestanding_static_tuple_defs(XiCgenCtx *ctx, FILE *out,
                                                   const XiModule *module) {
    if (!ctx || !out || !module || !ctx->freestanding_profile || !module->slot_const_literals)
        return false;
    bool emitted = false;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        XrType *tuple_type = NULL;
        const XrCtValue *value = NULL;
        if (!cg_freestanding_static_tuple_literal(ctx, slot, &tuple_type, &value))
            continue;
        const XiConstLiteral *lit = &module->slot_const_literals[slot];
        const XrCtTupleValue *tuple = &value->as.tuple_val;
        cg_emit_static_const_storage(out, lit);
        fprintf(out, "struct { ");
        int count = cg_static_tuple_type_count(tuple_type);
        for (uint16_t i = 0; i < (uint16_t) count; i++)
            cg_emit_static_tuple_field_decl(out, tuple_type, i);
        fprintf(out, "} ");
        cg_emit_static_tuple_name(ctx, out, module, slot);
        emit_aot_const_data_attrs(out, lit);
        fprintf(out, " = ");
        cg_emit_static_tuple_initializer(ctx, out, tuple_type, tuple);
        fprintf(out, ";\n");
        emitted = true;
    }
    if (emitted)
        fprintf(out, "\n");
    return emitted;
}

static void cg_emit_static_struct_field_lvalue_in_module(XiCgenCtx *ctx, FILE *out,
                                                         const XiModule *module,
                                                         const XrAggregateLayout *sl, int64_t slot,
                                                         int64_t field_idx) {
    char fname[128];
    cg_emit_static_struct_name(ctx, out, module, slot);
    cg_struct_field_c_name(sl, field_idx, fname, sizeof(fname));
    fprintf(out, ".%s", fname);
}

#define CG_STATIC_TUPLE_PATH_MAX 8

typedef struct {
    int64_t slot;
    XrType *type;
    uint16_t depth;
    int64_t fields[CG_STATIC_TUPLE_PATH_MAX];
} CgStaticTuplePath;

static bool cg_static_tuple_path_append_field(CgStaticTuplePath *path, int64_t field_idx) {
    if (!path || !path->type || path->depth >= CG_STATIC_TUPLE_PATH_MAX || field_idx < 0 ||
        field_idx >= cg_static_tuple_type_count(path->type))
        return false;
    XrType *elem_type = cg_static_tuple_type_element(path->type, (int) field_idx);
    if (!elem_type || elem_type->kind != XR_KIND_TUPLE)
        return false;
    path->fields[path->depth++] = field_idx;
    path->type = elem_type;
    return true;
}

static bool cg_freestanding_static_tuple_object_path_depth(XiCgenCtx *ctx, const XiValue *value,
                                                           CgStaticTuplePath *out_path, int depth) {
    if (!ctx || !value || depth > CG_STATIC_TUPLE_PATH_MAX)
        return false;
    const XiValue *v = cg_unwrap_identity_value(value);
    XrType *type = NULL;
    int64_t slot = -1;
    if (cg_freestanding_static_tuple_value(ctx, v, &type, &slot)) {
        if (out_path)
            *out_path = (CgStaticTuplePath) {.slot = slot, .type = type, .depth = 0};
        return true;
    }
    if (!v || v->op != XI_TUPLE_GET || v->nargs < 1)
        return false;
    CgStaticTuplePath parent = {0};
    if (!cg_freestanding_static_tuple_object_path_depth(ctx, v->args[0], &parent, depth + 1))
        return false;
    if (!cg_static_tuple_path_append_field(&parent, v->aux_int))
        return false;
    if (out_path)
        *out_path = parent;
    return true;
}

static bool cg_freestanding_static_tuple_object_path(XiCgenCtx *ctx, const XiValue *value,
                                                     CgStaticTuplePath *out_path) {
    return cg_freestanding_static_tuple_object_path_depth(ctx, value, out_path, 0);
}

static void cg_emit_static_tuple_path_lvalue(XiCgenCtx *ctx, FILE *out,
                                             const CgStaticTuplePath *path) {
    if (!path)
        return;
    cg_emit_static_tuple_name(ctx, out, ctx ? ctx->module : NULL, path->slot);
    for (uint16_t i = 0; i < path->depth; i++)
        fprintf(out, ".f%" PRId64, path->fields[i]);
}

static bool cg_freestanding_static_struct_nested_field_value(
    XiCgenCtx *ctx, const XiValue *value, const XrAggregateLayout **out_parent_layout,
    int64_t *out_slot, int64_t *out_parent_field_idx, const XrAggregateLayout **out_nested_layout,
    const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    const XrAggregateLayout *parent = NULL;
    int64_t slot = -1;
    const XiModule *module = NULL;
    if (!cg_freestanding_static_struct_value_ex(ctx, v->args[0], &parent, &slot, &module) ||
        !parent)
        return false;
    int64_t parent_field_idx = -1;
    if (!cg_static_struct_field_access_index(v, parent, &parent_field_idx))
        return false;
    const XrAggregateFieldLayout *field = &parent->fields[parent_field_idx];
    if (!field || field->native_type != XR_NATIVE_STRUCT || !field->sub_layout)
        return false;
    if (out_parent_layout)
        *out_parent_layout = parent;
    if (out_slot)
        *out_slot = slot;
    if (out_parent_field_idx)
        *out_parent_field_idx = parent_field_idx;
    if (out_nested_layout)
        *out_nested_layout = field->sub_layout;
    if (out_module)
        *out_module = module;
    return true;
}

static void cg_emit_static_struct_nested_field_lvalue(XiCgenCtx *ctx, FILE *out,
                                                      const XiModule *module,
                                                      const XrAggregateLayout *parent, int64_t slot,
                                                      int64_t parent_field_idx,
                                                      const XrAggregateLayout *nested,
                                                      int64_t nested_field_idx) {
    char fname[128];
    cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, parent, slot, parent_field_idx);
    cg_struct_field_c_name(nested, nested_field_idx, fname, sizeof(fname));
    fprintf(out, ".%s", fname);
}

static bool cg_freestanding_static_struct_fixed_array_field_value(
    XiCgenCtx *ctx, const XiValue *value, const XrAggregateLayout **out_layout, int64_t *out_slot,
    int64_t *out_field_idx, const XrAggregateFieldLayout **out_field, const XiModule **out_module) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    const XrAggregateLayout *sl = NULL;
    int64_t slot = -1;
    const XiModule *module = NULL;
    if (!cg_freestanding_static_struct_value_ex(ctx, v->args[0], &sl, &slot, &module) || !sl)
        return false;
    int64_t field_idx = -1;
    if (!cg_static_struct_field_access_index(v, sl, &field_idx))
        return false;
    const XrAggregateFieldLayout *field = &sl->fields[field_idx];
    if (!cg_static_struct_native_fixed_array_supported(field))
        return false;
    if (out_layout)
        *out_layout = sl;
    if (out_slot)
        *out_slot = slot;
    if (out_field_idx)
        *out_field_idx = field_idx;
    if (out_field)
        *out_field = field;
    if (out_module)
        *out_module = module;
    return true;
}

static bool emit_static_struct_field_get_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || (v->op != XI_AGG_GET && v->op != XI_LOAD_FIELD) || v->nargs < 1)
        return false;
    const XrAggregateLayout *nested_parent = NULL;
    const XrAggregateLayout *nested_sl = NULL;
    int64_t nested_slot = -1;
    int64_t nested_parent_field_idx = -1;
    const XiModule *nested_module = NULL;
    if (cg_freestanding_static_struct_nested_field_value(ctx, v->args[0], &nested_parent,
                                                         &nested_slot, &nested_parent_field_idx,
                                                         &nested_sl, &nested_module) &&
        nested_sl) {
        int64_t field_idx = -1;
        if (!cg_static_struct_field_access_index(v, nested_sl, &field_idx))
            return false;
        const XrAggregateFieldLayout *field = &nested_sl->fields[field_idx];
        if (cg_static_struct_native_fixed_array_supported(field)) {
            fprintf(out, "xr_array_ref((void *)&");
            cg_emit_static_struct_nested_field_lvalue(ctx, out, nested_module, nested_parent,
                                                      nested_slot, nested_parent_field_idx,
                                                      nested_sl, field_idx);
            fprintf(out, "[0], %u, %u)",
                    (unsigned) cg_static_struct_array_ref_elem_native_type(field),
                    (unsigned) field->elem_count);
            return true;
        }
        if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
            fprintf(out, "xr_aggregate_ref((void *)&");
            cg_emit_static_struct_nested_field_lvalue(ctx, out, nested_module, nested_parent,
                                                      nested_slot, nested_parent_field_idx,
                                                      nested_sl, field_idx);
            fprintf(out, ", (uint16_t)sizeof(");
            cg_emit_static_struct_nested_field_lvalue(ctx, out, nested_module, nested_parent,
                                                      nested_slot, nested_parent_field_idx,
                                                      nested_sl, field_idx);
            fprintf(out, "))");
            return true;
        }
        uint8_t native_type = field->native_type;
        XrRep field_rep = cg_struct_native_rep(native_type);
        const char *conv_suffix =
            emit_conversion_prefix(out, v->type, field_rep, cg_value_plan_storage_rep(ctx, v));
        if (field_rep == XR_REP_F64)
            fprintf(out, "(double)");
        else if (field_rep == XR_REP_I64)
            fprintf(out, "(int64_t)");
        cg_emit_static_struct_nested_field_lvalue(ctx, out, nested_module, nested_parent,
                                                  nested_slot, nested_parent_field_idx, nested_sl,
                                                  field_idx);
        emit_conversion_suffix(out, conv_suffix);
        return true;
    }
    const XrAggregateLayout *sl = NULL;
    int64_t slot = -1;
    const XiModule *module = NULL;
    if (!cg_freestanding_static_struct_value_ex(ctx, v->args[0], &sl, &slot, &module) || !sl)
        return false;
    int64_t field_idx = -1;
    if (!cg_static_struct_field_access_index(v, sl, &field_idx))
        return false;
    const XrAggregateFieldLayout *field = &sl->fields[field_idx];
    if (cg_static_struct_native_fixed_array_supported(field)) {
        XrRep result_rep = cg_value_plan_storage_rep(ctx, v);
        if (result_rep == XR_REP_PTR || result_rep == XR_REP_RAWPTR) {
            fprintf(out, "&");
            cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, sl, slot, field_idx);
            fprintf(out, "[0]");
            return true;
        }
        fprintf(out, "xr_array_ref((void *)&");
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, sl, slot, field_idx);
        fprintf(out, "[0], %u, %u)", (unsigned) cg_static_struct_array_ref_elem_native_type(field),
                (unsigned) field->elem_count);
        return true;
    }
    if (field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
        fprintf(out, "xr_aggregate_ref((void *)&");
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, sl, slot, field_idx);
        fprintf(out, ", (uint16_t)sizeof(");
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, sl, slot, field_idx);
        fprintf(out, "))");
        return true;
    }
    uint8_t native_type = sl->fields[field_idx].native_type;
    XrRep field_rep = cg_struct_native_rep(native_type);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, field_rep, cg_value_plan_storage_rep(ctx, v));
    if (field_rep == XR_REP_F64)
        fprintf(out, "(double)");
    else if (field_rep == XR_REP_I64)
        fprintf(out, "(int64_t)");
    cg_emit_static_struct_field_lvalue_in_module(ctx, out, module, sl, slot, field_idx);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_static_tuple_get_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!v || v->op != XI_TUPLE_GET || v->nargs < 1)
        return false;
    CgStaticTuplePath receiver = {0};
    if (!cg_freestanding_static_tuple_object_path(ctx, v->args[0], &receiver) || v->aux_int < 0 ||
        v->aux_int >= cg_static_tuple_type_count(receiver.type))
        return false;
    XrType *elem_type = cg_static_tuple_type_element(receiver.type, (int) v->aux_int);
    if (elem_type && elem_type->kind == XR_KIND_TUPLE) {
        fprintf(stderr, "[xi_cgen] ERROR: freestanding static nested tuple value must be consumed "
                        "through scalar/string .N fields before runtime lowering\n");
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return true;
    }
    uint8_t native_type = XR_NATIVE_VALUE;
    if (!cg_static_tuple_native_for_type(elem_type, &native_type))
        return false;
    XrRep elem_rep = cg_struct_native_rep(native_type);
    const char *conv_suffix =
        emit_conversion_prefix(out, v->type, elem_rep, cg_value_plan_storage_rep(ctx, v));
    if (elem_rep == XR_REP_F64)
        fprintf(out, "(double)");
    else if (elem_rep == XR_REP_I64)
        fprintf(out, "(int64_t)");
    cg_emit_static_tuple_path_lvalue(ctx, out, &receiver);
    fprintf(out, ".f%" PRId64, v->aux_int);
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool cg_is_identity_copy_or_move(const XiValue *v) {
    return v && (v->op == XI_MOVE || xi_copy_is_identity_alias(v));
}

static bool cg_is_static_const_ref_alias(const XiValue *v) {
    return v && v->nargs >= 1 &&
           (v->op == XI_BOX || v->op == XI_UNBOX || cg_is_identity_copy_or_move(v));
}

static bool cg_static_struct_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f, const XiValue *target,
                                           int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_struct_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_struct_fixed_array_field_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                             const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_INDEX_GET && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_struct_fixed_array_field_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_static_struct_nested_field_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *target, int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_LOAD_FIELD) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_struct_nested_field_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_struct_nested_field_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                              const XiValue *v) {
    if (!cg_freestanding_static_struct_nested_field_value(ctx, v, NULL, NULL, NULL, NULL, NULL))
        return false;
    return cg_static_struct_nested_field_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_struct_fixed_array_field_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                                   const XiValue *v) {
    if (!cg_freestanding_static_struct_fixed_array_field_value(ctx, v, NULL, NULL, NULL, NULL,
                                                               NULL))
        return false;
    return cg_static_struct_fixed_array_field_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_value_is_elided_static_struct_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *v) {
    if (!cg_freestanding_static_struct_value(ctx, v, NULL, NULL))
        return false;
    return cg_static_struct_ref_safe_uses(ctx, f, v, 0);
}

static bool cg_static_tuple_ref_safe_uses(XiCgenCtx *ctx, const XiFunc *f, const XiValue *target,
                                          int depth) {
    if (!ctx || !f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_TUPLE_GET && a == 0) {
                    if (cg_freestanding_static_tuple_object_path(ctx, v, NULL) &&
                        !cg_static_tuple_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_static_const_ref_alias(v) && a == 0) {
                    if (!cg_static_tuple_ref_safe_uses(ctx, f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_static_tuple_const_ref(XiCgenCtx *ctx, const XiFunc *f,
                                                      const XiValue *v) {
    if (!cg_freestanding_static_tuple_object_path(ctx, v, NULL))
        return false;
    return cg_static_tuple_ref_safe_uses(ctx, f, v, 0);
}

static const XiValue *cg_trace_struct_new_depth(const XiValue *v, int depth) {
    if (!v || depth > 8)
        return NULL;

    while (v && cg_is_identity_copy_or_move(v) && v->nargs >= 1) {
        if (++depth > 8)
            return NULL;
        v = v->args[0];
    }

    if (!v)
        return NULL;
    if (v->op == XI_AGG_NEW)
        return v;
    if (v->op != XI_PHI)
        return NULL;

    const XiValue *origin = NULL;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        const XiValue *arg_origin = cg_trace_struct_new_depth(arg, depth + 1);
        if (!arg_origin)
            return NULL;
        if (!origin)
            origin = arg_origin;
        else if (origin != arg_origin)
            return NULL;
    }
    return origin;
}

static const XiValue *cg_trace_struct_new(const XiValue *v) {
    return cg_trace_struct_new_depth(v, 0);
}

static const XiValue *cg_trace_struct_new_identity(const XiValue *v) {
    while (v && (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_RETAIN) && v->nargs >= 1)
        v = v->args[0];
    return (v && v->op == XI_AGG_NEW) ? v : NULL;
}

static const XrAggregateLayout *cg_type_struct_layout(const XrType *type) {
    if (!type || (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE) ||
        !type->instance.class_ref)
        return NULL;
    return type->instance.class_ref->struct_layout;
}

static const XrAggregateLayout *cg_struct_layout_for_shared_slot_in_func(const XiFunc *f,
                                                                         int slot) {
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiValue *origin = cg_trace_struct_new_identity(v->args[0]);
            if (origin && cg_struct_native_heap_supported((const XrAggregateLayout *) origin->aux))
                return (const XrAggregateLayout *) origin->aux;
        }
    }
    return NULL;
}

static const XrAggregateLayout *cg_struct_layout_for_shared_slot(const XiCgenCtx *ctx,
                                                                 const XiFunc *f, int slot) {
    const XrAggregateLayout *sl = cg_struct_layout_for_shared_slot_in_func(f, slot);
    if (sl)
        return sl;
    if (ctx && ctx->module && ctx->module->init != f)
        return cg_struct_layout_for_shared_slot_in_func(ctx->module->init, slot);
    return NULL;
}

static bool cg_value_traces_to_heap_struct_shared_depth(const XiCgenCtx *ctx, const XiFunc *f,
                                                        const XiValue *v,
                                                        const XrAggregateLayout **out_layout,
                                                        int *out_slot, int depth) {
    if (!v || depth > 8)
        return false;
    while (v && (cg_is_identity_copy_or_move(v) || v->op == XI_RETAIN) && v->nargs >= 1) {
        if (++depth > 8)
            return false;
        v = v->args[0];
    }
    if (!v)
        return false;

    if (v->op == XI_GET_SHARED) {
        int slot = (int) v->aux_int;
        const XrAggregateLayout *sl = cg_struct_layout_for_shared_slot(ctx, f, slot);
        if (!cg_struct_native_heap_supported(sl))
            return false;
        if (out_layout)
            *out_layout = sl;
        if (out_slot)
            *out_slot = slot;
        return true;
    }

    if (v->op != XI_PHI || v->nargs == 0)
        return false;

    bool saw_base = false;
    const XrAggregateLayout *sl = NULL;
    int slot = -1;
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg == v)
            continue;
        const XrAggregateLayout *arg_layout = NULL;
        int arg_slot = -1;
        if (!cg_value_traces_to_heap_struct_shared_depth(ctx, f, arg, &arg_layout, &arg_slot,
                                                         depth + 1))
            return false;
        if (!saw_base) {
            saw_base = true;
            sl = arg_layout;
            slot = arg_slot;
            continue;
        }
        if (slot != arg_slot || !cg_struct_layout_same_shape(sl, arg_layout))
            return false;
    }
    if (!saw_base || !cg_struct_native_heap_supported(sl))
        return false;
    if (out_layout)
        *out_layout = sl;
    if (out_slot)
        *out_slot = slot;
    return true;
}

static bool cg_value_traces_to_heap_struct_shared(const XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *v,
                                                  const XrAggregateLayout **out_layout,
                                                  int *out_slot) {
    return cg_value_traces_to_heap_struct_shared_depth(ctx, f, v, out_layout, out_slot, 0);
}

static const XrAggregateLayout *cg_value_struct_layout(XiCgenCtx *ctx, const XiFunc *f,
                                                       const XiValue *v) {
    if (!v)
        return NULL;

    const XiValue *cur = v;
    for (int depth = 0; cur && depth <= 8; depth++) {
        if (cur->op == XI_AGG_NEW)
            return (const XrAggregateLayout *) cur->aux;
        if ((cur->op == XI_COPY || cur->op == XI_MOVE || cur->op == XI_RETAIN) && cur->nargs >= 1) {
            cur = cur->args[0];
            continue;
        }
        break;
    }

    const XrAggregateLayout *shared_layout = NULL;
    if (cg_value_traces_to_heap_struct_shared(ctx, f, v, &shared_layout, NULL))
        return shared_layout;

    return cg_type_struct_layout(v->type);
}

static bool emit_struct_aggregate_box_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XiValue *value, const char *prefix) {
    if (!ctx || !out || !value || !cg_value_plan_is_struct_aggregate(ctx, value))
        return false;
    const XrAggregateLayout *sl = cg_value_struct_layout(ctx, f, value);
    if (!cg_struct_native_heap_supported(sl))
        return false;
    const XaotValuePlan *plan = cg_value_plan(ctx, value);
    char tname_buf[128];
    const char *tname = (plan && plan->rep.c_type) ? plan->rep.c_type : NULL;
    if (!tname || !tname[0]) {
        cg_struct_heap_type_name(tname_buf, sizeof(tname_buf), prefix, sl);
        tname = tname_buf;
    }

    fprintf(out, "({ %s *_s = (%s*)xrt_arc_alloc(sizeof(%s)); *_s = ", tname, tname, tname);
    emit_vref(out, value);
    fprintf(out, "; ");
    if (xr_aggregate_layout_header_size(sl) == 0) {
        fprintf(out, "xr_aggregate_ref(_s, (uint16_t)sizeof(%s)); })", tname);
    } else {
        fprintf(out,
                "_s->_size = (uint32_t)sizeof(%s); _s->_layout = UINT32_C(%" PRIu32 "); "
                "xr_mkptr(_s, XR_TAG_AGG_REF); })",
                tname, (uint32_t) cg_struct_layout_hash(sl));
    }
    return true;
}

static bool cg_heap_struct_shared_alias_safe_uses(const XiCgenCtx *ctx, const XiFunc *f,
                                                  const XiValue *target, int slot, int depth) {
    if (!f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] != target)
                    continue;
                if (&phi->value == target)
                    continue;
                int alias_slot = -1;
                if (!cg_value_traces_to_heap_struct_shared(ctx, f, &phi->value, NULL,
                                                           &alias_slot) ||
                    alias_slot != slot)
                    return false;
                if (!cg_heap_struct_shared_alias_safe_uses(ctx, f, &phi->value, slot, depth + 1))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_AGG_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_identity_copy_or_move(v) && a == 0) {
                    int alias_slot = -1;
                    if (!cg_value_traces_to_heap_struct_shared(ctx, f, v, NULL, &alias_slot) ||
                        alias_slot != slot)
                        return false;
                    if (!cg_heap_struct_shared_alias_safe_uses(ctx, f, v, slot, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_heap_struct_alias(const XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    const XiValue *target = v;
    if (!v)
        return false;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1)
        target = v->args[0];
    int slot = -1;
    if (!cg_value_traces_to_heap_struct_shared(ctx, f, target, NULL, &slot))
        return false;
    return cg_heap_struct_shared_alias_safe_uses(ctx, f, target, slot, 0);
}

/* Check whether `target` is only observed through field operations or
 * same-origin identity nodes. */
static bool cg_struct_uses_safe_depth(const XiFunc *f, const XiValue *target, const XiValue *origin,
                                      int depth) {
    if (depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] != target)
                    continue;
                if (&phi->value == target)
                    continue;
                if (cg_trace_struct_new(&phi->value) != origin)
                    return false;
                if (!cg_struct_uses_safe_depth(f, &phi->value, origin, depth + 1))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_AGG_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_identity_copy_or_move(v) && a == 0) {
                    if (cg_trace_struct_new(v) != origin)
                        return false;
                    if (!cg_struct_uses_safe_depth(f, v, origin, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

/* Check if XI_AGG_NEW value can be inlined as a local C struct.
 * True iff all transitive same-origin uses are field get/set ops. */
static bool cg_struct_can_inline(const XiFunc *f, const XiValue *target) {
    XR_DCHECK(f != NULL && target != NULL, "cg_struct_can_inline: NULL");
    const XiValue *origin = cg_trace_struct_new(target);
    return origin == target && cg_struct_uses_safe_depth(f, target, origin, 0);
}

static bool cg_value_traces_to_inlined_struct(const XiFunc *f, const XiValue *v) {
    const XiValue *origin = cg_trace_struct_new(v);
    return origin && cg_struct_can_inline(f, origin);
}

static bool cg_value_only_used_by_layout_struct_new(const XiFunc *f, const XiValue *target) {
    bool seen = false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if (v->op == XI_AGG_NEW && a == 0 && v->aux) {
                    seen = true;
                    continue;
                }
                /* dup/drop the ownership pass attached to the type operand: the
                 * shared slot holds the tagged-int type id, so the retain/release
                 * is a runtime no-op and is elided alongside the load itself. */
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                return false;
            }
        }
    }
    return seen;
}

/* The (tagged-int) type-id load consumed only by a layout-backed struct new, or
 * an ARC dup/drop the ownership pass attached to that load. AOT constructs the
 * value from the compile-time layout, so it never needs the runtime class
 * descriptor; retaining a tagged int is also a no-op. */
static bool cg_value_is_elided_layout_struct_type_load(const XiFunc *f, const XiValue *v) {
    if (!v)
        return false;
    const XiValue *target = v;
    if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && v->nargs >= 1)
        target = v->args[0];
    return target && target->op == XI_GET_SHARED &&
           cg_value_only_used_by_layout_struct_new(f, target);
}

static void emit_struct_field_ref(FILE *out, const XrAggregateLayout *sl, const XiValue *origin,
                                  int64_t idx) {
    char fname[128];
    cg_struct_field_c_name(sl, idx, fname, sizeof(fname));
    fprintf(out, "_st%u.%s", origin ? origin->id : 0, fname);
}

static void emit_struct_inline_field_get_expr(FILE *out, const XrAggregateLayout *sl,
                                              const XiValue *origin, int64_t idx,
                                              XrRep result_rep) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "xr_aggregate_ref(&");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, ", (uint16_t)sizeof(");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, "))");
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        if (result_rep == XR_REP_PTR || result_rep == XR_REP_RAWPTR) {
            fprintf(out, "&");
            emit_struct_field_ref(out, sl, origin, idx);
            fprintf(out, "[0]");
            return;
        }
        fprintf(out, "xr_array_ref(&");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, "[0], %u, %u)", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, ", XR_TAG_ARRAY)");
        return;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, ", XR_TAG_MAP)");
        return;
    }
    emit_struct_field_ref(out, sl, origin, idx);
}

static void emit_struct_field_store_value(XiCgenCtx *ctx, FILE *out, const XrAggregateLayout *sl,
                                          int64_t idx, const XiValue *value) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "(xrt_array_t*)(");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ").ptr");
        return;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "(xrt_map_t*)(");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ").ptr");
        return;
    }
    XrRep field_rep = cg_struct_field_rep(sl, idx);
    if (field_rep != XR_REP_TAGGED)
        fprintf(out, "(%s)", cg_struct_field_c_type(sl, idx));
    emit_value_as_rep_ctx(ctx, out, value, field_rep);
}

static void emit_struct_set_result_value(XiCgenCtx *ctx, FILE *out, const XiValue *value) {
    if (cg_value_plan_is_aggregate(ctx, value)) {
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        return;
    }
    emit_value_as_rep_ctx(ctx, out, value, cg_rep(value));
}

static void emit_struct_inline_field_set_expr(XiCgenCtx *ctx, FILE *out,
                                              const XrAggregateLayout *sl, const XiValue *origin,
                                              int64_t idx, const XiValue *value) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "(memcpy(&");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ".ptr, sizeof(");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, ")), ");
        emit_struct_set_result_value(ctx, out, value);
        fprintf(out, ")");
        return;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "(xrt_fixed_array_copy(&");
        emit_struct_field_ref(out, sl, origin, idx);
        fprintf(out, "[0], ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ", %u, %u), ", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        emit_struct_set_result_value(ctx, out, value);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "(");
    emit_struct_field_ref(out, sl, origin, idx);
    fprintf(out, " = ");
    emit_struct_field_store_value(ctx, out, sl, idx, value);
    fprintf(out, ")");
}

static bool cg_value_is_nested_struct_field_ref(const XiValue *v) {
    if (!v || v->op != XI_AGG_GET)
        return false;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) v->aux;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, v->aux_int);
    return field && field->native_type == XR_NATIVE_STRUCT && field->sub_layout &&
           cg_struct_native_heap_supported(sl) &&
           cg_struct_native_heap_supported(field->sub_layout);
}

static bool cg_nested_struct_ref_safe_uses(const XiFunc *f, const XiValue *target, int depth) {
    if (!f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_AGG_GET || v->op == XI_AGG_SET) && a == 0)
                    continue;
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                if (cg_is_identity_copy_or_move(v) && a == 0) {
                    if (!cg_nested_struct_ref_safe_uses(f, v, depth + 1))
                        return false;
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_nested_struct_ref(const XiFunc *f, const XiValue *v) {
    const XiValue *target = v;
    while (target && cg_is_identity_copy_or_move(target) && target->nargs >= 1)
        target = target->args[0];
    return cg_value_is_nested_struct_field_ref(target) &&
           cg_nested_struct_ref_safe_uses(f, target, 0);
}

static void emit_struct_field_boxed_value(FILE *out, const XrAggregateLayout *sl, int64_t idx,
                                          const XiValue *value) {
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (!field) {
        emit_value_as_rep(out, value, XR_REP_TAGGED);
        return;
    }

    switch (field->native_type) {
        case XR_NATIVE_F32:
        case XR_NATIVE_F64:
            fprintf(out, "XR_FROM_FLOAT(");
            emit_value_as_rep(out, value, XR_REP_F64);
            fprintf(out, ")");
            break;
        case XR_NATIVE_BOOL:
            fprintf(out, "XR_FROM_BOOL(");
            emit_value_as_rep(out, value, XR_REP_I64);
            fprintf(out, ")");
            break;
        case XR_NATIVE_I8:
        case XR_NATIVE_I16:
        case XR_NATIVE_I32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            fprintf(out, "XR_FROM_INT(");
            emit_value_as_rep(out, value, XR_REP_I64);
            fprintf(out, ")");
            break;
        default:
            emit_value_as_rep(out, value, XR_REP_TAGGED);
            break;
    }
}

static void emit_struct_runtime_field_get(XiCgenCtx *ctx, FILE *out, const XrAggregateLayout *sl,
                                          int64_t idx, const XiValue *object,
                                          const XrType *result_type, XrRep result_rep) {
    const char *fname =
        (sl && sl->field_names && idx >= 0 && idx < sl->field_count) ? sl->field_names[idx] : NULL;
    const char *conv_suffix = emit_conversion_prefix(out, result_type, XR_REP_TAGGED, result_rep);
    fprintf(out, "xrt_map_get_owned((xrt_map_t*)");
    emit_vref(out, object);
    fprintf(out, ".ptr, ");
    cg_emit_str_value(ctx, out, fname ? fname : "?");
    fprintf(out, ")");
    emit_conversion_suffix(out, conv_suffix);
}

static void emit_struct_runtime_field_set(XiCgenCtx *ctx, FILE *out, const XrAggregateLayout *sl,
                                          int64_t idx, const XiValue *object,
                                          const XiValue *value) {
    const char *fname =
        (sl && sl->field_names && idx >= 0 && idx < sl->field_count) ? sl->field_names[idx] : NULL;
    fprintf(out, "(xrt_map_set((xrt_map_t*)");
    emit_vref(out, object);
    fprintf(out, ".ptr, ");
    cg_emit_str_value(ctx, out, fname ? fname : "?");
    fprintf(out, ", ");
    emit_struct_field_boxed_value(out, sl, idx, value);
    fprintf(out, "), ");
    emit_value_as_rep(out, value, cg_rep(value));
    fprintf(out, ")");
}

static void emit_struct_fallback_new_expr(FILE *out, const XrAggregateLayout *sl,
                                          const char *prefix) {
    if (cg_struct_native_heap_supported(sl)) {
        char tname[128];
        cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
        if (xr_aggregate_layout_header_size(sl) == 0) {
            fprintf(out,
                    "({ %s *_s = (%s*)xrt_arc_alloc(sizeof(%s)); "
                    "xr_aggregate_ref(_s, (uint16_t)sizeof(%s)); })",
                    tname, tname, tname, tname);
        } else {
            fprintf(out,
                    "({ %s *_s = (%s*)xrt_arc_alloc(sizeof(%s)); _s->_size = "
                    "(uint32_t)sizeof(%s); _s->_layout = UINT32_C(%" PRIu32 "); "
                    "xr_mkptr(_s, XR_TAG_AGG_REF); })",
                    tname, tname, tname, tname, (uint32_t) cg_struct_layout_hash(sl));
        }
        return;
    }

    int64_t cap = sl && sl->field_count > 0 ? (int64_t) sl->field_count * 2 : 8;
    fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
}

static void emit_struct_heap_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                             const XrAggregateLayout *sl, const XiValue *object) {
    const XrAggregateLayout *alias_layout = NULL;
    int slot = -1;
    if (cg_value_traces_to_heap_struct_shared(ctx, f, object, &alias_layout, &slot) &&
        cg_struct_layout_hash(alias_layout) == cg_struct_layout_hash(sl)) {
        fprintf(out, "%s[%d].ptr", ctx->shared_name, slot);
        return;
    }
    emit_vref(out, object);
    fprintf(out, ".ptr");
}

static bool emit_struct_heap_nested_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiValue *object, const char *prefix);
static void emit_struct_field_lvalue(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                     const XrAggregateLayout *sl, int64_t idx,
                                     const XiValue *object, const char *prefix);

static void emit_struct_heap_field_lvalue(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XrAggregateLayout *sl, int64_t idx,
                                          const XiValue *object, const char *prefix) {
    const XiValue *origin = cg_trace_struct_new(object);
    if (origin && cg_struct_can_inline(f, origin) &&
        cg_struct_layout_same_shape((const XrAggregateLayout *) origin->aux, sl)) {
        emit_struct_field_ref(out, sl, origin, idx);
        return;
    }
    char tname[128];
    char fname[128];
    cg_struct_heap_type_name(tname, sizeof(tname), prefix, sl);
    cg_struct_field_c_name(sl, idx, fname, sizeof(fname));
    fprintf(out, "((%s*)", tname);
    if (!emit_struct_heap_nested_object_ptr_expr(ctx, out, f, object, prefix))
        emit_struct_heap_object_ptr_expr(ctx, out, f, sl, object);
    fprintf(out, ")->%s", fname);
}

static void emit_struct_field_lvalue(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                     const XrAggregateLayout *sl, int64_t idx,
                                     const XiValue *object, const char *prefix) {
    const XrAggregateLayout *static_layout = NULL;
    int64_t static_slot = -1;
    const XiModule *static_module = NULL;
    if (cg_freestanding_static_struct_value_ex(ctx, object, &static_layout, &static_slot,
                                               &static_module) &&
        cg_struct_layout_same_shape(static_layout, sl)) {
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, static_module, static_layout,
                                                     static_slot, idx);
        return;
    }
    const XiValue *origin = cg_trace_struct_new(object);
    if (origin && cg_struct_can_inline(f, origin) &&
        cg_struct_layout_same_shape((const XrAggregateLayout *) origin->aux, sl)) {
        emit_struct_field_ref(out, sl, origin, idx);
        return;
    }
    if (cg_value_plan_is_struct_aggregate(ctx, object)) {
        char fname[128];
        cg_struct_field_c_name(sl, idx, fname, sizeof(fname));
        emit_vref(out, object);
        fprintf(out, ".%s", fname);
        return;
    }
    emit_struct_heap_field_lvalue(ctx, out, f, sl, idx, object, prefix);
}

static bool emit_struct_heap_nested_object_ptr_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                    const XiValue *object, const char *prefix) {
    const XiValue *target = object;
    while (target && cg_is_identity_copy_or_move(target) && target->nargs >= 1)
        target = target->args[0];
    if (!cg_value_is_elided_nested_struct_ref(f, target))
        return false;
    const XrAggregateLayout *parent = (const XrAggregateLayout *) target->aux;
    fprintf(out, "&");
    emit_struct_field_lvalue(ctx, out, f, parent, target->aux_int, target->args[0], prefix);
    return true;
}

static bool emit_struct_heap_field_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XrAggregateLayout *sl, int64_t idx,
                                            const XiValue *object, XrRep result_rep,
                                            const char *prefix) {
    if (!cg_struct_native_heap_supported(sl) || idx < 0 || idx >= sl->field_count)
        return false;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "xr_aggregate_ref(&");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", (uint16_t)sizeof(");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, "))");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        if (result_rep == XR_REP_PTR || result_rep == XR_REP_RAWPTR) {
            fprintf(out, "&");
            emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
            fprintf(out, "[0]");
            return true;
        }
        fprintf(out, "xr_array_ref(&");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, "[0], %u, %u)", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", XR_TAG_ARRAY)");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_MAP_REF) {
        fprintf(out, "xr_mkptr(");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", XR_TAG_MAP)");
        return true;
    }
    emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
    return true;
}

static void emit_struct_fallback_field_get(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XrAggregateLayout *sl, int64_t idx,
                                           const XiValue *object, const XrType *result_type,
                                           XrRep result_rep, const char *prefix) {
    if (emit_struct_heap_field_get_expr(ctx, out, f, sl, idx, object, result_rep, prefix))
        return;
    emit_struct_runtime_field_get(ctx, out, sl, idx, object, result_type, result_rep);
}

static bool emit_struct_heap_field_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                            const XrAggregateLayout *sl, int64_t idx,
                                            const XiValue *object, const XiValue *value,
                                            const char *prefix) {
    if (!cg_struct_native_heap_supported(sl) || idx < 0 || idx >= sl->field_count)
        return false;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, idx);
    if (field && field->native_type == XR_NATIVE_STRUCT) {
        fprintf(out, "(memcpy(&");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ", ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ".ptr, sizeof(");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, ")), ");
        emit_struct_set_result_value(ctx, out, value);
        fprintf(out, ")");
        return true;
    }
    if (field && field->native_type == XR_NATIVE_ARRAY) {
        fprintf(out, "(xrt_fixed_array_copy(&");
        emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
        fprintf(out, "[0], ");
        emit_value_as_rep_ctx(ctx, out, value, XR_REP_TAGGED);
        fprintf(out, ", %u, %u), ", (unsigned) field->elem_native_type,
                (unsigned) field->elem_count);
        emit_struct_set_result_value(ctx, out, value);
        fprintf(out, ")");
        return true;
    }
    fprintf(out, "(");
    emit_struct_field_lvalue(ctx, out, f, sl, idx, object, prefix);
    fprintf(out, " = ");
    emit_struct_field_store_value(ctx, out, sl, idx, value);
    fprintf(out, ")");
    return true;
}

static void emit_struct_fallback_field_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                           const XrAggregateLayout *sl, int64_t idx,
                                           const XiValue *object, const XiValue *value,
                                           const char *prefix) {
    if (emit_struct_heap_field_set_expr(ctx, out, f, sl, idx, object, value, prefix))
        return;
    emit_struct_runtime_field_set(ctx, out, sl, idx, object, value);
}

static const XiValue *cg_trace_fixed_array_field_ref(const XiValue *v) {
    while (v && cg_is_identity_copy_or_move(v) && v->nargs >= 1)
        v = v->args[0];
    if (!v || v->op != XI_AGG_GET || v->nargs < 1)
        return NULL;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) v->aux;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, v->aux_int);
    return field && field->native_type == XR_NATIVE_ARRAY && cg_struct_native_heap_supported(sl)
               ? v
               : NULL;
}

static bool cg_fixed_array_ref_safe_uses(const XiFunc *f, const XiValue *target, int depth) {
    if (!f || !target || depth > 8)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->control == target)
            return false;
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                if ((v->op == XI_INDEX_GET || v->op == XI_INDEX_SET) && a == 0)
                    continue;
                if (cg_is_identity_copy_or_move(v) && a == 0) {
                    if (!cg_fixed_array_ref_safe_uses(f, v, depth + 1))
                        return false;
                    continue;
                }
                if ((v->op == XI_RETAIN || v->op == XI_RELEASE) && a == 0)
                    continue;
                return false;
            }
        }
    }
    return true;
}

static bool cg_value_is_elided_fixed_array_ref(const XiFunc *f, const XiValue *v) {
    const XiValue *target = cg_trace_fixed_array_field_ref(v);
    return target && cg_fixed_array_ref_safe_uses(f, target, 0);
}

static bool cg_fixed_array_const_index(const XiValue *v, int64_t *out) {
    v = cg_unwrap_identity_value(v);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT)
        return false;
    if (out)
        *out = v->aux_int;
    return true;
}

static bool cg_fixed_array_control_proves_index_lt_count(const XiValue *control,
                                                         const XiValue *index,
                                                         uint16_t elem_count) {
    const XiValue *v = cg_unwrap_identity_value(control);
    int64_t bound = 0;
    return v && v->op == XI_LT && v->nargs >= 2 && cg_array_same_value(v->args[0], index) &&
           cg_fixed_array_const_index(v->args[1], &bound) && bound >= 0 &&
           bound <= (int64_t) elem_count;
}

static bool cg_fixed_array_index_bounds_proven(const XiValue *access, uint16_t elem_count) {
    if (!access || access->nargs < 2)
        return false;
    int64_t idx = 0;
    if (cg_fixed_array_const_index(access->args[1], &idx))
        return idx >= 0 && idx < (int64_t) elem_count;
    if (!access->block || !cg_array_value_known_nonnegative(access->args[1], NULL, 0) ||
        !cg_array_block_has_no_side_effect_before(access->block, access) ||
        access->block->npreds == 0)
        return false;
    for (uint16_t i = 0; i < access->block->npreds; i++) {
        const XiBlock *pred = access->block->preds[i];
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != access->block ||
            !cg_fixed_array_control_proves_index_lt_count(pred->control, access->args[1],
                                                          elem_count) ||
            !cg_array_block_has_no_side_effect_after(pred, pred->control))
            return false;
    }
    return true;
}

static bool emit_struct_fixed_array_index_get_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix) {
    const XiValue *ref = (v && v->op == XI_INDEX_GET && v->nargs >= 2)
                             ? cg_trace_fixed_array_field_ref(v->args[0])
                             : NULL;
    if (!ref)
        return false;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) ref->aux;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, ref->aux_int);
    if (!field)
        return false;
    XrRep elem_rep = cg_struct_native_rep(field->elem_native_type);
    const XrAggregateLayout *static_struct_layout = NULL;
    int64_t static_struct_slot = -1;
    int64_t static_struct_field_idx = -1;
    const XiModule *static_struct_module = NULL;
    bool static_struct_field_read = cg_freestanding_static_struct_fixed_array_field_value(
        ctx, ref, &static_struct_layout, &static_struct_slot, &static_struct_field_idx, NULL,
        &static_struct_module);
    bool unchecked = cg_fixed_array_index_bounds_proven(v, field->elem_count);
    const char *conv_suffix = emit_conversion_prefix(out, v->type, elem_rep, cg_rep(v));
    if (!unchecked) {
        /* OOB (incl. negative) throws E0430, catchable + matching the VM, rather
         * than abort()'ing the process. */
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) xrt_fixed_index_oob(_idx, %u); ",
                (unsigned) field->elem_count, (unsigned) field->elem_count);
    }
    if (elem_rep == XR_REP_F64)
        fprintf(out, "(double)");
    else if (elem_rep != XR_REP_TAGGED)
        fprintf(out, "(int64_t)");
    if (static_struct_field_read)
        cg_emit_static_struct_field_lvalue_in_module(ctx, out, static_struct_module,
                                                     static_struct_layout, static_struct_slot,
                                                     static_struct_field_idx);
    else
        emit_struct_field_lvalue(ctx, out, f, sl, ref->aux_int, ref->args[0], prefix);
    fprintf(out, "[");
    if (unchecked)
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
    else
        fprintf(out, "_idx");
    fprintf(out, "]");
    if (!unchecked)
        fprintf(out, "; })");
    emit_conversion_suffix(out, conv_suffix);
    return true;
}

static bool emit_struct_fixed_array_index_set_expr(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                                   const XiValue *v, const char *prefix) {
    const XiValue *ref = (v && v->op == XI_INDEX_SET && v->nargs >= 3)
                             ? cg_trace_fixed_array_field_ref(v->args[0])
                             : NULL;
    if (!ref)
        return false;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) ref->aux;
    const XrAggregateFieldLayout *field = cg_struct_field(sl, ref->aux_int);
    if (!field)
        return false;
    bool unchecked = cg_fixed_array_index_bounds_proven(v, field->elem_count);
    if (unchecked) {
        fprintf(out, "(");
    } else {
        fprintf(out, "({ int64_t _idx = ");
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
        fprintf(out, "; if (XR_UNLIKELY(_idx < 0 || _idx >= %u)) xrt_fixed_index_oob(_idx, %u); ",
                (unsigned) field->elem_count, (unsigned) field->elem_count);
    }
    emit_struct_field_lvalue(ctx, out, f, sl, ref->aux_int, ref->args[0], prefix);
    fprintf(out, "[");
    if (unchecked)
        emit_value_as_rep(out, v->args[1], XR_REP_I64);
    else
        fprintf(out, "_idx");
    fprintf(out, "] = (%s)", cg_struct_native_c_type(field->elem_native_type));
    emit_value_as_rep(out, v->args[2], cg_struct_native_rep(field->elem_native_type));
    fprintf(out, unchecked ? ")" : "; })");
    return true;
}
