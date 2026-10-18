/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_source_stop.c - Real native Source throws before lexical cleanup
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
    uint32_t filter=0;CHECK(xir_vm_debug_snapshot_exception_filter(snapshot,&filter)==XR_XIR_VALUE_OK && filter==XR_XIR_VM_DEBUG_EXCEPTION_ALL);
    xr_xir_value_drop(&error);
}
static void source_new(NativeSourceFixture *f,unsigned backend,bool mapped) {
    memset(f,0,sizeof(*f));f->child_native=backend!=2;
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
    XrXirProgramSpec native=native_task_program;native.code=(XrXirCodeLease){f,source_code_release};
    XrXirProgram *program=NULL;
    f->registration=native_task_native_source;
    memcpy(f->offsets,f->registration.offsets,sizeof(f->offsets));f->registration.offsets=f->offsets;
    XrXirVmDebugNative receiving={&native,&f->registration,mask,NT_FUNCTIONS};
    CHECK(xr_xir_compile_vm_debug_mixed_program_take(&lowered,&receiving,debug,&program)==XR_XIR_OK);
    memset(f->offsets,0,sizeof(f->offsets));f->registration.version=0;
    if(mapped) {
        XrXirStatus mapped_status=xir_vm_debug_source_map(debug,context,map);
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
    CHECK(xdap_current_free(f->session)==XR_XIR_CALL_READY);f->session=NULL;CHECK(f->releases==1);
    if(escaped) source_exception(escaped);
    xr_xir_vm_debug_snapshot_drop(escaped);
    CHECK(!xr_xir_domain_budget_stats(f->domain).metadata_live);xr_xir_domain_drop(f->domain);
    CHECK(!runtime_live && !runtime_bytes);effects_source_owners_free();
}
static void source_case(unsigned backend,unsigned finish,bool mapped,bool enabled,bool root) {
    NativeSourceFixture f;source_new(&f,backend,mapped);
    CHECK(xdap_current_break_on_throw(f.session,enabled ? XR_XIR_VM_DEBUG_EXCEPTION_ALL : XR_XIR_VM_DEBUG_EXCEPTION_NONE)==
        (enabled && !mapped ? XR_XIR_VALUE_UNSUPPORTED : XR_XIR_VALUE_OK));
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==
        (mapped ? XR_XIR_VALUE_OK : XR_XIR_VALUE_UNSUPPORTED));
    CHECK(xdap_current_break_on_throw(f.session,enabled ? XR_XIR_VM_DEBUG_EXCEPTION_ALL : XR_XIR_VM_DEBUG_EXCEPTION_NONE)==
        (enabled && !mapped ? XR_XIR_VALUE_UNSUPPORTED : XR_XIR_VALUE_OK));
    CHECK(xdap_current_start(f.session,root ? NT_FAIL : NT_AWAITING,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;unsigned stops=0;bool ended=false;
    for(unsigned i=0;i<64;++i) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        if(result.state==XDAP_CURRENT_STOPPED) {
            CHECK(mapped && enabled && stops<2 && f.outputs==stops);
            if(stops) CHECK(f.values[0]==89);
            XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(f.session,&snapshot)==XR_XIR_VALUE_OK);
            source_exception(snapshot);XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
            CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
            CHECK(info.function==NT_FAIL && info.instruction==f.instructions[stops] && (root ? info.thread==1 : info.thread>1));
            CHECK(info.code==(f.child_native ? XR_XIR_VM_DEBUG_NATIVE : XR_XIR_VM_DEBUG_VM));
            if(f.child_native) CHECK(!info.value_count);
            size_t allocations=runtime_attempts;uint64_t work=xr_xir_domain_budget_stats(f.domain).work;
            for(unsigned j=0;j<16;++j) {
                XdapCurrentResult held={0};CHECK(xdap_current_poll(f.session,UINT64_MAX,&held)==XR_XIR_VALUE_OK);
                CHECK(held.state==XDAP_CURRENT_STOPPED && held.debug_epoch==result.debug_epoch && f.outputs==stops);xdap_current_result_drop(&held);
            }
            CHECK(runtime_attempts==allocations && xr_xir_domain_budget_stats(f.domain).work==work);
            CHECK(xdap_current_continue(f.session,result.debug_epoch+1,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
            if(f.child_native) CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_STEP_IN)==XR_XIR_VALUE_UNSUPPORTED);
            if(!escaped) escaped=snapshot;else xr_xir_vm_debug_snapshot_drop(snapshot);
            ++stops;
            if(finish==SOURCE_STOP) CHECK(xdap_current_stop(f.session)==XR_XIR_CALL_READY);
            else if(finish==SOURCE_CANCEL) CHECK(xdap_current_cancel(f.session)==XR_XIR_CALL_CANCEL_REQUESTED);
            else if(finish==SOURCE_FREE) { CHECK(xdap_current_free(f.session)==XR_XIR_CALL_READY);f.session=NULL;ended=true; }
            else if(finish==SOURCE_DETACH) CHECK(xdap_current_detach(f.session,NULL,NULL)==XR_XIR_CALL_READY);
            else CHECK(xdap_current_continue(f.session,result.debug_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if(result.state==XDAP_CURRENT_ENDED) {
            CHECK(result.outcome.status==(finish==SOURCE_STOP || finish==SOURCE_CANCEL ? XR_XIR_CALL_CANCELLED :
                root ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));ended=true;
            xdap_current_result_drop(&result);break;
        }
        xdap_current_result_drop(&result);
        if(!f.session) break;
    }
    CHECK(ended && stops==(mapped && enabled ? finish!=SOURCE_CONTINUE ? 1u : 2u : 0u));
    if(finish==SOURCE_CONTINUE || finish==SOURCE_DETACH) {
        const int64_t expected[]={89,91,17,73,95};CHECK(f.outputs==(root ? 2u : 5u));
        CHECK(!memcmp(f.values,expected,(root ? 2 : 5)*sizeof(*expected)));
    }
    source_close(&f,escaped);
    printf("NATIVE_SOURCE_STOP backend=%u finish=%u mapped=%u enabled=%u root=%u originalThrows=%u physical=0/0\n",backend,finish,mapped,enabled,root,stops);
}
static size_t source_fault(unsigned backend,size_t fail) {
    NativeSourceFixture f;source_new(&f,backend,true);
    CHECK(xdap_current_break_on_throw(f.session,XR_XIR_VM_DEBUG_EXCEPTION_ALL)==XR_XIR_VALUE_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirCallStatus start=xdap_current_start(f.session,NT_AWAITING,NULL,0);XdapCurrentResult result={0};
    if(start==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(f.session,10000,&result)==XR_XIR_VALUE_OK);
        CHECK(fail==SIZE_MAX ? result.state==XDAP_CURRENT_STOPPED :
            result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM);
    } else CHECK(fail!=SIZE_MAX && start==XR_XIR_CALL_OOM);
    size_t attempts=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(fail==SIZE_MAX || attempts>fail);
    xdap_current_result_drop(&result);
    XrXirCallStatus freed=xdap_current_free(f.session);f.session=NULL;
    CHECK(freed==XR_XIR_CALL_READY || (fail!=SIZE_MAX && freed==XR_XIR_CALL_OOM));
    source_close(&f,NULL);return attempts;
}
int main(int argc,char **argv) {
    if(argc==2 && (!strcmp(argv[1],"--census") || !strcmp(argv[1],"--resources") || !strcmp(argv[1],"--boundary"))) {
        bool census=!strcmp(argv[1],"--census");
        bool boundary=!strcmp(argv[1],"--boundary");
        for(unsigned backend=0;backend<2;++backend) {
            size_t sites=source_fault(backend,SIZE_MAX);CHECK(sites);
            if(boundary) { CHECK(sites>1);source_fault(backend,0);source_fault(backend,sites-1); }
            else if(!census) for(size_t i=0;i<sites;++i) source_fault(backend,i);
            printf("NATIVE_SOURCE_CAPTURE backend=%u sites=%zu fullFI=%u firstLast=%u physical=0/0\n",backend,sites,!census && !boundary,boundary);
        }
        return 0;
    }
    CHECK(argc==1);
    for(unsigned backend=0;backend<3;++backend) for(unsigned finish=SOURCE_CONTINUE;finish<=SOURCE_DETACH;++finish)
        for(unsigned root=0;root<2;++root) source_case(backend,finish,true,true,root!=0);
    source_case(1,SOURCE_CONTINUE,false,true,false);source_case(1,SOURCE_CONTINUE,true,false,false);
    puts("Real native Source origin, cleanup-before-stop exclusion, controller epochs and owned exception escape: PASS");return 0;
}
