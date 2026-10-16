/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_class.inc.c - Private identities on the original compiler Domain
 *
 * KEY CONCEPT:
 *   Checked factories create identities. Field receiving visits the actual
 *   graph before mutation; opaque handle equality only rejects write cycles.
 */
static bool ctfe_class_operation(XrXirOp op) {
    return op==XR_XIR_CLASS_NEW || op==XR_XIR_CLASS_GET || op==XR_XIR_CLASS_SET || op==XR_XIR_OBJECT_PLACE;
}
static XrXirCtfeStatus ctfe_class_member_read(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *receiver,uint32_t field,XrXirValue *output) {
    const XrXirTypeNode *node=xr_xir_type_node(run->module->types,(XrXirType)receiver->type);
    if (!node || !xr_xir_type_is_class(run->module->types,(XrXirType)receiver->type) || field>=node->nominal.field_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    XrXirCtfeStatus status=ctfe_validation_work(run,state,(XrXirType)receiver->type);
    if (status==XR_XIR_CTFE_OK) status=ctfe_validation_work(run,state,node->nominal.fields[field]);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (ctfe_frame_spare(state)<256) return XR_XIR_CTFE_FRAME_LIMIT;
    if (!scalar_compile_work(run,512)) return XR_XIR_CTFE_BUDGET;
    XrXirValueAdmission admission=ctfe_value_admission(state);admission.scratch_bytes-=256;
    return ctfe_value_status(xr_xir_class_get(receiver,field,&admission,output));
}
static XrXirCtfeStatus ctfe_class_loan_begin(ScalarRun *run,CtfeOwnedState *state,
    const XrXirValue *receiver,XrXirValueAdmission *admission,XrXirCellLoan *loan) {
    if (ctfe_frame_spare(state)<256) return XR_XIR_CTFE_FRAME_LIMIT;
    if (!scalar_compile_work(run,512+3*sizeof(*loan)+state->value_header_work)) return XR_XIR_CTFE_BUDGET;
    admission->scratch_bytes-=256;
    XrXirCtfeStatus status=ctfe_value_status(xr_xir_class_loan_prepare(receiver,&state->authority,admission,loan));
    if (status==XR_XIR_CTFE_OK) xr_xir_class_loan_commit(receiver,loan,&state->authority);
    return status;
}
static XrXirCtfeStatus ctfe_class_step(ScalarRun *run,VmState *vm,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[vm->instruction];
    bool create=op->op==XR_XIR_CLASS_NEW;XrXirValue receiver={0},result={0};
    XrXirType type=op->type;
    if (!create) { receiver=ctfe_nominal_operand(run,op->args[0]);type=(XrXirType)receiver.type; }
    XrXirCtfeStatus status=ctfe_validation_work(run,state,type);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);XrXirValueStatus received=XR_XIR_VALUE_OK;
    if (create) {
        uint32_t count=op->args[1];
        if (count && (!vm->arguments || count>run->layout->outgoing_count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        if (!scalar_compile_work(run,(uint64_t)count*(2*sizeof(XrXirValue)+4))) return XR_XIR_CTFE_BUDGET;
        for (uint32_t i=0;i<count;++i) {
            vm->arguments[i]=ctfe_nominal_operand(run,run->function->operands[op->args[0]+i]);
            status=ctfe_admission_work(run,state,&vm->arguments[i]);if (status!=XR_XIR_CTFE_OK) return status;
        }
        received=xr_xir_class_new(type,count?vm->arguments:NULL,count,&admission,&result);
    } else {
        status=ctfe_admission_work(run,state,&receiver);if (status!=XR_XIR_CTFE_OK) return status;
        if (op->op==XR_XIR_CLASS_GET) status=ctfe_class_member_read(run,state,&receiver,(uint32_t)op->immediate,&result);
        else {
            XrXirValue incoming=ctfe_nominal_operand(run,op->args[1]),previous=state->forbidden_class;
            state->forbidden_class=receiver;status=ctfe_mutation_work(run,state,&receiver,&incoming);state->forbidden_class=previous;
            if (status==XR_XIR_CTFE_OK) { admission=ctfe_value_admission(state);received=xr_xir_class_set(&receiver,(uint32_t)op->immediate,&incoming,&admission); }
        }
    }
    if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(received);
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
