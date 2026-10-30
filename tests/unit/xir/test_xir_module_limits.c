/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_limits.c - Separate module inventories on one receiving owner
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"module limits %d: %s\n",__LINE__,#c);exit(1);}}while(0)

#if defined(XR_MODULE_LIMITS_NATIVE)
extern const XrXirProgramSpec source_artifact_program;
int main(void) {
    const XrXirProgramSpec *spec=&source_artifact_program;uint32_t answer=UINT32_MAX;
    for(uint32_t f=0;f<spec->entry_count;++f)if(spec->declarations->functions[f].exported) {
        CHECK(answer==UINT32_MAX && spec->entries[f].result==XR_XIR_I64 && !spec->entries[f].parameter_count);answer=f;
    }
    CHECK(answer!=UINT32_MAX);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,spec,&program)==XR_XIR_OK&&program);xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,answer,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    puts("Original 300-function Source generated C: retired Program owner, exported answer42 PASS");return 0;
}
#else
#include "xray_source_entry.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
/* The fault provider only counts actual physical blocks; it keeps no allocation
 * registry or capacity limit. Canonical CLI/SDK consumers are separate. */
static size_t attempts,fail_at=SIZE_MAX,live;
static void *physical_malloc(size_t bytes) {if(attempts++==fail_at)return NULL;void *p=xr_malloc(bytes);if(p)++live;return p;}
static void physical_free(void *p) {if(p){CHECK(live);--live;}xr_free(p);}
#undef xr_malloc
#undef xr_free
#define xr_malloc physical_malloc
#define xr_free physical_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
/* This distinct provider exposes the unchanged codec's physical field cursor
 * for exact wire mutations and per-module inventory measurements. */
#include "xir/xxir.c"

static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats value={0};CHECK(xr_compile_resources_stats(resources,&value)==XR_COMPILE_RESOURCE_OK);return value;
}
static size_t original_source(char *text,size_t capacity) {
    size_t used=0;
    for(unsigned i=0;i<300;++i){int n=snprintf(text+used,capacity-used,"fn pad%u(value:i64=1)->i64{return value}\n",i);CHECK(n>0&&(size_t)n<capacity-used);used+=(size_t)n;}
    const char tail[]="fn add(left:i64=40,right:i64=2)->i64{return left+right}\nexport fn answer()->i64{return add()}\n";
    CHECK(sizeof(tail)<=capacity-used);memcpy(text+used,tail,sizeof(tail));used+=sizeof(tail)-1;
    uint8_t digest[32];xr_sha256((const uint8_t *)text,used,digest);
    static const uint8_t expected[32]={0x19,0xa1,0x95,0x06,0xef,0xd8,0xc6,0xa5,0xb3,0x74,0xf6,0xa0,0xaa,0xdc,0x5b,0xf5,0x43,0x8b,0x87,0xc5,0x7f,0xc8,0x44,0xc6,0x37,0x8d,0x28,0xde,0xef,0x72,0x62,0xd5};
    CHECK(!memcmp(digest,expected,sizeof(expected)));return used;
}
static XrXirArtifact *source_closed(const XrXirCompileContext *context,const char *text,size_t length) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"module-limits",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,XR_PUBLIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source status%u %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked);xr_compile_session_free(session);XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL, NULL)==XR_XIR_OK&&closed);
    xr_xir_compile_source_result_free(&result);xr_xir_compile_source_failure_free(&failure);return closed;
}
static void reject_packet(const XrXirCompileContext *context,const XrXirCheckedPacket *packet,XrXirStatus expected) {
    uint64_t before=stats(context->resources).held_bytes;XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,packet->bytes,packet->length,&output,&diagnostic);
    if(status!=expected)fprintf(stderr,"packet status%u expected%u\n",(unsigned)status,(unsigned)expected);
    CHECK(status==expected&&!output&&diagnostic.status==expected&&stats(context->resources).held_bytes==before);
}
static CheckedCursor cursor(const XrXirCompileContext *context,size_t position) {
    return (CheckedCursor){NULL,NULL,position,SIZE_MAX,context->limits,*context,XR_XIR_OK,false,NULL,0};
}
/* Locate fields by the same completed typed producer, never by scanning for a
 * repeated byte pattern. No allocation or mutation occurs on this size pass. */
