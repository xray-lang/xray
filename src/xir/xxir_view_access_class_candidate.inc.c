/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_class_candidate.inc.c - Local constructor storage boundaries
 *
 * KEY CONCEPT:
 *   A verified fresh object with only value payloads cannot contain the storage
 *   borrowed at entry. A Class result or a factory name alone proves nothing.
 */
static XrXirStatus view_access_class_constructor(ViewAccess *a,uint32_t target,
    XrXirType type,bool *fresh) {
    *fresh=false;const XrXirModule *module=a->resolver->check.module;
    const XrXirTypeNode *node=xr_xir_type_node(module->types,type);
    if(!node || !xr_xir_type_is_class(module->types,type) || node->parameter_span ||
        !module->declarations || !module->declarations->functions || target>=module->function_count)
        return XR_XIR_OK;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[target];
    const XrXirFunction *body=&module->functions[target];
    if(identity->method_kind!=XR_XIR_CONSTRUCTOR || identity->native_callable ||
        identity->nominal_owner!=node->nominal.declaration+1u || body->result!=type)
        return XR_XIR_OK;
    for(uint32_t f=0;f<node->nominal.field_count;++f) {
        bool plain=false;XrXirStatus status=view_access_plain_storage(a,node->nominal.fields[f],&plain);
        if(status!=XR_XIR_OK || !plain)return status;
    }
    uint32_t returns=0,progress=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        XrXirStatus status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);
        if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&body->instructions[i];if(op->op!=XR_XIR_RETURN)continue;
        if(op->args[0]<body->parameter_count || op->args[0]-body->parameter_count>=i)return XR_XIR_OK;
        const XrXirInstruction *value=&body->instructions[op->args[0]-body->parameter_count];
        if(value->op!=XR_XIR_CLASS_NEW || value->type!=type)return XR_XIR_OK;
        ++returns;
    }
    *fresh=returns!=0;return XR_XIR_OK;
}
static XrXirStatus view_access_class_local_cell(ViewAccess *a,uint32_t cell,bool *stable) {
    *stable=false;
    if(cell<a->body->parameter_count || cell-a->body->parameter_count>=a->instruction)return XR_XIR_OK;
    XrXirStatus status=view_access_stable(a,cell,stable);if(status!=XR_XIR_OK || !*stable)return status;
    *stable=false;uint32_t progress=0;
    for(uint32_t i=cell-a->body->parameter_count+1u;i<=a->instruction;++i) {
        status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&a->body->instructions[i];
        for(uint32_t p=0;p<view_access_operands(a,op);++p) {
            uint32_t value=xr_xir_op_uses_operand_table(op->op)?a->body->operands[op->args[0]+p]:op->args[p];
            if(value==cell && (op->op!=XR_XIR_CELL_READ || p))return XR_XIR_OK;
        }
    }
    *stable=true;return XR_XIR_OK;
}
static XrXirStatus view_access_class_seed(ViewAccess *a,uint32_t value,bool *fresh) {
    *fresh=false;const XrXirModule *module=a->resolver->check.module;
    XrXirType type=xr_xir_operand_type(a->body,value);uint32_t progress=0;
    if(!xr_xir_type_is_class(module->types,type))return XR_XIR_OK;
    for(uint32_t depth=0;depth<a->body->instruction_count && value>=a->body->parameter_count;++depth) {
        XrXirStatus status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        uint32_t i=value-a->body->parameter_count;if(i>=a->body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&a->body->instructions[i];
        if(op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN) {
            if(op->args[0]>=value || xr_xir_operand_type(a->body,op->args[0])!=type)return XR_XIR_OK;
            value=op->args[0];continue;
        }
        if(op->op==XR_XIR_CELL_READ) {
            uint32_t cell=op->args[0];bool stable=false;
            status=view_access_class_local_cell(a,cell,&stable);if(status!=XR_XIR_OK || !stable)return status;
            const XrXirInstruction *owner=&a->body->instructions[cell-a->body->parameter_count];
            if(owner->op!=XR_XIR_CELL_NEW || xr_xir_operand_type(a->body,owner->args[0])!=type)return XR_XIR_OK;
            value=owner->args[0];continue;
        }
        if(op->op!=XR_XIR_CALL || op->immediate<0 || (uint64_t)op->immediate>=module->function_count)
            return XR_XIR_OK;
        bool stable=false;status=view_access_stable(a,value,&stable);if(status!=XR_XIR_OK || !stable)return status;
        return view_access_class_constructor(a,(uint32_t)op->immediate,type,fresh);
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_class_fresh_input(ViewAccess *a,const XrXirViewRootEvent *fresh,
    const XrXirViewRootEvent *input,bool *disjoint) {
    *disjoint=false;XrXirType borrowed=XR_XIR_UNIT;bool entry=false;
    XrXirStatus status=view_access_entry_root_type(a,input,&borrowed,&entry);
    if(status!=XR_XIR_OK || !entry)return status;
    const XrXirTypes *types=a->resolver->check.module->types;
    const XrXirTypeNode *slice=xr_xir_type_node(types,borrowed);
    if(!slice || slice->kind!=XR_XIR_TYPE_SLICE || fresh->kind!=XR_XIR_VIEW_ROOT_OBJECT ||
        fresh->root_function!=a->function || fresh->opaque_suffix || fresh->selection_count)
        return XR_XIR_OK;
    XrXirType type=xr_xir_operand_type(a->body,fresh->root_value);
    for(uint32_t p=0;p<fresh->step_count;++p) {
        const XrXirViewRootStep *step=&fresh->steps[p];
        if(xr_xir_type_is_class(types,step->container)) {
            const XrXirTypeNode *node=xr_xir_type_node(types,step->container);
            if(p || step->container!=type || step->operation!=XR_XIR_FIELD_PLACE ||
                step->selector>=node->nominal.field_count)return XR_XIR_OK;
        } else {
            bool plain=false;status=view_access_plain_storage(a,step->container,&plain);
            if(status!=XR_XIR_OK || !plain)return status;
        }
    }
    return view_access_class_seed(a,fresh->root_value,disjoint);
}
