/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_entry.inc.c - Compatible grants installed at callee entry
 *
 * KEY CONCEPT:
 *   A body is checked under the temporary grants prepared by actual Call
 *   entry. These access relations neither admit a caller nor identify roots.
 */
static XrXirStatus view_access_entry_origin(ViewAccess *a,const XrXirViewLoanOrigin *event,bool *input) {
    *input=false;
    if (event->kind!=XR_XIR_VIEW_LOAN_INPUT || event->function!=a->function ||
        event->value>=a->body->parameter_count || event->repeated_phi ||
        !view_flags_at(a->resolver->check.module->types,a->resolver->check.flags,event->type)) return XR_XIR_OK;
    for (uint32_t p=0;p<event->choice_count;++p) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        if (event->choices[p].block>=a->body->block_count) return XR_XIR_BAD_STRUCTURE;
        if (a->repeated[event->choices[p].block]) return XR_XIR_OK;
    }
    *input=true;return XR_XIR_OK;
}
static XrXirStatus view_access_entry_selections(ViewAccess *a,const uint32_t *left,uint32_t left_count,
    const uint32_t *right,uint32_t right_count,bool *separate) {
    *separate=false;uint32_t common=left_count<right_count?left_count:right_count;
    for (uint32_t p=0;p<common;++p) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        if (left[p]!=right[p]) {*separate=true;break;}
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_entry_pair(ViewAccess *a,const XrXirViewLoanOrigin *left,
    const XrXirViewLoanOrigin *right,bool *safe) {
    *safe=false;bool l=false,r=false;XrXirStatus status=view_access_entry_origin(a,left,&l);
    if (status==XR_XIR_OK) status=view_access_entry_origin(a,right,&r);
    if (status!=XR_XIR_OK || !l || !r) return status;
    if (left->value!=right->value) {*safe=true;return XR_XIR_OK;}
    return view_access_entry_selections(a,left->selection,left->selection_count,
        right->selection,right->selection_count,safe);
}
static XrXirStatus view_access_entry_root_type(ViewAccess *a,const XrXirViewRootEvent *event,
    XrXirType *type,bool *input) {
    *input=false;
    if (event->kind!=XR_XIR_VIEW_ROOT_BORROWED_PARAMETER || event->root_function!=a->function ||
        event->root_value>=a->body->parameter_count || event->opaque_suffix || event->dynamic_identity ||
        event->step_count) return XR_XIR_OK;
    const XrXirTypes *types=a->resolver->check.module->types;
    *type=a->body->parameters[event->root_value];
    for (uint32_t p=0;p<event->selection_count;++p) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node=xr_xir_type_node(types,*type);uint32_t field=event->selection[p];
        if (!node) return XR_XIR_BAD_TYPE;
        if (field==UINT32_MAX) {
            if (node->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_BAD_TYPE;*type=node->element;
        } else {
            if (node->kind!=XR_XIR_TYPE_TUPLE || field>=node->parameter_count) return XR_XIR_BAD_TYPE;
            *type=node->parameters[field].type;
        }
    }
    *input=view_access_leaf(a,*type);return XR_XIR_OK;
}
static XrXirStatus view_access_entry_cell(ViewAccess *a,const XrXirViewRootEvent *event,bool *input) {
    *input=false;
    if (!a->cells || event->kind!=XR_XIR_VIEW_ROOT_PARAMETER || event->root_function!=a->function ||
        event->root_value>=a->body->parameter_count || event->step_count || event->selection_count ||
        event->opaque_suffix || event->dynamic_identity ||
        !xr_xir_type_is_cell(a->resolver->check.module->types,a->body->parameters[event->root_value])) return XR_XIR_OK;
    if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
    XrXirCellProofRole role=a->cells->role(a->cells->owner,a->function,event->root_value);
    *input=role==XR_XIR_CELL_PROOF_SCOPED_REF || role==XR_XIR_CELL_PROOF_LEXICAL_CLEANUP;return XR_XIR_OK;
}
static XrXirStatus view_access_entry_regions(ViewAccess *a,const XrXirViewRootEvent *left,
    const XrXirViewRootEvent *right,bool *safe) {
    *safe=false;bool l=false,r=false,lc=false,rc=false;XrXirType lt=XR_XIR_UNIT,rt=XR_XIR_UNIT;
    XrXirStatus status=view_access_entry_cell(a,left,&lc);
    if (status==XR_XIR_OK) status=view_access_entry_cell(a,right,&rc);
    if (status==XR_XIR_OK) status=view_access_entry_root_type(a,left,&lt,&l);
    if (status==XR_XIR_OK) status=view_access_entry_root_type(a,right,&rt,&r);
    if (status!=XR_XIR_OK) return status;
    if (lc || rc) {
        /* All scoped Cell loans and temporary view inputs commit jointly.
         * Their regions are compatible; physical root identity is unchanged. */
        *safe=(l || lc) && (r || rc) && left->root_value!=right->root_value;return XR_XIR_OK;
    }
    if (status!=XR_XIR_OK || !l || !r || (!view_access_mutable(a,lt) && !view_access_mutable(a,rt))) return status;
    /* Entry grants with a mutable leaf exclude overlap. Narrowing a grant
     * preserves that relation without equating scopes or physical roots. */
    XR_CHECK(left->root_function==right->root_function,"compatible input regions belong to one actual callee entry");
    if (left->root_value!=right->root_value) {*safe=true;return XR_XIR_OK;}
    return view_access_entry_selections(a,left->selection,left->selection_count,
        right->selection,right->selection_count,safe);
}
