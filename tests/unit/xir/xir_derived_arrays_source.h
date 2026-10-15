/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_arrays_source.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_ARRAYS_SOURCE_H
#define XIR_DERIVED_ARRAYS_SOURCE_H
#include "xir_derived_arrays_runtime.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_vm.h"
static const char derived_array_table_source[]=
    "fn make()->Array<i8?> {var xs=Array<i8?>.withCapacity(6);var i:i8=1;while(i<=4){if(i==2 || i==4){xs.push(null)}else{xs.push(i*i)};i=i+1};return xs}\n"
    "export fn table()->Array<i8?> {return comptime make()}\n"
    "export fn checksum()->i64 {const xs=table();const a=xs[0];const b=xs[1];const c=xs[2];const d=xs[3];if(b!=null || d!=null){return -1};return (a! as i64)+(c! as i64)*1000}\n";
static XrXirStatus derived_array_source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="derived-array-table";size_t length=strlen(text);
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
static uint32_t derived_array_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length==length && !memcmp(body->name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void derived_array_recipe(const XrXirCompileContext *context,const XrXirModule *module) {
    const XrXirFunction *body=&module->functions[derived_array_function(module,"table")];
    XrXirTypeDescription array={0},nullable={0};
    CHECK(xr_xir_compile_type_description(context,module->types,body->result,&array)==XR_XIR_OK);
    CHECK(array.kind==XR_XIR_TYPE_ARRAY && !array.field_count && array.node);
    CHECK(xr_xir_compile_type_description(context,module->types,array.node->element,&nullable)==XR_XIR_OK);
    CHECK(nullable.kind==XR_XIR_TYPE_NULLABLE && !nullable.field_count && nullable.node && nullable.node->element==XR_XIR_I8);
    unsigned capacity=0,push=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        XrXirOp op=body->instructions[i].op;
        CHECK(op!=XR_XIR_CALL && op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_FUNCTION_REF);
        capacity+=op==XR_XIR_ARRAY_WITH_CAPACITY;push+=op==XR_XIR_ARRAY_PUSH;
    }
    CHECK(capacity==1 && push==4);
}
static XrXirArtifact *derived_array_lower(XrXirCompileContext *caller) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&derived_array_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=derived_array_source(&context,derived_array_table_source,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"constant table Source status=%u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    derived_array_recipe(&context,xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    derived_array_recipe(&context,xr_xir_compile_artifact_module(received));
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    *caller=context;return lowered;
}
#endif // XIR_DERIVED_ARRAYS_SOURCE_H
