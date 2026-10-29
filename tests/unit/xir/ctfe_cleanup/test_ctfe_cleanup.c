/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ctfe_cleanup.c - Real cleanup frames on independently received Checked data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "base/xmalloc.h"
enum { PROBE_NONE,PROBE_FRAME,PROBE_PANIC,PROBE_REPLACED };
static size_t attempts,live;
static unsigned armed,phase;
static bool injected,deny_allocations;
static void *observed_malloc(size_t size) {
    ++attempts;
    if (deny_allocations || (armed && armed==phase && !injected)) {injected=true;return NULL;}
    void *pointer=xr_malloc(size);if(pointer)++live;return pointer;
}
static void observed_free(void *pointer) {if(pointer){CHECK(live);--live;}xr_free(pointer);}
#undef xr_malloc
#undef xr_free
#define xr_malloc(size) observed_malloc(size)
#define xr_free(pointer) observed_free(pointer)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_error.h"
#include "xir/xxir_panic.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_compile_memory.h"
#include "toolchain/xcompiler_session.h"

/* Observe unchanged evaluator call sites, never a production hook. Canonical
 * Source, CLI and installed consumers are verified against their own archives. */
static XrCompileResourceStatus probe_calloc(XrCompileResources *,size_t,size_t,void **,const char *);
static void probe_free(void *,const char *);
static XrXirValueStatus probe_erase(const XrXirValue *,XrXirValueAdmission *,XrXirValue *,const char *);
static XrXirValueStatus probe_panic(XrXirDomain *,const XrXirPanicPayload *,XrXirValue *);
static XrXirValueStatus probe_replaced(const XrXirValue *,XrXirType,XrXirValueAdmission *,XrXirValue *);
#define xr_compile_resources_calloc(r,n,s,o) probe_calloc(r,n,s,o,__func__)
#define xr_compile_resources_free(p) probe_free(p,__func__)
#define xr_xir_error_erase(v,a,o) probe_erase(v,a,o,__func__)
#define xr_xir_panic_info_new probe_panic
#define xr_xir_panic_info_replaced_error probe_replaced
#include "xir/xxir_ctfe.c"
#undef xr_compile_resources_calloc
#undef xr_compile_resources_free
#undef xr_xir_error_erase
#undef xr_xir_panic_info_new
#undef xr_xir_panic_info_replaced_error

