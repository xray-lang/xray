/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_import_types.inc.c - Exact data identities across independent pools
 *
 * KEY CONCEPT:
 *   Foreign nominal ordinals have no meaning in the destination. A temporary
 *   pair walk compares qualified declarations and every closed data component.
 */
typedef struct CtfeImportTypePair { XrXirType source,target; } CtfeImportTypePair;
typedef struct CtfeImportTypePairs {
    const XrXirCompileContext *context;
    CtfeImportTypePair *pairs;
    uint32_t count,capacity;
} CtfeImportTypePairs;
typedef struct CtfeImportNominal {
    XrXirLiteral module,name;
    uint32_t exported,arity,kind,flags,field_count,variant_count;
    const XrXirNominalField *declarations;
    const XrXirNominalFieldIdentity *identities;
    const XrXirNominalVariant *variants;
    XrXirNominalNativeRecord native;
} CtfeImportNominal;
static CtfeImportNominal ctfe_import_nominal(const XrXirTypes *types,const XrXirTypeNode *node) {
    uint32_t index=node->nominal.declaration;const XrXirNominalTable *table=types->nominals;
    if (table->declarations) {
        const XrXirNominalDeclaration *d=&table->declarations[index];
        return (CtfeImportNominal){d->module,d->name,d->exported,d->parameter_count,d->kind,d->flags,
            d->field_count,d->variant_count,d->fields,NULL,d->variants,d->native};
    }
    const XrXirNominalIdentity *d=&table->identities[index];
    return (CtfeImportNominal){d->module,d->name,d->exported,d->arity,d->kind,d->flags,
        d->field_count,d->variant_count,NULL,d->fields,d->variants,d->native};
}
static XrXirCtfeStatus ctfe_import_name(const XrXirCompileContext *context,XrXirLiteral source,XrXirLiteral target) {
    if (!xir_compile_work(context,(uint64_t)source.length+2*sizeof(source))) return XR_XIR_CTFE_BUDGET;
    return source.length==target.length && (!source.length || !memcmp(source.bytes,target.bytes,source.length))?
        XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARGUMENT;
}
static XrXirCtfeStatus ctfe_import_type_pair(CtfeImportTypePairs *work,XrXirType source,XrXirType target) {
    if (!xir_compile_work(work->context,sizeof(CtfeImportTypePair))) return XR_XIR_CTFE_BUDGET;
    if ((uint32_t)source<XR_XIR_CONSTRUCTED_TYPE_BASE || (uint32_t)target<XR_XIR_CONSTRUCTED_TYPE_BASE)
        return source==target?XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARGUMENT;
    for (uint32_t i=0;i<work->count;++i) {
        if (!xir_compile_work(work->context,sizeof(CtfeImportTypePair))) return XR_XIR_CTFE_BUDGET;
        if (work->pairs[i].source==source && work->pairs[i].target==target) return XR_XIR_CTFE_OK;
    }
    if (work->count==work->capacity) {
        uint64_t capacity=work->capacity?(uint64_t)work->capacity*2:1;
        if (capacity>UINT32_MAX || capacity>SIZE_MAX/sizeof(*work->pairs)) return XR_XIR_CTFE_BUDGET;
        void *memory=work->pairs;
        XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_resize(
            work->context->resources,&memory,(size_t)capacity*sizeof(*work->pairs))));
        if (status!=XR_XIR_CTFE_OK) return status;
        work->pairs=memory;work->capacity=(uint32_t)capacity;
    }
    work->pairs[work->count++]=(CtfeImportTypePair){source,target};return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_import_nominal_pair(CtfeImportTypePairs *work,
    const XrXirTypes *source_types,const XrXirTypeNode *source,
    const XrXirTypes *target_types,const XrXirTypeNode *target) {
    CtfeImportNominal from=ctfe_import_nominal(source_types,source),to=ctfe_import_nominal(target_types,target);
    if (!xir_compile_work(work->context,2*sizeof(from)+sizeof(from.native))) return XR_XIR_CTFE_BUDGET;
    if (from.exported!=to.exported || from.arity!=to.arity || from.kind!=to.kind || from.flags!=to.flags ||
        from.field_count!=to.field_count || from.variant_count!=to.variant_count ||
        from.native.native_id!=to.native.native_id ||
        memcmp(from.native.source_fingerprint,to.native.source_fingerprint,sizeof(from.native.source_fingerprint)) ||
        source->nominal.argument_count!=target->nominal.argument_count || source->nominal.field_count!=target->nominal.field_count)
        return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus status=ctfe_import_name(work->context,from.module,to.module);
    if (status==XR_XIR_CTFE_OK) status=ctfe_import_name(work->context,from.name,to.name);
    for (uint32_t i=0;status==XR_XIR_CTFE_OK && i<from.field_count;++i) {
        if (!xir_compile_work(work->context,2*sizeof(XrXirNominalFieldIdentity))) return XR_XIR_CTFE_BUDGET;
        XrXirLiteral left=from.declarations?from.declarations[i].name:from.identities[i].name;
        XrXirLiteral right=to.declarations?to.declarations[i].name:to.identities[i].name;
        uint32_t a=from.declarations?from.declarations[i].flags:from.identities[i].flags;
        uint32_t b=to.declarations?to.declarations[i].flags:to.identities[i].flags;
        if (a!=b) return XR_XIR_CTFE_BAD_ARGUMENT;
        status=ctfe_import_name(work->context,left,right);
        if (status==XR_XIR_CTFE_OK) status=ctfe_import_type_pair(work,source->nominal.fields[i],target->nominal.fields[i]);
    }
    for (uint32_t i=0;status==XR_XIR_CTFE_OK && i<from.variant_count;++i) {
        if (!xir_compile_work(work->context,2*sizeof(XrXirNominalVariant))) return XR_XIR_CTFE_BUDGET;
        if (from.variants[i].field_begin!=to.variants[i].field_begin || from.variants[i].field_count!=to.variants[i].field_count)
            return XR_XIR_CTFE_BAD_ARGUMENT;
        status=ctfe_import_name(work->context,from.variants[i].name,to.variants[i].name);
    }
    for (uint32_t i=0;status==XR_XIR_CTFE_OK && i<source->nominal.argument_count;++i)
        status=ctfe_import_type_pair(work,source->nominal.arguments[i],target->nominal.arguments[i]);
    return status;
}
static XrXirCtfeStatus ctfe_import_types_equal(const XrXirCompileContext *context,
    const XrXirTypes *source_types,XrXirType source,const XrXirTypes *target_types,XrXirType target) {
    CtfeImportTypePairs work={.context=context};XrXirCtfeStatus status=ctfe_import_type_pair(&work,source,target);
    for (uint32_t i=0;status==XR_XIR_CTFE_OK && i<work.count;++i) {
        if (!xir_compile_work(context,2*sizeof(XrXirTypeNode))) { status=XR_XIR_CTFE_BUDGET;break; }
        /* Enqueue can resize the pair vector; only immutable pool nodes remain borrowed. */
        const XrXirTypeNode *from=xr_xir_type_node(source_types,work.pairs[i].source);
        const XrXirTypeNode *to=xr_xir_type_node(target_types,work.pairs[i].target);
        if (!from || !to || from->kind!=to->kind || from->flags!=to->flags || from->parameter_count!=to->parameter_count ||
            from->parameter_span || to->parameter_span) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
        if (from->kind==XR_XIR_TYPE_NOMINAL) status=ctfe_import_nominal_pair(&work,source_types,from,target_types,to);
        else if (from->kind==XR_XIR_TYPE_TUPLE) {
            for (uint32_t p=0;status==XR_XIR_CTFE_OK && p<from->parameter_count;++p) {
                if (!xir_compile_work(context,2*sizeof(XrXirCallableParameter))) { status=XR_XIR_CTFE_BUDGET;break; }
                if (from->parameters[p].mode || to->parameters[p].mode) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
                status=ctfe_import_type_pair(&work,from->parameters[p].type,to->parameters[p].type);
            }
        } else if (from->kind==XR_XIR_TYPE_ARRAY || from->kind==XR_XIR_TYPE_NULLABLE)
            status=ctfe_import_type_pair(&work,from->element,to->element);
        else status=XR_XIR_CTFE_NOT_SUPPORTED;
    }
    xr_compile_resources_free(work.pairs);return status;
}
