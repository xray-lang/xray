/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_atomic_source.h - Owned Source through portable Atomic runtime bodies
 */
#ifndef XIR_MIXED_ATOMIC_SOURCE_H
#define XIR_MIXED_ATOMIC_SOURCE_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
static const char *const atomic_runtime_entry_names[]={
    "constI64","aliasI64","rootParamI64","rootAliasI64","workerLocalI64","pureLibrary",
    "constF64","rootParamF64","constBool","rootParamBool","holdFixedI64","holdI64",
    "holdF64","holdBool","rebindI64"
};
enum {ATOMIC_RUNTIME_ENTRIES=15};
typedef struct AtomicSourcePrepare {
    const XrXirCompileContext *context;
    XrCompilerSession *session;
    XrXirSourceResult source;
    XrXirCheckedPacket packet;
    XrXirArtifact *checked,*closed,*lowered;
    XrXirCSource generated;
    uint32_t entries[ATOMIC_RUNTIME_ENTRIES];
    XrXirStatus status;
    unsigned stage;
} AtomicSourcePrepare;
static bool atomic_source_stage(AtomicSourcePrepare *s,unsigned stage,XrXirStatus status) {
    s->stage=stage;s->status=status;return status==XR_XIR_OK;
}
static void atomic_source_entries(AtomicSourcePrepare *s) {
    const XrXirModule *m=xr_xir_compile_artifact_module(s->lowered);
    if(m)fprintf(stderr,"Atomic actual lowered modules=%u slots=%u functions=%u\n",m->declarations->module_count,m->declarations->slot_count,m->function_count);
    CHECK(m && m->declarations->slot_count==7 && m->declarations->module_count==4 && m->function_count==37);
    for(unsigned n=0;n<ATOMIC_RUNTIME_ENTRIES;++n) {
        s->entries[n]=UINT32_MAX;
        for(uint32_t f=0;f<m->function_count;++f) {
            const XrXirFunction *fn=&m->functions[f];
            if(m->declarations->functions[f].module!=m->declarations->root_module)continue;
            if(fn->name_length!=strlen(atomic_runtime_entry_names[n]) || memcmp(fn->name,atomic_runtime_entry_names[n],fn->name_length))continue;
            CHECK(s->entries[n]==UINT32_MAX && !fn->parameter_count);s->entries[n]=f;
        }
        CHECK(s->entries[n]!=UINT32_MAX);
    }
}
static bool atomic_source_prepare(AtomicSourcePrepare *s) {
    XrCompilerSessionStatus created=xr_compile_session_new(s->context->resources,&s->session);
    CHECK(created==XR_COMPILER_SESSION_OK || created==XR_COMPILER_SESSION_BUDGET || created==XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status=created==XR_COMPILER_SESSION_OK?XR_XIR_OK:created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(!atomic_source_stage(s,2,status)){CHECK(!s->session);return false;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_P20_ATOMIC_AUTHORITY};
    XrXirSourceRequest request={s->session,XR_P20_ATOMIC_RUNTIME_FIXTURES "/root.xr",&authority,s->context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    status=xr_xir_compile_source_check(&request,&s->source,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Atomic Source stage=3 status=%u at=%d:%d message=%s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    if(!atomic_source_stage(s,3,status)){CHECK(!s->source.checked && !s->source.snapshot);return false;}
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(s->source.snapshot);
    if(view) {
        fprintf(stderr,"Atomic actual Source complete=%u modules=%u\n",(unsigned)view->complete,view->module_count);
        for(uint32_t i=0;i<view->module_count;++i)fprintf(stderr,"Atomic actual Source module=%u path=%s\n",i,view->modules[i].path?view->modules[i].path:"<native>");
    }
    CHECK(view && view->complete && view->module_count==6);
    status=xr_xir_compile_checked_write(s->source.checked,&s->packet,NULL);
    if(!atomic_source_stage(s,4,status)){CHECK(!s->packet.bytes && !s->packet.length);return false;}
    xr_xir_compile_source_result_free(&s->source);xr_compile_session_free(s->session);s->session=NULL;
    status=xr_xir_compile_checked_read(s->context,s->packet.bytes,s->packet.length,&s->checked,NULL);
    memset(s->packet.bytes,0xA5,s->packet.length);xr_xir_compile_checked_packet_free(&s->packet);
    if(!atomic_source_stage(s,5,status)){CHECK(!s->checked);return false;}
    status=xr_xir_compile_specialize(s->checked,&s->closed,NULL, NULL);
    if(!atomic_source_stage(s,6,status)){CHECK(!s->closed);return false;}
    xr_xir_compile_artifact_free(s->checked);s->checked=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(s->closed,&target, NULL,&s->lowered,NULL);
    if(!atomic_source_stage(s,7,status)){CHECK(!s->lowered);return false;}
    xr_xir_compile_artifact_free(s->closed);s->closed=NULL;atomic_source_entries(s);
    status=xr_xir_compile_emit_c(s->lowered,"p20_atomic_runtime_fixture",4194304,&s->generated);
    if(!atomic_source_stage(s,8,status)){CHECK(!s->generated.text && !s->generated.length);return false;}
    CHECK(s->generated.text && !strstr(s->generated.text,"({") && !strstr(s->generated.text,"xr_xir_vm"));return true;
}
static void atomic_source_cleanup(AtomicSourcePrepare *s) {
    xr_xir_compile_source_result_free(&s->source);xr_compile_session_free(s->session);s->session=NULL;
    xr_xir_compile_checked_packet_free(&s->packet);xr_xir_compile_artifact_free(s->checked);s->checked=NULL;
    xr_xir_compile_artifact_free(s->closed);s->closed=NULL;
    xr_xir_compile_artifact_free(s->lowered);s->lowered=NULL;xr_xir_compile_c_source_free(&s->generated);
}
#endif
