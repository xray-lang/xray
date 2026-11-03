/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_callback.c - Actual Source bindings on an independent Root sibling
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_root_callback_internal.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "execution/xr_xir_host_time.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Root callback %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_runtime_allocations.h"

static const char callback_source[]=
 "import time\nvar state:i64=0\n"
 "fn worker()->i64{Coro.yield();time.sleep(3);return 11}\n"
 "export fn make()->fn(){const captured:i64=31;return fn(){const task=go worker();"
 "const result=await task;assert(result+captured==42);state+=1;time.sleep(7);state+=1}}\n"
 "export fn makePanic()->fn(){return fn(){assert(false)}}\n"
 "export fn makeCancel()->fn(){return fn(){defer{state+=4};time.sleep(500)}}\n"
 "export fn makeFatal()->fn(){return fn(){defer{assert(false)};assert(false)}}\n"
 "export fn parent()->i64{time.sleep(30);return state}\n"
 "export fn guardedParent()->i64{try{time.sleep(30);return state}"
 "catch panic{print(\"unexpected-parent-catch\");return -1}}\n"
 "export fn cleanupParent()->i64{defer{assert(false)};time.sleep(30);return state}\n"
 "fn readLoan(v:Slice<u8>)->i64{return (v[0] as i64)+(v[1] as i64)}\n"
 "export fn loanParent()->i64{var bytes:Array<u8>=[20,22];const loan=bytes[:];return readLoan(loan)}\n";
