/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_record_types.inc.c - Bounded receiving of canonical value shapes
 *
 * KEY CONCEPT:
 *   A heap stack closes dependencies before publishing a physical identity.
 *   Descriptions accept both declaration-backed and identity-backed pools.
 */
struct LowerRecordFrame { XrXirType type; uint32_t next; };

static bool lower_record_description(Lower *c, XrXirType type, XrXirTypeDescription *description) {
    if (xr_xir_compile_type_description(xr_xir_compile_artifact_context(c->request->closed),
        c->source->types, type, description) != XR_XIR_OK || description->kind != XR_XIR_TYPE_NOMINAL) return false;
    uint32_t flags = description->definition ? description->definition->flags : description->identity->flags;
    uint32_t native = description->definition ? description->definition->native.native_id : description->identity->native.native_id;
    return description->nominal_kind == XR_XIR_NOMINAL_STRUCT && !native && !(flags & ~XR_XIR_NOMINAL_MODULE_LITERAL);
}
static bool lower_class_parent_type(Lower *c, uint32_t declaration, XrXirType *parent) {
    const XrXirTypes *types = c->source->types;
    const XrXirNominalTable *table = types->nominals;
    if (!table || declaration >= table->count)
        return lower_unsupported(c, "class ancestry has no exact Checked declaration");
    if (!c->class_types) {
        c->class_types = lower_alloc(c, table->count, sizeof(*c->class_types));
        if (!c->class_types) return false;
        for (uint32_t i = 0; i < types->count; ++i) {
            const XrXirTypeNode *node = &types->nodes[i];
            XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
            if (!xr_xir_type_is_class(types, type) || node->nominal.argument_count) continue;
            uint32_t owner = node->nominal.declaration;
            if (c->class_types[owner]) {
                lower_error(c, XR_LIR_VERIFY_ERROR, "class declaration has multiple concrete Checked identities");
                return false;
            }
            c->class_types[owner] = type;
        }
    }
    *parent = c->class_types[declaration];
    if (!*parent) return lower_unsupported(c, "class ancestry has no exact non-generic Checked instance");
    return true;
}
static bool lower_class_description(Lower *c, XrXirType type, XrXirTypeDescription *description, XrXirType *base) {
    const XrXirArtifact *artifact = c->request->closed;
    if (!c->class_cycles) {
        XrXirStatus status = xr_xir_class_cycles_build(artifact, &c->class_cycles, NULL);
        if (status != XR_XIR_OK) {
            lower_error(c, status == XR_XIR_OUT_OF_MEMORY ? XR_LIR_OUT_OF_MEMORY : XR_LIR_VERIFY_ERROR,
                "class lowering requires Checked allocation-cycle facts");
            return false;
        }
    }
    XrXirClassCycleKind cycle;
    if (xr_xir_class_cycles_query(c->class_cycles, artifact, type, &cycle) != XR_XIR_OK ||
        cycle != XR_XIR_CLASS_CYCLE_ACYCLIC)
        return lower_unsupported(c, "class without a proven acyclic allocation layout");
    if (xr_xir_compile_type_description(xr_xir_compile_artifact_context(artifact), c->source->types,
        type, description) != XR_XIR_OK || description->nominal_kind != XR_XIR_NOMINAL_CLASS)
        return lower_unsupported(c, "class with unresolved Checked field types");
    uint32_t parent = description->definition ? description->definition->parent : description->identity->parent;
    return !parent || lower_class_parent_type(c, parent - 1, base);
}
static bool lower_record_member(Lower *c, const XrXirTypeDescription *description,
    uint32_t index, XrXirFieldDescription *field) {
    if (xr_xir_compile_description_field(xr_xir_compile_artifact_context(c->request->closed),
        description, index, field) != XR_XIR_OK || !field->resolved)
        return lower_unsupported(c, "record field has no resolved Checked description");
    return true;
}
static bool lower_record_finish(Lower *c, XrXirType type, const XrXirTypeDescription *description,
    bool class_type, XrXirType base) {
    uint32_t count = description->field_count;
    XrLirRecordField *fields = count ? lower_alloc(c, count, sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    uint32_t argument_count = description->instance->argument_count;
    XrLirRecordArgument *arguments = argument_count ? lower_alloc(c, argument_count, sizeof(*arguments)) : NULL;
    if (argument_count && !arguments) { xr_free(fields); return false; }
    bool good = true;
    for (uint32_t n = 0; n < count && good; ++n) {
        XrXirFieldDescription member = {0};
        good = lower_record_member(c, description, n, &member);
        XrLirType field = good ? lower_type(c, member.type) : XR_LIR_TYPE_INVALID;
        XrLirType element = good && field == XR_LIR_ARRAY ? lower_array_element(c, member.type) : XR_LIR_TYPE_INVALID;
        if (good && !(field == XR_LIR_STRING || field == XR_LIR_I64 || field == XR_LIR_I1 || field == XR_LIR_F64 ||
            (class_type && field == XR_LIR_ARRAY && element == XR_LIR_I64) ||
            xr_lir_inline_aggregate_type(c->module, field))) good = lower_unsupported(c, "record field has no canonical physical carrier");
        fields[n] = (XrLirRecordField){member.name.bytes, member.name.length, field, member.flags, element};
    }
    for (uint32_t a = 0; a < argument_count && good; ++a) {
        XrXirType actual = description->instance->arguments[a];
        arguments[a] = (XrLirRecordArgument){lower_type(c, actual), lower_array_element(c, actual)};
        if (!arguments[a].type) good = lower_unsupported(c, "struct type argument has no canonical physical carrier");
    }
    XrLirRecordIdentity identity = {.module=description->module.bytes, .module_length=description->module.length,
        .name=description->name.bytes, .name_length=description->name.length, .arguments=arguments,
        .argument_count=argument_count, .exported=description->definition ? description->definition->exported : description->identity->exported,
        .flags=class_type ? 0 : description->definition ? description->definition->flags : description->identity->flags};
    XrLirType physical = XR_LIR_TYPE_INVALID;
    if (good) good = lower_build(c, xr_lir_record_type(c->module, &identity, fields, count, &physical));
    if (good && class_type) {
        XrLirType parent = base ? lower_type(c, base) : XR_LIR_TYPE_INVALID;
        if (base && !parent) good = lower_unsupported(c, "class parent layout has no exact physical carrier");
        else good = lower_build(c, xr_lir_internal_class_type(c->module, physical, parent, &physical));
    }
    if (good) c->constructed_types[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE] = physical;
    xr_free(arguments);
    xr_free(fields);
    return good;
}
static bool lower_tuple_finish(Lower *c, XrXirType type, const XrXirTypeNode *node) {
    uint32_t count = node->parameter_count;
    if (!count || count > 65535) return lower_unsupported(c, "Tuple has no admitted positional arity");
    XrLirRecordField *fields = lower_alloc(c, count, sizeof(*fields));
    if (!fields) return false;
    for (uint32_t f = 0; f < count; ++f) {
        XrXirType field_type = node->parameters[f].type;
        XrLirType field = lower_type(c, field_type);
        XrLirType element = field == XR_LIR_ARRAY ? lower_array_element(c, field_type) : XR_LIR_TYPE_INVALID;
        if (field != XR_LIR_I64 && field != XR_LIR_I1 && field != XR_LIR_F64 && field != XR_LIR_STRING &&
            !(field == XR_LIR_ARRAY && element == XR_LIR_I64) && !xr_lir_inline_aggregate_type(c->module, field)) {
            xr_free(fields);
            return lower_unsupported(c, "Tuple field has no canonical inline value carrier");
        }
        fields[f] = (XrLirRecordField){.type=field,.array_element=element};
    }
    bool good = lower_build(c, xr_lir_tuple_type(c->module, fields, count,
        &c->constructed_types[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE]));
    xr_free(fields);
    return good;
}
static bool lower_receive_record_shapes(Lower *c, XrXirType type) {
    uint32_t capacity = c->source->types->count;
    if (!c->constructed_types) c->constructed_types = lower_alloc(c, capacity, sizeof(*c->constructed_types));
    if (!c->constructed_types) return false;
    if (c->constructed_types[(uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE]) return true;
    if (!c->constructed_frames) c->constructed_frames = lower_alloc(c, capacity, sizeof(*c->constructed_frames));
    LowerRecordFrame *frames = c->constructed_frames;
    if (!frames) return false;
    uint32_t depth = 1;
    frames[0] = (LowerRecordFrame){type, 0};
    bool good = true;
    while (depth && good) {
        LowerRecordFrame *frame = &frames[depth - 1];
        const XrXirTypeNode *node = xr_xir_type_node(c->source->types, frame->type);
        if (node && node->kind == XR_XIR_TYPE_NULLABLE && xr_xir_type_is_class(c->source->types, node->element))
            return lower_unsupported(c, "Nullable class requires a receiving optional owner layout");
        XrXirTypeDescription description = {0};
        XrXirType base = 0;
        bool class_type = node && node->kind == XR_XIR_TYPE_NOMINAL && xr_xir_type_is_class(c->source->types, frame->type);
        bool record = node && node->kind == XR_XIR_TYPE_NOMINAL &&
            (class_type ? lower_class_description(c, frame->type, &description, &base) : lower_record_description(c, frame->type, &description));
        if (class_type && !record) return false;
        bool callable = node && node->kind == XR_XIR_TYPE_CALLABLE;
        bool tuple = node && node->kind == XR_XIR_TYPE_TUPLE;
        if (!node || (!record && !callable && !tuple && node->kind != XR_XIR_TYPE_NULLABLE)) {
            good = lower_unsupported(c, "Nullable payload has no inline POD layout"); break;
        }
        uint32_t count = record ? description.field_count + description.instance->argument_count + (base != 0) :
            callable ? node->parameter_count + 1u : tuple ? node->parameter_count : 1u;
        if (frame->next < count) {
            XrXirType dependency = node->element;
            if (record) {
                if (frame->next < description.field_count) {
                    XrXirFieldDescription member = {0};
                    if (!lower_record_member(c, &description, frame->next, &member)) { good = false; break; }
                    dependency = member.type;
                } else {
                    uint32_t argument = frame->next - description.field_count;
                    dependency = argument < description.instance->argument_count ? description.instance->arguments[argument] : base;
                }
            } else if (callable) dependency = frame->next ? node->parameters[frame->next - 1].type : node->result;
            else if (tuple) dependency = node->parameters[frame->next].type;
            ++frame->next;
            const XrXirTypeNode *child = xr_xir_type_node(c->source->types, dependency);
            if (!child || c->constructed_types[(uint32_t)dependency - XR_XIR_CONSTRUCTED_TYPE_BASE]) continue;
            if (child->kind == XR_XIR_TYPE_NULLABLE || child->kind == XR_XIR_TYPE_NOMINAL ||
                child->kind == XR_XIR_TYPE_CALLABLE || child->kind == XR_XIR_TYPE_TUPLE) {
                if (depth == capacity) { good = lower_unsupported(c, "cyclic canonical record dependency"); break; }
                frames[depth++] = (LowerRecordFrame){dependency, 0};
            } else if (child->kind == XR_XIR_TYPE_FIXED_ARRAY)
                good = lower_receive_type(c, dependency);
            continue;
        }
        if (record) good = lower_record_finish(c, frame->type, &description, class_type, base);
        else if (callable) good = lower_receive_callable(c, frame->type, node);
        else if (tuple) good = lower_tuple_finish(c, frame->type, node);
        else {
            XrLirType element = lower_pod_element(c, node->element);
            if (!element || element == XR_LIR_ARRAY) good = lower_unsupported(c, "Nullable payload has no inline POD layout");
            else good = lower_build(c, xr_lir_nullable_type(c->module, element,
                &c->constructed_types[(uint32_t)frame->type - XR_XIR_CONSTRUCTED_TYPE_BASE]));
        }
        --depth;
    }
    return good;
}
