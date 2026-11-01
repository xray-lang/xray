/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mem_borrow_source.c - Complete memory source and owned return facts
 *
 * KEY CONCEPT:
 *   A cold reader retains genuine receiver relationships after the Source
 *   producer retires. Removing their facts cannot turn a wrapper into a root.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_internal.h"
#include "base/xresource_limits.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_view_liveness_internal.h"
#include "base/xcompile_request.h"
#include "toolchain/xcompiler_session.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef XR_MEM_COLD_ROOT
#define XR_MEM_COLD_ROOT "."
#endif
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"mem Source %d: %s\n",__LINE__,#c);exit(1);}} while (0)
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
typedef struct Cancel {uint32_t polls,stop;} Cancel;
static bool cancel_at(const void *opaque) {
    Cancel *state=(Cancel *)opaque;return ++state->polls==state->stop;
}
static XrCompileRequestControl request_control(Cancel *state) {
    XrCompileRequestControl control={0};XrCompileRequestOptions options={cancel_at,state,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);return control;
}
static void return_controls(const XrXirCompileContext *context,const XrXirModule *module,
    const XrXirConstruction *construction,const XrXirConstructionSpec *base) {
    const XrXirBorrowReturns *original=xr_xir_compile_construction_borrow_returns(construction);
    CHECK(original && original->count==2);uint64_t baseline=held(context->resources);
    for (uint32_t kind=0;kind<8;++kind) {
        XrXirBorrowReturn rows[3]={original->rows[0],original->rows[1],{0}};
        XrXirBorrowReturns table={rows,2};XrXirConstructionSpec specification=*base;
        if (!kind) rows[0].function=module->function_count;
        if (kind==1) rows[0].native_callable=rows[1].native_callable;
        if (kind==2) rows[0].field=1;
        if (kind==3) rows[0].receiver=1;
        if (kind==4) table.count=1;
        if (kind==5) table.rows=NULL;
        if (kind==6) rows[0].nominal=module->types->nominals->count;
        if (kind==7) {
            rows[2]=rows[1];rows[1]=rows[0];rows[0].function=0;table.count=3;
        }
        specification.borrow_returns=&table;XrXirConstruction *received=NULL;
        XrXirStatus status=xr_xir_compile_construction_new_v5(context,module->types,&specification,
            xr_xir_compile_construction_slots(construction),&received,NULL);
        if (status==XR_XIR_OK) {
            XrXirArtifact *output=NULL;
            CHECK(xr_xir_compile_recheck_v2(context,module,received,&output,NULL,NULL)!=XR_XIR_OK && !output);
        } else CHECK(!received);
        xr_xir_compile_construction_free(received);CHECK(held(context->resources)==baseline);
    }
    XrXirConstructionSpec specification=*base;specification.borrow_returns=original;
    Cancel state={0,UINT32_MAX};XrCompileRequestControl control=request_control(&state);
    XrXirConstruction *received=NULL;
    CHECK(xr_xir_compile_construction_new_v5(context,module->types,&specification,
        xr_xir_compile_construction_slots(construction),&received,&control)==XR_XIR_OK && received);
    xr_xir_compile_construction_free(received);CHECK(state.polls>2 && held(context->resources)==baseline);
    state=(Cancel){0,state.polls/2};control=request_control(&state);received=NULL;
    CHECK(xr_xir_compile_construction_new_v5(context,module->types,&specification,
        xr_xir_compile_construction_slots(construction),&received,&control)==XR_XIR_CANCELLED && !received);
    CHECK(held(context->resources)==baseline);
    state=(Cancel){0,UINT32_MAX};control=request_control(&state);
    CHECK(xir_compile_view_liveness_function_verified(context,module,original->rows[0].function,
        NULL,NULL,NULL,&control)==XR_XIR_OK && state.polls>2);
    uint32_t stop=state.polls/2;state=(Cancel){0,stop};control=request_control(&state);
    CHECK(xir_compile_view_liveness_function_verified(context,module,original->rows[0].function,
        NULL,NULL,NULL,&control)==XR_XIR_CANCELLED && held(context->resources)==baseline);
    state=(Cancel){0,UINT32_MAX};control=request_control(&state);
    CHECK(xir_compile_view_liveness_function_verified(context,module,original->rows[0].function,
        NULL,NULL,NULL,&control)==XR_XIR_OK && held(context->resources)==baseline);
    XrXirCompileContext bounded=*context;bounded.limits.frame_bytes=1;
    CHECK(xir_compile_view_liveness_function_verified(&bounded,module,original->rows[0].function,
        NULL,NULL,NULL,NULL)==XR_XIR_BUDGET && held(context->resources)==baseline);
}
static void physical_flags(const XrXirCompileContext *context) {
    XrXirCallableParameter field={(XrXirType)256,XR_PARAM_READ};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=&field,.parameter_count=1},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8}
    };
    XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    for (uint32_t i=0;i<3;++i) CHECK(!xr_xir_compile_type_arena_borrowed(arena,(XrXirType)(256+i)));
    CHECK(xr_xir_compile_type_arena_borrowed(arena,(XrXirType)259)==1);
    xr_xir_compile_type_arena_drop(arena);
}
static void source_control(const XrXirSourceRequest *request,const char *text,bool accepted,const char *name) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(request,
        &(XrXirSourceText){NULL,text,strlen(text)},&source,&diagnostic,NULL);
    if ((status==XR_XIR_OK)!=accepted) fprintf(stderr,"case %s status%u %d:%d %s\n",
        name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK((status==XR_XIR_OK)==accepted && (accepted?source.checked!=NULL:source.checked==NULL));
    xr_xir_compile_source_result_free(&source);
}
static void caller_controls(const XrXirCompileContext *context,const char *stdlib) {
    static const struct {const char *name,*source;bool accepted;} cases[]={
        {"receiver-slice","import mem\nfn use(b:mem.Buffer)->i64 {const v=b.asBytes();return len(v)}\nprint(1)\n",true},
        {"pointer-copy-length","import mem\nfn use(b:mem.Buffer)->i64 {const p=b.borrowPtr();const q=p;const n=len(b);unsafe {return (q[0] as i64)+n}}\nprint(1)\n",true},
        {"local-buffer","import mem\nfn use()->i64 {var b=mem.alloc(4);const p=b.borrowPtr();unsafe {return (p[0] as i64)+len(b)}}\nprint(1)\n",true},
        {"raw-alias-write","import mem\nfn use(b:mem.Buffer)->i64 {const p=b.borrowPtr();const q=p;unsafe {p[0]=1 as u8;return q[0] as i64}}\nprint(1)\n",true},
        {"resize-after-pointer","import mem\nfn use(b:mem.Buffer)->i64 {unsafe {const p=b.borrowPtr();p[0]=1 as u8};b.resize(8);return len(b)}\nprint(1)\n",true},
        {"null-return","import mem\nfn use()->MutPtr<u8> {return MutPtr<u8>.null()}\nprint(1)\n",true},
        {"resize-live-pointer","import mem\nfn use(b:mem.Buffer) {const p=b.borrowPtr();b.resize(8);unsafe {print(p[0])}}\nprint(1)\n",false},
        {"alias-resize-live-slice","import mem\nfn use(a:mem.Buffer,b:mem.Buffer) {const v=a.asBytes();b.resize(8);print(len(v))}\nprint(1)\n",false},
        {"pointer-write-live-slice","import mem\nfn use(b:mem.Buffer) {const v=b.asBytes();const p=b.borrowPtr();unsafe {p[0]=1 as u8};print(len(v))}\nprint(1)\n",false},
        {"rebind-buffer","import mem\nfn use() {var b=mem.alloc(4);const p=b.borrowPtr();b=mem.alloc(8);unsafe {print(p[0])}}\nprint(1)\n",false},
        {"alias-demand-survives","import mem\nfn use(b:mem.Buffer) {const p=b.borrowPtr();const q=p;unsafe {print(p[0])};b.resize(8);unsafe {print(q[0])}}\nprint(1)\n",false},
        {"local-buffer-return","import mem\nfn use()->MutPtr<u8> {const b=mem.alloc(4);return b.borrowPtr()}\nprint(1)\n",false},
        {"temporary-buffer","import mem\nfn use() {const p=mem.alloc(4).borrowPtr();unsafe {print(p[0])}}\nprint(1)\n",false},
        {"pointer-array-escape","import mem\nfn use(b:mem.Buffer) {const p=b.borrowPtr();const escaped=[p];print(len(escaped))}\nprint(1)\n",false},
        {"bound-method-escape","import mem\nfn use(b:mem.Buffer) {const f=b.borrowPtr;const p=f();unsafe {print(p[0])}}\nprint(1)\n",false},
        {"pointer-capture","import mem\nfn use(b:mem.Buffer) {const p=b.borrowPtr();const f=fn()->i64{unsafe{return p[0] as i64}};b.resize(8);print(f())}\nprint(1)\n",false},
        {"unknown-return","import mem\nfn use(f:fn()->MutPtr<u8>)->i64 {const p=f();unsafe{return p[0] as i64}}\nprint(1)\n",false}
    };
    for (uint32_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        Cancel state={0,UINT32_MAX};XrCompileRequestControl control=request_control(&state);
        CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,cases[i].name,NULL};
        XrXirSourceRequest request={session,NULL,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
        source_control(&request,cases[i].source,cases[i].accepted,cases[i].name);
        CHECK(state.polls);
        xr_compile_session_free(session);
    }
}
#include "xir_mem_borrow_library_cases.h"
int main(int argc,char **argv) {
    CHECK(argc==2);char stdlib[2048];
    CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",argv[1])>0);
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};uint64_t frame=held(resources);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"mem-borrow-source",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    static const char input[]="import mem\nprint(1)\n";
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,input,sizeof(input)-1},&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source %u module%u %d:%d %s\n",status,
        diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    const XrXirBorrowReturns *returns=xr_xir_compile_construction_borrow_returns(construction);
    CHECK(returns && returns->count==2 && returns->rows);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    XrXirArtifact *cold=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    construction=xr_xir_compile_artifact_construction(cold);
    returns=xr_xir_compile_construction_borrow_returns(construction);CHECK(returns && returns->count==2);
    const XrXirModule *module=xr_xir_compile_artifact_module(cold);
    uint32_t count=xr_xir_compile_construction_count(construction);
    XrXirStatus allocation=XR_XIR_OK;
    XrXirConstructionRow *rows=count?xir_compile_calloc(&context,count,sizeof(*rows),&allocation):NULL;
    CHECK(allocation==XR_XIR_OK && (!count || rows));
    for (uint32_t n=0;n<count;++n) rows[n]=*xr_xir_compile_construction_row(construction,n);
    uint32_t bindings=xr_xir_compile_construction_materialization_count(construction);
    XrXirMaterializationBinding *materializations=bindings?xir_compile_calloc(&context,bindings,sizeof(*materializations),&allocation):NULL;
    CHECK(allocation==XR_XIR_OK && (!bindings || materializations));
    for (uint32_t n=0;n<bindings;++n) materializations[n]=*xr_xir_compile_construction_materialization(construction,n);
    XrXirConstructionSpec specification={rows,count,materializations,bindings,
        xr_xir_compile_construction_comptime(construction),NULL};
    XrXirConstruction *missing=NULL;
    CHECK(xr_xir_compile_construction_new_v5(&context,module->types,&specification,
        xr_xir_compile_construction_slots(construction),&missing,NULL)==XR_XIR_OK);
    XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_recheck_v2(&context,module,missing,&rejected,NULL,NULL)!=XR_XIR_OK && !rejected);
    return_controls(&context,module,construction,&specification);
    mem_direct_built_rejections(&context,module,construction);
    xr_xir_compile_construction_free(missing);xr_compile_resources_free(rows);xr_compile_resources_free(materializations);
    xr_xir_compile_artifact_free(cold);CHECK(held(resources)==frame);
    physical_flags(&context);caller_controls(&context,stdlib);
    mem_library_case(&context,stdlib,false);mem_library_case(&context,stdlib,true);
    mem_library_root_rejection(&context);
    mem_direct_source_rejections(&context,stdlib);
    CHECK(held(resources)==frame);xr_compile_resources_release(resources);
    puts("mem source return facts PASS");return 0;
}
