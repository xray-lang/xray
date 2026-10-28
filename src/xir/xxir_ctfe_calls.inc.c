/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_calls.inc.c - Finite compiler-owned direct and indirect call activations
 *
 * KEY CONCEPT:
 *   Each request admits all direct and Function producer targets. Numeric and
 *   CFG execution share scalar_step; compiler calls never enter Instance admission.
 */
#include "xxir_native_callable.h"

typedef struct CtfeCellLoan {
    XrXirValue value;XrXirCellLoan loan;
    bool projected;
} CtfeCellLoan;
typedef struct CtfeCallFrame {
    struct CtfeCallFrame *parent;
    uint32_t function, return_value;
    size_t bytes;
    uint64_t epoch;
    CtfeCellLoan *loans;
    struct CtfeViewBinding *views;
    struct CtfeCallExecution *execution;
    uint32_t loan_count,loan_capacity,cleanup_parent;
    bool loans_committed,cleanup,invoked;
    uint8_t exit_kind;
    XrXirValue returned;
    XrXirPanicPayload panic;
    VmState state;
    ScalarRun run;
} CtfeCallFrame;

typedef struct CtfeCallExecution {
    const XrXirCtfeCallRequest *request;
    const XrXirArtifact *artifact;
    CtfeCallFrame *top;
    uint64_t steps, live_bytes,epoch,loan_bytes;
    uint32_t depth;
    CtfeOwnedState owned;
} CtfeCallExecution;

#include "xxir_ctfe_view.inc.c"

/* Scalar types and admitted opcodes use the same rules as leaf execution.
 * Calls are handled separately, before any runtime scalar CALL dispatcher. */
