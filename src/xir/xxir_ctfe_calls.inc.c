/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_calls.inc.c - Finite compiler-owned direct call activations
 *
 * KEY CONCEPT:
 *   Each request checks its whole actual reachable scalar closure. Numeric and
 *   CFG execution share scalar_step; direct calls never enter runtime admission.
 */
typedef struct CtfeCallFrame {
    struct CtfeCallFrame *parent;
    uint32_t function, return_value;
    size_t bytes;
    VmState state;
    ScalarRun run;
} CtfeCallFrame;

typedef struct CtfeCallExecution {
    const XrXirCtfeCallRequest *request;
    const XrXirArtifact *artifact;
    CtfeCallFrame *top;
    uint64_t steps, live_bytes;
    uint32_t depth;
    CtfeOwnedState owned;
} CtfeCallExecution;

/* Scalar types and opcode costs are the same rules consumed by leaf execution.
 * Calls are handled separately, before any runtime scalar CALL dispatcher. */
static XrXirCtfeStatus ctfe_call_body_admit(const XrXirArtifact *artifact,uint32_t f) {
    const XrXirModule *module=&artifact->module;
    const XrXirCompileContext *work=&artifact->context;
    const XrXirFunction *body=&module->functions[f];
    const XrXirFunctionLayout *layout=&artifact->layouts[f];
    if (!xir_compile_work(work,8)) return XR_XIR_CTFE_BUDGET;
    if (!module->declarations || !module->declarations->functions) return XR_XIR_CTFE_NOT_SUPPORTED;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
    if (identity->cleanup_owner || identity->nominal_owner ||
        (module->generics && (module->generics[f].parameter_count || module->generics[f].argument_count)) ||
        layout->path_count)
        return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus admitted=ctfe_body_type_admit(artifact,body->result,false);
    if (admitted!=XR_XIR_CTFE_OK) return admitted;
    for (uint32_t m=0;m<module->declarations->module_count;++m) {
        if (!xir_compile_work(work,1)) return XR_XIR_CTFE_BUDGET;
        if (module->declarations->modules[m].initializer==f) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    for (uint32_t a=0;a<body->parameter_count;++a) {
        if (!xir_compile_work(work,1)) return XR_XIR_CTFE_BUDGET;
        admitted=ctfe_body_type_admit(artifact,body->parameters[a],true);
        if (admitted!=XR_XIR_CTFE_OK) return admitted;
    }
    for (uint32_t b=0;b<body->block_count;++b) {
        if (!xir_compile_work(work,2)) return XR_XIR_CTFE_BUDGET;
        if (body->blocks[b].panic || body->blocks[b].frontier) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (!xir_compile_work(work,3)) return XR_XIR_CTFE_BUDGET;
        const XrXirInstruction *op=&body->instructions[i];uint64_t ignored=0;
        admitted=ctfe_tuple_instruction_admit(artifact,body,op);
        if (admitted!=XR_XIR_CTFE_OK) return admitted;
        bool unit=ctfe_unit_result(op->op) ||
            ((op->op==XR_XIR_CALL || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_TUPLE_FIELD) && op->type==XR_XIR_UNIT);
        if (!ctfe_storage_instruction(module,body,op) || (unit && op->type!=XR_XIR_UNIT) ||
            (!unit && op->type==XR_XIR_UNIT))
            return XR_XIR_CTFE_NOT_SUPPORTED;
        if (op->op==XR_XIR_CALL) {
            if (!xir_compile_work(work,5)) return XR_XIR_CTFE_BUDGET;
            if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count)
                return XR_XIR_CTFE_BAD_ARTIFACT;
            const XrXirFunction *callee=&module->functions[(uint32_t)op->immediate];
            if (op->type!=callee->result || op->args[1]!=callee->parameter_count ||
                op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0])
                return XR_XIR_CTFE_BAD_ARTIFACT;
        } else if (!ctfe_opcode_work(op->op,&ignored)) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (op->op==XR_XIR_EQUAL) {
            if (!xir_compile_work(work,2)) return XR_XIR_CTFE_BUDGET;
            if (!ctfe_parameter_type(xr_xir_operand_type(body,op->args[0]))) return XR_XIR_CTFE_NOT_SUPPORTED;
        }
        if (op->op==XR_XIR_CONVERT_NUMBER &&
            (!xr_xir_type_is_number(op->type) ||
            !xr_xir_type_is_number(xr_xir_operand_type(body,op->args[0]))))
            return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    return XR_XIR_CTFE_OK;
}

