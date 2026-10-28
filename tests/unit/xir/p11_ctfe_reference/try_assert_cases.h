/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * try_assert_cases.h - Source-created forced-error panic ownership cases
 */
#include "xir/xxir_format.h"
static XrXirArtifact *slice_try_artifact(XrXirCompileContext *context) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=slice_source(context,"try_assert_owned",&source,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"Try Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirFunction *fold=&module->functions[slice_function(module,"foldedCode")];bool constant=false;
    for(uint32_t i=0;i<fold->instruction_count;++i) {
        const XrXirInstruction *op=&fold->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_CALL_INDIRECT);
        constant|=op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==446;
    }
    CHECK(constant);return slice_lower(context,&source);
}
static void slice_try_error(const XrXirPanicPayload *panic) {
    XrXirValue error={0},text={0},number={0};
    CHECK(panic->detail.code==446 && xr_xir_panic_error_borrow(panic,&error));
    CHECK(error.type>=XR_XIR_CONSTRUCTED_TYPE_BASE && xr_xir_value_arena(&error));
    XrXirFormatCursor root={0},child={0};XrXirFormatAggregate aggregate={0};
    CHECK(xr_xir_value_format_cursor(&error,&root) && xr_xir_value_format_aggregate(&root,&aggregate));
    CHECK(aggregate.name.length==10 && !memcmp(aggregate.name.bytes,"ProbeError",10));
    CHECK(aggregate.member.length==3 && !memcmp(aggregate.member.bytes,"Bad",3) && aggregate.count==2);
    CHECK(xr_xir_value_format_child(&root,0,&child) && xr_xir_value_format_leaf(&child,&text));
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==6 && !memcmp(bytes,"\xc3\xa9\0\xe7\x95\x8c",6));
    CHECK(xr_xir_value_format_child(&root,1,&child) && xr_xir_value_format_leaf(&child,&number));
    CHECK(number.type==XR_XIR_I64 && number.payload==23);
}
static void slice_try_pause(XrXirCtfeCallRequest *request,CtfeCallExecution *execution,unsigned phase) {
    bool owned=false;XrXirCellProvenance *cells=NULL;void *closure=NULL;
    CHECK(ctfe_call_prepare_received(request,&owned,&cells,&closure)==XR_XIR_CTFE_OK && owned);
    *execution=(CtfeCallExecution){.request=request,.artifact=request->evaluation.artifact,.steps=request->evaluation.limits.steps};
    CHECK(ctfe_owned_new(execution->artifact,owned,&execution->owned)==XR_XIR_CTFE_OK);
    execution->owned.cells=cells;execution->owned.closure=closure;execution->owned.loan_bytes=&execution->loan_bytes;
    execution->owned.reachable=closure?(uint8_t *)((uint32_t *)closure+execution->artifact->module.function_count):NULL;
    CHECK(ctfe_call_frame_new(execution,request->evaluation.function,UINT32_MAX,&execution->top)==XR_XIR_CTFE_OK);
    execution->depth=1;execution->live_bytes=execution->top->bytes;
    for(unsigned step=0;step<1024;++step) {
        CtfeCallFrame *frame=execution->top;slice_activate(execution);
        if(frame->state.leaving) {
            if(phase==1 && frame->exit_kind==CTFE_EXIT_PANIC && frame->state.landing) return;
            XrXirValue value={0};bool done=false;
            CHECK(ctfe_cleanup_continue(execution,&value,&done)==XR_XIR_CTFE_OK && !done && !value.type);
        } else {
            XrXirOp opcode=frame->run.function->instructions[frame->state.instruction].op;
            if((!phase && opcode==XR_XIR_PANIC_ERROR) || (phase==2 && opcode==XR_XIR_PANIC_MESSAGE)) return;
            XirScalarCoreAction action={0};CHECK(ctfe_call_step(execution,&action)==XR_XIR_CTFE_OK);
            if(action.kind==XIR_SCALAR_RETURN) {
                XrXirValue value={0};CHECK(frame->parent && ctfe_call_return(execution,&action.value,&value)==XR_XIR_CTFE_OK && !value.type);
            } else CHECK(action.kind==XIR_SCALAR_CONTINUE);
        }
    }
    CHECK(false);
}
static void slice_try_owner(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_try_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"raw");CtfeCallExecution execution={0};slice_try_pause(&request,&execution,0);
    XirScalarCoreAction action={0};CHECK(ctfe_call_step(&execution,&action)==XR_XIR_CTFE_OK);
    CHECK(execution.top->exit_kind==CTFE_EXIT_PANIC);slice_try_error(&execution.top->panic);
    XrXirPanicPayload held={0},copy={0};CHECK(xr_xir_panic_copy(&execution.top->panic,&held)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_panic_copy(&held,&copy)==XR_XIR_VALUE_OK);XrXirValue info={0},message={0};
    CHECK(xr_xir_panic_info_new(execution.owned.domain,&copy,&info)==XR_XIR_VALUE_OK);
    slice_execution_free(&execution);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    slice_try_error(&held);xr_xir_panic_drop(&copy);
    CHECK(xr_xir_panic_info_message(&info,&message)==XR_XIR_VALUE_OK);
    static const char expected[]="try! failed: ProbeError.Bad(\"\xc3\xa9\0\xe7\x95\x8c\", 23)";
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&message,&bytes,&length));
    CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&message);xr_xir_value_drop(&info);xr_xir_panic_drop(&held);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source Error retains enum/UTF8/NUL/arena after packet, activation and compiler owner retire");
}
static void slice_try_vm(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_try_artifact(&context);
    uint32_t entry=slice_function(&artifact->module,"raw");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;config.abi_version=35;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_BAD_ABI && !instance);
    config.abi_version=XR_XIR_CALL_ABI_VERSION;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC);slice_try_error(&result.outcome.panic);
    XrXirCallResult held={0};CHECK(xr_xir_call_result_copy(&result.outcome,&held)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_compile_resources_release(context.resources);
    slice_try_error(&held.panic);xr_xir_call_result_drop(&held);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual VM unhandled try panic owns original Error after Instance and Program retirement");
}
static void slice_try_resources(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_try_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"message");
    for(unsigned point=0;point<3;++point) {
        CtfeCallExecution execution={0};slice_try_pause(&request,&execution,point?2:1);
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=source_fixture_compile_attempts;source_fixture_compile_injected=false;
        source_fixture_compile_fail_at=attempts+(point==2?1:0);
        XrXirCtfeStatus status;
        if(!point) status=ctfe_panic_land(&execution);
        else {XirScalarCoreAction action={0};status=ctfe_call_step(&execution,&action);}
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
        if(!point) slice_try_error(&execution.top->panic);
        slice_execution_free(&execution);
    }
    XrXirValue answer={0};CHECK(xr_xir_compile_ctfe_calls(&request,&answer)==XR_XIR_CTFE_OK && answer.type==XR_XIR_STRING);
    xr_xir_value_drop(&answer);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual panic owner/message OOM prefixes unwind held storage; original resource owner reused");
}
static void slice_try_controls(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_try_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"message");
    for(unsigned mode=0;mode<2;++mode) {
        SliceArgumentCancel cancel={0,1};XrCompileRequestControl control={0};
        XrCompileRequestOptions options=mode?(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER}:
            (XrCompileRequestOptions){.cancel_poll=slice_argument_cancel_poll,.cancel_context=&cancel};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);request.evaluation.control=&control;
        XrXirValue output={0};CHECK(xr_xir_compile_ctfe_calls(&request,&output)==(mode?XR_XIR_CTFE_DEADLINE:XR_XIR_CTFE_CANCELLED));
        CHECK(!output.type && !output.payload);
    }
    request.evaluation.control=NULL;XrXirValue value={0};CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);
    xr_xir_value_drop(&value);request=slice_request(artifact,"raw");
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_TRY_ASSERT_FAILURE && !value.type && !value.payload);
    request=slice_request(artifact,"originalPanic");CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK && value.type==XR_XIR_I64 && value.payload==420);
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Cancelled/deadline stay outside business/panic; uncaught try has exact failure and ordinary panic stays420");
}
static void slice_try_memory(void) {
    uint64_t peak=0;CHECK(slice_memory_attempt("try_assert_owned",0,&peak)==XR_XIR_OK && peak>1);
    CHECK(slice_memory_attempt("try_assert_owned",peak-1,NULL)==XR_XIR_MEMORY_LIMIT);
    CHECK(slice_memory_attempt("try_assert_owned",peak,NULL)==XR_XIR_OK && slice_memory_attempt("try_assert_owned",peak+1,NULL)==XR_XIR_OK);
    fprintf(stderr,"Try Source measured held peak %llu +/-1; no fixed size contract\n",(unsigned long long)peak);
}