typedef struct ProbeFrame {void *memory;size_t size,ordinal;} ProbeFrame;
static ProbeFrame frames[32];
static size_t frame_count,frame_ordinal,cleanup_ordinal,cleanup_bytes;
static size_t erased_requests,erase_allocations;
static uint32_t cleanup_function;
static bool observing,calibrating;
static XrCompileResourceStatus probe_calloc(XrCompileResources *resources,size_t count,size_t size,void **output,const char *caller) {
    bool frame=observing && !strcmp(caller,"ctfe_call_frame_new");
    if(frame)++frame_ordinal;
    unsigned previous=phase;
    if(frame && armed==PROBE_FRAME && frame_ordinal==cleanup_ordinal) {
        CHECK(count==1 && size==cleanup_bytes);phase=PROBE_FRAME;
    }
    XrCompileResourceStatus status=xr_compile_resources_calloc(resources,count,size,output);phase=previous;
    if(frame && calibrating && status==XR_COMPILE_RESOURCE_OK) {
        CHECK(frame_count<sizeof(frames)/sizeof(frames[0]));frames[frame_count++]=(ProbeFrame){*output,size,frame_ordinal};
    }
    return status;
}
static void probe_free(void *memory,const char *caller) {
    if(memory && calibrating && !strcmp(caller,"ctfe_call_frame_free")) {
        const CtfeCallFrame *frame=memory;
        if(frame->cleanup && frame->function==cleanup_function) {
            CHECK(frame->parent && frame->execution->artifact->module.declarations->functions[frame->function].cleanup_owner==frame->parent->function+1);
            CHECK(!cleanup_ordinal);
            for(size_t i=0;i<frame_count;++i)if(frames[i].memory==memory) {cleanup_ordinal=frames[i].ordinal;cleanup_bytes=frames[i].size;}
            CHECK(cleanup_ordinal && cleanup_bytes==frame->bytes);
        }
    }
    xr_compile_resources_free(memory);
}
static bool old_error(const XrXirValue *value) {
    XrXirEnumBorrow concrete={0};
    if(xr_xir_enum_borrow(value,&concrete)!=XR_XIR_VALUE_OK || concrete.field_count!=2) return false;
    const char *bytes=NULL;size_t length=0;const char expected[]="old \303\251\000\347\225\214";
    return concrete.fields[1].type==XR_XIR_I64 && concrete.fields[1].payload==23 &&
        xr_xir_string_view(&concrete.fields[0],&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length);
}
static XrXirValueStatus probe_erase(const XrXirValue *value,XrXirValueAdmission *admission,XrXirValue *output,const char *caller) {
    bool watched=calibrating && !strcmp(caller,"ctfe_cleanup_panic_escape") && old_error(value);
    size_t before=attempts;XrXirValueStatus status=xr_xir_error_erase(value,admission,output);
    if(watched) {++erased_requests;erase_allocations+=attempts-before;}
    return status;
}
static XrXirValueStatus probe_panic(XrXirDomain *domain,const XrXirPanicPayload *panic,XrXirValue *output) {
    unsigned previous=phase;
    if(observing && panic->detail.code==446 && panic->error.type==XR_XIR_ERROR && panic->replaced_error.type==XR_XIR_ERROR)phase=PROBE_PANIC;
    XrXirValueStatus status=xr_xir_panic_info_new(domain,panic,output);phase=previous;return status;
}
static XrXirValueStatus probe_replaced(const XrXirValue *info,XrXirType type,XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirFaultDetail detail={0};unsigned previous=phase;
    if(observing && xr_xir_panic_info_detail(info,&detail) && detail.code==446)phase=PROBE_REPLACED;
    XrXirValueStatus status=xr_xir_panic_info_replaced_error(info,type,admission,output);phase=previous;return status;
}

