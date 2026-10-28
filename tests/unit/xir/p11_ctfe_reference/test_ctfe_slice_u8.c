/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ctfe_slice_u8.c - Real compiler-evaluated scoped byte constants
 *
 * KEY CONCEPT:
 *   Source must produce a constant before packet and VM execution. Frontend
 *   storage dies before the independent receiving chain observes its answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_compile_owner.h"
#include "xir/xxir_ctfe.h"
#include "base/xsha256.h"
#include "xir/xxir_vm.h"
/* White-box fault companions compile the same evaluator, never a copied interpreter. */
#include "xir/xxir_ctfe.c"
#include "xir_ctfe_slice_u8_cases.h"

static uint32_t slice_function(const XrXirModule *module, const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for (uint32_t i=0;i<module->function_count;++i) {
        const XrXirFunction *body=&module->functions[i];
        if (body->name_length==length && !memcmp(body->name,name,length)) { CHECK(found==UINT32_MAX);found=i; }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirStatus slice_source(const XrXirCompileContext *context,const char *name,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic) {
    char path[1024],text[4096],identity[96];
    int n=snprintf(path,sizeof(path),"%s/%s.xr",XR_CTFE_SLICE_FIXTURES,name);
    CHECK(n>0 && (size_t)n<sizeof(path));FILE *file=fopen(path,"rb");CHECK(file);
    size_t bytes=fread(text,1,sizeof(text)-1,file);CHECK(bytes && !ferror(file) && feof(file) && !fclose(file));text[bytes]=0;
    n=snprintf(identity,sizeof(identity),"ctfe-slice-%s",name);CHECK(n>0 && (size_t)n<sizeof(identity));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,identity,NULL};
    const char *stdlib=!strcmp(name,"from_utf8")?XR_CTFE_UTF8_STDLIB:NULL;
    XrXirSourceRequest request={session,NULL,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,bytes};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,diagnostic,NULL);
    xr_compile_session_free(session);memset(text,0xa5,sizeof(text));memset(identity,0xa5,sizeof(identity));return status;
}
static XrXirArtifact *slice_lower(const XrXirCompileContext *context,XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    uint8_t identity[32];CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(!memcmp(identity,lowered->checked_identity,32));return lowered;
}
static XrXirCtfeCallRequest slice_request(const XrXirArtifact *artifact,const char *function) {
    XrXirCtfeCallRequest request={.evaluation={.artifact=artifact,
        .function=slice_function(&artifact->module,function),.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
        .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,artifact->checked_identity,32);return request;
}
static void slice_context_new(XrXirCompileContext *context) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    CHECK(xr_compile_resources_new(&limits,&context->resources)==XR_COMPILE_RESOURCE_OK);context->limits=xr_xir_compile_default_limits();
}
static XrXirArtifact *slice_real_artifact(XrXirCompileContext *context) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(slice_source(context,"slice_u8_local",&source,&diagnostic)==XR_XIR_OK);
    return slice_lower(context,&source);
}
static void slice_activate(CtfeCallExecution *execution) {
    CtfeCallFrame *frame=execution->top;
    execution->owned.frame_spare=execution->request->evaluation.limits.frame_bytes-execution->live_bytes-execution->loan_bytes;
    execution->owned.loan_base=execution->loan_bytes;
    execution->owned.authority=(XrXirCellAuthority){execution,frame,frame->epoch};execution->owned.activation=frame;
}
static void slice_pause_new(const XrXirCtfeCallRequest *request,CtfeCallExecution *execution) {
    CHECK(xr_xir_compile_artifact_verify(request->evaluation.artifact,NULL)==XR_XIR_OK);
    bool owned=false;XrXirCellProvenance *cells=NULL;void *closure=NULL;
    CHECK(ctfe_call_prepare_received(request,&owned,&cells,&closure)==XR_XIR_CTFE_OK && owned);
    *execution=(CtfeCallExecution){.request=request,.artifact=request->evaluation.artifact,.steps=request->evaluation.limits.steps};
    execution->owned.control=request->evaluation.control;
    CHECK(ctfe_owned_new(execution->artifact,owned,&execution->owned)==XR_XIR_CTFE_OK);
    execution->owned.cells=cells;execution->owned.closure=closure;execution->owned.loan_bytes=&execution->loan_bytes;
    execution->owned.reachable=closure?(uint8_t *)((uint32_t *)closure+execution->artifact->module.function_count):NULL;
    CHECK(ctfe_call_frame_new(execution,request->evaluation.function,UINT32_MAX,&execution->top)==XR_XIR_CTFE_OK);
    execution->depth=1;execution->live_bytes=execution->top->bytes;
    for (unsigned step=0;step<1024;++step) {
        slice_activate(execution);
        if (execution->top->run.function->instructions[execution->top->state.instruction].op==XR_XIR_VIEW_NEW) return;
        XirScalarCoreAction action={0};CHECK(ctfe_call_step(execution,&action)==XR_XIR_CTFE_OK && action.kind==XIR_SCALAR_CONTINUE);
    }
    CHECK(false);
}
static void slice_execution_free(CtfeCallExecution *execution) {
    ctfe_call_frames_free(execution);ctfe_owned_free(&execution->owned);CHECK(!execution->loan_bytes);
}
static void slice_resources(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_real_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");
    XrCompileResourceStats initial={0},before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    for (size_t site=0;site<2;++site) {
        CtfeCallExecution execution={0};slice_pause_new(&request,&execution);
        CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+site;
        CHECK(ctfe_view_step(&execution)==XR_XIR_CTFE_OUT_OF_MEMORY);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(source_fixture_compile_injected && !execution.top->views && !execution.loan_bytes);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        CHECK(ctfe_view_step(&execution)==XR_XIR_CTFE_OK && execution.top->views);
        slice_execution_free(&execution);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==initial.held_bytes);
    }
    CtfeCallExecution execution={0};slice_pause_new(&request,&execution);
    uint64_t spare=execution.owned.frame_spare;
    execution.owned.frame_spare=sizeof(CtfeViewBinding)-1;
    CHECK(ctfe_view_step(&execution)==XR_XIR_CTFE_FRAME_LIMIT && !execution.top->views && !execution.loan_bytes);
    execution.owned.frame_spare=spare;CHECK(ctfe_view_step(&execution)==XR_XIR_CTFE_OK);
    slice_execution_free(&execution);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Source-derived View OOM/frame rollback and reuse passed");
}
static bool slice_cancel(const void *opaque) {
    unsigned *count=(unsigned *)opaque;return ++*count>=2;
}
static void slice_authority(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_real_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");CtfeCallExecution execution={0};slice_pause_new(&request,&execution);
    CHECK(ctfe_view_step(&execution)==XR_XIR_CTFE_OK);slice_activate(&execution);
    CtfeCallFrame *frame=execution.top;CtfeViewBinding *binding=frame->views;CHECK(binding);
    XrXirValue value={binding->type,0,0};memcpy(&value.payload,&binding->pin.body,sizeof(value.payload));
    XrXirValueAdmission admission=ctfe_value_admission(&execution.owned);
    CtfeViewAccessContext access_context={&execution,frame,&admission,XR_XIR_CTFE_OK};
    XrXirViewAccess access={&admission,ctfe_view_authorize,&access_context};int64_t length=-1;
    CHECK(xr_xir_view_len(&value,&access,&length)==XR_XIR_VALUE_OK && length==4);
    --execution.owned.authority.epoch;length=-1;
    CHECK(xr_xir_view_len(&value,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);slice_activate(&execution);
    CtfeCallFrame *other=NULL;CHECK(ctfe_call_frame_new(&execution,frame->function,UINT32_MAX,&other)==XR_XIR_CTFE_OK);
    execution.top=other;execution.live_bytes+=other->bytes;slice_activate(&execution);length=-1;
    CHECK(xr_xir_view_len(&value,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);
    execution.top=frame;execution.live_bytes-=other->bytes;ctfe_call_frame_free(other);slice_activate(&execution);
    XrXirValue late={0};CHECK(xr_xir_value_copy(&value,&late)==XR_XIR_VALUE_OK);
    ctfe_view_frame_close(frame);length=-1;
    CHECK(xr_xir_view_len(&value,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);
    slice_execution_free(&execution);length=-1;
    CHECK(xr_xir_view_len(&late,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);
    xr_xir_value_drop(&late);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual frame/epoch/retired origin and late drop passed");
}
static void slice_controls(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_real_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");XrXirValue output={0};
    request.evaluation.limits.steps=0;CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type);
    request.evaluation.limits.steps=XR_XIR_CTFE_MAX_STEPS;
    request.call_depth=0;CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_DEPTH_LIMIT && !output.type);request.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH;
    XrCompileRequestControl control={0};XrCompileRequestOptions options={.deadline_mode=XR_COMPILE_DEADLINE_AFTER,.timeout_ns=0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);request.evaluation.control=&control;
    CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_DEADLINE && !output.type);
    request.evaluation.control=NULL;CtfeCallExecution execution={0};slice_pause_new(&request,&execution);
    for (unsigned step=0;step<1024 && (!execution.top->views || !execution.top->views->next);++step) {
        slice_activate(&execution);XirScalarCoreAction action={0};CHECK(ctfe_call_step(&execution,&action)==XR_XIR_CTFE_OK && action.kind==XIR_SCALAR_CONTINUE);
    }
    CHECK(execution.top->views && execution.top->views->next);slice_activate(&execution);
    const XrXirInstruction *cleanup=NULL;
    for (uint32_t i=0;i<execution.top->run.function->instruction_count;++i) {
        const XrXirInstruction *op=&execution.top->run.function->instructions[i];
        if (op->op==XR_XIR_VIEW_CLEANUP && op->args[1]>=2) {cleanup=op;break;}
    }
    CHECK(cleanup);
    uint32_t first=execution.top->run.function->operands[cleanup->args[0]];
    CHECK(xr_xir_scalar_load(execution.top->run.frame,execution.top->run.layout->offsets[first]));
    size_t frame_bytes=execution.top->run.layout->frame_bytes;void *saved=xr_malloc(frame_bytes);CHECK(saved);
    memcpy(saved,execution.top->run.frame,frame_bytes);uint64_t loan_bytes=execution.loan_bytes;
    unsigned polls=0;control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){.cancel_poll=slice_cancel,.cancel_context=&polls};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);execution.owned.control=&control;
    CHECK(ctfe_view_cleanup(&execution,cleanup)==XR_XIR_CTFE_CANCELLED);
    CHECK(!memcmp(saved,execution.top->run.frame,frame_bytes) && execution.loan_bytes==loan_bytes);
    xr_free(saved);CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_CANCELLED);
    slice_execution_free(&execution);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual cancellation preserves all owner words before atomic cleanup");
}

