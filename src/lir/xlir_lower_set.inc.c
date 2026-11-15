/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_set.inc.c - Lower canonical Set values and actual places
 *
 * KEY CONCEPT:
 *   Only the closed I64 Set family has a qualified physical carrier.
 */
static bool lower_set_read(LowerFunction *f,uint32_t block,XrLirValue receiver,XrLirValue *output) {
    XrLirType type=lower_value_type(f,receiver);
    if(type==XR_LIR_SET_I64) {*output=receiver;return true;}
    if(type!=XR_LIR_SET_I64_CELL && type!=XR_LIR_SET_I64_REF && type!=XR_LIR_STATE_REF)
        return lower_unsupported(f->owner,"Set receiver requires an actual typed binding");
    XrLirInstruction read={.op=type==XR_LIR_SET_I64_CELL?XR_LIR_CELL_READ:
        type==XR_LIR_SET_I64_REF?XR_LIR_SET_REF_READ:XR_LIR_STATE_READ,.type=XR_LIR_SET_I64,
        .operands=&receiver,.operand_count=1,.source=lower_source(f->owner)};
    return lower_build(f->owner,xr_lir_emit_value(f->owner->module,f->blocks[block],&read,output));
}
static bool lower_set_instruction(LowerFunction *f,uint32_t block,uint32_t index,bool *handled) {
    Lower *c=f->owner;const XrXirInstruction *op=&f->source->instructions[index];
    uint32_t result=f->source->parameter_count+index;*handled=false;
    if(op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_PLACE_READ || op->op==XR_XIR_PLACE_WRITE) {
        XrXirType type=op->op==XR_XIR_PLACE_WRITE?xr_xir_operand_type(f->source,op->args[0]):op->type;
        if(!lower_set_type(c,type))return true;*handled=true;
        if(op->op==XR_XIR_CELL_PLACE) {
            if(f->array_places && f->array_places[result]!=UINT32_MAX)return true;
            return lower_value(f,op->args[0],&f->values[result]);
        }
        uint32_t cell=f->array_places?f->array_places[op->args[0]]:UINT32_MAX;
        if(cell!=UINT32_MAX) {
            if(op->op==XR_XIR_PLACE_READ)return lower_managed_copy(f,block,f->current[cell],&f->values[result]);
            XrLirValue input={0};return lower_value(f,op->args[1],&input) && lower_managed_copy(f,block,input,&f->current[cell]);
        }
        XrLirValue args[2]={{0}};if(!lower_value(f,op->args[0],&args[0]))return false;
        if(op->op==XR_XIR_PLACE_READ)return lower_set_read(f,block,args[0],&f->values[result]);
        if(!lower_value(f,op->args[1],&args[1]))return false;
        XrLirType receiver=lower_value_type(f,args[0]);
        XrLirInstruction write={.op=receiver==XR_LIR_STATE_REF?XR_LIR_STATE_WRITE:
            receiver==XR_LIR_SET_I64_REF?XR_LIR_SET_REF_WRITE:XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,
            .operands=args,.operand_count=2,.source=lower_source(c)};
        return lower_build(c,xr_lir_emit_effect(c->module,f->blocks[block],&write));
    }
    XrLirOp operation;uint32_t count=0;
    switch(op->op) {
    case XR_XIR_SET_NEW:
        if(!lower_set_type(c,op->type))return true;
        if(op->args[1])return lower_unsupported(c,"direct grouped Set construction is not a Source Set(Array) recipe");
        operation=XR_LIR_SET_NEW;break;
    case XR_XIR_SET_ADD:operation=XR_LIR_SET_ADD;count=2;break;
    case XR_XIR_HASH_LEN:operation=XR_LIR_SET_LEN;count=1;break;
    case XR_XIR_HASH_CONTAINS:operation=XR_LIR_SET_CONTAINS;count=2;break;
    case XR_XIR_HASH_DELETE:operation=XR_LIR_SET_REMOVE;count=2;break;
    case XR_XIR_HASH_CLEAR:operation=XR_LIR_SET_CLEAR;count=1;break;
    default:return true;
    }
    if(count && !lower_set_type(c,xr_xir_operand_type(f->source,op->args[0])))return true;
    *handled=true;XrLirValue args[2]={{0}};bool good=true;
    uint32_t cell=count && f->array_places?f->array_places[op->args[0]]:UINT32_MAX;
    bool mutation=operation==XR_LIR_SET_ADD || operation==XR_LIR_SET_REMOVE || operation==XR_LIR_SET_CLEAR;
    if(count) {
        if(mutation && cell!=UINT32_MAX) {
            XrLirInstruction make={.op=XR_LIR_CELL_NEW,.type=XR_LIR_SET_I64_CELL,.operands=&f->current[cell],.operand_count=1,.source=lower_source(c)};
            good=lower_build(c,xr_lir_emit_value(c->module,f->blocks[block],&make,&args[0]));
        } else good=lower_value(f,op->args[0],&args[0]);
        if(good && mutation && lower_value_type(f,args[0])==XR_LIR_STATE_REF)good=lower_set_borrow(f,block,&args[0]);
        if(good && !mutation)good=lower_set_read(f,block,args[0],&args[0]);
    }
    if(good && count>1)good=lower_value(f,op->args[1],&args[1]);
    XrLirInstruction instruction={.op=operation,.type=lower_type(c,op->type),.operands=args,.operand_count=count,.source=lower_source(c)};
    if(good)good=lower_build(c,instruction.type==XR_LIR_UNIT?xr_lir_emit_effect(c->module,f->blocks[block],&instruction):
        xr_lir_emit_value(c->module,f->blocks[block],&instruction,&f->values[result]));
    if(good && mutation && cell!=UINT32_MAX) {
        good=lower_set_read(f,block,args[0],&f->current[cell]);
        XrLirInstruction drop={.op=XR_LIR_SET_CELL_DROP,.type=XR_LIR_UNIT,.operands=args,.operand_count=1,.source=lower_source(c)};
        if(good)good=lower_build(c,xr_lir_emit_effect(c->module,f->blocks[block],&drop));
    }
    return good;
}