static XrXirCompileContext context_new(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);return context;
}
static uint64_t held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirArtifact *receive(const XrXirCompileContext *receiver,const char *path,uint8_t identity[32]) {
    FILE *file=fopen(path,"rb");CHECK(file);
    char text[16384];size_t length=fread(text,1,sizeof(text),file);
    CHECK(length && length<sizeof(text) && feof(file) && !ferror(file) && fclose(file)==0);
    XrXirCompileContext producer=context_new();uint64_t empty=held(&producer);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(producer.resources,&session)==XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"ctfe-cleanup",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&producer,XR_CTFE_CLEANUP_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source %u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);memset(text,'!',sizeof(text));xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_resources_release(producer.resources);producer.resources=NULL;
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(receiver,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,'!',packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(empty>0 && xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);return lowered;
}
static XrXirCtfeCallRequest selected(const XrXirArtifact *artifact,const uint8_t identity[32],const char *name) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t found=UINT32_MAX;
    for(uint32_t i=0;i<module->function_count;++i)if(module->functions[i].name_length==strlen(name) &&
        !memcmp(module->functions[i].name,name,strlen(name))) {CHECK(found==UINT32_MAX);found=i;}
    CHECK(found!=UINT32_MAX);
    XrXirCtfeCallRequest request={.evaluation={.artifact=artifact,.function=found,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
        .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(request.evaluation.checked_identity,identity,32);return request;
}
static void number(const XrXirArtifact *artifact,const uint8_t identity[32],const char *name,int64_t expected) {
    XrXirCtfeCallRequest request=selected(artifact,identity,name);XrXirValue value={0};
    XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&value);
    if(status!=XR_XIR_CTFE_OK)fprintf(stderr,"CTFE %s status%u\n",name,(unsigned)status);
    CHECK(status==XR_XIR_CTFE_OK && value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);
}
static void failures(const XrXirCompileContext *context,const XrXirArtifact *artifact,const uint8_t identity[32]) {
    const char *names[]={"ctfeDouble","ctfeDouble446","returned"};
    uint64_t before=held(context);size_t blocks=live;
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        XrXirCtfeCallRequest request=selected(artifact,identity,names[i]);XrXirValue output={0};
        XrXirCtfeStatus expected=i<2?XR_XIR_CTFE_CLEANUP_PANIC:XR_XIR_CTFE_DIVIDE_BY_ZERO;
        CHECK(xr_xir_compile_ctfe_calls(&request,&output)==expected);
        CHECK(!output.type && !output.reserved && !output.payload && held(context)==before && live==blocks);
        number(artifact,identity,"ctfeOldGraph",42);CHECK(held(context)==before && live==blocks);
    }
}
static void allocation_failures(const XrXirCompileContext *context,const XrXirArtifact *artifact,const uint8_t identity[32]) {
    XrXirCtfeCallRequest request=selected(artifact,identity,"ctfeBothMessage");XrXirValue output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    XrXirCtfeCallRequest parent=selected(artifact,identity,"bothFailure");const XrXirFunction *body=&module->functions[parent.evaluation.function];
    cleanup_function=UINT32_MAX;
    for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_CLEANUP_REGISTER) {
        CHECK(cleanup_function==UINT32_MAX);cleanup_function=(uint32_t)body->instructions[i].immediate;
    }
    CHECK(cleanup_function!=UINT32_MAX);observing=calibrating=true;
    CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_OK);calibrating=false;
    CHECK(cleanup_ordinal && cleanup_bytes && erased_requests);xr_xir_value_drop(&output);uint64_t before=held(context);size_t blocks=live;
    for(unsigned mode=PROBE_FRAME;mode<=PROBE_REPLACED;++mode) {
        armed=mode;phase=0;frame_ordinal=0;injected=false;
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&output);armed=0;
        CHECK(injected && status==XR_XIR_CTFE_OUT_OF_MEMORY && !output.type && !output.reserved && !output.payload);
        CHECK(held(context)==before && live==blocks);
        CHECK(xr_xir_compile_ctfe_calls(&request,&output)==XR_XIR_CTFE_OK);xr_xir_value_drop(&output);
        CHECK(held(context)==before && live==blocks);
        fprintf(stderr,"CTFE semantic allocation boundary%u: OOM, unchanged output, rollback and fresh same-owner retry\n",mode);
    }
    fprintf(stderr,"Actual replaced Error erasure: calls=%zu physical allocation requests=%zu (observation, no added refusal policy)\n",erased_requests,erase_allocations);
    observing=false;
}
int main(void) {
    XrXirCompileContext context=context_new();uint64_t empty=held(&context);uint8_t identity[32]={0};
    XrXirArtifact *artifact=receive(&context,XR_CTFE_CLEANUP_ORIGINAL,identity);
    number(artifact,identity,"answer",0);xr_xir_compile_artifact_free(artifact);CHECK(held(&context)==empty);
    artifact=receive(&context,XR_CTFE_CLEANUP_FIXTURE,identity);
    const char *names[]={"ctfeOldGraph","ctfePendingLocal","ctfeLexicalOutside","ctfeLexicalInside","ctfeRemaining","ctfeBusinessLocal"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)number(artifact,identity,names[i],42);
    failures(&context,artifact,identity);allocation_failures(&context,artifact,identity);
    XrXirCtfeCallRequest request=selected(artifact,identity,"ctfeBothMessage");XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_OK);xr_xir_compile_artifact_free(artifact);
    xr_compile_resources_release(context.resources);context.resources=NULL;
    const char expected[]="try! failed: Failure.Bad(\"new \303\251\000\347\225\214\", 31)";
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value,&bytes,&length));
    CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    size_t calls=attempts;deny_allocations=true;xr_xir_value_drop(&value);deny_allocations=false;
    CHECK(empty>0 && calls==attempts && !live);
    puts("CTFE cleanup: lexical handlers, preserved pending panic, complete Error graphs, double failure, retry and retired owned message PASS");return 0;
}