static XrXirStatus slice_inline_source(const XrXirCompileContext *context,const char *source,XrXirSourceResult *result) {
    char text[4096];size_t length=strlen(source);CHECK(length<sizeof(text));memcpy(text,source,length+1);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"ctfe-slice-boundary",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Boundary Source status%u %s\n",(unsigned)status,diagnostic.message);
    xr_compile_session_free(session);memset(text,0xa5,sizeof(text));return status;
}
static void slice_boundaries(void) {
    static const char *const programs[]={
        "fn read(v:Slice<u8>)->i64{return len(v)}\nfn answer()->i64{var xs:Array<u8>=[7,8];const v:Slice<u8>=xs[:];return read(v)}\nexport fn run()->i64{return answer()}\n",
        "fn echo(v:Slice<u8>)->Slice<u8>{return v}\nfn answer()->i64{var xs:Array<u8>=[7,8];const v:Slice<u8>=xs[:];const w:Slice<u8>=echo(v);return len(w)}\nexport fn run()->i64{return answer()}\n"
    };
    for (size_t i=0;i<sizeof(programs)/sizeof(programs[0]);++i) {
        XrXirCompileContext context={0};slice_context_new(&context);XrXirSourceResult source={0};
        CHECK(slice_inline_source(&context,programs[i],&source)==XR_XIR_OK);XrXirArtifact *artifact=slice_lower(&context,&source);
        uint32_t function=slice_function(&artifact->module,"run");
        XrXirCtfeCallRequest request=slice_request(artifact,"run");XrXirValue denied={0};
        XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_calls(&request,&denied);
        if (!i) CHECK(evaluated==XR_XIR_CTFE_OK && denied.type==XR_XIR_I64 && denied.payload==2);
        else {
            CHECK(evaluated==XR_XIR_CTFE_NOT_SUPPORTED && !denied.type && !denied.payload);
            uint32_t echo=slice_function(&artifact->module,"echo");
            CHECK(xr_xir_compile_ctfe_type_admit(&context,artifact->module.types,
                artifact->module.functions[echo].result)==XR_XIR_CTFE_NOT_SUPPORTED);
        }
        xr_xir_value_drop(&denied);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue ordinary={0};CHECK(xr_xir_instance_take_result(instance,&ordinary)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(ordinary.type==XR_XIR_I64 && ordinary.payload==2);xr_xir_value_drop(&ordinary);
        xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    puts("Slice argument CTFE/VM2; borrowed return VM2, CTFE NOT_SUPPORTED");
}
static XrXirArtifact *slice_argument_artifact(XrXirCompileContext *context) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(slice_source(context,"slice_u8_arguments",&source,&diagnostic)==XR_XIR_OK);
    return slice_lower(context,&source);
}
static const XrXirInstruction *slice_pause_arguments(XrXirCtfeCallRequest *request,CtfeCallExecution *execution) {
    slice_pause_new(request,execution);uint32_t pair=slice_function(&execution->artifact->module,"pair");
    for (unsigned i=0;i<2048;++i) {
        slice_activate(execution);CtfeCallFrame *frame=execution->top;
        const XrXirInstruction *op=&frame->run.function->instructions[frame->state.instruction];
        if (op->op==XR_XIR_CALL && op->immediate==pair) return op;
        XirScalarCoreAction action={0};CHECK(ctfe_call_step(execution,&action)==XR_XIR_CTFE_OK);
        if (action.kind==XIR_SCALAR_RETURN) {
            CHECK(execution->top->parent && !execution->top->state.frontier);XrXirValue output={0};
            CHECK(ctfe_call_return(execution,&action.value,&output)==XR_XIR_CTFE_OK && !output.type);
        } else CHECK(action.kind==XIR_SCALAR_CONTINUE);
    }
    CHECK(false);return NULL;
}
static void slice_argument_read(CtfeCallExecution *execution,const XrXirValue *value) {
    XrXirValueAdmission admission=ctfe_value_admission(&execution->owned);
    CtfeViewAccessContext context={execution,execution->top,&admission,XR_XIR_CTFE_OK};
    XrXirViewAccess access={&admission,ctfe_view_authorize,&context};int64_t length=-1;
    CHECK(xr_xir_view_len(value,&access,&length)==XR_XIR_VALUE_OK && length==4);
}
static XrXirValue slice_actual_argument(const CtfeCallFrame *frame,const XrXirInstruction *op) {
    return ctfe_nominal_operand(&frame->run,frame->run.function->operands[op->args[0]]);
}
static void slice_argument_resources(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_argument_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");
    for (size_t index=0;index<2;++index) {
        CtfeCallExecution execution={0};const XrXirInstruction *op=slice_pause_arguments(&request,&execution);
        CtfeCallFrame *parent=execution.top;XrXirValue original=slice_actual_argument(parent,op);
        uint32_t cursor=parent->state.instruction;uint64_t loans=execution.loan_bytes;
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
        source_fixture_compile_fail_at=source_fixture_compile_attempts+(index?3:0);source_fixture_compile_injected=false;
        CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(execution.top==parent && parent->state.instruction==cursor && execution.loan_bytes==loans);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        slice_argument_read(&execution,&original);
        CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_OK && execution.top!=parent);
        slice_execution_free(&execution);
    }
    CtfeCallExecution execution={0};const XrXirInstruction *op=slice_pause_arguments(&request,&execution);
    CtfeCallFrame *parent=execution.top,*probe=NULL;XrXirValue original=slice_actual_argument(parent,op);
    CHECK(ctfe_call_frame_new(&execution,(uint32_t)op->immediate,UINT32_MAX,&probe)==XR_XIR_CTFE_OK);
    uint64_t child_bytes=probe->bytes;ctfe_call_frame_free(probe);uint64_t maximum=request.evaluation.limits.frame_bytes;
    request.evaluation.limits.frame_bytes=execution.live_bytes+execution.loan_bytes+child_bytes+sizeof(CtfeViewBinding);
    slice_activate(&execution);uint64_t loans=execution.loan_bytes;uint32_t cursor=parent->state.instruction;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_FRAME_LIMIT);
    CHECK(execution.top==parent && parent->state.instruction==cursor && execution.loan_bytes==loans);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    request.evaluation.limits.frame_bytes=maximum;slice_activate(&execution);slice_argument_read(&execution,&original);
    CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_OK);slice_execution_free(&execution);
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual Slice argument pack OOM/frame prefix rollback and reuse passed");
}
static void slice_argument_authority(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_argument_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");CtfeCallExecution execution={0};
    const XrXirInstruction *op=slice_pause_arguments(&request,&execution);CtfeCallFrame *parent=execution.top;
    XrXirValue original=slice_actual_argument(parent,op);uint64_t loans=execution.loan_bytes;
    --execution.owned.authority.epoch;
    CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_BAD_ARTIFACT && execution.top==parent && execution.loan_bytes==loans);
    slice_activate(&execution);slice_argument_read(&execution,&original);
    CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_OK);CtfeCallFrame *sibling=execution.top;
    slice_activate(&execution);XrXirValue incoming=ctfe_nominal_operand(&sibling->run,0);slice_argument_read(&execution,&incoming);
    CHECK(incoming.payload!=original.payload && incoming.type==original.type);
    XrXirValue late={0};CHECK(xr_xir_value_copy(&incoming,&late)==XR_XIR_VALUE_OK);
    execution.top=parent;slice_activate(&execution);CtfeCallFrame *other=NULL;
    CHECK(ctfe_call_frame_new(&execution,sibling->function,UINT32_MAX,&other)==XR_XIR_CTFE_OK);
    CHECK(xr_xir_view_scope_active(&sibling->views->pin));
    CHECK(ctfe_view_argument(&execution,other,0,&incoming)==XR_XIR_CTFE_BAD_ARTIFACT && !other->views);
    ctfe_call_frame_free(other);slice_argument_read(&execution,&original);
    execution.top=sibling;slice_activate(&execution);
    XrXirValueAdmission admission=ctfe_value_admission(&execution.owned);
    CtfeViewAccessContext denied={&execution,sibling,&admission,XR_XIR_CTFE_OK};
    XrXirViewAccess access={&admission,ctfe_view_authorize,&denied};slice_execution_free(&execution);
    CHECK(xr_xir_domain_current(admission.domain) && xr_xir_value_argument(&late,admission.arena,(XrXirType)late.type));
    int64_t length=-1;CHECK(xr_xir_view_len(&late,&access,&length)==XR_XIR_VALUE_BAD_ARGUMENT && length==-1);
    xr_xir_value_drop(&late);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual caller epoch and live foreign sibling Slice grants denied");
}
typedef struct SliceArgumentCancel { unsigned polls,stop; } SliceArgumentCancel;
static bool slice_argument_cancel_poll(const void *opaque) {
    SliceArgumentCancel *cancel=(SliceArgumentCancel *)opaque;return ++cancel->polls>=cancel->stop;
}
static void slice_argument_cancel(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_argument_artifact(&context);
    XrXirCtfeCallRequest request=slice_request(artifact,"answer");CtfeCallExecution execution={0};
    const XrXirInstruction *op=slice_pause_arguments(&request,&execution);CtfeCallFrame *parent=execution.top;
    uint64_t loans=execution.loan_bytes;uint32_t cursor=parent->state.instruction;
    size_t attempts=source_fixture_compile_attempts;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    SliceArgumentCancel cancel={0,4};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=slice_argument_cancel_poll,.cancel_context=&cancel};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    execution.owned.control=&control;request.evaluation.control=&control;
    CHECK(ctfe_call_enter(&execution,op,false)==XR_XIR_CTFE_CANCELLED && cancel.polls>=cancel.stop);
    CHECK(source_fixture_compile_attempts>=attempts+3 && execution.top==parent && execution.loan_bytes==loans);
    CHECK(parent->state.instruction==cursor && xr_xir_view_scope_active(&parent->views->pin));
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_CANCELLED);
    slice_execution_free(&execution);xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Cancelled Slice argument prefix leaves caller grant intact until terminal cleanup");
}