static XrXirCtfeStatus ctfe_call_body_admit(const XrXirArtifact *artifact,uint32_t f,CtfeTypeInfo **summaries) {
    const XrXirModule *module=&artifact->module;
    const XrXirFunction *body=&module->functions[f];

    if (!module->declarations || !module->declarations->functions) return XR_XIR_CTFE_NOT_SUPPORTED;
    const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
    if (identity->native_callable) {
        if (xir_native_callable_verify(module,f)!=XR_XIR_OK) return XR_XIR_CTFE_BAD_ARTIFACT;
    } else if (!body->block_count || !body->instruction_count) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus owner=ctfe_nominal_owner_admit(artifact,identity->nominal_owner);
    if (owner!=XR_XIR_CTFE_OK) return owner;
    if (module->generics && (module->generics[f].parameter_count || module->generics[f].argument_count))
        return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeStatus admitted=ctfe_body_type_admit(artifact,body->result,false,summaries);
    if (admitted!=XR_XIR_CTFE_OK) return admitted;
    for (uint32_t m=0;m<module->declarations->module_count;++m) {

        if (module->declarations->modules[m].initializer==f) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    for (uint32_t a=0;a<body->parameter_count;++a) {

        if (identity->cleanup_owner && ctfe_slice_u8_type(module->types,body->parameters[a]))
            return XR_XIR_CTFE_NOT_SUPPORTED;
        admitted=ctfe_body_type_admit(artifact,body->parameters[a],true,summaries);
        if (admitted!=XR_XIR_CTFE_OK) return admitted;
    }
    for (uint32_t b=0;b<body->block_count;++b) {

    }
    for (uint32_t i=0;i<body->instruction_count;++i) {

        const XrXirInstruction *op=&body->instructions[i];
        admitted=ctfe_tuple_instruction_admit(artifact,body,op,summaries);
        if (admitted!=XR_XIR_CTFE_OK) return admitted;
        bool unit=ctfe_unit_result(op->op) || (op->op==XR_XIR_PANIC_CATCH && op->type==XR_XIR_UNIT) ||
            ((op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT || op->op==XR_XIR_INVOKE_RESULT || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_UNWRAP) && op->type==XR_XIR_UNIT);
        if (!ctfe_storage_instruction(module,body,op) || (unit && op->type!=XR_XIR_UNIT) ||
            (!unit && op->type==XR_XIR_UNIT))
            return XR_XIR_CTFE_NOT_SUPPORTED;
        if (op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_CLEANUP_REGISTER) {

            if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count) return XR_XIR_CTFE_BAD_ARTIFACT;
            const XrXirFunction *callee=&module->functions[(uint32_t)op->immediate];
            if (op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0]) return XR_XIR_CTFE_BAD_ARTIFACT;
            if ((op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE || op->op==XR_XIR_CLEANUP_REGISTER) &&
                (op->type!=callee->result || op->args[1]!=callee->parameter_count)) return XR_XIR_CTFE_BAD_ARTIFACT;
        } else if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {

            const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,xr_xir_operand_type(body,(uint32_t)op->immediate));
            if (!signature || signature->parameter_count!=op->args[1] || signature->result!=op->type) return XR_XIR_CTFE_BAD_ARTIFACT;
        } else if (!ctfe_opcode_admitted(op->op)) return XR_XIR_CTFE_NOT_SUPPORTED;
        if (op->op==XR_XIR_EQUAL) {

            admitted=ctfe_equal_admit(artifact,xr_xir_operand_type(body,op->args[0]));
            if (admitted!=XR_XIR_CTFE_OK) return admitted;
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
static XrXirCtfeStatus ctfe_call_cells_admit(const XrXirArtifact *artifact,uint32_t function,XrXirCellProvenance **proof) {
    const XrXirFunction *body=&artifact->module.functions[function];
    for (uint32_t p=0;p<body->parameter_count;++p) {

        if (!xr_xir_type_is_cell(artifact->module.types,body->parameters[p])) continue;
        if (!*proof) {
            XrXirCtfeStatus status=ctfe_compile_status(xr_xir_compile_cell_provenance_verified(&artifact->context,&artifact->module,proof,NULL));
            if (status!=XR_XIR_CTFE_OK) return status;
        }
        XrXirCellProofRole role=xr_xir_cell_provenance_role(*proof,function,p);
        if (role!=XR_XIR_CELL_PROOF_OWNED_CAPTURE && role!=XR_XIR_CELL_PROOF_SCOPED_REF &&
            role!=XR_XIR_CELL_PROOF_LEXICAL_CLEANUP) return XR_XIR_CTFE_NOT_SUPPORTED;
    }
    return XR_XIR_CTFE_OK;
}

static XrXirCtfeStatus ctfe_call_closure(const XrXirArtifact *artifact,uint32_t selected,bool *owned,
    XrXirCellProvenance **cells,void **closure) {
    const XrXirModule *module=&artifact->module;
    uint32_t count=module->function_count;
    uint64_t bytes=(uint64_t)count*(sizeof(uint32_t)+sizeof(uint8_t));
    if (!count) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (bytes>SIZE_MAX) return XR_XIR_CTFE_OVERFLOW;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(artifact->context.resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    uint32_t *queue=memory;uint8_t *seen=(uint8_t *)(queue+count);
    uint32_t first=0,last=1;bool functions=false;CtfeTypeInfo *summaries=NULL;

    queue[0]=selected;seen[selected]=1;
    while (first<last && status==XR_XIR_CTFE_OK) {

        uint32_t f=queue[first++];
        status=ctfe_call_cells_admit(artifact,f,cells);if (status!=XR_XIR_CTFE_OK) break;
        status=ctfe_call_body_admit(artifact,f,&summaries);
        if (status!=XR_XIR_CTFE_OK) break;
        if (artifact->layouts[f].owned_count || module->functions[f].result==XR_XIR_STRING) *owned=true;
        const XrXirFunction *body=&module->functions[f];
        for (uint32_t i=0;i<body->instruction_count;++i) {

            const XrXirInstruction *op=&body->instructions[i];
            if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_CLEANUP_REGISTER) continue;
            if (op->op==XR_XIR_FUNCTION_REF) functions=true;
            uint32_t target=(uint32_t)op->immediate;

            if (seen[target]) continue;
            if (last>=count) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }

            seen[target]=1;queue[last++]=target;
        }
    }
    if (status==XR_XIR_CTFE_OK && functions) { *closure=memory;memory=NULL; }
    xr_compile_resources_free(summaries);xr_compile_resources_free(memory);return status;
}