static void prefix(CheckedCursor *c,const XrXirArtifact *artifact) {
    XrXirModule m=artifact->module;XrXirConstruction *construction=artifact->construction;
    (void)checked_u32(c,m.linkage_kind);(void)checked_count(c,m.function_count,&c->structural.functions,28);
    (void)checked_u32(c,m.declarations!=NULL);
    for(uint32_t f=0;f<m.function_count;++f){XrXirFunction body=m.functions[f];checked_function(c,&body);}
    if(m.declarations){XrXirDeclarations declarations=*m.declarations;checked_declarations(c,&declarations,m.function_count);}
    checked_generics(c,&m);checked_types(c,&m);checked_construction(c,&m,&construction);checked_defaults(c,&m);CHECK(c->status==XR_XIR_OK);
}
static XrXirCompileLimits used(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    CheckedCursor c=cursor(context,0);XrXirModule m=artifact->module;XrXirConstruction *construction=artifact->construction;
    checked_module_fields(&c,&m,&construction,XR_XIR_EVIDENCE_INSTANCE);CHECK(c.status==XR_XIR_OK);
    return (XrXirCompileLimits){context->limits.functions-c.structural.functions,context->limits.parameters-c.structural.parameters,
        context->limits.blocks-c.structural.blocks,context->limits.instructions-c.structural.instructions,context->limits.frame_bytes};
}
static void put_u32(uint8_t *p,uint32_t value) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i));}
static void malformed(const XrXirCompileContext *context,XrXirArtifact *closed,XrXirCheckedPacket *packet) {
    CheckedCursor locate=cursor(context,64);prefix(&locate,closed);CHECK(closed->module.provenance->kind==XR_XIR_EVIDENCE_INSTANCE);
    size_t nested=locate.position+4;locate=cursor(context,nested);prefix(&locate,closed->module.provenance->source);size_t leaf=locate.position;
    CHECK(packet->bytes[leaf]==XR_XIR_EVIDENCE_TEMPLATE);
    put_u32(packet->bytes+leaf,XR_XIR_EVIDENCE_INSTANCE);checked_digest(packet->bytes,packet->length,packet->bytes+32,NULL);
    reject_packet(context,packet,XR_XIR_BAD_STRUCTURE);
    put_u32(packet->bytes+leaf,XR_XIR_EVIDENCE_TEMPLATE);checked_digest(packet->bytes,packet->length,packet->bytes+32,NULL);
    uint32_t functions=closed->module.provenance->source->module.function_count;
    XrXirCompileContext tight=*context;tight.limits.functions=functions;
    put_u32(packet->bytes+nested+4,functions+1);checked_digest(packet->bytes,packet->length,packet->bytes+32,NULL);
    reject_packet(&tight,packet,XR_XIR_BUDGET);
    put_u32(packet->bytes+nested+4,UINT32_MAX);checked_digest(packet->bytes,packet->length,packet->bytes+32,NULL);
    reject_packet(context,packet,XR_XIR_BAD_STRUCTURE);
    put_u32(packet->bytes+nested+4,functions);checked_digest(packet->bytes,packet->length,packet->bytes+32,NULL);
    XrXirCheckedPacket truncated=*packet;truncated.length-=4;
    uint8_t header[64];memcpy(header,packet->bytes,sizeof(header));
    uint64_t payload=truncated.length-64;for(unsigned i=0;i<8;++i)packet->bytes[24+i]=(uint8_t)(payload>>(8*i));
    checked_digest(packet->bytes,truncated.length,packet->bytes+32,NULL);reject_packet(context,&truncated,XR_XIR_BAD_STRUCTURE);memcpy(packet->bytes,header,sizeof(header));
    XrXirModule m=closed->module;XrXirProvenance p=*m.provenance;XrXirArtifact source=*p.source;m.provenance=&p;p.source=&source;
    uint64_t held=stats(context->resources).held_bytes;source.module.provenance=&p;
    CHECK(xr_xir_compile_verify_v2(context,&m,closed->construction,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirProvenance template=*closed->module.provenance->source->module.provenance;template.source=&source;source.module.provenance=&template;
    CHECK(xr_xir_compile_verify_v2(context,&m,closed->construction,NULL)==XR_XIR_BAD_STRUCTURE&&stats(context->resources).held_bytes==held);
    puts("Nested closed/cycle and signed packet nested-kind/physical-tail controls reject without owner changes");
}
typedef struct PublicControl {size_t polls;bool cancelled;} PublicControl;
static int public_cancel(const void *opaque) {PublicControl *control=(PublicControl *)opaque;++control->polls;return control->cancelled;}
static void child_limit(const XrXirCompileContext *context) {
    const char text[]="fn unused<T>(value:T)->T{return value}\nexport fn answer()->i64{return 42}\n";
    uint64_t held=stats(context->resources).held_bytes;XrXirArtifact *closed=source_closed(context,text,sizeof(text)-1);
    CHECK(closed->module.function_count<closed->module.provenance->source->module.function_count);
    XrXirCompileContext tight=*context;tight.limits.functions=closed->module.function_count;
    CHECK(xr_xir_compile_artifact_context(closed)->limits.functions>tight.limits.functions);
    CHECK(xr_xir_compile_verify_v2(&tight,&closed->module,closed->construction,NULL)==XR_XIR_BUDGET);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);reject_packet(&tight,&packet,XR_XIR_BUDGET);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(closed);CHECK(stats(context->resources).held_bytes==held);
    puts("Actual smaller Closed inventory cannot widen current caller limits for its larger Source owner");
}
static void public_answer(char *text,size_t length) {
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    PublicControl control={0};options.cancel_poll=public_cancel;options.cancel_context=&control;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"original302",text,length}};XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
    size_t polls=control.polls;CHECK(polls);control.cancelled=true;memset(text,'!',length);
    XrSourceExecutionOptions execution_options=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&execution_options,&execution)==XR_SOURCE_OK);XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);invocation.name="answer";invocation.name_length=6;
    XrSourceCall *call=NULL;CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&runtime,&call)==XR_SOURCE_OK&&call);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);XrSourceEvent event={0};
    for(unsigned i=0;i<4096;++i){CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK&&result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK&&!call);int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK&&value==42);xray_source_result_free(result);CHECK(control.polls==polls);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==3&&!strcmp(argv[1],"--emit")));char text[24000];size_t length=original_source(text,sizeof(text));
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t empty=stats(context.resources).held_bytes;
    XrXirArtifact *closed=source_closed(&context,text,length);CHECK(closed->module.function_count==606&&closed->module.provenance->source->module.function_count==606);
    if(argc==3){XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};XrXirArtifact *lowered=NULL;CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"source_artifact",UINT64_C(64)<<20,&code)==XR_XIR_OK);FILE *f=fopen(argv[2],"wb");CHECK(f&&fwrite(code.text,1,code.length,f)==code.length&&!fclose(f));xr_xir_compile_c_source_free(&code);xr_xir_compile_artifact_free(lowered);}
    else {
        child_limit(&context);
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);malformed(&context,closed,&packet);
        XrXirCompileLimits a=used(&context,closed),b=used(&context,closed->module.provenance->source);XrXirCompileLimits exact=context.limits;
        exact.functions=a.functions>b.functions?a.functions:b.functions;exact.parameters=a.parameters>b.parameters?a.parameters:b.parameters;
        exact.blocks=a.blocks>b.blocks?a.blocks:b.blocks;exact.instructions=a.instructions>b.instructions?a.instructions:b.instructions;
        printf("Actual module inventories functions/parameters/blocks/instructions: Closed %u/%u/%u/%u Source %u/%u/%u/%u\n",a.functions,a.parameters,a.blocks,a.instructions,b.functions,b.parameters,b.blocks,b.instructions);
        XrXirCompileContext receiver={NULL,exact};CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);uint64_t baseline=stats(receiver.resources).held_bytes;
        XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK&&received);xr_xir_compile_artifact_free(received);received=NULL;
        uint32_t *axes[]={&receiver.limits.functions,&receiver.limits.parameters,&receiver.limits.blocks,&receiver.limits.instructions};
        for(unsigned i=0;i<4;++i){CHECK(*axes[i]);--*axes[i];reject_packet(&receiver,&packet,XR_XIR_BUDGET);++*axes[i];}
        fail_at=attempts;reject_packet(&receiver,&packet,XR_XIR_OUT_OF_MEMORY);fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK&&received);
        xr_xir_compile_artifact_free(closed);closed=NULL;xr_xir_compile_checked_packet_free(&packet);CHECK(stats(context.resources).held_bytes==empty);xr_compile_resources_release(context.resources);context.resources=NULL;
        CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
        CHECK(stats(receiver.resources).held_bytes==baseline);xr_compile_resources_release(receiver.resources);
        public_answer(text,length);CHECK(!live);puts("Original 300-function Source: separate606/606 module caps, packet controls, cross-owner retirement and exported VM42 PASS");
    }
    xr_xir_compile_artifact_free(closed);if(context.resources){CHECK(stats(context.resources).held_bytes==empty);xr_compile_resources_release(context.resources);}CHECK(!live);return 0;
}
#endif