static XrXirStatus slice_memory_attempt(const char *name,uint64_t limit,uint64_t *peak) {
    XrCompileResourceLimits limits={limit?XR_COMPILE_MEMORY_FINITE:XR_COMPILE_MEMORY_ORDINARY,limit,false};
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0},spent={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=slice_source(&context,name,&source,&diagnostic);
    CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);if(peak)*peak=spent.peak_bytes;
    if (status!=XR_XIR_OK) CHECK(status==XR_XIR_MEMORY_LIMIT && !source.checked && !source.snapshot);
    else CHECK(source.checked && source.snapshot);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK && spent.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return status;
}
static void slice_memory(void) {
    uint64_t peak=0;CHECK(slice_memory_attempt("slice_u8_local",0,&peak)==XR_XIR_OK && peak);
    CHECK(slice_memory_attempt("slice_u8_local",peak,NULL)==XR_XIR_OK);
    CHECK(slice_memory_attempt("slice_u8_local",peak-1,NULL)==XR_XIR_MEMORY_LIMIT);
    fprintf(stderr,"Source Slice current held peak %llu; exact/minus1 typed memory boundary passed\n",(unsigned long long)peak);
}
static void slice_argument_memory(void) {
    uint64_t peak=0;
    CHECK(slice_memory_attempt("slice_u8_arguments",0,&peak)==XR_XIR_OK && peak);
    CHECK(slice_memory_attempt("slice_u8_arguments",peak,NULL)==XR_XIR_OK);
    CHECK(slice_memory_attempt("slice_u8_arguments",peak-1,NULL)==XR_XIR_MEMORY_LIMIT);
    fprintf(stderr,"Argument Source dynamic held peak %llu; exact/minus1 typed memory cleanup passed\n",(unsigned long long)peak);
}
static void slice_runtime_work(void) {
    XrXirCompileContext context={0};slice_context_new(&context);XrXirArtifact *artifact=slice_real_artifact(&context);
    const XrXirTypes *types=artifact->module.types;XrXirType array=XR_XIR_UNIT,cell=XR_XIR_UNIT,view=XR_XIR_UNIT;
    for (uint32_t i=0;i<types->count;++i) if (types->nodes[i].kind==XR_XIR_TYPE_ARRAY && types->nodes[i].element==XR_XIR_U8) array=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
    for (uint32_t i=0;i<types->count;++i) {
        if (types->nodes[i].kind==XR_XIR_TYPE_CELL && types->nodes[i].element==array) cell=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
        if (ctfe_slice_u8_type(types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i))) view=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
    }
    CHECK(array && cell && view);XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,types,&arena)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetControls controls={UINT64_MAX,UINT64_MAX,UINT64_C(1048576),UINT64_MAX,UINT64_MAX};XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new_budgeted(UINT64_MAX,&controls,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.work=UINT64_C(1048576),.scratch_bytes=65536};
    XrXirValue fill={XR_XIR_U8,0,7},values={0},root={0},output={0};
    CHECK(xr_xir_array_repeat(array,1,&fill,&admission,&values)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,cell,&values,&admission,&root)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec spec={{&domain,&admission,1},0,1,XR_XIR_TYPE_SLICE};
    admission.work=0;CHECK(xr_xir_view_new(view,&root,NULL,&spec,&admission,&output)==XR_XIR_VALUE_LIMIT && !output.type);
    admission.work=UINT64_C(1048576);CHECK(xr_xir_view_new(view,&root,NULL,&spec,&admission,&output)==XR_XIR_VALUE_OK);
    XrXirViewOriginPin pin={0};xr_xir_view_scope_pin_created(&output,&spec.authority,&pin);
    xr_xir_view_scope_close(&pin);xr_xir_value_drop(&output);
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(domain);CHECK(budget.bound && budget.work<=budget.work_limit);
    CHECK(xr_xir_domain_work(domain,budget.work_limit-budget.work));admission.work=UINT64_C(1048576);
    CHECK(xr_xir_view_new(view,&root,NULL,&spec,&admission,&output)==XR_XIR_VALUE_LIMIT && !output.type);
    xr_xir_value_drop(&root);xr_xir_value_drop(&values);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    xr_xir_compile_artifact_free(artifact);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Real runtime View admission/domain work exhaustion remains denied");
}

