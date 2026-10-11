/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Complete same-operation physical type correspondence, including unused fields.
 */
static XrXirStatus effect_body_literal_compare(const XrXirCompileContext *work,
    XrXirLiteral left,XrXirLiteral right,bool *same) {
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if ((left.length && !left.bytes) || (right.length && !right.bytes)) return XR_XIR_BAD_STRUCTURE;
    if (left.length!=right.length) return effect_body_difference(same);
    return effect_body_bytes_match(work,left.bytes,right.bytes,left.length,1,same);
}
static XrXirStatus effect_body_constraints_compare(const XrXirCompileContext *work,
    const XrXirConstraint *left,const XrXirConstraint *right,uint32_t count,bool *same) {
    if (count && (!left || !right)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t p=0;p<count && status==XR_XIR_OK;++p) {
        const XrXirConstraint *a=&left[p],*b=&right[p];
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if ((a->interface_count && !a->interfaces) || (b->interface_count && !b->interfaces))
            return XR_XIR_BAD_STRUCTURE;
        if (a->markers!=b->markers || a->interface_count!=b->interface_count) return effect_body_difference(same);
        for (uint32_t i=0;i<a->interface_count && status==XR_XIR_OK;++i)
            status=effect_body_application_match(work,&a->interfaces[i],&b->interfaces[i],same);
    }
    return status;
}
static XrXirStatus effect_body_nominal_compare(const XrXirCompileContext *work,
    const XrXirNominalDeclaration *a,const XrXirNominalDeclaration *b,bool *same) {
    if (!xir_compile_work(work,8+sizeof(a->native))) return XR_XIR_BUDGET;
    if ((a->field_count && !a->fields) || (b->field_count && !b->fields) ||
        (a->variant_count && !a->variants) || (b->variant_count && !b->variants) ||
        (a->parameter_count && !a->constraints) || (b->parameter_count && !b->constraints))
        return XR_XIR_BAD_STRUCTURE;
    if (a->exported!=b->exported || a->parameter_count!=b->parameter_count ||
        a->field_count!=b->field_count || a->kind!=b->kind || a->variant_count!=b->variant_count ||
        a->flags!=b->flags || a->native.native_id!=b->native.native_id ||
        memcmp(a->native.source_fingerprint,b->native.source_fingerprint,32)) return effect_body_difference(same);
    XrXirStatus status=effect_body_literal_compare(work,a->module,b->module,same);
    if (status==XR_XIR_OK) status=effect_body_literal_compare(work,a->name,b->name,same);
    if (status==XR_XIR_OK) status=effect_body_constraints_compare(work,a->constraints,b->constraints,a->parameter_count,same);
    for (uint32_t f=0;f<a->field_count && status==XR_XIR_OK;++f) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (a->fields[f].type!=b->fields[f].type || a->fields[f].flags!=b->fields[f].flags) {
            XrXirStatus difference=effect_body_difference(same);
            if (difference!=XR_XIR_OK) return difference;
        }
        status=effect_body_literal_compare(work,a->fields[f].name,b->fields[f].name,same);
    }
    for (uint32_t v=0;v<a->variant_count && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (a->variants[v].field_begin!=b->variants[v].field_begin || a->variants[v].field_count!=b->variants[v].field_count) {
            XrXirStatus difference=effect_body_difference(same);
            if (difference!=XR_XIR_OK) return difference;
        }
        status=effect_body_literal_compare(work,a->variants[v].name,b->variants[v].name,same);
    }
    return status;
}
static XrXirStatus effect_body_interface_compare(const XrXirCompileContext *work,
    const XrXirInterfaceDeclaration *a,const XrXirInterfaceDeclaration *b,bool *same) {
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    if ((a->parameter_count && !a->constraints) || (b->parameter_count && !b->constraints) ||
        (a->parent_count && !a->parents) || (b->parent_count && !b->parents) ||
        (a->method_count && !a->methods) || (b->method_count && !b->methods)) return XR_XIR_BAD_STRUCTURE;
    if (a->exported!=b->exported || a->parameter_count!=b->parameter_count ||
        a->parent_count!=b->parent_count || a->method_count!=b->method_count) return effect_body_difference(same);
    XrXirStatus status=effect_body_literal_compare(work,a->module,b->module,same);
    if (status==XR_XIR_OK) status=effect_body_literal_compare(work,a->name,b->name,same);
    if (status==XR_XIR_OK) status=effect_body_constraints_compare(work,a->constraints,b->constraints,a->parameter_count,same);
    for (uint32_t p=0;p<a->parent_count && status==XR_XIR_OK;++p)
        status=effect_body_application_match(work,&a->parents[p],&b->parents[p],same);
    for (uint32_t m=0;m<a->method_count && status==XR_XIR_OK;++m) {
        const XrXirInterfaceMethod *x=&a->methods[m],*y=&b->methods[m];
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if ((x->own_parameter_count && !x->constraints) || (y->own_parameter_count && !y->constraints))
            return XR_XIR_BAD_STRUCTURE;
        if (x->signature!=y->signature || x->receiver!=y->receiver || x->own_parameter_count!=y->own_parameter_count) {
            return effect_body_difference(same);
        }
        status=effect_body_literal_compare(work,x->name,y->name,same);
        if (status==XR_XIR_OK) status=effect_body_constraints_compare(work,x->constraints,y->constraints,x->own_parameter_count,same);
    }
    return status;
}
static XrXirStatus effect_body_type_tables_compare(const XrXirCompileContext *work,
    const XrXirTypes *a,const XrXirTypes *b,bool *same) {
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    if (!!a->nominals!=!!b->nominals || !!a->interfaces!=!!b->interfaces) return effect_body_difference(same);
    XrXirStatus status=XR_XIR_OK;
    if (a->nominals) {
        const XrXirNominalTable *x=a->nominals,*y=b->nominals;
        if (x->identities || y->identities) return XR_XIR_BAD_STRUCTURE;
        if ((x->count && !x->declarations) || (y->count && !y->declarations)) return XR_XIR_BAD_STRUCTURE;
        if (x->count!=y->count) return effect_body_difference(same);
        for (uint32_t n=0;n<x->count && status==XR_XIR_OK;++n)
            status=effect_body_nominal_compare(work,&x->declarations[n],&y->declarations[n],same);
    }
    if (status==XR_XIR_OK && a->interfaces) {
        const XrXirInterfaceTable *x=a->interfaces,*y=b->interfaces;
        if ((x->count && !x->declarations) || (y->count && !y->declarations)) return XR_XIR_BAD_STRUCTURE;
        if (x->count!=y->count) return effect_body_difference(same);
        for (uint32_t i=0;i<x->count && status==XR_XIR_OK;++i)
            status=effect_body_interface_compare(work,&x->declarations[i],&y->declarations[i],same);
    }
    return status;
}
static XrXirStatus effect_body_type_vector_compare(const XrXirCompileContext *work,
    const XrXirType *left,const XrXirType *right,uint32_t count,bool *same) {
    if (count && (!left || !right)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i=0;i<count;++i) {
        if (!xir_compile_work(work,2*sizeof(*left))) return XR_XIR_BUDGET;
        if (left[i]!=right[i]) {
            XrXirStatus status=effect_body_difference(same);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_body_type_nodes_compare(const XrXirCompileContext *work,
    const XrXirTypes *a,const XrXirTypes *b,bool *same) {
    if (!a || !b || (a->count && !a->nodes) || (b->count && !b->nodes)) return XR_XIR_BAD_STRUCTURE;
    if (a->count!=b->count) return effect_body_difference(same);
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t t=0;t<a->count && status==XR_XIR_OK;++t) {
        const XrXirTypeNode *x=&a->nodes[t],*y=&b->nodes[t];
        if (!xir_compile_work(work,2*sizeof(*x))) return XR_XIR_BUDGET;
        if ((x->parameter_count && !x->parameters) || (y->parameter_count && !y->parameters) ||
            (x->nominal.argument_count && !x->nominal.arguments) || (y->nominal.argument_count && !y->nominal.arguments) ||
            (x->nominal.field_count && !x->nominal.fields) || (y->nominal.field_count && !y->nominal.fields))
            return XR_XIR_BAD_STRUCTURE;
        if (x->kind!=y->kind || x->element!=y->element || x->result!=y->result || x->flags!=y->flags ||
            x->parameter_span!=y->parameter_span || x->parameter_count!=y->parameter_count ||
            x->nominal.declaration!=y->nominal.declaration || x->nominal.argument_count!=y->nominal.argument_count ||
            x->nominal.field_count!=y->nominal.field_count) return effect_body_difference(same);
        for (uint32_t p=0;p<x->parameter_count;++p) {
            if (!xir_compile_work(work,2*sizeof(*x->parameters))) return XR_XIR_BUDGET;
            if (x->parameters[p].mode!=y->parameters[p].mode || x->parameters[p].type!=y->parameters[p].type) {
                XrXirStatus difference=effect_body_difference(same);
                if (difference!=XR_XIR_OK) return difference;
            }
        }
        status=effect_body_type_vector_compare(work,x->nominal.arguments,y->nominal.arguments,x->nominal.argument_count,same);
        if (status==XR_XIR_OK) status=effect_body_type_vector_compare(work,x->nominal.fields,y->nominal.fields,
            x->nominal.field_count,same);
    }
    return status;
}
