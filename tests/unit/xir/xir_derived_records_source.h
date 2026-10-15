/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derived_records_source.h - Owned ordinary constant table receiving
 */
#ifndef XIR_DERIVED_RECORDS_SOURCE_H
#define XIR_DERIVED_RECORDS_SOURCE_H
#include "xir_derived_records_runtime.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_vm.h"
static const char derived_records_table_source[]=
    "export struct Entry{index:i8;square:i8}\n"
    "fn make()->Array<Entry>{var xs=Array<Entry>.withCapacity(4);var i:i8=0;while(i<4){xs.push(Entry{index:i,square:i*i});i=i+1};return xs}\n"
    "export fn table()->Array<Entry>{return comptime make()}\n"
    "export fn checksum()->i64{const xs=table();var i:i64=0;var encoded:i64=0;while(i<4){const x=xs[i];encoded=encoded|((x.index as i64)<<(i*16));encoded=encoded|((x.square as i64)<<(i*16+8));i=i+1};return encoded}\n";
static XrXirStatus derived_records_source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[1024],name[]="ordinary-records-oracle";size_t length=strlen(text);
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
static uint32_t derived_records_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if(body->name_length==length && !memcmp(body->name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void derived_records_recipe(const XrXirCompileContext *context,const XrXirModule *module) {
    const XrXirFunction *body=&module->functions[derived_records_function(module,"table")];XrXirTypeDescription array={0},entry={0};
    CHECK(xr_xir_compile_type_description(context,module->types,body->result,&array)==XR_XIR_OK && array.kind==XR_XIR_TYPE_ARRAY);
    CHECK(xr_xir_compile_type_description(context,module->types,array.node->element,&entry)==XR_XIR_OK);
    CHECK(entry.nominal_kind==XR_XIR_NOMINAL_STRUCT && entry.field_count==2);
    const char *names[]={"index","square"};
    for(uint32_t f=0;f<2;++f) {
        XrXirFieldDescription field={0};CHECK(xr_xir_compile_description_field(context,&entry,f,&field)==XR_XIR_OK);
        CHECK(field.resolved && field.type==XR_XIR_I8 && field.template_type==XR_XIR_I8);
        CHECK(field.name.length==strlen(names[f]) && !memcmp(field.name.bytes,names[f],field.name.length));
    }
    const int64_t expected[]={0,0,1,1,2,4,3,9};unsigned capacity=0,push=0,record=0,number=0;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        capacity+=op->op==XR_XIR_ARRAY_WITH_CAPACITY;push+=op->op==XR_XIR_ARRAY_PUSH;record+=op->op==XR_XIR_STRUCT_NEW;
        if(op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I8){CHECK(number<8 && op->immediate==expected[number]);++number;}
    }
    CHECK(capacity==1 && push==4 && record==4 && number==8);
}
static XrXirArtifact *derived_records_lower(XrXirCompileContext *caller) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&derived_records_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=derived_records_source(&context,derived_records_table_source,&source,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"constant table Source status=%u at %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    derived_records_recipe(&context,xr_xir_compile_artifact_module(source.checked));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
    derived_records_recipe(&context,xr_xir_compile_artifact_module(received));
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    *caller=context;return lowered;
}
#endif // XIR_DERIVED_RECORDS_SOURCE_H
