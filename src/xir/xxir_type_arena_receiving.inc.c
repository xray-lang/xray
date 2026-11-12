/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena_receiving.inc.c - Exact identities below authentic Source roots
 */
static XirTypeIdentity type_identity(const XrXirTypeArena *arena,XrXirType type) {
    if (type==XR_XIR_ERROR) return (XirTypeIdentity){arena && arena->error_identity ? arena->error_identity : arena,type};
    if ((uint32_t)type<XR_XIR_CONSTRUCTED_TYPE_BASE) return (XirTypeIdentity){NULL,type};
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    return arena && index<arena->types.count && arena->identities ? arena->identities[index] :
        (XirTypeIdentity){arena,type};
}
XR_FUNC bool xir_type_arena_identity(const XrXirTypeArena *arena,
    XrXirType type, XirTypeIdentity *output) {
    if (!output || (uint32_t)type < XR_XIR_CONSTRUCTED_TYPE_BASE ||
        !xr_xir_type_node(arena ? &arena->types : NULL, type)) return false;
    *output = type_identity(arena, type);
    return true;
}
XR_FUNC bool xir_type_arena_same_type(const XrXirTypeArena *left,XrXirType left_type,
    const XrXirTypeArena *right,XrXirType right_type) {
    if ((uint32_t)left_type>=XR_XIR_CONSTRUCTED_TYPE_BASE && !xr_xir_type_node(left ? &left->types : NULL,left_type)) return false;
    if ((uint32_t)right_type>=XR_XIR_CONSTRUCTED_TYPE_BASE && !xr_xir_type_node(right ? &right->types : NULL,right_type)) return false;
    XirTypeIdentity a=type_identity(left,left_type),b=type_identity(right,right_type);
    return a.owner==b.owner && a.type==b.type;
}
XR_FUNC uint64_t xir_type_arena_lookup_work(const XrXirTypeArena *target) {
    return target ? (uint64_t)target->types.count+1 : 1;
}
XR_FUNC bool xir_type_arena_local_type(const XrXirTypeArena *target,
    const XrXirTypeArena *source,XrXirType type,XrXirType *output) {
    if (!output) return false;
    if ((uint32_t)type<XR_XIR_CONSTRUCTED_TYPE_BASE) {
        if (!xir_type_arena_same_type(source,type,target,type)) return false;
        *output=type;return true;
    }
    if (!source || !target || !xr_xir_type_node(&source->types,type)) return false;
    for (uint32_t i=0;i<target->types.count;++i) {
        XrXirType candidate=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        if (xir_type_arena_same_type(source,type,target,candidate)) {*output=candidate;return true;}
    }
    return false;
}
typedef struct TypeReceiving {
    const XrXirCompileContext *context;
    XrXirTypeArena *source,*target;
    XrXirType *map;
    uint32_t *queue,begin,end;
    XirTypeIdentity *identities;
    const XrXirTypeArena *error_identity;
} TypeReceiving;
static XrXirValueStatus type_receiving_edge(TypeReceiving *r,XrXirType source,XrXirType target) {

    if ((uint32_t)source<XR_XIR_CONSTRUCTED_TYPE_BASE || (uint32_t)target<XR_XIR_CONSTRUCTED_TYPE_BASE) {
        if (source!=target) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (source==XR_XIR_ERROR) r->error_identity=type_identity(r->source,source).owner;
        return XR_XIR_VALUE_OK;
    }
    uint32_t from=(uint32_t)source-XR_XIR_CONSTRUCTED_TYPE_BASE,to=(uint32_t)target-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (from>=r->source->types.count || to>=r->target->types.count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (r->map[from]) return r->map[from]==target ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
    XirTypeIdentity identity=type_identity(r->source,source),previous=r->identities[to];
    if (previous.owner!=r->target && (previous.owner!=identity.owner || previous.type!=identity.type))
        return XR_XIR_VALUE_BAD_ARGUMENT;

    r->map[from]=target;r->identities[to]=identity;r->queue[r->end++]=from;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus type_receiving_literal(XrXirLiteral a,XrXirLiteral b) {
    if (a.length!=b.length) return XR_XIR_VALUE_BAD_ARGUMENT;

    return !a.length || !memcmp(a.bytes,b.bytes,a.length) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static bool type_receiving_nominal_identity(const XrXirNominalIdentity *from,
    const XrXirNominalIdentity *to) {
    if (from->exported!=to->exported || from->arity!=to->arity || from->field_count!=to->field_count ||
        from->kind!=to->kind || from->variant_count!=to->variant_count || from->flags!=to->flags ||
        !!from->parent!=!!to->parent)
        return false;
    if (from->native.native_id!=to->native.native_id ||
        memcmp(from->native.source_fingerprint,to->native.source_fingerprint,sizeof(from->native.source_fingerprint)))
        return false;
    XrXirValueStatus status=type_receiving_literal(from->module,to->module);
    if (status==XR_XIR_VALUE_OK) status=type_receiving_literal(from->name,to->name);
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<from->field_count;++i) {

        if (from->fields[i].flags!=to->fields[i].flags) return false;
        status=type_receiving_literal(from->fields[i].name,to->fields[i].name);
    }
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<from->variant_count;++i) {

        if (from->variants[i].field_begin!=to->variants[i].field_begin ||
            from->variants[i].field_count!=to->variants[i].field_count) return false;
        status=type_receiving_literal(from->variants[i].name,to->variants[i].name);
    }
    return status==XR_XIR_VALUE_OK;
}
static bool type_receiving_nominal_header(void *owner,const XrXirTypes *source,
    const XrXirTypeNode *a,const XrXirTypes *target,const XrXirTypeNode *b) {
    (void)owner;
    if(a->nominal.argument_count!=b->nominal.argument_count ||
        a->nominal.field_count!=b->nominal.field_count)return false;
    uint32_t from=a->nominal.declaration,to=b->nominal.declaration;
    for(uint32_t depth=0;depth<source->nominals->count;++depth) {
        if(from>=source->nominals->count || to>=target->nominals->count)return false;
        const XrXirNominalIdentity *left=&source->nominals->identities[from];
        const XrXirNominalIdentity *right=&target->nominals->identities[to];
        if(!type_receiving_nominal_identity(left,right))return false;
        if(!left->parent)return true;
        from=left->parent-1;to=right->parent-1;
    }
    return false;
}
static XrXirValueStatus type_receiving_nominal(TypeReceiving *r,const XrXirTypeNode *a,const XrXirTypeNode *b) {
    if (!type_receiving_nominal_header(r,&r->source->types,a,&r->target->types,b)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<a->nominal.argument_count;++i)
        status=type_receiving_edge(r,a->nominal.arguments[i],b->nominal.arguments[i]);
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<a->nominal.field_count;++i)
        status=type_receiving_edge(r,a->nominal.fields[i],b->nominal.fields[i]);
    return status;
}
static XrXirValueStatus type_receiving_union(TypeReceiving *r,const XrXirTypeNode *a,const XrXirTypeNode *b) {
    for (uint32_t p=0;p<a->parameter_count;++p) {
        XrXirType source=a->parameters[p].type;bool found=false;
        const XrXirTypeNode *from=xr_xir_type_node(&r->source->types,source);
        if (!from || from->kind!=XR_XIR_TYPE_NOMINAL) return XR_XIR_VALUE_BAD_ARGUMENT;
        for (uint32_t q=0;q<b->parameter_count && !found;++q) {
            XrXirType target=b->parameters[q].type;
            const XrXirTypeNode *to=xr_xir_type_node(&r->target->types,target);
            if (!to || to->kind!=XR_XIR_TYPE_NOMINAL ||
                !type_receiving_nominal_header(r,&r->source->types,from,&r->target->types,to)) continue;
            XrXirErrorMatch match={&r->source->types,&r->target->types,NULL,0,source,target,
                r,type_receiving_nominal_header};
            XrXirStatus same=xr_xir_compile_error_substitution_accepts(r->context,&match);
            if (same==XR_XIR_BAD_TYPE) continue;
            if (same!=XR_XIR_OK) return arena_status(same);
            XrXirValueStatus status=type_receiving_edge(r,source,target);
            if (status!=XR_XIR_VALUE_OK) return status;
            found=true;
        }
        if (!found) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus type_receiving_node(TypeReceiving *r,uint32_t index) {
    const XrXirTypeNode *a=&r->source->types.nodes[index];
    const XrXirTypeNode *b=xr_xir_type_node(&r->target->types,r->map[index]);

    if (!b || a->kind!=b->kind || a->flags!=b->flags || a->parameter_span || b->parameter_span ||
        a->parameter_count!=b->parameter_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (a->kind==XR_XIR_TYPE_NOMINAL) return type_receiving_nominal(r,a,b);
    if (a->kind==XR_XIR_TYPE_UNION) return type_receiving_union(r,a,b);
    XrXirValueStatus status=type_receiving_edge(r,a->element,b->element);
    if (status==XR_XIR_VALUE_OK) status=type_receiving_edge(r,a->result,b->result);
    for (uint32_t p=0;status==XR_XIR_VALUE_OK && p<xr_xir_type_parameter_slots(a);++p) {

        if (a->parameters[p].mode!=b->parameters[p].mode || a->parameters[p].escaping != b->parameters[p].escaping) return XR_XIR_VALUE_BAD_ARGUMENT;
        status=type_receiving_edge(r,a->parameters[p].type,b->parameters[p].type);
    }
    return status;
}
static XrXirValueStatus type_arena_receive(const XrXirCompileContext *context,
    XrXirTypeArena *target,XrXirTypeArena *source,const XirTypeReceivingRoot *roots,uint32_t count) {
    if (!xir_compile_context_valid(context) || !source || !target || source==target ||
        target->received_from || target->identities || (count && !roots) ||
        context->resources!=target->context.resources ||
        atomic_load_explicit(&target->references,memory_order_acquire)!=1) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (const XrXirTypeArena *previous=source;previous;previous=previous->received_from) {

        if (previous==target) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    TypeReceiving r={.context=context,.source=source,.target=target};
    XrXirStatus allocated=XR_XIR_OK;
    if (source->types.count) r.map=xir_compile_calloc(context,source->types.count,sizeof(*r.map),&allocated);
    if (allocated==XR_XIR_OK && source->types.count) r.queue=xir_compile_calloc(context,source->types.count,sizeof(*r.queue),&allocated);
    if (allocated==XR_XIR_OK && target->types.count) r.identities=xir_compile_calloc(context,target->types.count,sizeof(*r.identities),&allocated);
    XrXirValueStatus status=arena_status(allocated);
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<target->types.count;++i) {

        r.identities[i]=(XirTypeIdentity){target,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i)};
    }
    for (uint32_t i=0;status==XR_XIR_VALUE_OK && i<count;++i) status=type_receiving_edge(&r,roots[i].source,roots[i].target);
    while (status==XR_XIR_VALUE_OK && r.begin<r.end) status=type_receiving_node(&r,r.queue[r.begin++]);

    /* Each arena allocation pins its own resource ledger. Retaining the source
     * keeps that ledger and the complete qualified-identity lineage alive. */
    if (status==XR_XIR_VALUE_OK && !xr_xir_compile_type_arena_retain(source)) status=XR_XIR_VALUE_REFCOUNT_LIMIT;
    if (status==XR_XIR_VALUE_OK) {
        target->received_from=source;target->identities=r.identities;target->error_identity=r.error_identity;r.identities=NULL;
    }
    xr_compile_resources_free(r.identities);xr_compile_resources_free(r.queue);xr_compile_resources_free(r.map);
    return status;
}

XR_FUNC XrXirValueStatus xir_compile_type_arena_receive(const XrXirCompileContext *context,
    XrXirTypeArena *target, XrXirTypeArena *source, const XirTypeReceivingRoot *roots, uint32_t count) {
    if (!xir_compile_context_valid(context) || !source ||
        context->resources != source->context.resources) return XR_XIR_VALUE_BAD_ARGUMENT;
    return type_arena_receive(context, target, source, roots, count);
}

static bool type_arena_i64_views_catalog(const XrXirTypeArena *arena) {
    if (!arena || arena->types.count != 6) return false;
    const XrXirTypeNode *cell = &arena->types.nodes[4], *slice = &arena->types.nodes[5];
    if (cell->kind != XR_XIR_TYPE_CELL || cell->element != XR_XIR_CONSTRUCTED_TYPE_BASE ||
        slice->kind != XR_XIR_TYPE_SLICE || slice->element != XR_XIR_I64) return false;
    static const XrXirType elements[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_U8, XR_XIR_U64};
    for (uint32_t i = 0; i < 4; ++i) {
        const XrXirTypeNode *node = &arena->types.nodes[i];
        if (node->kind != XR_XIR_TYPE_ARRAY || node->element != elements[i]) return false;
    }
    return true;
}
XR_FUNC XrXirValueStatus xir_native_type_arena_receive_i64_array(XrXirTypeArena *target,
    const XrXirValue *array) {
    if (!type_arena_i64_views_catalog(target) || !xr_xir_value_valid(array)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeArena *source = xr_xir_value_arena(array);
    const XrXirTypeNode *from = xr_xir_type_node(source ? &source->types : NULL, (XrXirType)array->type);
    if (!from || from->kind != XR_XIR_TYPE_ARRAY || from->element != XR_XIR_I64) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirTypeReceivingRoot root = {(XrXirType)array->type, XR_XIR_CONSTRUCTED_TYPE_BASE};
    XrXirValueStatus status = type_arena_receive(&target->context, target,
        (XrXirTypeArena *)source, &root, 1);
    if (status == XR_XIR_VALUE_OK) target->native_i64_views = true;
    return status;
}
XR_FUNC XrXirValueStatus xir_native_type_arena_derive_i64_views(const XrXirTypeArena *catalog,
    const XrXirValue *array, XrXirTypeArena **output) {
    if (!output || *output || !type_arena_i64_views_catalog(catalog)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirTypeArena *derived = NULL;
    XrXirValueStatus status = xr_xir_compile_type_arena_new(&catalog->context, &catalog->types, &derived);
    if (status == XR_XIR_VALUE_OK) status = xir_native_type_arena_receive_i64_array(derived, array);
    if (status != XR_XIR_VALUE_OK) xr_xir_compile_type_arena_drop(derived);
    else *output = derived;
    return status;
}
XR_FUNC bool xir_native_type_arena_i64_views(const XrXirTypeArena *arena) {
    return arena && arena->native_i64_views;
}