static XrXirCtfeStatus ctfe_call_prepare_received(const XrXirCtfeCallRequest *request,bool *owned,
    XrXirCellProvenance **cells,void **closure) {
    const XrXirCtfeRequest *input=&request->evaluation;
    const XrXirArtifact *artifact=input->artifact;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    if (artifact->module.stage!=XR_XIR_LOWERED || input->function>=artifact->module.function_count)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    if (artifact->evaluation_only && input->function!=artifact->module.provenance->evaluation.callee)
        return XR_XIR_CTFE_BAD_ARGUMENT;
    if (artifact->module.declarations && artifact->module.declarations->functions &&
        (artifact->module.declarations->functions[input->function].cleanup_owner ||
         artifact->module.declarations->functions[input->function].method_kind==XR_XIR_MATERIALIZER)) return XR_XIR_CTFE_NOT_SUPPORTED;
    const XrXirCompileContext *context=&artifact->context;

    const XrXirTarget *target=xr_xir_compile_artifact_target(artifact);
    if (!target || target->architecture!=XR_XIR_ARCH_X86_64 || target->abi_version!=XR_XIR_VALUE_ABI_VERSION ||
        input->target.architecture!=target->architecture || input->target.abi_version!=target->abi_version)
        return XR_XIR_CTFE_PROFILE;
    for (uint32_t b=0;b<32;++b) {

        if (input->checked_identity[b]!=artifact->checked_identity[b]) return XR_XIR_CTFE_IDENTITY;
    }
    status=ctfe_call_closure(artifact,input->function,owned,cells,closure);
    if (status!=XR_XIR_CTFE_OK) return status;
    const XrXirFunction *body=&artifact->module.functions[input->function];
    status=xr_xir_compile_ctfe_type_admit(context,artifact->module.types,body->result);if (status!=XR_XIR_CTFE_OK) return status;
    if (input->argument_count!=body->parameter_count) return XR_XIR_CTFE_BAD_ARGUMENT;
    for (uint32_t a=0;a<input->argument_count;++a) {

        status=ctfe_external_argument_admit(artifact,body->parameters[a],&input->arguments[a],NULL);
        if (status!=XR_XIR_CTFE_OK) return status;
    }
    if (!xr_xir_type_is_owned(artifact->module.types,body->result)) {
        XrXirLayout storage={0};
        status=ctfe_compile_status(xr_xir_compile_layout(context,artifact->module.types,
            body->result,target,XR_XIR_LAYOUT_STORAGE,&storage));
        if (status==XR_XIR_CTFE_OK && storage.size>input->limits.output_bytes) return XR_XIR_CTFE_OUTPUT_LIMIT;
    }
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
    uint32_t cells=0;const XrXirFunction *body=&artifact->module.functions[function];
    for (uint32_t p=0;p<body->parameter_count;++p) {

        if (xr_xir_type_is_cell(artifact->module.types,body->parameters[p])) ++cells;
    }
    uint64_t bytes=(uint64_t)sizeof(CtfeCallFrame)+artifact->layouts[function].frame_bytes+scratch+(uint64_t)cells*sizeof(CtfeCellLoan);
    uint64_t maximum=execution->request->evaluation.limits.frame_bytes;
    if (bytes>SIZE_MAX || execution->live_bytes>maximum || execution->loan_bytes>maximum-execution->live_bytes || bytes>maximum-execution->live_bytes-execution->loan_bytes)
        return XR_XIR_CTFE_FRAME_LIMIT;

    void *memory=NULL;
    status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(artifact->context.resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;

    CtfeCallFrame *frame=memory;
    frame->execution=execution;frame->parent=execution->top;frame->function=function;frame->return_value=return_value;frame->bytes=(size_t)bytes;
    frame->epoch=++execution->epoch;frame->loan_capacity=cells;
    frame->loans=(CtfeCellLoan *)((uint8_t *)(frame+1)+artifact->layouts[function].frame_bytes+scratch);
    /* The Artifact is borrowed synchronously and cannot grow during execution. */
    frame->run=(ScalarRun){&artifact->module,&artifact->module.functions[function],
        &artifact->layouts[function],frame+1,execution->owned.arena,artifact->context.resources};
    if (scratch) {

        frame->state.arguments=(XrXirValue *)((uint8_t *)(frame+1)+artifact->layouts[function].frame_bytes);
    }
    *output=frame;return XR_XIR_CTFE_OK;
}

#include "xxir_ctfe_loans.inc.c"

static void ctfe_call_frames_free(CtfeCallExecution *execution) {
    while (execution->top) {
        CtfeCallFrame *frame=execution->top;execution->top=frame->parent;
        ctfe_call_frame_free(frame);
    }
}

static XrXirCtfeStatus ctfe_call_enter(CtfeCallExecution *execution,
    const XrXirInstruction *op,bool cleanup) {
    CtfeCallFrame *parent=execution->top;
    const XrXirArtifact *artifact=execution->artifact;
    const XrXirFunction *body=&artifact->module.functions[parent->function];
    uint32_t callee=(uint32_t)op->immediate,captures=0;const XrXirFunctionBinding *binding=NULL;
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        XrXirValue function=ctfe_nominal_operand(&parent->run,(uint32_t)op->immediate);
        XrXirCtfeStatus resolved=ctfe_function_binding(&execution->owned,&function,&binding);if (resolved!=XR_XIR_CTFE_OK) return resolved;
        callee=binding->entry;captures=binding->capture_count;
    }
    const XrXirFunction *target=&artifact->module.functions[callee];
    const XrXirFunctionLayout *target_layout=&artifact->layouts[callee];
    if (captures>target->parameter_count || op->args[1]!=target->parameter_count-captures) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (artifact->module.declarations->functions[callee].native_callable && (captures || cleanup))
        return XR_XIR_CTFE_NOT_SUPPORTED;
    CtfeCallFrame *child=NULL;
    XrXirCtfeStatus status=ctfe_call_frame_new(execution,callee,
        cleanup?UINT32_MAX:body->parameter_count+parent->state.instruction,&child);
    if (status!=XR_XIR_CTFE_OK) return status;
    child->cleanup=cleanup;child->invoked=op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT;uint64_t spare=execution->owned.frame_spare;
    execution->owned.frame_spare-=child->bytes;
    for (uint32_t p=0;p<target->parameter_count;++p) {

        XrXirValue argument={0};
        if (p<captures) argument=binding->captures[p];
        else { uint32_t id=body->operands[op->args[0]+p-captures];argument=ctfe_nominal_operand(&parent->run,id); }
        if (ctfe_slice_u8_type(artifact->module.types,target->parameters[p])) {
            status=p<captures?XR_XIR_CTFE_NOT_SUPPORTED:ctfe_view_argument(execution,child,p,&argument);
            if (status!=XR_XIR_CTFE_OK) break;
            continue;
        }
        status=ctfe_value_type_admit(&parent->run,&execution->owned,target->parameters[p]);
        if (status!=XR_XIR_CTFE_OK) break;

        if (!xr_xir_value_argument(&argument,execution->owned.arena,target->parameters[p])) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
        status=ctfe_argument_store(&child->run,target_layout->offsets[p],&argument,&execution->owned,false);
        if (status!=XR_XIR_CTFE_OK) break;
    }
    if (status==XR_XIR_CTFE_OK) status=ctfe_call_loans_prepare(execution,child,captures);
    if (status!=XR_XIR_CTFE_OK) { execution->owned.frame_spare=spare;ctfe_call_frame_free(child);return status; }

    execution->owned.frame_spare=spare;ctfe_call_loans_commit(execution,child);
    if (!cleanup && !child->invoked) ++parent->state.instruction;execution->top=child;++execution->depth;execution->live_bytes+=child->bytes;
    return XR_XIR_CTFE_OK;
}