static void slice_constant(const SliceConstantCase *selected) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=slice_source(&context,selected->name,&source,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source %s status=%u line=%d reason=%s\n",selected->name,(unsigned)status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirFunction *body=&module->functions[slice_function(module,"run")];bool constant=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE);
        if (op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==selected->expected) constant=true;
    }
    CHECK(constant);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrXirRunContext run={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,0,0,0,0};XrXirValue value={0};
    CHECK(xr_xir_compile_vm_run(lowered,slice_function(xr_xir_compile_artifact_module(lowered),"run"),&run,NULL,0,&value)==XR_XIR_RUN_OK);
    CHECK(!run.live_bytes && run.allocations==run.frees);xr_xir_compile_artifact_free(lowered);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==selected->expected);xr_xir_value_drop(&value);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("%s: constant=%lld producer-death/packet/VM passed\n",selected->name,(long long)selected->expected);
}
#include "ctfe_from_utf8_resources.h"
#include "try_assert_cases.h"

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"try-rejections")){slice_try_rejections();return 0;}
    if(argc==2 && !strcmp(argv[1],"try-owner")){slice_try_owner();return 0;}
    if(argc==2 && !strcmp(argv[1],"try-vm")){slice_try_vm();return 0;}
    if(argc==2 && !strcmp(argv[1],"try-resources")){slice_try_resources();return 0;}
    if(argc==2 && !strcmp(argv[1],"try-controls")){slice_try_controls();return 0;}
    if(argc==2 && !strcmp(argv[1],"try-memory")){slice_try_memory();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-packet")) {slice_native_packet();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-oom")) {slice_native_oom();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-control")) {slice_native_control();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-lifetime")) {slice_native_lifetime();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-types")) {slice_native_types();return 0;}
    if (argc==2 && !strcmp(argv[1],"native-memory")) {slice_native_memory();return 0;}

    if (argc!=2) return 2;
    if (!strcmp(argv[1],"argument-resources")) {slice_argument_resources();return 0;}
    if (!strcmp(argv[1],"argument-authority")) {slice_argument_authority();return 0;}
    if (!strcmp(argv[1],"argument-cancel")) {slice_argument_cancel();return 0;}
    if (!strcmp(argv[1],"argument-memory")) {slice_argument_memory();return 0;}
    if (!strcmp(argv[1],"boundaries")) {slice_boundaries();return 0;}
    if (!strcmp(argv[1],"memory")) {slice_memory();return 0;}
    if (!strcmp(argv[1],"runtime-work")) {slice_runtime_work();return 0;}
    if (!strcmp(argv[1],"resources")) {slice_resources();return 0;}
    if (!strcmp(argv[1],"authority")) {slice_authority();return 0;}
    if (!strcmp(argv[1],"controls")) {slice_controls();return 0;}
    for (size_t i=0;i<sizeof(slice_constant_cases)/sizeof(slice_constant_cases[0]);++i)
        if (!strcmp(argv[1],slice_constant_cases[i].name)) {slice_constant(&slice_constant_cases[i]);return 0;}
    return 2;
}
