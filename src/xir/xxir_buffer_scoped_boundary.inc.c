/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_buffer_scoped_boundary.inc.c - Actual begin-to-callback boundary proofs
 *
 * KEY CONCEPT:
 *   A mutable callback signature is admitted only at its complete actual use
 *   sites. An equal callable type, an exported name or a descriptor is no grant.
 */
#include "xxir_native_callable.h"
#include "xxir_operand_roles.h"
#include "xxir_receive_control.h"
static const uint8_t buffer_scoped_operands[XR_XIR_OP_COUNT]={0,
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) operands,
#include "xxir_ops.def"
#undef XR_XIR_OP
};
static bool buffer_scoped_body(const XrXirFunction *body) {
    return body && (!body->instruction_count || body->instructions) &&
        (!body->operand_count || body->operands) && (!body->block_count || body->blocks) &&
        (!body->parameter_count || body->parameters) &&
        body->instruction_count<=UINT32_MAX-body->parameter_count;
}
static XrXirStatus buffer_scoped_alias(const XrXirFunction *body,uint32_t *value,
    XrCompileRequestControl *control,uint32_t *progress) {
    uint64_t count=(uint64_t)body->parameter_count+body->instruction_count;
    for(uint32_t steps=0;;++steps) {
        XrXirStatus status=xir_receive_step(control,progress);if(status!=XR_XIR_OK)return status;
        if(*value>=count)return XR_XIR_BAD_STRUCTURE;
        if(*value<body->parameter_count)return XR_XIR_OK;
        const XrXirInstruction *op=&body->instructions[*value-body->parameter_count];
        if(op->op!=XR_XIR_COPY&&op->op!=XR_XIR_OWNED_RETAIN&&op->op!=XR_XIR_FUNCTION_WEAKEN)return XR_XIR_OK;
        if(steps>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        *value=op->args[0];
    }
}

static XrXirStatus buffer_scoped_begin(const XrXirModule *module,const XrXirFunction *body,
    uint32_t value,XrCompileRequestControl *control,uint32_t *progress,uint32_t *instruction) {
    XrXirStatus status=buffer_scoped_alias(body,&value,control,progress);
    if (status!=XR_XIR_OK) return status;
    if (value<body->parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t index=value-body->parameter_count;
    const XrXirInstruction *op=&body->instructions[index];
    if(op->op==XR_XIR_INVOKE_RESULT) {
        if(op->immediate<0 || (uint64_t)op->immediate>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        uint32_t parent=(uint32_t)op->immediate;
        const XrXirInstruction *invoke=body->instructions+parent;
        if(invoke->op!=XR_XIR_INVOKE || invoke->type!=op->type ||
            invoke->targets[0]>=body->block_count || invoke->targets[0]==invoke->targets[1])return XR_XIR_BAD_TYPE;
        const XrXirBlock *normal=body->blocks+invoke->targets[0];
        if(!normal->count || normal->first!=index)return XR_XIR_BAD_STRUCTURE;
        op=invoke;index=parent;
    } else if(op->op!=XR_XIR_CALL)return XR_XIR_BAD_TYPE;
    if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count ||
        op->args[1]!=3 || op->args[0]>body->operand_count || body->operand_count-op->args[0]<3 ||
        module->declarations->functions[op->immediate].native_callable!=XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN)
        return XR_XIR_BAD_TYPE;
    status=xir_native_callable_verify(module,(uint32_t)op->immediate);
    if(status==XR_XIR_OK&&op->type!=module->functions[op->immediate].result)status=XR_XIR_BAD_TYPE;
    if(status==XR_XIR_OK)for(uint32_t p=0;p<3;++p) {
        uint32_t argument=body->operands[op->args[0]+p];
        if(argument>=(uint64_t)body->parameter_count+body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        if(xr_xir_operand_type(body,argument)!=module->functions[op->immediate].parameters[p])
            return XR_XIR_BAD_TYPE;
    }
    if(status==XR_XIR_OK)*instruction=index;
    return status;
}
static XrXirStatus buffer_scoped_argument(const XrXirModule *module,const XrXirFunction *body,
    const XrXirInstruction *call,XrCompileRequestControl *control,uint32_t *progress) {
    if (call->args[1]!=1 || call->args[0]>=body->operand_count) return XR_XIR_BAD_TYPE;
    uint32_t instruction=0;
    return buffer_scoped_begin(module,body,body->operands[call->args[0]],control,progress,&instruction);
}
static uint32_t buffer_scoped_use_count(const XrXirModule *module,const XrXirFunction *body,
    const XrXirInstruction *op) {
    if(op->op==XR_XIR_RETURN)return body->result!=XR_XIR_UNIT;
    if(op->op==XR_XIR_NULLABLE_SOME&&xr_xir_nullable_is_unit(module->types,op->type))return 0;
    if(op->op==XR_XIR_SLOT_INIT||op->op==XR_XIR_SLOT_STORE)
        return xr_xir_slot_payload_operands(module->declarations->slots,module->declarations->slot_count,op);
    if(op->op==XR_XIR_CELL_NEW||op->op==XR_XIR_CELL_READ||op->op==XR_XIR_CELL_WRITE||op->op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType type=op->op==XR_XIR_CELL_NEW?op->type:xr_xir_operand_type(body,op->args[0]);
        return xr_xir_cell_payload_operands(module->types,type,op->op);
    }
    return xr_xir_op_uses_operand_table(op->op)?op->args[1]:buffer_scoped_operands[op->op];
}
static bool buffer_scoped_callback_shape(const XrXirModule *module,uint32_t function) {
    const XrXirFunction *body=&module->functions[function];
    const XrXirFunctionIdentity *identity=&module->declarations->functions[function];
    if(!buffer_scoped_body(body) || !body->parameter_count || body->result!=XR_XIR_UNIT ||
        identity->exported || identity->native_callable || identity->nominal_owner || identity->member_access ||
        identity->method_kind || identity->cleanup_owner || identity->test_role ||
        (identity->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)))return false;
    const XrXirTypeNode *tail=xr_xir_type_node(module->types,body->parameters[body->parameter_count-1]);
    return tail && tail->kind==XR_XIR_TYPE_MUT_SLICE && tail->element==XR_XIR_U8;
}
static XrXirStatus buffer_scoped_target(const XrXirModule *module,const XrXirFunction *body,
    const XrXirInstruction *call,XrCompileRequestControl *control,uint32_t *progress,uint32_t *reference) {
    if(call->immediate<0 || (uint64_t)call->immediate>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    uint32_t value=(uint32_t)call->immediate;
    XrXirStatus status=buffer_scoped_alias(body,&value,control,progress);
    if(status!=XR_XIR_OK)return status;
    if(value<body->parameter_count)return XR_XIR_BAD_TYPE;
    const XrXirInstruction *ref=body->instructions+value-body->parameter_count;
    if(ref->op!=XR_XIR_FUNCTION_REF || ref->immediate<0 ||
        (uint64_t)ref->immediate>=module->function_count)return XR_XIR_BAD_TYPE;
    const XrXirFunction *target=module->functions+ref->immediate;
    if(!buffer_scoped_callback_shape(module,(uint32_t)ref->immediate) ||
        ref->args[1]>=target->parameter_count || ref->args[1]+1u!=target->parameter_count)
        return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,ref->type);
    if(!signature || signature->parameter_count!=1 || !signature->parameters ||
        signature->parameters[0].mode!=XR_PARAM_READ ||
        signature->parameters[0].type!=target->parameters[target->parameter_count-1] ||
        signature->result!=XR_XIR_UNIT || (signature->flags&XR_XIR_CALLABLE_REST))return XR_XIR_BAD_TYPE;
    *reference=value;return XR_XIR_OK;
}
/* A normal END is required here. Exceptional disposal remains independently
 * checked by the complete affine, CFG and scope-cleanup passes. */
static XrXirStatus buffer_scoped_view_uses(const XrXirModule *module,const XrXirFunction *body,
    uint32_t view,XrCompileRequestControl *control,uint32_t *progress) {
    XrXirStatus status=buffer_scoped_alias(body,&view,control,progress);
    if(status!=XR_XIR_OK)return status;
    bool ended=false,called=false;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        status=xir_receive_step(control,progress);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=body->instructions+i;
        if(op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT)return XR_XIR_BAD_STRUCTURE;
        if(xr_xir_op_uses_owner_slots(op->op))continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        uint32_t count=buffer_scoped_use_count(module,body,op);
        if(table && (op->args[0]>body->operand_count || count>body->operand_count-op->args[0]))
            return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=op->op==XR_XIR_PHI?1u:0u;p<count;p+=op->op==XR_XIR_PHI?2u:1u) {
            status=xir_receive_step(control,progress);if(status!=XR_XIR_OK)return status;
            uint32_t value=table?body->operands[op->args[0]+p]:op->args[p];
            status=buffer_scoped_alias(body,&value,control,progress);if(status!=XR_XIR_OK)return status;
            if(value!=view)continue;
            if(!table && !p && (op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN))continue;
            if(!table && !p && op->op==XR_XIR_VIEW_END){ended=true;continue;}
            if(table && !p && count==1 && (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT)) {
                uint32_t reference=0;
                status=buffer_scoped_target(module,body,op,control,progress,&reference);
                if(status!=XR_XIR_OK)return status;
                called=true;continue;
            }
            return XR_XIR_BAD_TYPE;
        }
    }
    return ended&&called?XR_XIR_OK:XR_XIR_BAD_TYPE;
}
static XrXirStatus buffer_scoped_reference(const XrXirModule *module,const XrXirFunction *body,
    uint32_t reference,XrCompileRequestControl *control,bool *called) {
    uint32_t progress=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        XrXirStatus status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return status;
        const XrXirInstruction *op=&body->instructions[i];
        if(op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT)return XR_XIR_BAD_STRUCTURE;
        bool indirect=op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT;
        if (indirect) {
            if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            uint32_t callee=(uint32_t)op->immediate;
            status=buffer_scoped_alias(body,&callee,control,&progress);if (status!=XR_XIR_OK) return status;
            if (callee==reference) {
                status=buffer_scoped_argument(module,body,op,control,&progress);
                if(status==XR_XIR_OK)status=buffer_scoped_view_uses(module,body,
                    body->operands[op->args[0]],control,&progress);
                if (status!=XR_XIR_OK) return status;
                *called=true;
            }
        }
        if(xr_xir_op_uses_owner_slots(op->op))continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        uint32_t count=buffer_scoped_use_count(module,body,op);
        if (table && (op->args[0]>body->operand_count || count>body->operand_count-op->args[0]))
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=op->op==XR_XIR_PHI?1u:0u;p<count;p+=op->op==XR_XIR_PHI?2u:1u) {
            status=xir_receive_step(control,&progress);if (status!=XR_XIR_OK) return status;
            uint32_t value=table?body->operands[op->args[0]+p]:op->args[p];
            status=buffer_scoped_alias(body,&value,control,&progress);if (status!=XR_XIR_OK) return status;
            if (value!=reference) continue;
            if (table || p || (op->op!=XR_XIR_COPY && op->op!=XR_XIR_OWNED_RETAIN && op->op!=XR_XIR_FUNCTION_WEAKEN))
                return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_buffer_scoped_callback(const XrXirCompileContext *context,
    XirEffectInput input,uint32_t function,bool *accepted) {
    const XrXirModule *module=input.module;
    if (!xir_compile_context_valid(context) || !module || !module->functions || !input.construction || !accepted ||
        function>=module->function_count || !module->declarations || !module->declarations->functions)
        return XR_XIR_BAD_STRUCTURE;
    *accepted=false;XrXirStatus entry=xir_receive_poll(input.control);if(entry!=XR_XIR_OK)return entry;
    const XrXirFunction *body=&module->functions[function];
    if(!buffer_scoped_callback_shape(module,function))return XR_XIR_OK;
    bool called=false;uint32_t progress=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *caller=&module->functions[f];
        if(!buffer_scoped_body(caller))return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i=0;i<caller->instruction_count;++i) {
            XrXirStatus status=xir_receive_step(input.control,&progress);if (status!=XR_XIR_OK) return status;
            const XrXirInstruction *op=&caller->instructions[i];
            if (!xr_xir_op_references_function(op->op) || op->immediate!=(int64_t)function) continue;
            if (op->op==XR_XIR_FUNCTION_REF) {
                if (op->args[1]>=body->parameter_count || op->args[1]+1u!=body->parameter_count) return XR_XIR_BAD_TYPE;
                status=buffer_scoped_reference(module,caller,caller->parameter_count+i,input.control,&called);
            } else return XR_XIR_BAD_TYPE;
            if (status!=XR_XIR_OK) return status;
        }
    }
    *accepted=called;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_compile_buffer_scoped_site(const XrXirCompileContext *context,
    XirEffectInput input,uint32_t function,uint32_t instruction,bool *accepted) {
    const XrXirModule *module=input.module;
    if(!xir_compile_context_valid(context) || !accepted || !module || !input.construction ||
        !module->functions || !module->declarations || !module->declarations->functions ||
        function>=module->function_count)return XR_XIR_BAD_STRUCTURE;
    *accepted=false;XrXirStatus status=xir_receive_poll(input.control);if(status!=XR_XIR_OK)return status;
    const XrXirFunction *body=module->functions+function;
    if(!buffer_scoped_body(body) || instruction>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *begin=body->instructions+instruction;
    if((begin->op!=XR_XIR_CALL && begin->op!=XR_XIR_INVOKE) || begin->immediate<0 ||
        (uint64_t)begin->immediate>=module->function_count ||
        module->declarations->functions[begin->immediate].native_callable!=XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN)
        return XR_XIR_OK;
    uint32_t progress=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        status=xir_receive_step(input.control,&progress);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *call=body->instructions+i;
        if(call->op!=XR_XIR_CALL_INDIRECT && call->op!=XR_XIR_INVOKE_INDIRECT)continue;
        if(call->args[1]!=1 || call->args[0]>=body->operand_count)continue;
        uint32_t parent=UINT32_MAX;
        status=buffer_scoped_begin(module,body,body->operands[call->args[0]],input.control,&progress,&parent);
        if(status==XR_XIR_BAD_TYPE)continue;
        if(status!=XR_XIR_OK)return status;
        if(parent!=instruction)continue;
        uint32_t reference=0;
        status=buffer_scoped_target(module,body,call,input.control,&progress,&reference);
        if(status!=XR_XIR_OK)return status;
        bool called=false;
        status=buffer_scoped_reference(module,body,reference,input.control,&called);
        if(status!=XR_XIR_OK)return status;
        *accepted=called;return XR_XIR_OK;
    }
    return XR_XIR_OK;
}