#include "xxir_ctfe_error_flow.inc.c"
#include "xxir_ctfe_panic_flow.inc.c"

static XrXirCtfeStatus ctfe_call_return(CtfeCallExecution *execution,
    const XrXirValue *value,XrXirValue *output) {
    CtfeCallFrame *frame=execution->top;
    const XrXirArtifact *artifact=execution->artifact;
    const XrXirFunction *body=&artifact->module.functions[frame->function];
    XrXirCtfeStatus validation=ctfe_value_type_admit(&frame->run,&execution->owned,body->result);
    if (validation!=XR_XIR_CTFE_OK) return validation;
    if (!ctfe_result_value(value,body->result,execution->owned.arena)) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!frame->parent) {

        return ctfe_result_publish(&frame->run,value,body->result,
            execution->request->evaluation.limits.output_bytes,&execution->owned,output);
    }
    CtfeCallFrame *parent=frame->parent;
    const XrXirFunction *caller=&artifact->module.functions[parent->function];
    const XrXirFunctionLayout *layout=&artifact->layouts[parent->function];

    if (!frame->cleanup && !frame->invoked && (frame->return_value>=layout->slot_count || (uint32_t)xr_xir_operand_type(caller,frame->return_value)!=value->type))
        return XR_XIR_CTFE_BAD_ARTIFACT;

    if (frame->cleanup) {
        if (value->type!=XR_XIR_UNIT || !parent->state.cleanup_waiting) return XR_XIR_CTFE_BAD_ARTIFACT;
        parent->state.frontier=frame->cleanup_parent;parent->state.cleanup_waiting=false;
    }
    if (frame->invoked) {
        XrXirCtfeStatus status=ctfe_invoke_deliver(execution,parent,value,false);if (status!=XR_XIR_CTFE_OK) return status;
    }
    if (!frame->cleanup && !frame->invoked && value->type!=XR_XIR_UNIT) {
        XrXirCtfeStatus status=ctfe_argument_store(&parent->run,layout->offsets[frame->return_value],
            value,&execution->owned,false);
        if (status!=XR_XIR_CTFE_OK) return status;
    }
    execution->top=parent;--execution->depth;execution->live_bytes-=frame->bytes;
    ctfe_call_frame_free(frame);return XR_XIR_CTFE_OK;
}

