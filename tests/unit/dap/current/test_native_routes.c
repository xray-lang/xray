/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_routes.c - Authenticated native Source exception routing
 */
#include "app/dap/xdap_current.h"
#include "app/dap/xdap_current_control.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm_debug_internal.h"
#include "xir/xxir_call_native_source_internal.h"
#include "xir/xxir_enum.h"
#include "toolchain/xcompiler_session.h"
#include "program/xr_xir_source_product.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"native Source stop %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "../../xir/xir_go_source_native_compile_owner.h"
#include "../../xir/xir_runtime_allocations.h"
#include "native_task.h"
XR_DATA const XirCallNativeSourceCode native_task_native_source;

typedef struct NativeSourceFixture {
    XrXirDomain *domain;
    XdapCurrent *session;
    unsigned outputs,releases;
    int64_t values[8];
    uint32_t instructions[2];
    XirCallNativeSourceCode registration;
    uint32_t offsets[NT_FUNCTIONS];
    bool child_native;
    unsigned char native[NT_FUNCTIONS];
    uint32_t accepted[NT_FUNCTIONS];
} NativeSourceFixture;
enum { SOURCE_CONTINUE,SOURCE_STOP,SOURCE_CANCEL,SOURCE_FREE,SOURCE_DETACH };
static XrXirOutputStatus source_output(void *owner,const XrXirOutputGroup *group) {
    NativeSourceFixture *f=owner;CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==1);
    CHECK(f->outputs<8 && group->values[0].type==XR_XIR_I64);f->values[f->outputs++]=group->values[0].payload;return XR_XIR_OUTPUT_OK;
}
static void source_code_release(void *owner) { ++((NativeSourceFixture *)owner)->releases; }
static void source_exception(XrXirVmDebugSnapshot *snapshot) {
    XrXirValue error={0};CHECK(xr_xir_vm_debug_snapshot_exception(snapshot,&error)==XR_XIR_VALUE_OK);
    XrXirEnumBorrow shape={0};CHECK(xr_xir_enum_borrow(&error,&shape)==XR_XIR_VALUE_OK);
    CHECK(shape.name.length==7 && !memcmp(shape.name.bytes,"Problem",7) && shape.field_count==1);
    const char expected[]="child\0\xCE\xB1";const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&shape.fields[0],&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    bool visible=false;CHECK(xr_xir_vm_debug_snapshot_type_visible(snapshot,0,&error,(XrXirType)error.type,&visible)==XR_XIR_VALUE_OK && visible);
    uint32_t filter=0;CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_OK && (filter==XR_XIR_VM_DEBUG_EXCEPTION_ALL || filter==XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT || filter==XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT));
    xr_xir_value_drop(&error);
}
static void source_new(NativeSourceFixture *f,unsigned backend,unsigned mapped) {
    memset(f,0,sizeof(*f));
    for(unsigned i=0;i<NT_FUNCTIONS;++i) f->accepted[i]=UINT32_MAX;
    const XrXirCompileContext *context=effects_source_owner(67108864,128000000);
    XrCompilerSession *compiler=NULL;CHECK(xr_compile_session_new(context->resources,&compiler)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_NATIVE_TASK_FIXTURE_DIR};
    XrXirSourceRequest request={compiler,XR_NATIVE_TASK_FIXTURE_DIR "/native_task_throw.xr",&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceProductRequest product_request={request,native_task_program.target};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_product_build(&product_request,&product,&diagnostic)==XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(compiler);
    const XrXirSourceDebugView *map=xr_xir_compile_source_product_debug(product);CHECK(map);
    CHECK(map->functions[NT_FAIL].throw_count==2);
    for(unsigned i=0;i<2;++i) f->instructions[i]=map->functions[NT_FAIL].throw_sites[i].instruction;
    XrXirSourceProductPacketView packet={0};XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read_lower(context,packet.bytes,packet.length,&native_task_program.target,&lowered,NULL)==XR_XIR_OK);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&f->domain)==XR_XIR_VALUE_OK);
    XrXirVmDebug *debug=NULL;CHECK(xr_xir_vm_debug_new(f->domain,64,&debug)==XR_XIR_VALUE_OK);
    unsigned char mask[NT_FUNCTIONS];memset(mask,backend==0 ? 0 : 1,sizeof(mask));
    mask[NT_FAIL]=backend==2 ? 0 : 1;if(backend==1) mask[NT_BUSY]=0;
    if(backend==3) { memset(mask,0,sizeof(mask));mask[NT_RELAY]=1; }
    if(backend==4) { memset(mask,1,sizeof(mask));mask[NT_RELAY]=0; }
    XrXirProgramSpec native=native_task_program;native.code=(XrXirCodeLease){f,source_code_release};
    XrXirProgram *program=NULL;
    f->registration=native_task_native_source;
    memcpy(f->offsets,f->registration.offsets,sizeof(f->offsets));f->registration.offsets=f->offsets;
    XrXirVmDebugNative receiving={&native,&f->registration,mask,NT_FUNCTIONS};
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&lowered,&receiving,debug,&program)==XR_XIR_OK);
    memset(f->offsets,0,sizeof(f->offsets));f->registration.version=0;
    memcpy(f->native,mask,sizeof(mask));f->child_native=mask[NT_FAIL]!=0;
    const uint32_t selected[]={NT_FAIL,NT_CATCHING,NT_INDIRECT,NT_RECURSIVE};
    const char *markers[]={"NATIVE_ACCEPT fail","NATIVE_ACCEPT catching","NATIVE_ACCEPT indirect","NATIVE_ACCEPT recursive"};
    for(unsigned m=0;m<4;++m) {
        FILE *file=fopen(XR_NATIVE_TASK_FIXTURE_DIR "/native_task_throw.xr","rb");CHECK(file);
        char line[512];uint32_t row=0,expected=0;
        while(fgets(line,sizeof(line),file)) { ++row;if(strstr(line,markers[m])) { CHECK(!expected);expected=row; } }
        CHECK(!fclose(file) && expected);const XrXirSourceDebugFunction *fn=&map->functions[selected[m]];
        unsigned matches=0;
        for(uint32_t h=0;h<fn->handler_count;++h) for(uint32_t s=0;s<fn->handlers[h].selector_count;++s) {
            uint32_t accepted=fn->handlers[h].selectors[s].accepted;
            if(fn->points[accepted].range.line==(int32_t)expected) { f->accepted[selected[m]]=accepted;++matches; }
        }
        CHECK(matches==1);
    }
    if(mapped) {
        XrXirSourceDebugFunction *functions=NULL;XrXirSourceDebugView partial=*map;
        if(mapped==2) {
            CHECK(map->functions[NT_CATCHING].route_count);
            size_t bytes=(size_t)map->function_count*sizeof(*functions);
            CHECK(xr_compile_resources_alloc(context->resources,bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
            memcpy(functions,map->functions,bytes);functions[NT_CATCHING].routes=NULL;functions[NT_CATCHING].route_count=0;
            partial.functions=functions;
        }
        XrXirStatus mapped_status=xir_vm_debug_source_map(debug,context,functions ? &partial : map);
        if(functions) { memset(functions,0xff,(size_t)map->function_count*sizeof(*functions));xr_compile_resources_free(functions); }
        if(mapped_status!=XR_XIR_OK) fprintf(stderr,"native Source map status=%u functions=%u\n",mapped_status,map->function_count);
        CHECK(mapped_status==XR_XIR_OK);
    }
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,source_output,f};
    CHECK(xdap_current_new(f->domain,&debug,program,&config,&f->session)==XR_XIR_VALUE_OK);
    xr_xir_compile_program_drop(program);CHECK(!f->releases);
}
static void source_close(NativeSourceFixture *f,XrXirVmDebugSnapshot *escaped) {
    CHECK(xdap_current_free(f->session)==XR_XIR_CALL_READY);f->session=NULL;CHECK(f->releases<=1);
    if(escaped) source_exception(escaped);
    /* An owned Function in a VM ancestor snapshot can retain its code lease. */
    xr_xir_vm_debug_snapshot_drop(escaped);CHECK(f->releases==1);
    CHECK(!xr_xir_domain_budget_stats(f->domain).metadata_live);xr_xir_domain_drop(f->domain);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
}