/* Numeric visitation state bounds construction of the present reachable set.
 * It is not a stored proof: every queued body is fully admitted on this request. */
static XrXirCtfeStatus ctfe_call_closure(const XrXirArtifact *artifact,uint32_t selected,bool *owned) {
    const XrXirModule *module=&artifact->module;
    uint32_t count=module->function_count;
    uint64_t bytes=(uint64_t)count*(sizeof(uint32_t)+sizeof(uint8_t));
    if (!count || bytes>SIZE_MAX) return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(artifact->context.resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    uint32_t *queue=memory;uint8_t *seen=(uint8_t *)(queue+count);
    uint32_t first=0,last=1;
    if (!xir_compile_work(&artifact->context,sizeof(*queue)+1)) { status=XR_XIR_CTFE_BUDGET;goto done; }
    queue[0]=selected;seen[selected]=1;
    while (first<last && status==XR_XIR_CTFE_OK) {
        if (!xir_compile_work(&artifact->context,2)) { status=XR_XIR_CTFE_BUDGET;break; }
        uint32_t f=queue[first++];
        status=ctfe_call_body_admit(artifact,f);
        if (status!=XR_XIR_CTFE_OK) break;
        if (artifact->layouts[f].owned_count || module->functions[f].result==XR_XIR_STRING) *owned=true;
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            if (!xir_compile_work(&artifact->context,1)) { status=XR_XIR_CTFE_BUDGET;break; }
            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL) continue;
            uint32_t target=(uint32_t)op->immediate;
            if (!xir_compile_work(&artifact->context,1)) { status=XR_XIR_CTFE_BUDGET;break; }
            if (seen[target]) continue;
            if (last>=count) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if (!xir_compile_work(&artifact->context,sizeof(*queue)+2)) { status=XR_XIR_CTFE_BUDGET;break; }
            seen[target]=1;queue[last++]=target;
        }
    }
done:
    xr_compile_resources_free(memory);return status;
}