static unsigned guest_clock_reads,unexpected_output;
static XrXirTimeStatus failed_guest_clock(void *context,XrXirClockKind kind,int64_t *output) {
    (void)context;(void)kind;(void)output;++guest_clock_reads;return XR_XIR_TIME_FAILED;
}
static XrXirOutputStatus counted_output(void *context,const XrXirOutputGroup *group) {
    (void)context;(void)group;++unexpected_output;return XR_XIR_OUTPUT_OK;
}
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t i=0;i<module->function_count;++i)
        if(module->functions[i].name_length==strlen(name) &&
            !memcmp(module->functions[i].name,name,strlen(name)))return i;
    CHECK(false);return UINT32_MAX;
}
static XrXirProgram *compile_program(const XrXirCompileContext *context,
    const char *stdlib,const char *directory,uint32_t ids[8]) {
    char file[2048];CHECK(snprintf(file,sizeof(file),"%s/callback.xr",directory)>0);
    FILE *out=fopen(file,"wb");CHECK(out && fwrite(callback_source,1,sizeof(callback_source)-1,out)==sizeof(callback_source)-1 && !fclose(out));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,file,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const char *const names[]={"make","makePanic","makeCancel","makeFatal","parent","guardedParent","cleanupParent","loanParent"};
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    for(unsigned i=0;i<8;++i)ids[i]=named(module,names[i]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
    return program;
}
static XrXirInstance *new_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,failed_guest_clock,NULL,NULL};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,counted_output,NULL};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    return instance;
}
static XrXirValue make_function(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};unsigned steps=0;
    do{CHECK(++steps<10000);result=xr_xir_instance_poll_bounded(instance,1);}while(result.outcome.status==XR_XIR_CALL_READY);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_function_binding(&function));return function;
}
static XrXirInstanceResult park_parent(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result={0};unsigned steps=0;
    do{CHECK(++steps<10000);result=xr_xir_instance_poll_bounded(instance,1);}while(result.outcome.status==XR_XIR_CALL_READY);
    XrXirWaitRequest wait={0};
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED &&
        xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&wait)==XR_XIR_CALL_READY &&
        wait.kind==XR_XIR_WAIT_TIMER_MS && wait.after_ms==30);
    return result;
}
static XrXirInstanceResult drive(XrXirInstance *instance,bool cancel_callback,
    const XrXirInstanceResult *parent,const XirHostWaitDeadline *original,bool *reselected) {
    XrXirInstanceResult result={0};XrXirHostWait wait={0};uint64_t epoch=0,wake=0;
    unsigned steps=0;bool cancelled=false;
    for(;;) {
        CHECK(++steps<100000);result=xr_xir_instance_poll_bounded(instance,1);
        if(result.outcome.status==XR_XIR_CALL_READY)continue;
        if(result.outcome.status!=XR_XIR_CALL_SUSPENDED)break;
        if(result.outcome.wake!=parent->outcome.wake && xir_instance_callback_active(instance)) {
            XrXirWaitRequest old_wait={0};XirHostWaitDeadline retained={0};
            CHECK(xr_xir_instance_wait_request(instance,parent->epoch,parent->outcome.wake,&old_wait)==XR_XIR_CALL_READY &&
                old_wait.kind==XR_XIR_WAIT_TIMER_MS);
            CHECK(xir_instance_host_wait_deadline(instance,parent->epoch,parent->outcome.wake,
                xr_time_monotonic_ns(),&retained)==XR_XIR_CALL_READY &&
                retained.start_ns==original->start_ns && retained.duration_ns==original->duration_ns);
            *reselected=true;
        }
        if(cancel_callback&&!cancelled&&xir_instance_callback_active(instance)) {
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            cancelled=true;continue;
        }
        XrXirWaitRequest request={0};
        CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
        if(request.kind==XR_XIR_WAIT_CHANNEL)continue;
        CHECK(request.kind==XR_XIR_WAIT_TIMER_MS||request.kind==XR_XIR_WAIT_YIELD);
        if(epoch!=result.epoch||wake!=result.outcome.wake) {
            XirHostWaitDeadline deadline={0};
            CHECK(xir_instance_host_wait_deadline(instance,result.epoch,result.outcome.wake,
                request.kind==XR_XIR_WAIT_TIMER_MS?xr_time_monotonic_ns():0,&deadline)==XR_XIR_CALL_READY);
            wait=(XrXirHostWait){deadline.start_ns,deadline.duration_ns,deadline.active};
            epoch=result.epoch;wake=result.outcome.wake;
        }
        XrXirHostWaitStatus due=xr_xir_host_wait_poll(&wait,UINT64_C(1000000));
        CHECK(due==XR_XIR_HOST_WAIT_PENDING||due==XR_XIR_HOST_WAIT_DUE);
        if(due==XR_XIR_HOST_WAIT_DUE) {
            CHECK(xr_xir_instance_resume(instance,epoch,wake)==XR_XIR_CALL_READY);epoch=wake=0;
        }
    }
    if(cancel_callback)CHECK(cancelled);
    return result;
}
static void callback_cases(XrXirProgram *program,const uint32_t ids[8]) {
    for(unsigned mode=0;mode<5;++mode) {
        size_t baseline=runtime_live,bytes=runtime_bytes;
        XrXirInstance *instance=new_instance(program);
        XrXirValue function=make_function(instance,ids[mode==4?1:mode>2?3:mode]);
        XrXirInstanceResult parent=park_parent(instance,ids[mode==4?6:mode==1?5:4]);
        XrXirCall *original=instance->call;
        uint64_t original_ticket=original->executor_ticket;
        XirHostWaitDeadline before={0};
        CHECK(xir_instance_host_wait_deadline(instance,parent.epoch,parent.outcome.wake,
            xr_time_monotonic_ns(),&before)==XR_XIR_CALL_READY);
        CHECK(xir_instance_callback_begin(instance,&(XrXirValue){XR_XIR_I64,0,42})==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xir_instance_callback_begin(instance,&function)==XR_XIR_CALL_READY);
        CHECK(xir_instance_callback_active(instance) && instance->call==original && instance->epoch==parent.epoch &&
            original->executor_ticket==original_ticket);
        CHECK(xir_instance_callback_begin(instance,&function)==XR_XIR_CALL_BAD_STATE);
        CHECK(xir_instance_callback_retire(instance,instance->callback->call)==XR_XIR_CALL_BAD_STATE);
        /* The independent activation owns the real capture and code lease. */
        xr_xir_value_drop(&function);
        XrXirWaitRequest original_wait={0};
        CHECK(xr_xir_instance_wait_request(instance,parent.epoch,parent.outcome.wake,&original_wait)==XR_XIR_CALL_READY &&
            original_wait.kind==XR_XIR_WAIT_TIMER_MS);
        XirHostWaitDeadline after={0};
        CHECK(xir_instance_host_wait_deadline(instance,parent.epoch,parent.outcome.wake,
            xr_time_monotonic_ns(),&after)==XR_XIR_CALL_READY && before.start_ns==after.start_ns &&
            before.duration_ns==after.duration_ns);
        bool reselected=false;
        XrXirInstanceResult terminal=drive(instance,mode==2,&parent,&before,&reselected);
        if(mode==0||mode==2)CHECK(reselected);
        CHECK(!xir_instance_callback_active(instance)&&instance->call==original&&
            instance->epoch==parent.epoch&&original->executor_ticket==original_ticket);
        XrXirCallStatus expected=mode==0?XR_XIR_CALL_RETURNED:mode==2?XR_XIR_CALL_CANCELLED:
            mode==3||mode==4?XR_XIR_CALL_CLEANUP_FAILED:XR_XIR_CALL_ASSERTION;
        if(terminal.outcome.status!=expected)fprintf(stderr,"callback mode%u actual%u expected%u\n",mode,terminal.outcome.status,expected);
        CHECK(terminal.outcome.status==expected && !unexpected_output && !guest_clock_reads);
        if(mode==0) {
            XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED &&
                result.type==XR_XIR_I64 && result.payload==2);xr_xir_value_drop(&result);
        } else CHECK(xr_xir_call_result_valid(&terminal.outcome));
        if(mode==3||mode==4)CHECK(terminal.outcome.prior_status==XR_XIR_CALL_ASSERTION &&
            xr_xir_call_panic_payload(terminal.outcome.prior_status,&terminal.outcome.prior_panic));
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        CHECK(closed==XR_XIR_CALL_READY||closed==expected);
        CHECK(runtime_live==baseline&&runtime_bytes==bytes);
    }
    {
        XrXirInstance *instance=new_instance(program);
        XrXirValue function=make_function(instance,ids[0]);
        CHECK(xr_xir_instance_start(instance,ids[7],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result={0};unsigned blocked=0,steps=0;
        do {
            CHECK(++steps<10000);
            bool eligible=true;
            CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY);
            if (!eligible && xr_xir_call_state(instance->call)==XR_XIR_CALL_READY) {
                CHECK(xir_instance_callback_begin(instance,&function)==XR_XIR_CALL_BAD_STATE);
                ++blocked;
            }
            result=xr_xir_instance_poll_bounded(instance,1);
        } while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(blocked && result.outcome.status==XR_XIR_CALL_RETURNED && !xir_instance_callback_active(instance));
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
            value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);xr_xir_value_drop(&function);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    XrXirInstance *first=new_instance(program),*other=new_instance(program);
    XrXirValue foreign=make_function(other,ids[0]);
    (void)park_parent(first,ids[4]);
    CHECK(xir_instance_callback_begin(first,&foreign)==XR_XIR_CALL_BAD_ARGUMENT &&
        !xir_instance_callback_active(first));
    xr_xir_value_drop(&foreign);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(other)==XR_XIR_CALL_READY);
}
int main(int argc,char **argv) {
    CHECK(argc==3);
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    limits.profile_allocations=true;
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};uint32_t ids[8];
    XrXirProgram *program=compile_program(&context,argv[1],argv[2],ids);
    callback_cases(program,ids);xr_xir_compile_program_drop(program);
    CHECK(!runtime_live&&!runtime_bytes);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK &&
        after.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(resources);
    puts("Root callback: Source-owned captures, sibling await, original host deadlines, isolated panic/cancel and fatal cleanup");
    return 0;
}
