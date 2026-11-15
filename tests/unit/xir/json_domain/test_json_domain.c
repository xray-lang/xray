/* Folded JSON: exact static domain, real Source/Checked cold ownership. */
#include "xir/xxir_json_domain.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_types.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "xir/xxir_value.h"
#include "hir/xhir_literal.h"
#include "hir/xhir_conversion.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_source_compile_owner.h"
static const char source[]="struct User {name:string;age:i64}\n"
    "var user=User{name:\"Alice\",age:30}\n"
    "var data:JSON.Object={\"name\":\"Alice\",\"age\":30,\"ok\":true,\"ratio\":1.5,\"nil\":null}\n"
    "data[\"traceId\"]=\"req-1\"\n"
    "var direct:Map<string,JSON.Value>=data\nvar back:JSON.Object=direct\n"
    "var present:JSON.Value=null\nvar absent:JSON.Value?=null\nvar some:JSON.Value?=present\n"
    "var path:JSON.Path=[\"user\",0]\n";
static void zero(void) {CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);}
static XrXirStatus check_text(const char *text,size_t length,XrCompileRequestControl *control,XrXirSourceResult *output) {
    XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(made);
    context.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;
    made=xr_compile_session_new(context.resources,&session);
    if(made==XR_COMPILE_RESOURCE_OK && control)made=xr_compile_session_bind_request(session,control);
    XrXirStatus status=xir_compile_resource_status(made);
    if(status==XR_XIR_OK) {
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"folded-json-domain",NULL};
        XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
        XrXirSourceDiagnostic diagnostic={0};
        status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){NULL,text,length},output,&diagnostic,NULL);
        CHECK(diagnostic.status==status);
        if(status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_CANCELLED)
            fprintf(stderr,"JSON source status=%u line=%d column=%d: %s\n",(unsigned)status,
                diagnostic.line,diagnostic.column,diagnostic.message);
    }
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);return status;
}
static void facts(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module && module->stage==XR_XIR_CHECKED);
    uint32_t objects=0,nullable=0,inject=0,nulls=0,none=0,some=0;
    for(uint32_t t=0;t<module->types->count;++t) {
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        if(xr_xir_json_object_type(module->types,type))++objects;
        if(xr_xir_type_is_nullable(module->types,type) && xr_xir_nullable_element(module->types,type)==XR_XIR_JSON_VALUE)++nullable;
    }
    CHECK(objects==1 && nullable==1);
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        for(uint32_t i=0;i<fn->instruction_count;++i) {
            const XrXirInstruction *op=&fn->instructions[i];
            if(op->op==XR_XIR_JSON_NULL){CHECK(op->type==XR_XIR_JSON_VALUE);++nulls;}
            if(op->op==XR_XIR_JSON_VALUE_INJECT) {
                CHECK(op->type==XR_XIR_JSON_VALUE && xr_xir_json_implicit_scalar(xr_xir_operand_type(fn,op->args[0])));++inject;
            }
            if(op->op==XR_XIR_NULLABLE_NONE && xr_xir_nullable_element(module->types,op->type)==XR_XIR_JSON_VALUE)++none;
            if(op->op==XR_XIR_NULLABLE_SOME && xr_xir_nullable_element(module->types,op->type)==XR_XIR_JSON_VALUE)++some;
        }
    }
    CHECK(inject>=5 && nulls>=2 && none>=1 && some>=1);
    XrXirDiagnostic diagnostic={0};CHECK(xr_xir_compile_artifact_verify(artifact,&diagnostic)==XR_XIR_OK);
}
static void packet_failures(const XrXirArtifact *producer) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCheckedPacket measured={0};XrXirDiagnostic diagnostic={0};
    source_fixture_compile_attempts=0;
    CHECK(xr_xir_compile_checked_write(producer,&measured,&diagnostic)==XR_XIR_OK);
    size_t writer_sites=source_fixture_compile_attempts;CHECK(writer_sites);
    xr_xir_compile_checked_packet_free(&measured);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    for(size_t p=0;p<writer_sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p;source_fixture_compile_injected=false;
        XrXirCheckedPacket refused={0};
        CHECK(xr_xir_compile_checked_write(producer,&refused,&diagnostic)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !refused.bytes && !refused.length);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer,&packet,&diagnostic)==XR_XIR_OK);
    live=source_fixture_compile_live;bytes=source_fixture_compile_bytes;
    size_t reader_sites=0;
    for(size_t p=0;p<=reader_sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
        XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
        XrXirStatus status=xir_compile_resource_status(made);XrXirArtifact *received=NULL;
        if(status==XR_XIR_OK) {
            context.limits=xr_xir_compile_default_limits();
            status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,&diagnostic);
        }
        if(!p) {CHECK(status==XR_XIR_OK);reader_sites=source_fixture_compile_attempts;CHECK(reader_sites);facts(received);}
        else {CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !received);}
        xr_xir_compile_artifact_free(received);if(context.resources)xr_compile_resources_release(context.resources);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    xr_xir_compile_checked_packet_free(&packet);
    printf("Folded JSON packet FI: writer=%zu reader=%zu\n",writer_sites,reader_sites);
}
typedef struct JsonColdCancel {size_t polls,after;} JsonColdCancel;
static bool json_cold_cancel(const void *opaque) {
    JsonColdCancel *state=(JsonColdCancel *)opaque;return ++state->polls>=state->after;
}
static void packet_controls(const XrXirArtifact *producer) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    JsonColdCancel state={0,SIZE_MAX};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=json_cold_cancel,.cancel_context=&state};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirCheckedPacket packet={0};CHECK(xir_checked_write(producer,&packet,NULL,&control)==XR_XIR_OK);
    size_t writes=state.polls;xr_xir_compile_checked_packet_free(&packet);CHECK(writes);
    for(size_t point=1;point<=writes;++point) {
        state=(JsonColdCancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_checked_write(producer,&packet,NULL,&control)==XR_XIR_CANCELLED && !packet.bytes && !packet.length);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    size_t held=source_fixture_compile_live,held_bytes=source_fixture_compile_bytes;
    state=(JsonColdCancel){0,SIZE_MAX};control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *received=NULL;CHECK(xir_checked_read(&context,packet.bytes,packet.length,&received,NULL,&control)==XR_XIR_OK);
    size_t reads=state.polls;facts(received);xr_xir_compile_artifact_free(received);received=NULL;CHECK(reads);
    for(size_t point=1;point<=reads;++point) {
        state=(JsonColdCancel){0,point};control=(XrCompileRequestControl){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_checked_read(&context,packet.bytes,packet.length,&received,NULL,&control)==XR_XIR_CANCELLED && !received);
        CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==held_bytes);
    }
    xr_compile_resources_release(context.resources);xr_xir_compile_checked_packet_free(&packet);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    printf("Folded JSON packet control: writer=%zu reader=%zu cancellation prefixes\n",writes,reads);
}
static void owned_receiving(void) {
    char *buffer=malloc(sizeof(source)-1);CHECK(buffer);memcpy(buffer,source,sizeof(source)-1);
    XrXirSourceResult result={0};CHECK(check_text(buffer,sizeof(source)-1,NULL,&result)==XR_XIR_OK);
    memset(buffer,0xa5,sizeof(source)-1);free(buffer);facts(result.checked);packet_failures(result.checked);packet_controls(result.checked);
    XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,&diagnostic)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirArtifact *received=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,&diagnostic)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);facts(received);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_lower(received,&target,NULL,&lowered,&diagnostic)!=XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(received);zero();
}
typedef struct Cancel {size_t polls,trigger;} Cancel;
static bool cancel(const void *opaque) {Cancel *state=(Cancel *)opaque;++state->polls;return state->trigger && state->polls>=state->trigger;}
static void failures(void) {
    size_t sites=0;
    for(size_t p=0;p<=sites;++p) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=p?p-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirSourceResult result={0};XrXirStatus status=check_text(source,sizeof(source)-1,NULL,&result);
        if(!p) {CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);facts(result.checked);}
        else {CHECK(source_fixture_compile_injected && status==XR_XIR_OUT_OF_MEMORY);CHECK(!result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    size_t polls=0;
    for(size_t p=0;p<=polls;++p) {
        Cancel state={0,p};XrCompileRequestControl control={0};
        CHECK(xr_compile_request_control_init(&(XrCompileRequestOptions){.cancel_poll=cancel,.cancel_context=&state},&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult result={0};XrXirStatus status=check_text(source,sizeof(source)-1,&control,&result);
        if(!p) {CHECK(status==XR_XIR_OK);polls=state.polls;CHECK(polls);}
        else {CHECK(status==XR_XIR_CANCELLED);CHECK(!result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);zero();
    }
    printf("Folded JSON: FI=%zu cancel-prefixes=%zu physical=0/0\n",sites,polls);
}
static XrXirStatus check_script(const XrXirCompileContext *compile,const char *logical,const char *text,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrCompileRequestControl *control,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;XrCompileResourceStatus made=xr_compile_session_new(compile->resources,&session);
    if(made!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(made);
    if(control)made=xr_compile_session_bind_request(session,control);
    if(made!=XR_COMPILE_RESOURCE_OK){xr_compile_session_free(session);return xir_compile_resource_status(made);}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,JSON_LIBRARY_ROOT};
    char path[2048];int length=snprintf(path,sizeof(path),"%s/%s",JSON_LIBRARY_ROOT,logical);
    CHECK(length>0 && (size_t)length<sizeof(path));
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,
        .context=compile,.linkage_kind=kind,.libraries=catalog};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&(XrXirSourceText){logical,text,strlen(text)},output,&diagnostic,NULL);
    if(status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_CANCELLED)
        fprintf(stderr,"JSON library %s: %u %s\n",logical,(unsigned)status,diagnostic.message);
    xr_compile_session_free(session);return status;
}
#include "json_catalog_controls.inc.c"
static void cold_catalog(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext producer={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&producer.resources)==XR_COMPILE_RESOURCE_OK);
    const char text[]="export fn wrap(value:JSON.Value)->JSON.Object{return {\"value\":value}}\n"
        "export fn empty()->JSON.Value{return null}\n";
    XrXirSourceResult source_result={0};CHECK(check_script(&producer,"library.xr",text,XR_XIR_LIBRARY,NULL,NULL,&source_result)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source_result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source_result);xr_compile_resources_release(producer.resources);
    XrXirCompileContext receiving={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&receiving.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,JSON_LIBRARY_ROOT},"library.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    json_catalog_failures(&input);
    XrXirStatus catalog_status=xr_xir_compile_library_catalog_new_v2(&receiving,&input,1,&catalog);
    if(catalog_status!=XR_XIR_OK)fprintf(stderr,"JSON cold Catalog status=%u\n",(unsigned)catalog_status);
    CHECK(catalog_status==XR_XIR_OK);
    xr_compile_resources_release(receiving.resources);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));
    /* Catalog and importing session share the actual receiving ledger. The
     * consumer artifact owns its own lifetime lease when that Catalog retires. */
    const XrXirCompileContext *consumer=xr_xir_compile_library_catalog_context(catalog);
    CHECK(consumer && consumer->resources);
    const char body[]="import {wrap,empty} from \"./library\"\nclass Map{}\n"
        "var object:JSON.Object=wrap(\"payload\")\nvar value:JSON.Value=empty()\n";
    json_consumer_failures(catalog,body);
    CHECK(check_script(consumer,"consumer.xr",body,XR_XIR_PROGRAM,catalog,NULL,&source_result)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_artifact_verify(source_result.checked,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source_result);zero();
}
static void canonical(void) {
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_JSON_VALUE},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_STRING,.result=XR_XIR_JSON_VALUE}};
    XrXirTypes types={.nodes=nodes,.count=2};XrXirType array=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,object=(XrXirType)(array+1);
    CHECK(XR_XIR_JSON_ARM_COUNT==7 && xr_xir_json_array_type(&types,array) && xr_xir_json_object_type(&types,object));
    CHECK(xr_xir_json_inject_arm(&types,array)==XR_XIR_JSON_ARRAY_ARM && xr_xir_json_inject_arm(&types,object)==XR_XIR_JSON_OBJECT_ARM);
    CHECK(!xr_xir_json_implicit_scalar(array) && !xr_xir_json_implicit_scalar(object));
    CHECK(xr_xir_json_inject_arm(&types,XR_XIR_UNIT)==XR_XIR_JSON_ARM_COUNT);
    CHECK(!xr_xir_json_implicit_scalar(XR_XIR_I8) && !xr_xir_json_implicit_scalar(XR_XIR_F32));
    nodes[1].element=XR_XIR_I64;CHECK(!xr_xir_json_object_type(&types,object));nodes[1].element=XR_XIR_STRING;
    nodes[0].element=XR_XIR_I64;CHECK(!xr_xir_json_array_type(&types,array));nodes[0].element=XR_XIR_JSON_VALUE;
    nodes[0].flags=1;CHECK(!xr_xir_json_array_type(&types,array));nodes[0].flags=0;
    XrXirValue forged={XR_XIR_JSON_VALUE,0,0};CHECK(!xr_xir_value_valid(&forged));
    XrXirInstruction null={0};const char *reason=NULL;XrHirConversionOrigin origin={.node_id=1};
    CHECK(xr_hir_json_null_check(origin,true,XR_XIR_JSON_VALUE,NULL,&null,&reason)==XR_XIR_OK && null.op==XR_XIR_JSON_NULL);
    null=(XrXirInstruction){.op=XR_XIR_INVALID};
    CHECK(xr_hir_json_null_check(origin,true,XR_XIR_UNIT,NULL,&null,&reason)==XR_XIR_BAD_TYPE && null.op==XR_XIR_INVALID);
}
int main(void) {
    puts("JSON owner phase: canonical logical domain");fflush(stdout);canonical();
    puts("JSON owner phase: Source producer retirement / cold packet / packet FI and cancellation");fflush(stdout);owned_receiving();
    puts("JSON owner phase: every Source allocation and cancellation");fflush(stdout);failures();
    puts("JSON owner phase: independent cold Catalog and consumer import");fflush(stdout);cold_catalog();
    puts("Folded JSON static Source/HIR/Checked PASS; runtime and codec APIs remain unaccepted");return 0;
}
