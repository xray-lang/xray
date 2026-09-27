/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena_nominal.inc.c - Aligned closed declaration arena projection
 *
 * KEY CONCEPT:
 *   Sizing and copying walk identical charged spans in one allocation.
 */
typedef struct ArenaNominalCursor {
    char *base;
    uint64_t offset, limit, work;
    XrXirValueStatus status;
} ArenaNominalCursor;
static void *arena_nominal_span(ArenaNominalCursor *c, uint64_t count,
                                size_t size, size_t alignment) {
    if (c->status != XR_XIR_VALUE_OK || !count) return NULL;
    uint64_t padding = (alignment - c->offset % alignment) % alignment;
    if (c->offset > c->limit || padding > c->limit - c->offset ||
        count > (c->limit - c->offset - padding) / size || count > c->work) {
        c->status = XR_XIR_VALUE_LIMIT; return NULL;
    }
    c->offset += padding;
    void *span = c->base ? c->base + (size_t) c->offset : NULL;
    c->offset += count * size; c->work -= count;
    return span;
}
static XrXirLiteral arena_nominal_name(ArenaNominalCursor *c, XrXirLiteral source) {
    char *bytes = arena_nominal_span(c, (uint64_t) source.length + 1, 1, 1);
    if (bytes) { memcpy(bytes, source.bytes, source.length); bytes[source.length] = 0; }
    return (XrXirLiteral) {bytes, source.length};
}
static XrXirNominalTable *arena_nominals(ArenaNominalCursor *c,
                                       const XrXirNominalTable *source) {
    if (!source) return NULL;
    XrXirNominalTable *table = arena_nominal_span(c, 1, sizeof(*table), _Alignof(XrXirNominalTable));
    if (!source->identities || source->declarations) { c->status = XR_XIR_VALUE_BAD_ARGUMENT; return NULL; }
    XrXirNominalIdentity *identities = arena_nominal_span(c, source->count,
        sizeof(*identities), _Alignof(XrXirNominalIdentity));
    if (table && c->status == XR_XIR_VALUE_OK) *table = (XrXirNominalTable) {NULL, source->count, identities};
    for (uint32_t i = 0; i < source->count && c->status == XR_XIR_VALUE_OK; ++i) {
        const XrXirNominalIdentity *input = &source->identities[i];
        XrXirNominalIdentity d = {0};
        d.exported = input->exported; d.arity = input->arity;
        XrXirNominalFieldIdentity *fields = arena_nominal_span(c, input->field_count,
            sizeof(*fields), _Alignof(XrXirNominalFieldIdentity));
        d.fields = fields; d.field_count = input->field_count;
        d.module = arena_nominal_name(c, input->module);
        d.name = arena_nominal_name(c, input->name);
        for (uint32_t f = 0; f < input->field_count && c->status == XR_XIR_VALUE_OK; ++f) {
            XrXirNominalFieldIdentity field = input->fields[f];
            field.name = arena_nominal_name(c, field.name);
            if (fields && c->status == XR_XIR_VALUE_OK) fields[f] = field;
        }
        if (identities && c->status == XR_XIR_VALUE_OK) identities[i] = d;
    }
    return c->status == XR_XIR_VALUE_OK ? table : NULL;
}

static void arena_nominal_nodes(ArenaNominalCursor *c, const XrXirTypes *source, XrXirTypeNode *nodes) {
    if (!source) return;
    for (uint32_t i = 0; i < source->count && c->status == XR_XIR_VALUE_OK; ++i) {
        const XrXirNominalType *n = &source->nodes[i].nominal;
        XrXirType *arguments = arena_nominal_span(c, n->argument_count, sizeof(*arguments), _Alignof(XrXirType));
        XrXirType *fields = arena_nominal_span(c, n->field_count, sizeof(*fields), _Alignof(XrXirType));
        if (nodes && c->status == XR_XIR_VALUE_OK) {
            if (n->argument_count) memcpy(arguments, n->arguments, (size_t) n->argument_count * sizeof(*arguments));
            if (n->field_count) memcpy(fields, n->fields, (size_t) n->field_count * sizeof(*fields));
            nodes[i].nominal.arguments = arguments; nodes[i].nominal.fields = fields;
        }
    }
}