#include "xxir_ctfe_native_callable.inc.c"
#include "xxir_ctfe_cleanup.inc.c"
#include "xxir_ctfe_dispatch.inc.c"

static XrXirCtfeStatus ctfe_call_execute(CtfeCallExecution *execution,XrXirValue *output) {
    const XrXirArtifact *artifact=execution->artifact;
    for (;;) {
        CtfeCallFrame *frame=execution->top;
        const XrXirFunction *body=&artifact->module.functions[frame->function];
        ScalarRun *run=&frame->run;
        if (!(execution->steps & 63u)) {
            XrXirCtfeStatus controlled=ctfe_control_poll(execution->request->evaluation.control);
            if (controlled!=XR_XIR_CTFE_OK) return controlled;
        }
        if (!execution->steps) return XR_XIR_CTFE_STEP_LIMIT;
        --execution->steps;
        bool native=artifact->module.declarations->functions[frame->function].native_callable!=0;
        if (native && xir_native_callable_verify(&artifact->module,frame->function)!=XR_XIR_OK)
            return XR_XIR_CTFE_BAD_ARTIFACT;
        if (!native && frame->state.instruction>=body->instruction_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        execution->owned.frame_spare=execution->request->evaluation.limits.frame_bytes-execution->live_bytes-execution->loan_bytes;
    execution->owned.loan_base=execution->loan_bytes;
        execution->owned.authority=(XrXirCellAuthority){execution,frame,frame->epoch};execution->owned.activation=frame;
        if (frame->state.leaving) {
            bool done=false;XrXirCtfeStatus status=ctfe_cleanup_continue(execution,output,&done);
            if (status!=XR_XIR_CTFE_OK || done) return status;continue;
        }
        uint32_t at=frame->state.instruction;execution->owned.fault=(XrXirFaultDetail){0};
        XirScalarCoreAction action={0};
        XrXirCtfeStatus stepped=native?ctfe_native_callable_step(execution,&action):ctfe_call_step(execution,&action);
        if (stepped!=XR_XIR_CTFE_OK) {
            XrXirCtfeStatus caught=ctfe_panic_fault(execution,stepped,at);
            if (caught!=XR_XIR_CTFE_OK) return caught;continue;
        }
        if (action.kind==XIR_SCALAR_RETURN) {
            if (frame->state.frontier) {
                XrXirCtfeStatus status=ctfe_value_graph_admit(run,&execution->owned,&action.value);
                if (status!=XR_XIR_CTFE_OK) return status;
                status=ctfe_value_status(xr_xir_value_copy(&action.value,&frame->returned));
                if (status!=XR_XIR_CTFE_OK) return status;
                frame->exit_kind=CTFE_EXIT_RETURN;frame->state.leaving=true;frame->state.exit_target=0;continue;
            }
            bool outer=!frame->parent;
            XrXirCtfeStatus status=ctfe_call_return(execution,&action.value,output);
            if (status!=XR_XIR_CTFE_OK || outer) return status;
        } else if (action.kind!=XIR_SCALAR_CONTINUE) return XR_XIR_CTFE_BAD_ARTIFACT;
    }
}

static XrXirCtfeStatus ctfe_call_run_received(const XrXirCtfeCallRequest *request,XrXirValue *output,
    bool owned,XrXirCellProvenance *cells,void *closure) {
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    if (status!=XR_XIR_CTFE_OK) { if (cells) xr_xir_compile_cell_provenance_free(cells);xr_compile_resources_free(closure);return status; }
    CtfeCallExecution execution={.request=request,.artifact=request->evaluation.artifact,.steps=request->evaluation.limits.steps};
    execution.owned.control=request->evaluation.control;
    status=ctfe_owned_new(execution.artifact,owned,&execution.owned);
    if (status!=XR_XIR_CTFE_OK) { if (cells) xr_xir_compile_cell_provenance_free(cells);xr_compile_resources_free(closure);return status; }
    execution.owned.loan_bytes=&execution.loan_bytes;
    execution.owned.cells=cells;execution.owned.closure=closure;
    execution.owned.reachable=closure?(uint8_t *)((uint32_t *)closure+execution.artifact->module.function_count):NULL;
    CtfeCallFrame *first=NULL;
    status=ctfe_call_frame_new(&execution,request->evaluation.function,UINT32_MAX,&first);
    if (status!=XR_XIR_CTFE_OK) { ctfe_owned_free(&execution.owned);return status; }

    execution.top=first;execution.depth=1;execution.live_bytes=first->bytes;
    execution.owned.frame_spare=request->evaluation.limits.frame_bytes-first->bytes;
    const XrXirFunctionLayout *layout=&execution.artifact->layouts[first->function];
    for (uint32_t p=0;p<request->evaluation.argument_count;++p) {

        status=ctfe_external_argument_store(&first->run,layout->offsets[p],first->run.function->parameters[p],&request->evaluation.arguments[p],&execution.owned);
        if (status!=XR_XIR_CTFE_OK) break;
    }
    if (status==XR_XIR_CTFE_OK) status=ctfe_call_execute(&execution,output);
    ctfe_call_frames_free(&execution);ctfe_owned_free(&execution.owned);return status;
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_calls(const XrXirCtfeCallRequest *request,
    XrXirValue *output) {
    if (!request || !output || output->type || output->reserved || output->payload ||
        !request->evaluation.artifact || (request->evaluation.argument_count && !request->evaluation.arguments) ||
        request->evaluation.limits.steps>XR_XIR_CTFE_MAX_STEPS ||
        request->evaluation.limits.frame_bytes>XR_XIR_CTFE_MAX_FRAME_BYTES ||
        request->evaluation.limits.output_bytes>XR_XIR_CTFE_MAX_OUTPUT_BYTES ||
        request->call_depth>XR_XIR_CTFE_MAX_CALL_DEPTH) return XR_XIR_CTFE_BAD_ARGUMENT;
    XrXirCtfeStatus status=ctfe_control_poll(request->evaluation.control);
    if (status!=XR_XIR_CTFE_OK) return status;
    status=ctfe_compile_status(xr_xir_compile_artifact_verify(request->evaluation.artifact,NULL));
    if (status!=XR_XIR_CTFE_OK) return status;
    bool owned=false;XrXirCellProvenance *cells=NULL;void *closure=NULL;
    status=ctfe_call_prepare_received(request,&owned,&cells,&closure);
    if (status!=XR_XIR_CTFE_OK) {
        xr_xir_compile_cell_provenance_free(cells);xr_compile_resources_free(closure);return status;
    }
    return ctfe_call_run_received(request,output,owned,cells,closure);
}
#include "xxir_ctfe_batch.inc.c"
