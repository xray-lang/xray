/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_nominal.inc.c - Authentic closed struct and active enum execution
 *
 * KEY CONCEPT:
 *   Checked declaration authority remains with the Artifact. Common Value
 *   operations own construction and receiving on the original compiler Domain.
 */
#include "xxir_struct.h"
#include "xxir_enum.h"
static XrXirValue ctfe_nominal_operand(const ScalarRun *run,uint32_t id) {
    XrXirType type=xr_xir_operand_type(run->function,id);
    return type==XR_XIR_UNIT?(XrXirValue){0}:(XrXirValue){(uint32_t)type,0,xr_xir_scalar_load(run->frame,run->layout->offsets[id])};
}
static const XrXirNominalVariant *ctfe_enum_variant_info(const XrXirTypes *types,const XrXirTypeNode *node,uint32_t variant) {
    const XrXirNominalTable *table=types->nominals;uint32_t count=table->declarations?
        table->declarations[node->nominal.declaration].variant_count:table->identities[node->nominal.declaration].variant_count;
    if (variant>=count) return NULL;
    return table->declarations?&table->declarations[node->nominal.declaration].variants[variant]:&table->identities[node->nominal.declaration].variants[variant];
}
static bool ctfe_nominal_opcode(XrXirOp op) {
    return op==XR_XIR_RECORD_SET || (op>=XR_XIR_STRUCT_NEW && op<=XR_XIR_STRUCT_SET) || (op>=XR_XIR_ENUM_NEW && op<=XR_XIR_ENUM_GET);
}
static XrXirCtfeStatus ctfe_index_path_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state);
static XrXirCtfeStatus ctfe_nominal_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    if (op->op==XR_XIR_STRUCT_SET || op->op==XR_XIR_RECORD_SET) return ctfe_index_path_step(run,vm,state);
    bool create=op->op==XR_XIR_STRUCT_NEW || op->op==XR_XIR_ENUM_NEW;
    XrXirValue receiver={0},result={0};XrXirType type=op->type;
    if (!create) { receiver=ctfe_nominal_operand(run,op->args[0]);type=(XrXirType)receiver.type; }
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    XrXirValueStatus value_status=XR_XIR_VALUE_OK;
    if (create) {
        uint32_t count=op->args[1];if (count && (!vm->arguments || count>run->layout->outgoing_count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        const XrXirTypeNode *node=ctfe_composite_node(run->module->types,type);
        if (!node) return XR_XIR_CTFE_BAD_ARTIFACT;
        const XrXirNominalVariant *variant=op->op==XR_XIR_ENUM_NEW?ctfe_enum_variant_info(run->module->types,node,(uint32_t)op->immediate):NULL;
        if (op->op==XR_XIR_ENUM_NEW && (!variant || variant->field_count!=count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        if (!scalar_compile_work(run,(uint64_t)count*(2*sizeof(XrXirValue)+4))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t i=0;i<count;++i) {
            vm->arguments[i]=ctfe_nominal_operand(run,run->function->operands[op->args[0]+i]);
            status=ctfe_admission_work(run,state,&vm->arguments[i]);if (status!=XR_XIR_CTFE_OK) return status;
        }
        value_status=op->op==XR_XIR_STRUCT_NEW?xr_xir_struct_new(type,count?vm->arguments:NULL,count,&admission,&result):
            xr_xir_enum_new(type,(uint32_t)op->immediate,count?vm->arguments:NULL,count,&admission,&result);
    } else if (op->op==XR_XIR_ENUM_TAG) {
        uint32_t variant=0;value_status=xr_xir_enum_variant(&receiver,&variant);result=(XrXirValue){XR_XIR_I64,0,variant};
    } else {
        status=ctfe_admission_work(run,state,&receiver);if (status!=XR_XIR_CTFE_OK) return status;
        value_status=op->op==XR_XIR_STRUCT_GET?xr_xir_struct_get(&receiver,(uint32_t)op->immediate,&admission,&result):
            xr_xir_enum_get(&receiver,(uint32_t)op->immediate,op->args[1],&admission,&result);
    }
    status=ctfe_value_status(value_status);
    if (status==XR_XIR_CTFE_OK) {
        if (op->type!=XR_XIR_UNIT) {
            uint32_t offset=run->layout->offsets[run->function->parameter_count+vm->instruction];
            if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,offset,&result);
            else xr_xir_scalar_store(run->frame,offset,result.payload);
        }
        ++vm->instruction;
    }
    xr_xir_value_drop(&result);return status;
}
