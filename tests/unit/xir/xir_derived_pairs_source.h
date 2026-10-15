/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_pairs_source.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_PAIRS_SOURCE_H
#define XIR_DERIVED_PAIRS_SOURCE_H
#include "xir_derived_pairs_runtime.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_vm.h"
static const char derived_pairs_table_source[]=
    "fn primes()->(i64,i64){return (2,3)}\n"
    "export fn table()->(i64,i64){return comptime primes()}\n"
    "export fn checksum()->i64{const (a,b)=table();return a+b*10}\n";
static XrXirStatus derived_pairs_source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="ordinary-pair-oracle";size_t length=strlen(text);
    CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));
    return status;
}
static uint32_t derived_pairs_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length==length && !memcmp(body->name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void derived_pairs_recipe(const XrXirCompileContext *context,const XrXirModule *module) {
    const XrXirFunction *body=&module->functions[derived_pairs_function(module,"table")];
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(context,module->types,body->result,&description)==XR_XIR_OK);
    CHECK(description.kind==XR_XIR_TYPE_TUPLE && description.field_count==2);
    for(uint32_t f=0;f<description.field_count;++f) {
        XrXirFieldDescription field={0};
        CHECK(xr_xir_compile_description_field(context,&description,f,&field)==XR_XIR_OK);
        CHECK(field.resolved && field.type==XR_XIR_I64 && field.template_type==XR_XIR_I64 && !field.flags);
    }
    unsigned tuple=0,pairs=0;const int64_t expected[]={2,3};
    for(uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        tuple+=op->op==XR_XIR_TUPLE_NEW;
        if(op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64) {
            CHECK(pairs<2 && op->immediate==expected[pairs]);++pairs;
        }
    }
    CHECK(tuple==1 && pairs==2);
}
static XrXirArtifact *derived_pairs_lower(XrXirCompileContext *caller) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&derived_pairs_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=derived_pairs_source(&context,derived_pairs_table_source,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"constant table Source status=%u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    derived_pairs_recipe(&context,xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    derived_pairs_recipe(&context,xr_xir_compile_artifact_module(received));
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    *caller=context;return lowered;
}
#endif // XIR_DERIVED_PAIRS_SOURCE_H
