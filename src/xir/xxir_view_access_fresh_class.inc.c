/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_fresh_class.inc.c - Closed Class storage distinct from entry borrows
 *
 * KEY CONCEPT:
 *   A fresh object with only scalar and immutable string fields cannot reach
 *   an earlier input's writable storage. Cell identity and allocation epochs
 *   are proved independently; a constructor name conveys no authority.
 */
typedef struct ViewFreshClass {
    XrXirTypeDescription description;
    XrXirFieldDescription field;
    uint32_t cell,allocation;
} ViewFreshClass;
static bool view_fresh_plain(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING ||
        xr_xir_integer_bits(type)!=0 || type==XR_XIR_F32 || type==XR_XIR_F64;
}
static const XrXirInstruction *view_fresh_instruction(const XrXirFunction *body,uint32_t value) {
    return value<body->parameter_count || value-body->parameter_count>=body->instruction_count?NULL:
        body->instructions+value-body->parameter_count;
}
static XrXirStatus view_fresh_fields(ViewAccess *a,XrXirType type,ViewFreshClass *scratch,bool *plain) {
    ViewRootCheck *c=&a->resolver->check;*plain=false;
    XrXirStatus status=xr_xir_compile_type_description(c->context,c->module->types,type,&scratch->description);
    if (status!=XR_XIR_OK) return status;
    const XrXirTypeDescription *d=&scratch->description;
    if (d->kind!=XR_XIR_TYPE_NOMINAL || d->nominal_kind!=XR_XIR_NOMINAL_CLASS || !d->node || !d->instance ||
        (d->definition!=NULL)==(d->identity!=NULL) || d->node->parameter_span || d->instance->argument_count ||
        d->variant_count || d->instance->field_count!=d->field_count ||
        (d->definition && (d->definition->parameter_count || d->definition->native.native_id)) ||
        (d->identity && (d->identity->arity || d->identity->native.native_id))) return XR_XIR_OK;
    for (uint32_t i=0;i<d->field_count;++i) {
        status=xr_xir_compile_description_field(c->context,d,i,&scratch->field);
        if (status!=XR_XIR_OK) return status;
        if (!scratch->field.resolved || !view_fresh_plain(scratch->field.type)) return XR_XIR_OK;
    }
    *plain=true;return XR_XIR_OK;
}
static XrXirStatus view_fresh_unwrap(ViewAccess *a,const XrXirFunction *body,XrXirType type,uint32_t *value) {
    uint64_t bound=(uint64_t)body->parameter_count+body->instruction_count;
    for (uint64_t depth=0;depth<bound;++depth) {
        if (!xir_compile_work(a->resolver->check.context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=view_fresh_instruction(body,*value);
        if (!op || op->type!=type || (op->op!=XR_XIR_COPY && op->op!=XR_XIR_OWNED_RETAIN)) return XR_XIR_OK;
        *value=op->args[0];
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus view_fresh_cell_reads(ViewAccess *a,uint32_t cell,bool *only_reads) {
    ViewRootCheck *c=&a->resolver->check;const XrXirFunction *body=a->body;*only_reads=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=body->instructions+i;uint32_t count=0,start=0,stride=1;
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_op_uses_owner_slots(op->op)) continue;
        if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) return XR_XIR_OK;
        bool table=xr_xir_op_uses_operand_table(op->op);
        if (table) {
            if (op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
            count=op->args[1];
            /* PHI predecessor labels and cleanup owner slots are not values. */
            if (op->op==XR_XIR_PHI) {if (count%2u) return XR_XIR_BAD_STRUCTURE;start=1;stride=2;}
        } else if (op->op==XR_XIR_CELL_NEW) count=xr_xir_cell_payload_operands(c->module->types,op->type,op->op);
        else if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
            count=xr_xir_cell_payload_operands(c->module->types,xr_xir_operand_type(body,op->args[0]),op->op);
        else if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
            const XrXirDeclarations *d=c->module->declarations;
            count=xr_xir_slot_payload_operands(d->slots,d->slot_count,op);
        } else if (op->op==XR_XIR_RETURN) count=xr_xir_function_completion(c->module,a->function)!=XR_XIR_UNIT;
        else if (op->op==XR_XIR_GENERATOR_YIELD) count=body->result!=XR_XIR_UNIT;
        else count=view_access_rules[op->op].operands;
        if (!table && count>2) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=start;p<count;p+=stride) {
            if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
            uint32_t value=table?body->operands[op->args[0]+p]:op->args[p];
            if (value==cell && (op->op!=XR_XIR_CELL_READ || p!=0)) return XR_XIR_OK;
        }
    }
    *only_reads=true;return XR_XIR_OK;
}
static XrXirStatus view_fresh_allocation(ViewAccess *a,const XrXirFunction *body,
    const XrXirInstruction *op,const ViewFreshClass *scratch,bool *valid) {
    ViewRootCheck *c=&a->resolver->check;*valid=false;
    const XrXirTypeDescription *d=&scratch->description;
    if (op->op!=XR_XIR_CLASS_NEW || op->type!=d->type || op->args[1]!=d->field_count) return XR_XIR_OK;
    if (op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<op->args[1];++p) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        uint32_t value=body->operands[op->args[0]+p];
        if (xr_xir_place_kind(body,value)!=XR_XIR_PLACE_NONE || xr_xir_operand_type(body,value)!=d->instance->fields[p]) return XR_XIR_OK;
    }
    *valid=true;return XR_XIR_OK;
}
static bool view_fresh_local(const XrXirFunction *body,uint32_t value,uint32_t before) {
    const XrXirInstruction *op=view_fresh_instruction(body,value);
    return op && value-body->parameter_count<before && op->op==XR_XIR_LOCAL_UNINIT && view_fresh_plain(op->type);
}
static XrXirStatus view_fresh_call_body(ViewAccess *a,const XrXirInstruction *call,
    const ViewFreshClass *scratch,bool *valid) {
    ViewRootCheck *c=&a->resolver->check;*valid=false;
    if (call->op!=XR_XIR_CALL || call->type!=scratch->description.type || call->args[1] ||
        call->type_arguments[0] || call->type_arguments[1] || call->targets[0] || call->targets[1] || call->immediate<0 ||
        (uint64_t)call->immediate>=c->module->function_count) return XR_XIR_OK;
    uint32_t target=(uint32_t)call->immediate;const XrXirFunction *body=c->module->functions+target;
    if (body->parameter_count || body->result!=call->type || !body->instructions || !body->blocks ||
        body->instruction_count<2 || body->block_count!=1 || body->blocks[0].first ||
        body->blocks[0].count!=body->instruction_count || body->blocks[0].panic || body->blocks[0].frontier ||
        xr_xir_function_is_generator(c->module,target) || (c->module->generics &&
        (c->module->generics[target].parameter_count || c->module->generics[target].argument_count))) return XR_XIR_OK;
    uint32_t allocation=body->instruction_count-2u;const XrXirInstruction *end=body->instructions+allocation+1u;
    if (end->op!=XR_XIR_RETURN || end->args[0]!=allocation) return XR_XIR_OK;
    for (uint32_t i=0;i<allocation;++i) {
        if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=body->instructions+i;
        if (op->targets[0] || op->targets[1] || op->type_arguments[0] || op->type_arguments[1]) return XR_XIR_OK;
        if (op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_SCALAR_LOCAL_WRITE || op->op==XR_XIR_OWNED_LOCAL_WRITE) {
            if (!xir_compile_work(c->context,2)) return XR_XIR_BUDGET;
            if (!view_fresh_local(body,op->args[0],i) ||
                !view_fresh_plain(xr_xir_operand_type(body,op->args[1])) ||
                xr_xir_place_kind(body,op->args[1])!=XR_XIR_PLACE_NONE) return XR_XIR_OK;
            continue;
        }
        if (!view_fresh_plain(op->type)) return XR_XIR_OK;
        switch (op->op) {
        case XR_XIR_CONST_BOOL:case XR_XIR_CONST_INT:case XR_XIR_CONST_FLOAT:
        case XR_XIR_CONST_RUNE:case XR_XIR_CONST_STRING:case XR_XIR_LOCAL_UNINIT:break;
        case XR_XIR_LOCAL_READ:case XR_XIR_SCALAR_LOCAL_READ:case XR_XIR_OWNED_LOCAL_READ:
            if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
            if (!view_fresh_local(body,op->args[0],i)) return XR_XIR_OK;break;
        case XR_XIR_COPY:case XR_XIR_SCALAR_COPY:case XR_XIR_OWNED_RETAIN:
            if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
            if (!view_fresh_plain(xr_xir_operand_type(body,op->args[0])) ||
                xr_xir_place_kind(body,op->args[0])!=XR_XIR_PLACE_NONE) return XR_XIR_OK;break;
        default:return XR_XIR_OK;
        }
    }
    if (!xir_compile_work(c->context,2)) return XR_XIR_BUDGET;
    return view_fresh_allocation(a,body,body->instructions+allocation,scratch,valid);
}
static XrXirStatus view_fresh_identity(ViewAccess *a,ViewFreshClass *scratch,bool *fresh) {
    ViewRootCheck *c=&a->resolver->check;XrXirType type=scratch->description.type;*fresh=false;
    uint32_t value=scratch->allocation;XrXirStatus status=view_fresh_unwrap(a,a->body,type,&value);
    if (status!=XR_XIR_OK) return status;
    const XrXirInstruction *read=view_fresh_instruction(a->body,value);
    if (!read || read->op!=XR_XIR_CELL_READ || read->type!=type) return XR_XIR_OK;
    scratch->cell=read->args[0];const XrXirInstruction *cell=view_fresh_instruction(a->body,scratch->cell);
    const XrXirTypeNode *cell_type=cell?xr_xir_type_node(c->module->types,cell->type):NULL;
    if (!cell || cell->op!=XR_XIR_CELL_NEW || !cell_type || cell_type->kind!=XR_XIR_TYPE_CELL || cell_type->element!=type) return XR_XIR_OK;
    bool stable=false;status=view_access_stable(a,scratch->cell,&stable);if (status!=XR_XIR_OK || !stable) return status;
    bool only_reads=false;status=view_fresh_cell_reads(a,scratch->cell,&only_reads);if (status!=XR_XIR_OK || !only_reads) return status;
    scratch->allocation=cell->args[0];status=view_fresh_unwrap(a,a->body,type,&scratch->allocation);
    if (status!=XR_XIR_OK) return status;
    const XrXirInstruction *producer=view_fresh_instruction(a->body,scratch->allocation);
    if (!producer || producer->type!=type) return XR_XIR_OK;
    status=view_access_stable(a,scratch->allocation,&stable);if (status!=XR_XIR_OK || !stable) return status;
    if (!xir_compile_work(c->context,1)) return XR_XIR_BUDGET;
    return producer->op==XR_XIR_CLASS_NEW?view_fresh_allocation(a,a->body,producer,scratch,fresh):
        view_fresh_call_body(a,producer,scratch,fresh);
}
static XrXirStatus view_access_fresh_class_input(ViewAccess *a,const XrXirViewRootEvent *object,
    const XrXirViewRootEvent *input,bool *disjoint) {
    *disjoint=false;
    if (object->kind!=XR_XIR_VIEW_ROOT_OBJECT || !object->dynamic_identity || input->kind!=XR_XIR_VIEW_ROOT_BORROWED_PARAMETER ||
        input->dynamic_identity || object->opaque_suffix || input->opaque_suffix || object->step_count || input->step_count ||
        object->selection_count || input->selection_count || object->root_function!=a->function || input->root_function!=a->function)
        return XR_XIR_OK;
    ViewRootCheck *c=&a->resolver->check;
    if (input->root_value>=a->body->parameter_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *leaf=xr_xir_type_node(c->module->types,a->body->parameters[input->root_value]);
    if (!leaf || (leaf->kind!=XR_XIR_TYPE_SLICE && leaf->kind!=XR_XIR_TYPE_MUT_SLICE) || !view_fresh_plain(leaf->element)) return XR_XIR_OK;
    if ((uint64_t)object->root_value>=(uint64_t)a->body->parameter_count+a->body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint64_t fixed=sizeof(ViewFreshClass);
    if (c->reserved>c->context->limits.frame_bytes || fixed>c->context->limits.frame_bytes-c->reserved) return XR_XIR_BUDGET;
    c->reserved+=fixed;ViewFreshClass scratch={0};scratch.allocation=object->root_value;
    XrXirStatus status=view_fresh_fields(a,xr_xir_operand_type(a->body,object->root_value),&scratch,disjoint);
    if (status==XR_XIR_OK && *disjoint) {*disjoint=false;status=view_fresh_identity(a,&scratch,disjoint);}
    if (status!=XR_XIR_OK) *disjoint=false;
    XR_CHECK(c->reserved>=fixed,"private Class evidence retains its exact receiving reservation");
    c->reserved-=fixed;return status;
}
