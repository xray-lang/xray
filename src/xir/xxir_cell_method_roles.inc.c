/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_method_roles.inc.c - Scoped suffixes of actual bound methods
 *
 * KEY CONCEPT:
 *   The Class receiver occupies capture zero. Only independently verified REF
 *   suffixes of every actual candidate acquire scoped Cell parameter roles.
 */
typedef struct CellProofMethodRoles {
    CellProofBuild *build;
    const XrXirTypeNode *signature;
} CellProofMethodRoles;
static XrXirStatus cell_proof_method_candidate(void *owner,const XirClassMethodTarget *target) {
    CellProofMethodRoles *query=owner;CellProofBuild *build=query->build;
    XrXirCellProofBodyHead head={0};if(!cell_proof_head(build,target->function,&head))return build->status;
    if(!head.parameter_count || head.parameter_count-1u!=query->signature->parameter_count)
        return XR_XIR_BAD_TYPE;
    CellProofFunction *callee=&build->proof->functions[target->function];
    for(uint32_t p=0;p<query->signature->parameter_count;++p) {
        const XrXirCallableParameter *parameter=&query->signature->parameters[p];
        if(!xr_xir_callable_parameter_storage_valid(build->body->domain.types,parameter) ||
            parameter->type!=head.parameters[p+1u])return XR_XIR_BAD_TYPE;
        if(parameter->mode!=XR_PARAM_REF)continue;
        if(cell_proof_tracking(build->proof,target->function,p+1u)!=1)return XR_XIR_BAD_TYPE;
        build->uses[callee->begin+p+1u]|=2u;
    }
    return XR_XIR_OK;
}
static bool cell_proof_method_roles(CellProofBuild *build,uint32_t f,uint32_t i,const XrXirInstruction *op) {
    if(!build->body->method_candidates)return cell_proof_error(build,f,i,XR_XIR_UNRESOLVED);
    const XrXirTypeNode *signature=xr_xir_callable_signature(build->body->domain.types,op->type);
    if(!signature)return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
    CellProofMethodRoles query={build,signature};
    XrXirStatus status=build->body->method_candidates(build->context,build->body->owner,f,i,
        cell_proof_method_candidate,&query);
    return status==XR_XIR_OK || cell_proof_error(build,f,i,status);
}
