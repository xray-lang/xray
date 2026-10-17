/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_structural_constraints.inc.c - Exact readonly fields in verified Record shapes
 */
#include "xxir_record_names_internal.h"
static XrXirStatus constraint_structural_shape(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirStructuralRequirement requirement,uint32_t count,
    const XrXirTypeNode **output) {
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    if (requirement.policy!=XR_XIR_STRUCTURAL_READONLY_RECEIVER) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *shape=xr_xir_record_signature(types,requirement.shape);
    if (!shape || shape->parameter_span>count) return XR_XIR_BAD_TYPE;
    XrXirStatus status=xr_xir_compile_type_expression_shape(context,types,requirement.shape,count);
    if (status==XR_XIR_OK) *output=shape;
    return status;
}
static XrXirStatus constraint_structural_overlap(const XrXirCompileContext *context,
    const XrXirTypes *types,const XrXirTypeNode *left,const XrXirTypeNode *right) {
    uint32_t a=0,b=0;
    while (a<left->parameter_count && b<right->parameter_count) {
        int comparison=0;
        XrXirStatus status=xir_record_name_compare(context,left->record.names[a],right->record.names[b],&comparison);
        if (status!=XR_XIR_OK) return status;
        if (!comparison) {
            status=xr_xir_compile_type_substitution_matches(context,types,NULL,0,
                left->parameters[a].type,right->parameters[b].type);
            if (status!=XR_XIR_OK) return status;
            ++a;++b;
        } else if (comparison<0) ++a; else ++b;
    }
    return XR_XIR_OK;
}
static XrXirStatus constraint_structural_structure(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirConstraint constraint,uint32_t count) {
    if (!!constraint.structural_requirements!=!!constraint.structural_count) return XR_XIR_BAD_STRUCTURE;
    if (constraint.structural_count>context->limits.parameters) return XR_XIR_BUDGET;
    for (uint32_t i=0;i<constraint.structural_count;++i) {
        const XrXirTypeNode *shape=NULL;
        XrXirStatus status=constraint_structural_shape(context,types,constraint.structural_requirements[i],count,&shape);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t j=0;j<i;++j) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirTypeNode *other=xr_xir_record_signature(types,constraint.structural_requirements[j].shape);
            status=constraint_structural_overlap(context,types,shape,other);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus constraint_structural_find(const XrXirCompileContext *context,
    const XrXirTypeNode *shape,XrXirLiteral name,uint32_t *field) {
    uint32_t begin=0,end=shape->parameter_count;
    while (begin<end) {
        uint32_t middle=begin+(end-begin)/2;int comparison=0;
        XrXirStatus status=xir_record_name_compare(context,name,shape->record.names[middle],&comparison);
        if (status!=XR_XIR_OK) return status;
        if (!comparison) {*field=middle;return XR_XIR_OK;}
        if (comparison<0) end=middle;else begin=middle+1;
    }
    return XR_XIR_BAD_TYPE;
}