typedef struct NativeRouteCase {
    uint32_t entry,selected,ancestors;
    int64_t returned;
    bool child,escape;
} NativeRouteCase;
static const NativeRouteCase route_cases[]={
    {NT_FAIL,NT_FAIL,0,0,false,true},
    {NT_CATCHING,NT_CATCHING,2,71,false,false},
    {NT_ESCAPE_ROOT,NT_ESCAPE_ROOT,2,0,false,true},
    {NT_INDIRECT,NT_INDIRECT,1,87,false,false},
    {NT_RECURSIVE,NT_RECURSIVE,5,83,false,false},
    {NT_AWAITING,NT_FAIL,0,17,true,true}
};
static bool route_matches(uint32_t filters,bool selected) {
    return (filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL) || (filters&(selected ? XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT));
}
static void native_route_case(unsigned backend,uint32_t filters,unsigned index) {
    const NativeRouteCase *expected=&route_cases[index];NativeSourceFixture f;source_new(&f,backend,true);
    CHECK(xdap_current_break_on_throw(f.session,filters)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(f.session,expected->entry,NULL,0)==XR_XIR_CALL_READY);
    unsigned logical=route_matches(filters,true) ? 0 : 1,stops=0;bool ended=false;
    XrXirVmDebugSnapshot *escaped=NULL;XirVmDebugExceptionRoute saved={0};
    for(unsigned i=0;i<64;++i) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        if(result.state==XDAP_CURRENT_STOPPED) {
            CHECK(logical<2 && route_matches(filters,logical==0 || !expected->escape));
            CHECK(f.outputs==logical && (!logical || f.values[0]==89));
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(f.session,&snapshot)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionLocal local={0};XirVmDebugExceptionRoute route={0};
            CHECK(xir_vm_debug_snapshot_exception_local(snapshot,&local)==XR_XIR_VALUE_OK);
            CHECK(xir_vm_debug_snapshot_exception_route(snapshot,&route)==XR_XIR_VALUE_OK);
            CHECK(local.function==NT_FAIL && local.kind==(logical ? XIR_VM_DEBUG_EXCEPTION_LOCAL_PROPAGATE : XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH));
            uint32_t filter=0;CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_OK);
            CHECK(filter==(uint32_t)(filters&XR_XIR_VM_DEBUG_EXCEPTION_ALL ? XR_XIR_VM_DEBUG_EXCEPTION_ALL :
                !logical || !expected->escape ? XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT));
            uint32_t selected=logical ? expected->selected : NT_FAIL,ancestors=logical ? expected->ancestors : 0;
            XirVmDebugExceptionRouteKind kind=!logical || !expected->escape ? XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED :
                expected->child ? XIR_VM_DEBUG_EXCEPTION_ROUTE_TASK_ESCAPE : XIR_VM_DEBUG_EXCEPTION_ROUTE_ROOT_ESCAPE;
            CHECK(route.kind==kind && route.function==selected && route.ancestors==ancestors);
            CHECK(kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED ? route.handler && route.accepted==f.accepted[selected] :
                !route.handler && route.accepted==UINT32_MAX);
            XrXirVmDebugFrameInfo current={0},chosen={0};XrXirValue name={0};
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&current,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            CHECK(current.function==NT_FAIL && current.instruction==f.instructions[logical] &&
                (expected->child ? current.thread>1 : current.thread==1));
            CHECK(current.code==(f.native[NT_FAIL] ? XR_XIR_VM_DEBUG_NATIVE : XR_XIR_VM_DEBUG_VM));
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,ancestors,&chosen,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            CHECK(chosen.function==selected && chosen.id==route.frame && chosen.thread==current.thread);
            CHECK(chosen.code==(f.native[selected] ? XR_XIR_VM_DEBUG_NATIVE : XR_XIR_VM_DEBUG_VM));
            CHECK(ancestors ? chosen.id!=current.id : chosen.id==current.id);
            size_t allocations=runtime_attempts;uint64_t work=xr_xir_domain_budget_stats(f.domain).work;
            for(unsigned held=0;held<16;++held) {
                XdapCurrentResult again={0};CHECK(xdap_current_poll(f.session,UINT64_MAX,&again)==XR_XIR_VALUE_OK);
                CHECK(again.state==XDAP_CURRENT_STOPPED && again.debug_epoch==result.debug_epoch && f.outputs==logical);
                xdap_current_result_drop(&again);
            }
            CHECK(allocations==runtime_attempts && work==xr_xir_domain_budget_stats(f.domain).work);
            CHECK(xdap_current_continue(f.session,result.debug_epoch+1,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
            if(!escaped) { escaped=snapshot;saved=route; } else xr_xir_vm_debug_snapshot_drop(snapshot);
            CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
            ++stops;++logical;
            if(logical==1 && !route_matches(filters,!expected->escape)) ++logical;
        } else if(result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==(!expected->child && expected->escape ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
            if(result.outcome.status==XR_XIR_CALL_RETURNED) CHECK(result.outcome.value.type==XR_XIR_I64 &&
                (int64_t)result.outcome.value.payload==expected->returned);
            ended=true;xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);
    }
    CHECK(ended && stops==(unsigned)route_matches(filters,true)+(unsigned)route_matches(filters,!expected->escape));
    const int64_t output[]={89,91,17,73,95};CHECK(f.outputs==(expected->child ? 5u : 2u));
    CHECK(!memcmp(f.values,output,(expected->child ? 5 : 2)*sizeof(*output)));
    CHECK(xdap_current_free(f.session)==XR_XIR_CALL_READY);f.session=NULL;CHECK(f.releases<=1);
    if(escaped) {
        XirVmDebugExceptionRoute after={0};CHECK(xir_vm_debug_snapshot_exception_route(escaped,&after)==XR_XIR_VALUE_OK);
        CHECK(after.kind==saved.kind && after.function==saved.function && after.handler==saved.handler &&
            after.accepted==saved.accepted && after.frame==saved.frame && after.ancestors==saved.ancestors);
    }
    source_close(&f,escaped);
    printf("NATIVE_ROUTE backend=%u filters=%u case=%u stops=%u physical=0/0\n",backend,filters,index,stops);CHECK(!fflush(stdout));
}
static size_t native_route_fault(unsigned backend,unsigned mode,size_t fail) {
    NativeSourceFixture f;source_new(&f,backend,true);
    uint32_t filters=mode==1 ? XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT : XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT;
    CHECK(xdap_current_break_on_throw(f.session,filters)==XR_XIR_VALUE_OK);
    if(mode==2) {
        CHECK(xdap_current_start(f.session,NT_CATCHING,NULL,0)==XR_XIR_CALL_READY);
        XdapCurrentResult first={0};CHECK(xdap_current_poll(f.session,10000,&first)==XR_XIR_VALUE_OK);
        CHECK(first.state==XDAP_CURRENT_STOPPED && !f.outputs);
        CHECK(xdap_current_continue(f.session,first.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        xdap_current_result_drop(&first);
    }
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCallStatus start=mode==2 ? XR_XIR_CALL_READY : xdap_current_start(f.session,mode==1 ? NT_ESCAPE_ROOT : NT_CATCHING,NULL,0);
    XdapCurrentResult result={0};
    if(start==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        CHECK(fail==SIZE_MAX ? result.state==XDAP_CURRENT_STOPPED :
            result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
        if(fail==SIZE_MAX) CHECK(f.outputs==(mode ? 1u : 0u));
    } else CHECK(fail!=SIZE_MAX && start==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(fail==SIZE_MAX || sites>fail);
    xdap_current_result_drop(&result);
    XrXirCallStatus freed=xdap_current_free(f.session);f.session=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (fail!=SIZE_MAX && freed==XR_XIR_CALL_OOM));
    CHECK(f.outputs<=2 && (!f.outputs || f.values[0]==89) && (f.outputs<2 || f.values[1]==91));
    source_close(&f,NULL);return sites;
}
static void native_route_lifecycle(unsigned backend,unsigned example,unsigned finish) {
    const NativeRouteCase *expected=&route_cases[example];NativeSourceFixture f;source_new(&f,backend,true);
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_ALL)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(f.session,expected->entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;XirVmDebugExceptionRoute saved={0};bool ended=false;unsigned stops=0;
    for(unsigned i=0;i<64;++i) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        if(result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stops<2 && f.outputs==stops);
            if(!stops) CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
            else {
                CHECK(xdap_current_snapshot(f.session,&escaped)==XR_XIR_VALUE_OK);
                CHECK(xir_vm_debug_snapshot_exception_route(escaped,&saved)==XR_XIR_VALUE_OK);
                CHECK(saved.kind==(expected->child ? XIR_VM_DEBUG_EXCEPTION_ROUTE_TASK_ESCAPE : XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED));
                CHECK(saved.function==expected->selected && saved.ancestors==expected->ancestors);
                if(finish==SOURCE_STOP) CHECK(xdap_current_stop(f.session)==XR_XIR_CALL_READY);
                else if(finish==SOURCE_CANCEL) CHECK(xdap_current_cancel(f.session)==XR_XIR_CALL_CANCEL_REQUESTED);
                else if(finish==SOURCE_FREE) { CHECK(xdap_current_free(f.session)==XR_XIR_CALL_READY);f.session=NULL;ended=true; }
                else CHECK(finish==SOURCE_DETACH && xdap_current_detach(f.session,NULL,NULL)==XR_XIR_CALL_READY);
            }
            ++stops;
        } else if(result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==(finish==SOURCE_DETACH ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_CANCELLED));
            if(finish==SOURCE_DETACH) CHECK(result.outcome.value.type==XR_XIR_I64 && (int64_t)result.outcome.value.payload==expected->returned);
            ended=true;xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);if(!f.session) break;
    }
    CHECK(ended && stops==2 && escaped);
    if(finish==SOURCE_DETACH) {
        const int64_t output[]={89,91,17,73,95};CHECK(f.outputs==(expected->child ? 5u : 2u));
        CHECK(!memcmp(f.values,output,(expected->child ? 5 : 2)*sizeof(*output)));
    }
    CHECK(xdap_current_free(f.session)==XR_XIR_CALL_READY);f.session=NULL;
    XirVmDebugExceptionRoute after={0};CHECK(xir_vm_debug_snapshot_exception_route(escaped,&after)==XR_XIR_VALUE_OK);
    CHECK(after.kind==saved.kind && after.frame==saved.frame && after.function==saved.function && after.handler==saved.handler &&
        after.accepted==saved.accepted && after.ancestors==saved.ancestors);
    source_close(&f,escaped);
    printf("NATIVE_ROUTE_LIFECYCLE backend=%u case=%u finish=%u physical=0/0\n",backend,example,finish);CHECK(!fflush(stdout));
}
static void native_route_control(unsigned backend,bool partial) {
    NativeSourceFixture f;source_new(&f,backend,partial ? 2 : 0);
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_ALL)==(partial ? XR_XIR_VALUE_OK : XR_XIR_VALUE_UNSUPPORTED));
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==XR_XIR_VALUE_UNSUPPORTED);
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_UNCAUGHT)==XR_XIR_VALUE_UNSUPPORTED);
    CHECK(xdap_current_start(f.session,NT_CATCHING,NULL,0)==XR_XIR_CALL_READY);
    bool ended=false;unsigned stops=0;
    for(unsigned poll=0;poll<64;++poll) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        if(result.state==XDAP_CURRENT_STOPPED) {
            CHECK(partial && stops<2 && f.outputs==stops);
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(f.session,&snapshot)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionRoute route={0};uint32_t filter=0;
            CHECK(xir_vm_debug_snapshot_exception_route(snapshot,&route)==XR_XIR_VALUE_OK);
            CHECK(route.kind==(stops ? XIR_VM_DEBUG_EXCEPTION_ROUTE_UNKNOWN : XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED));
            CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_OK && filter==XR_XIR_VM_DEBUG_EXCEPTION_ALL);
            xr_xir_vm_debug_snapshot_drop(snapshot);
            CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);++stops;
        } else if(result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.type==XR_XIR_I64 && result.outcome.value.payload==71);
            ended=true;xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);
    }
    CHECK(ended && stops==(partial ? 2u : 0u) && f.outputs==2 && f.values[0]==89 && f.values[1]==91);
    source_close(&f,NULL);
    printf("NATIVE_ROUTE_CONTROL backend=%u partial=%u stops=%u physical=0/0\n",backend,partial,stops);CHECK(!fflush(stdout));
}
static void native_route_running(unsigned backend) {
    NativeSourceFixture f;source_new(&f,backend,true);
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_ALL)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(f.session,NT_CATCHING,NULL,0)==XR_XIR_CALL_READY);
    bool observed=false;
    for(unsigned poll=0;poll<4096;++poll) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,1,&result)==XR_XIR_VALUE_OK);
        CHECK(result.state==XDAP_CURRENT_RUNNING && !f.outputs);xdap_current_result_drop(&result);
        uint64_t threads[8]={0};uint32_t count=0;
        XrXirValueStatus status=xdap_current_threads(f.session,threads,8,&count);
        CHECK(status==XR_XIR_VALUE_OK || status==XR_XIR_VALUE_UNSUPPORTED);
        if(status==XR_XIR_VALUE_OK) { CHECK(count);observed=true;break; }
    }
    CHECK(observed);
    XrXirValueStatus configured=xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_NONE);
    printf("NATIVE_ROUTE_RUNNING_ADMISSION backend=%u observed=1 status=%u\n",backend,(unsigned)configured);CHECK(!fflush(stdout));
    CHECK(configured==XR_XIR_VALUE_BAD_ARGUMENT);
    unsigned stops=0;bool ended=false;
    for(unsigned poll=0;poll<64;++poll) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        if(result.state==XDAP_CURRENT_STOPPED) {
            CHECK(stops<2 && f.outputs==stops);
            CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);++stops;
        } else if(result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==XR_XIR_CALL_RETURNED && result.outcome.value.type==XR_XIR_I64 && result.outcome.value.payload==71);
            ended=true;xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);
    }
    CHECK(ended && stops==2 && f.outputs==2 && f.values[0]==89 && f.values[1]==91);source_close(&f,NULL);
    printf("NATIVE_ROUTE_RUNNING backend=%u physical=0/0\n",backend);CHECK(!fflush(stdout));
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--controls")) {
        for(unsigned backend=0;backend<5;++backend) for(unsigned partial=0;partial<2;++partial) native_route_control(backend,partial!=0);
        for(unsigned backend=0;backend<5;++backend) native_route_running(backend);
        return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--lifecycle")) {
        const unsigned examples[]={1,3,5};
        for(unsigned backend=0;backend<5;++backend) for(unsigned e=0;e<3;++e)
            for(unsigned finish=SOURCE_STOP;finish<=SOURCE_DETACH;++finish) native_route_lifecycle(backend,examples[e],finish);
        return 0;
    }
    if(argc==2 && (!strcmp(argv[1],"--census") || !strcmp(argv[1],"--boundary") || !strcmp(argv[1],"--resources"))) {
        bool census=!strcmp(argv[1],"--census"),boundary=!strcmp(argv[1],"--boundary");
        for(unsigned backend=0;backend<5;++backend) for(unsigned mode=0;mode<3;++mode) {
            size_t sites=native_route_fault(backend,mode,SIZE_MAX);CHECK(sites>1);
            if(boundary) { native_route_fault(backend,mode,0);native_route_fault(backend,mode,sites-1); }
            else if(!census) for(size_t i=0;i<sites;++i) native_route_fault(backend,mode,i);
            printf("NATIVE_ROUTE_CAPTURE backend=%u mode=%u sites=%zu fullFI=%u firstLast=%u physical=0/0\n",backend,mode,sites,!census && !boundary,boundary);
            CHECK(!fflush(stdout));
        }
        return 0;
    }
    CHECK(argc==1);
    for(unsigned backend=0;backend<5;++backend) for(uint32_t filters=0;filters<8;++filters)
        for(unsigned example=0;example<6;++example) native_route_case(backend,filters,example);
    puts("Authentic mixed native Source selectors, resolved pending calls, root/Task boundaries and owned routes: PASS");
    return 0;
}
