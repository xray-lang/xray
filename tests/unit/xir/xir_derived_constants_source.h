/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_constants_source.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_CONSTANTS_SOURCE_H
#define XIR_DERIVED_CONSTANTS_SOURCE_H
#include "xir_derived_constants_runtime.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_vm.h"
static const char constant_table_source[]=
    "fn primes()->(i16,i16,i16,i16){return (2,3,5,7)}\n"
    "export fn table()->(i16,i16,i16,i16){return comptime primes()}\n"
    "export fn checksum()->i64{const (a,b,c,d)=table();return (a as i64)+(b as i64)*10+(c as i64)*100+(d as i64)*1000}\n";
static XrXirStatus constant_source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="derived-constant-table";size_t length=strlen(text);
    CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);
        return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));
    return status;
}
static uint32_t constant_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length==length && !memcmp(body->name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void constant_recipe(const XrXirCompileContext *context,const XrXirModule *module) {
    const XrXirFunction *body=&module->functions[constant_function(module,"table")];
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(context,module->types,body->result,&description)==XR_XIR_OK);
    CHECK(description.kind==XR_XIR_TYPE_TUPLE && description.field_count==4);
    for(uint32_t f=0;f<description.field_count;++f) {
        XrXirFieldDescription field={0};
        CHECK(xr_xir_compile_description_field(context,&description,f,&field)==XR_XIR_OK);
        CHECK(field.resolved && field.type==XR_XIR_I16 && field.template_type==XR_XIR_I16 && !field.flags);
    }
    unsigned tuple=0,constants=0;const int64_t expected[]={2,3,5,7};
    for(uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        tuple+=op->op==XR_XIR_TUPLE_NEW;
        if(op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I16) {
            CHECK(constants<4 && op->immediate==expected[constants]);++constants;
        }
    }
    CHECK(tuple==1 && constants==4);
}
static XrXirArtifact *constant_lower(XrXirCompileContext *caller) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&constant_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=constant_source(&context,constant_table_source,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"constant table Source status=%u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    constant_recipe(&context,xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    constant_recipe(&context,xr_xir_compile_artifact_module(received));
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    *caller=context;return lowered;
}
#endif // XIR_DERIVED_CONSTANTS_SOURCE_H
