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
    /* Instruction order alone does not order loop backedges or CFG predecessors.
     * Any replacement or exposed Cell alias in this body keeps the root unknown. */
    for(uint32_t i=0;i<a->body->instruction_count;++i) {
        status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&a->body->instructions[i];
        for(uint32_t p=0;p<view_access_operands(a,op);++p) {
            uint32_t value=xr_xir_op_uses_operand_table(op->op)?a->body->operands[op->args[0]+p]:op->args[p];
            if(value==cell && ((op->op!=XR_XIR_CELL_READ && op->op!=XR_XIR_CELL_TAKE) || p))return XR_XIR_OK;
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
        if(op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_TAKE) {
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

/* Valid byte views use an ordinary Cell or independent Buffer storage.
 * Neither representation owns this direct Class Array field's logical slot. */
static XrXirStatus view_access_class_byte_field_input(ViewAccess *a,const XrXirViewRootEvent *field,
    const XrXirViewRootEvent *input,uint32_t queried,uint32_t other,bool *disjoint) {
    *disjoint=false;const XrXirModule *module=a->resolver->check.module;
    if(!a->cells || !a->cells->owner || !a->cells->construction ||
        a->instruction>=a->body->instruction_count || field->kind!=XR_XIR_VIEW_ROOT_OBJECT ||
        field->root_function!=a->function || field->root_value>=a->body->parameter_count ||
        field->opaque_suffix || field->selection_count || field->step_count!=1 || !field->steps ||
        input->selection_count || input->root_value!=other ||
        (module->generics && module->generics[a->function].parameter_count))return XR_XIR_OK;
    const XrXirInstruction *use=a->body->instructions+a->instruction;
    if(use->op!=XR_XIR_ARRAY_PUSH || use->args[0]!=queried)return XR_XIR_OK;
    XrXirType borrowed=XR_XIR_UNIT;bool entry=false;
    XrXirStatus status=view_access_entry_root_type(a,input,&borrowed,&entry);
    if(status!=XR_XIR_OK || !entry)return status;
    const XrXirTypes *types=module->types;
    const XrXirTypeNode *slice=xr_xir_type_node(types,borrowed);
    if(!slice || slice->kind!=XR_XIR_TYPE_SLICE || slice->element!=XR_XIR_U8)return XR_XIR_OK;
    const XrXirViewRootStep *step=field->steps;
    if(step->function!=a->function || step->value!=queried || step->operation!=XR_XIR_FIELD_PLACE ||
        queried<a->body->parameter_count || queried-a->body->parameter_count>=a->body->instruction_count)return XR_XIR_OK;
    const XrXirInstruction *leaf=a->body->instructions+queried-a->body->parameter_count;
    if(leaf->op!=XR_XIR_FIELD_PLACE || leaf->immediate<0 || (uint64_t)leaf->immediate>UINT32_MAX ||
        step->selector!=(uint32_t)leaf->immediate || leaf->args[0]<a->body->parameter_count ||
        leaf->args[0]-a->body->parameter_count>=a->body->instruction_count)return XR_XIR_OK;
    const XrXirInstruction *object=a->body->instructions+leaf->args[0]-a->body->parameter_count;
    XrXirType receiver=a->body->parameters[field->root_value];
    const XrXirTypeNode *nominal=xr_xir_type_node(types,receiver),*array=xr_xir_type_node(types,leaf->type);
    if(object->op!=XR_XIR_OBJECT_PLACE || object->args[0]!=field->root_value || object->type!=receiver ||
        step->container!=receiver || !nominal || !xr_xir_type_is_class(types,receiver) || nominal->parameter_span ||
        nominal->nominal.argument_count || !array || array->kind!=XR_XIR_TYPE_ARRAY || array->element!=XR_XIR_U8 ||
        !types->nominals || nominal->nominal.declaration>=types->nominals->count ||
        step->selector>=nominal->nominal.field_count || nominal->nominal.fields[step->selector]!=leaf->type)return XR_XIR_OK;
    const XrXirNominalTable *table=types->nominals;uint32_t owner=nominal->nominal.declaration,flags=0;
    if(table->declarations) {
        const XrXirNominalDeclaration *decl=table->declarations+owner;
        if(decl->parameter_count || decl->parent || decl->native.native_id || step->selector>=decl->field_count ||
            !decl->fields || decl->fields[step->selector].type!=leaf->type)return XR_XIR_OK;
        flags=decl->fields[step->selector].flags;
    } else if(table->identities) {
        const XrXirNominalIdentity *identity=table->identities+owner;
        if(identity->arity || identity->parent || identity->native.native_id || step->selector>=identity->field_count ||
            !identity->fields)return XR_XIR_OK;
        flags=identity->fields[step->selector].flags;
    } else return XR_XIR_OK;
    *disjoint=(flags&XR_XIR_FIELD_MUTABLE)!=0;return XR_XIR_OK;
}