static XrXirCtfeStatus ctfe_call_prepare(const XrXirCtfeCallRequest *request,bool *owned) {
    const XrXirCtfeRequest *input=&request->evaluation;
    const XrXirArtifact *artifact=input->artifact;
    XrXirCtfeStatus status=ctfe_compile_status(xr_xir_compile_artifact_verify(artifact,NULL));
    if (status!=XR_XIR_CTFE_OK) return status;
    if (artifact->module.stage!=XR_XIR_LOWERED || input->function>=artifact->module.function_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    const XrXirCompileContext *work=&artifact->context;
    if (!xir_compile_work(work,1)) return XR_XIR_CTFE_BUDGET;
    const XrXirTarget *target=xr_xir_compile_artifact_target(artifact);
    if (!target || target->architecture!=XR_XIR_ARCH_X86_64 || target->abi_version!=XR_XIR_VALUE_ABI_VERSION ||
        input->target.architecture!=target->architecture || input->target.abi_version!=target->abi_version)
        return XR_XIR_CTFE_PROFILE;
    for (uint32_t b=0;b<32;++b) {
        if (!xir_compile_work(work,1)) return XR_XIR_CTFE_BUDGET;
        if (input->checked_identity[b]!=artifact->checked_identity[b]) return XR_XIR_CTFE_IDENTITY;
    }
    status=ctfe_call_closure(artifact,input->function,owned);
    if (status!=XR_XIR_CTFE_OK) return status;
    const XrXirFunction *body=&artifact->module.functions[input->function];
    if (input->argument_count!=body->parameter_count) return XR_XIR_CTFE_BAD_ARGUMENT;
    for (uint32_t a=0;a<input->argument_count;++a) {
        if (!xir_compile_work(work,1)) return XR_XIR_CTFE_BUDGET;
        if (xr_xir_tuple_signature(artifact->module.types,body->parameters[a])) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (!xr_xir_value_argument(&input->arguments[a],NULL,body->parameters[a])) return XR_XIR_CTFE_BAD_ARGUMENT;
    }
    XrXirLayout storage={0};
    status=ctfe_compile_status(xr_xir_compile_layout(work,artifact->module.types,
        body->result,target,XR_XIR_LAYOUT_STORAGE,&storage));
    if (status==XR_XIR_CTFE_OK && !xr_xir_type_is_owned(artifact->module.types,body->result) && storage.size>input->limits.output_bytes) return XR_XIR_CTFE_OUTPUT_LIMIT;
    return status;
}

static XrXirCtfeStatus ctfe_call_frame_new(CtfeCallExecution *execution,uint32_t function,
    uint32_t return_value,CtfeCallFrame **output) {
    const XrXirArtifact *artifact=execution->artifact;
    if (!output || *output || function>=artifact->module.function_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (execution->depth>=execution->request->call_depth) return XR_XIR_CTFE_DEPTH_LIMIT;
    uint64_t scratch=0;
    XrXirCtfeStatus status=ctfe_tuple_scratch(artifact,function,&scratch);
    if (status!=XR_XIR_CTFE_OK) return status;
    uint64_t bytes=(uint64_t)sizeof(CtfeCallFrame)+artifact->layouts[function].frame_bytes+scratch;
    uint64_t maximum=execution->request->evaluation.limits.frame_bytes;
    if (bytes>SIZE_MAX || execution->live_bytes>maximum || bytes>maximum-execution->live_bytes)
        return XR_XIR_CTFE_FRAME_LIMIT;
    if (!xir_compile_work(&artifact->context,(uint64_t)artifact->layouts[function].owned_count*2))
        return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;
    status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(artifact->context.resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!xir_compile_work(&artifact->context,2*sizeof(void *)+2*sizeof(uint32_t)+sizeof(size_t)+sizeof(ScalarRun))) {
        xr_compile_resources_free(memory);return XR_XIR_CTFE_BUDGET;
    }
    CtfeCallFrame *frame=memory;
    frame->parent=execution->top;frame->function=function;frame->return_value=return_value;frame->bytes=(size_t)bytes;
    /* The Artifact is borrowed synchronously and cannot grow during execution. */
    frame->run=(ScalarRun){&artifact->module,&artifact->module.functions[function],
        &artifact->layouts[function],frame+1,execution->owned.arena,artifact->context.resources};
    if (scratch) {
        if (!xir_compile_work(&artifact->context,sizeof(frame->state.arguments))) { xr_compile_resources_free(frame);return XR_XIR_CTFE_BUDGET; }
        frame->state.arguments=(XrXirValue *)((uint8_t *)(frame+1)+artifact->layouts[function].frame_bytes);
    }
    *output=frame;return XR_XIR_CTFE_OK;
}

static void ctfe_call_frames_free(CtfeCallExecution *execution) {
    while (execution->top) {
        CtfeCallFrame *frame=execution->top;execution->top=frame->parent;
        ctfe_owned_frame_clear(&frame->run);xr_compile_resources_free(frame);
    }
}

static XrXirCtfeStatus ctfe_call_enter(CtfeCallExecution *execution,
    const XrXirInstruction *op) {
    CtfeCallFrame *parent=execution->top;
    const XrXirArtifact *artifact=execution->artifact;
    const XrXirFunction *body=&artifact->module.functions[parent->function];
    const XrXirFunctionLayout *layout=&artifact->layouts[parent->function];
    uint32_t callee=(uint32_t)op->immediate;
    const XrXirFunction *target=&artifact->module.functions[callee];
    const XrXirFunctionLayout *target_layout=&artifact->layouts[callee];
    CtfeCallFrame *child=NULL;
    XrXirCtfeStatus status=ctfe_call_frame_new(execution,callee,
        body->parameter_count+parent->state.instruction,&child);
    if (status!=XR_XIR_CTFE_OK) return status;
    for (uint32_t p=0;p<target->parameter_count;++p) {
        if (!xir_compile_work(&artifact->context,6+sizeof(XrXirValue))) { status=XR_XIR_CTFE_BUDGET;break; }
        uint32_t id=body->operands[op->args[0]+p];
        XrXirValue argument={(uint32_t)xr_xir_operand_type(body,id),0,
            xr_xir_scalar_load(parent+1,layout->offsets[id])};
        status=ctfe_validation_work(&parent->run,&execution->owned,target->parameters[p]);
        if (status!=XR_XIR_CTFE_OK) break;
        if (!xr_xir_value_argument(&argument,execution->owned.arena,target->parameters[p])) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
        status=ctfe_argument_store(&child->run,target_layout->offsets[p],&argument,&execution->owned,false);
        if (status!=XR_XIR_CTFE_OK) break;
    }
    if (status!=XR_XIR_CTFE_OK) { ctfe_owned_frame_clear(&child->run);xr_compile_resources_free(child);return status; }
    if (!xir_compile_work(&artifact->context,sizeof(parent->state.instruction)+
        sizeof(execution->top)+sizeof(execution->depth)+sizeof(execution->live_bytes))) {
        ctfe_owned_frame_clear(&child->run);xr_compile_resources_free(child);return XR_XIR_CTFE_BUDGET;
    }
    ++parent->state.instruction;execution->top=child;++execution->depth;execution->live_bytes+=child->bytes;
    return XR_XIR_CTFE_OK;
}

static XrXirCtfeStatus ctfe_call_return(CtfeCallExecution *execution,
    const XrXirValue *value,XrXirValue *output) {
    CtfeCallFrame *frame=execution->top;
    const XrXirArtifact *artifact=execution->artifact;
    const XrXirFunction *body=&artifact->module.functions[frame->function];
    XrXirCtfeStatus validation=ctfe_validation_work(&frame->run,&execution->owned,body->result);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    if (!ctfe_result_value(value,body->result,execution->owned.arena)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!frame->parent) {
        if (!xir_compile_work(&artifact->context,sizeof(*output))) return XR_XIR_CTFE_BUDGET;
        return ctfe_result_publish(&frame->run,value,body->result,
            execution->request->evaluation.limits.output_bytes,&execution->owned,output);
    }
    CtfeCallFrame *parent=frame->parent;
    const XrXirFunction *caller=&artifact->module.functions[parent->function];
    const XrXirFunctionLayout *layout=&artifact->layouts[parent->function];
    if (!xir_compile_work(&artifact->context,4)) return XR_XIR_CTFE_BUDGET;
    if (frame->return_value>=layout->slot_count || (uint32_t)xr_xir_operand_type(caller,frame->return_value)!=value->type)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!xir_compile_work(&artifact->context,sizeof(execution->top)+sizeof(execution->depth)+
        sizeof(execution->live_bytes)+1))
        return XR_XIR_CTFE_BUDGET;
    if (value->type!=XR_XIR_UNIT) {
        XrXirCtfeStatus status=ctfe_argument_store(&parent->run,layout->offsets[frame->return_value],
            value,&execution->owned,false);
        if (status!=XR_XIR_CTFE_OK) return status;
    }
    execution->top=parent;--execution->depth;execution->live_bytes-=frame->bytes;
    ctfe_owned_frame_clear(&frame->run);xr_compile_resources_free(frame);return XR_XIR_CTFE_OK;
}

static XrXirCtfeStatus ctfe_call_execute(CtfeCallExecution *execution,XrXirValue *output) {
    const XrXirArtifact *artifact=execution->artifact;
    for (;;) {
        CtfeCallFrame *frame=execution->top;
        const XrXirFunction *body=&artifact->module.functions[frame->function];
        ScalarRun *run=&frame->run;
        if (!scalar_compile_work(run,1)) return XR_XIR_CTFE_BUDGET;
        if (!execution->steps) return XR_XIR_CTFE_STEP_LIMIT;
        --execution->steps;
        if (frame->state.instruction>=body->instruction_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        const XrXirInstruction *op=&body->instructions[frame->state.instruction];
        execution->owned.frame_spare=execution->request->evaluation.limits.frame_bytes-execution->live_bytes;
        if (op->op==XR_XIR_CALL) {
            if (!scalar_compile_work(run,2)) return XR_XIR_CTFE_BUDGET;
            XrXirCtfeStatus status=ctfe_call_enter(execution,op);
            if (status!=XR_XIR_CTFE_OK) return status;
            continue;
        }
        uint64_t work=0;
        if (!ctfe_opcode_work(op->op,&work)) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (!scalar_compile_work(run,work+1)) return XR_XIR_CTFE_BUDGET;
        XrXirCtfeStatus flow=ctfe_tuple_flow_work(run,op,&execution->owned);
        if (flow!=XR_XIR_CTFE_OK) return flow;
        if (ctfe_float_opcode(run,op)) {
            XrXirCtfeStatus status=ctfe_float_step(run,&frame->state.instruction);
            if (status!=XR_XIR_CTFE_OK) return status;
            continue;
        }
        if (ctfe_tuple_opcode(op->op)) {
            XrXirCtfeStatus status=ctfe_tuple_step(run,&frame->state,&execution->owned);
            if (status!=XR_XIR_CTFE_OK) return status;
            continue;
        }
        if (ctfe_cell_opcode(op->op)) {
            XrXirCtfeStatus status=ctfe_cell_step(run,&frame->state.instruction,&execution->owned);
            if (status!=XR_XIR_CTFE_OK) return status;
            continue;
        }
        if (ctfe_string_opcode(run,op)) {
            XrXirCtfeStatus status=ctfe_string_step(run,&frame->state.instruction,&execution->owned);
            if (status!=XR_XIR_CTFE_OK) return status;
            continue;
        }
        XirScalarCoreAction action;XrXirRunStatus ran=xir_scalar_core_step(run,&frame->state.instruction,&action);
        if (ran!=XR_XIR_RUN_OK) return ran==XR_XIR_RUN_STEP_LIMIT?XR_XIR_CTFE_BUDGET:
            ran==XR_XIR_RUN_DIVIDE_BY_ZERO?XR_XIR_CTFE_DIVIDE_BY_ZERO:
            ran==XR_XIR_RUN_NUMERIC_RANGE?XR_XIR_CTFE_NUMERIC_RANGE:XR_XIR_CTFE_BAD_ARTIFACT;
        if (action.kind==XIR_SCALAR_RETURN) {
            bool outer=!frame->parent;
            XrXirCtfeStatus status=ctfe_call_return(execution,&action.value,output);
            if (status!=XR_XIR_CTFE_OK || outer) return status;
        } else if (action.kind!=XIR_SCALAR_CONTINUE) return XR_XIR_CTFE_BAD_ARTIFACT;
    }
}

XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_calls(const XrXirCtfeCallRequest *request,
    XrXirValue *output) {
    if (!request || !output || output->type || output->reserved || output->payload ||
        !request->evaluation.artifact || (request->evaluation.argument_count && !request->evaluation.arguments) ||
        request->evaluation.limits.steps>XR_XIR_CTFE_MAX_STEPS ||
        request->evaluation.limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->evaluation.limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES ||
        request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH) return XR_XIR_CTFE_BAD_ARGUMENT;
    bool owned=false;
    XrXirCtfeStatus status=ctfe_call_prepare(request,&owned);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!xir_compile_work(&request->evaluation.artifact->context,sizeof(CtfeCallExecution)))
        return XR_XIR_CTFE_BUDGET;
    CtfeCallExecution execution={.request=request,.artifact=request->evaluation.artifact,
        .steps=request->evaluation.limits.steps};
    status=ctfe_owned_new(execution.artifact,owned,&execution.owned);
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeCallFrame *first=NULL;
    status=ctfe_call_frame_new(&execution,request->evaluation.function,UINT32_MAX,&first);
    if (status!=XR_XIR_CTFE_OK) { ctfe_owned_free(&execution.owned);return status; }
    if (!xir_compile_work(&execution.artifact->context,sizeof(execution.top)+
        sizeof(execution.depth)+sizeof(execution.live_bytes))) {
        xr_compile_resources_free(first);ctfe_owned_free(&execution.owned);return XR_XIR_CTFE_BUDGET;
    }
    execution.top=first;execution.depth=1;execution.live_bytes=first->bytes;
    const XrXirFunctionLayout *layout=&execution.artifact->layouts[first->function];
    for (uint32_t p=0;p<request->evaluation.argument_count;++p) {
        if (!xir_compile_work(&execution.artifact->context,1)) { status=XR_XIR_CTFE_BUDGET;break; }
        status=ctfe_argument_store(&first->run,layout->offsets[p],&request->evaluation.arguments[p],&execution.owned,true);
        if (status!=XR_XIR_CTFE_OK) break;
    }
    if (status==XR_XIR_CTFE_OK) status=ctfe_call_execute(&execution,output);
    ctfe_call_frames_free(&execution);ctfe_owned_free(&execution.owned);return status;
}
