/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_scope_source.c - Owned lexical groups across real Source exits
 *
 * KEY CONCEPT:
 *   Cold metadata and execution outlive Source; a type never grants a target.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"task scope %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_task_go_transfer_controls.h"
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirCallStatus execute_entry(XrXirInstance *instance,uint32_t entry,int64_t expected) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    }
    if(status!=XR_XIR_CALL_RETURNED&&status!=XR_XIR_CALL_CANCELLED)
        fprintf(stderr,"entry %u actual status %u\n",entry,status);
    if(status==XR_XIR_CALL_RETURNED) {
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        if(result.type!=XR_XIR_I64||result.payload!=expected)
            fprintf(stderr,"entry %u result type %u payload %lld expected %lld\n",entry,result.type,
                (long long)result.payload,(long long)expected);
        CHECK(result.type==XR_XIR_I64 && result.payload==expected);xr_xir_value_drop(&result);
    }
    return status;
}
static XrXirInstance *new_instance(XrXirProgram *program,uint32_t initializer) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(execute_entry(instance,initializer,0)==XR_XIR_CALL_RETURNED);
    return instance;
}
static void execute(XrXirProgram *program,const uint32_t ids[9]) {
    XrXirInstance *instance=new_instance(program,ids[0]);
    CHECK(execute_entry(instance,ids[4],42)==XR_XIR_CALL_RETURNED);
    CHECK(execute_entry(instance,ids[5],42)==XR_XIR_CALL_RETURNED);
    CHECK(execute_entry(instance,ids[6],1)==XR_XIR_CALL_RETURNED);
    CHECK(execute_entry(instance,ids[7],42)==XR_XIR_CALL_RETURNED);
    CHECK(execute_entry(instance,ids[8],42)==XR_XIR_CALL_RETURNED);
    CHECK(execute_entry(instance,ids[1],42)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(initializer_slot_controls==4 && task_go_transfers>=3);
    for(unsigned kind=0;kind<2;++kind) {
        instance=new_instance(program,ids[0]);
        unsigned transfers=task_go_transfers,refusals=task_go_refusals,oom=task_go_oom;
        unsigned cancellations=task_go_cancellations;
        task_go_cancel_test=true;
        CHECK(execute_entry(instance,ids[2+kind],42)==XR_XIR_CALL_CANCELLED);
        task_go_cancel_test=false;
        CHECK(task_go_cancel_at==3+kind && task_go_allocation==3+kind);
        CHECK(task_go_transfers==transfers && task_go_cancellations==cancellations+1);
        CHECK(task_go_refusals==refusals+2 && task_go_oom==oom+3);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        instance=new_instance(program,ids[0]);
        CHECK(execute_entry(instance,ids[2+kind],42)==XR_XIR_CALL_RETURNED);
        CHECK(task_go_transfers==transfers+1 && task_go_cancellations==cancellations+1);
        CHECK(task_go_refusals==refusals+4 && task_go_oom==oom+6);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    CHECK(task_go_refusals==(task_go_transfers+task_go_cancellations)*2);
    CHECK(task_go_oom==(task_go_transfers+task_go_cancellations)*3);
    printf("GO ownership: transfers=%u refusals=%u OOM=%u cancellations=%u; commit allocations=3/4\n",
        task_go_transfers,task_go_refusals,task_go_oom,task_go_cancellations);
}
#if defined(XR_TASK_SCOPE_SOURCE_NATIVE)
XR_DATA const XrXirProgramSpec task_scope_source_program;
XR_DATA const uint32_t task_scope_source_ids[9];
#else
static XrXirStatus check(const XrXirCompileContext *context,const char *text,XrXirSourceResult *result,
    XrCompileRequestControl *control) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TASK_SCOPE_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_TASK_SCOPE_SOURCE_FIXTURES "/scope.xr",&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=text?xr_xir_compile_source_check_text(&request,&(XrXirSourceText){"scope.xr",text,strlen(text)},result,&diagnostic,NULL):
        xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static bool cancelled(const void *pointer) {++*(unsigned *)pointer;return true;}
#include "xir_task_scope_boundary_cases.h"
static void rejections(const XrXirCompileContext *context) {
    const struct {const char *text;XrXirStatus status;} cases[]={
        {"scope {}",XR_XIR_BAD_STRUCTURE},
        {"scope {fn worker(){}}",XR_XIR_BAD_STRUCTURE},
        {"fn bad(f:fn()){scope {go f()}}",XR_XIR_BAD_TYPE},
        {"fn worker(values:ref Array<i64>){}\nfn run(){var values=[42];const task=go worker(ref values)}",XR_XIR_BAD_TYPE},
        {"fn worker(values:Array<i64>){}\nfn run(){var values=[42];const task=go worker(move values)}",XR_XIR_BAD_TYPE},
        {"fn bad(){var x=42;scope {go fn(){print(x)}()}}",XR_XIR_BAD_TYPE},
        {"enum ErrorCode{Bad}\nfn bad(){scope(isolated) {go fn() throws(ErrorCode){throw ErrorCode.Bad}()}}",XR_XIR_UNSUPPORTED},
        {"enum ErrorCode{Bad}\nfn bad() throws(ErrorCode){scope {go fn() throws(ErrorCode){throw ErrorCode.Bad}()}}",XR_XIR_BAD_TYPE},
        {"enum ErrorCode{Bad}\nfn bad(){try scope {go fn() throws(ErrorCode){throw ErrorCode.Bad}()}}",XR_XIR_BAD_TYPE},
        {"enum WorkError{Failed}\nfn failed() throws(WorkError){throw WorkError.Failed}\nfn worker(){}\n"
            "fn run() throws(WorkError){scope{go worker();try failed()}}",XR_XIR_BAD_TYPE},
        {"enum WorkError{Failed}\nfn failed() throws(WorkError){throw WorkError.Failed}\nfn worker(){}\n"
            "fn run(){try scope{go worker();try failed()}}",XR_XIR_BAD_TYPE},
        {"fn worker(){}\nfn run()->i64{var values=[42];const view:Slice<i64> =values[:];"
            "scope{go worker();values[0]=1};return view[0]}",XR_XIR_BAD_VALUE},
        {"fn worker(){}\nfn run(){var values=[42];var view:MutSlice<i64> =values[:];"
            "scope{go worker();print(values[0])};view[0]=1}",XR_XIR_BAD_VALUE},
        {"fn run(){var values=[42];const view:Slice<i64> =values[:];scope{go fn(){print(view[0])}()}}",XR_XIR_BAD_TYPE},
        {"fn worker(){}\nfn run(){const callback:nosuspend fn()=fn(){scope{go worker()}};callback()}",XR_XIR_BAD_TYPE}
    };
    for(size_t c=0;c<sizeof(cases)/sizeof(*cases);++c) {
        uint64_t before=held(context->resources);XrXirSourceResult result={0};
        XrXirStatus status=check(context,cases[c].text,&result,NULL);
        CHECK(status==cases[c].status && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);CHECK(held(context->resources)==before);
    }
    for(unsigned mode=0;mode<2;++mode) {
        unsigned calls=0;XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&calls};
        if(mode)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=held(context->resources);XrXirSourceResult result={0};
        CHECK(check(context,NULL,&result,&control)==(mode?XR_XIR_DEADLINE:XR_XIR_CANCELLED));
        CHECK(!result.checked && !result.snapshot && calls==(mode?0u:1u));
        xr_xir_compile_source_result_free(&result);CHECK(held(context->resources)==before);
    }
    XrXirSourceResult recovered={0};uint64_t before=held(context->resources);
    CHECK(check(context,NULL,&recovered,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&recovered);
    CHECK(held(context->resources)==before);
}
#endif
int main(int argc,char **argv) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t before=held(context.resources);XrXirProgram *program=NULL;
#if defined(XR_TASK_SCOPE_SOURCE_NATIVE)
    CHECK(argc==1);(void)argv;
    CHECK(xr_xir_compile_program_seal(&context,&task_scope_source_program,&program)==XR_XIR_OK);
    execute(program,task_scope_source_ids);
#else
    CHECK(argc==1 || argc==3);bool emit=argc==3 && !strcmp(argv[1],"--emit");CHECK(argc==1 || emit);
    XrXirSourceResult source={0};CHECK(check(&context,NULL,&source,NULL)==XR_XIR_OK);
    if(!emit)scope_normalized_controls(&context,source.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t ids[9]={module->declarations->entry_function,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    const char *names[8]={"main","moveStandalone","moveScoped","returnBoundary","breakBoundary","exitOrder",
        "panicBoundary","childFirstFailure"};
    for(uint32_t f=0;f<module->function_count;++f)
        for(unsigned n=0;n<8;++n)
            if(module->functions[f].name_length==strlen(names[n]) &&
                !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {CHECK(ids[1+n]==UINT32_MAX);ids[1+n]=f;}
    CHECK(ids[1]!=UINT32_MAX && ids[2]!=UINT32_MAX && ids[3]!=UINT32_MAX &&
        ids[4]!=UINT32_MAX && ids[5]!=UINT32_MAX && ids[6]!=UINT32_MAX && ids[7]!=UINT32_MAX && ids[8]!=UINT32_MAX);
    if(emit) {
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"task_scope_source",16777216,&generated)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && !strstr(generated.text,"({"));
        CHECK(fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t task_scope_source_ids[9]={%u,%u,%u,%u,%u,%u,%u,%u,%u};\n",
            ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7],ids[8])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(code);
    } else {
        CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);execute(program,ids);
        xr_xir_compile_program_drop(program);program=NULL;rejections(&context);scope_raw_controls(&context);
        scope_typed_exit_controls(&context);scope_cleanup_error_controls(&context);scope_receiving_controls(&context);
    }
#endif
    xr_xir_compile_program_drop(program);CHECK(held(context.resources)==before);
    xr_compile_resources_release(context.resources);
    CHECK(!runtime_live&&!runtime_bytes);xr_free(runtime_owned);runtime_owned=NULL;runtime_owned_capacity=0;
    puts("Lexical groups, pending exits and retired metadata PASS");return 0;
}
