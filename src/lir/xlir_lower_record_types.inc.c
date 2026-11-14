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
    return (flags & XR_XIR_NOMINAL_RECORD) != 0;
}
static bool lower_record_member(Lower *c, const XrXirTypeDescription *description,
    uint32_t index, XrXirFieldDescription *field) {
    if (xr_xir_compile_description_field(xr_xir_compile_artifact_context(c->request->closed),
        description, index, field) != XR_XIR_OK || !field->resolved)
        return lower_unsupported(c, "record field has no resolved Checked description");
    return true;
}
static bool lower_record_finish(Lower *c, XrXirType type, const XrXirTypeDescription *description) {
    uint32_t count = description->field_count;
    XrLirRecordField *fields = count ? lower_alloc(c, count, sizeof(*fields)) : NULL;
    if (count && !fields) return false;
    bool good = true;
    for (uint32_t n = 0; n < count && good; ++n) {
        XrXirFieldDescription member = {0};
        good = lower_record_member(c, description, n, &member);
        XrLirType field = good ? lower_type(c, member.type) : XR_LIR_TYPE_INVALID;
        if (good && !(field == XR_LIR_STRING || field == XR_LIR_I64 || field == XR_LIR_I1 || field == XR_LIR_F64 ||
            xr_lir_boxed_type(c->module, field))) good = lower_unsupported(c, "record field has no canonical physical carrier");
        fields[n] = (XrLirRecordField){member.name.bytes, member.name.length, field};
    }
    if (good) good = lower_build(c, xr_lir_record_type(c->module, fields, count,
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
        XrXirTypeDescription description = {0};
        bool record = node && node->kind == XR_XIR_TYPE_NOMINAL && lower_record_description(c, frame->type, &description);
        if (!node || (!record && node->kind != XR_XIR_TYPE_NULLABLE)) {
            good = lower_unsupported(c, "Nullable payload has no inline POD layout"); break;
        }
        uint32_t count = record ? description.field_count : 1u;
        if (frame->next < count) {
            XrXirType dependency = node->element;
            if (record) {
                XrXirFieldDescription member = {0};
                if (!lower_record_member(c, &description, frame->next, &member)) { good = false; break; }
                dependency = member.type;
            }
            ++frame->next;
            const XrXirTypeNode *child = xr_xir_type_node(c->source->types, dependency);
            if (!child || c->constructed_types[(uint32_t)dependency - XR_XIR_CONSTRUCTED_TYPE_BASE]) continue;
            if (child->kind == XR_XIR_TYPE_NULLABLE || child->kind == XR_XIR_TYPE_NOMINAL) {
                if (depth == capacity) { good = lower_unsupported(c, "cyclic canonical record dependency"); break; }
                frames[depth++] = (LowerRecordFrame){dependency, 0};
            } else if (!record && (child->kind == XR_XIR_TYPE_FIXED_ARRAY || child->kind == XR_XIR_TYPE_TUPLE))
                good = lower_receive_type(c, dependency);
            continue;
        }
        if (record) good = lower_record_finish(c, frame->type, &description);
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