static void slice_try_rejections(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic source_diagnostic={0};
    CHECK(slice_source(&context,"try_assert_owned",&source,&source_diagnostic)==XR_XIR_OK);
    const XrXirArtifact *artifact=source.checked;XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK && packet.length>=64);
    uint8_t contract[4];memcpy(contract,packet.bytes+12,4);memset(packet.bytes+12,0,4);packet.bytes[12]=100;
    XrXirArtifact *rejected=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE && !rejected);
    memcpy(packet.bytes+12,contract,4);xr_xir_compile_checked_packet_free(&packet);
    XrXirModule module=artifact->module;uint32_t f=slice_function(&module,"raw");
    const XrXirFunction *function=&module.functions[f];
    XrXirFunction *functions=xr_malloc(module.function_count*sizeof(*functions));
    XrXirInstruction *instructions=xr_malloc(function->instruction_count*sizeof(*instructions));CHECK(functions && instructions);
    memcpy(functions,module.functions,module.function_count*sizeof(*functions));
    memcpy(instructions,function->instructions,function->instruction_count*sizeof(*instructions));
    functions[f].instructions=instructions;module.functions=functions;
    CHECK(xr_xir_compile_structure_verify_v2(&context,&module,artifact->construction,NULL)==XR_XIR_OK);
    uint32_t boolean=UINT32_MAX,panic=UINT32_MAX;
    for(uint32_t i=0;i<function->instruction_count;++i) {
        if(instructions[i].op==XR_XIR_CONST_BOOL) boolean=function->parameter_count+i;
        if(instructions[i].op==XR_XIR_PANIC_ERROR) panic=i;
    }
    CHECK(boolean!=UINT32_MAX && panic!=UINT32_MAX);instructions[panic].args[0]=boolean;
    XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_structure_verify_v2(&context,&module,artifact->construction,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(diagnostic.function==f && diagnostic.instruction==panic);
    xr_free(instructions);xr_free(functions);xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual Source-derived PANIC_ERROR rejects Bool operand; old Checked100 and Call35 cannot enter");
}
