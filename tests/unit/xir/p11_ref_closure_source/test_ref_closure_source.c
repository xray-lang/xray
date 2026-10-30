/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ref_closure_source.c - Authentic ref callable parameters and owned queries
 *
 * KEY CONCEPT:
 *   Caller text and parser/session death precede complete query and packet
 *   receiving. Ref modes use the same Cell representation as named functions.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
static const XrCompileResourceLimits caps={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static const char combined[]=
    "export fn run()->i64{var n=2;var x=38;"
    "const first:fn(ref i64)->i64=fn(value)->i64{value+=n;return value};"
    "const second=fn(value:ref i64)->i64{value+=2;return value};first(ref x);return second(ref x)}\n";
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static XrXirStatus source(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    char bytes[4096],name[]="ref-closure-source";size_t length=strlen(text);CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
    XrCompilerSession *session=NULL;XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));return status;
}
static void query(const XrXirSourceResult *result,uint32_t expected_parameters,uint32_t expected_markers) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result->snapshot);
    CHECK(view && view->complete && syntax && syntax->declaration_count==view->declaration_count);
    uint32_t parameters=0,markers=0;
    for (uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *decl=&view->declarations[i];
        if (decl->kind!=XR_XIR_SOURCE_PARAMETER || strcmp(decl->name,"value")) continue;
        CHECK(decl->type.known && decl->type.type==XR_XIR_I64 && decl->mutable && decl->parent);
        const XrXirSourceDeclaration *parent=&view->declarations[decl->parent-1];
        CHECK(parent->parameter_count==1 && parent->parameters[0].known && parent->parameters[0].type==XR_XIR_I64);
        for (uint32_t m=0;m<syntax->marker_count;++m) {
            const XrXirSourceMarker *marker=&syntax->markers[m];
            if (marker->declaration!=decl->id || marker->role!=XR_XIR_SOURCE_MARKER_REF) continue;
            CHECK(marker->range.line>0 && marker->range.column>0 && marker->range.end_column-marker->range.column==3);++markers;
        }
        ++parameters;
    }
    CHECK(parameters==expected_parameters && markers==expected_markers);
}
static void signatures(const XrXirModule *module,uint32_t expected) {
    uint32_t found=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length<8 || memcmp(body->name,"$closure",8)) continue;
        CHECK(body->parameter_count && xr_xir_type_is_cell(module->types,body->parameters[body->parameter_count-1]));
        CHECK(xr_xir_cell_element(module->types,body->parameters[body->parameter_count-1])==XR_XIR_I64);++found;
    }
    CHECK(found==expected);
    bool callable=false;
    for (uint32_t i=0;i<module->types->count;++i) {
        const XrXirTypeNode *node=&module->types->nodes[i];
        if (node->kind!=XR_XIR_TYPE_CALLABLE || node->parameter_count!=1 || node->parameters[0].mode!=XR_PARAM_REF) continue;
        CHECK(xr_xir_type_is_cell(module->types,node->parameters[0].type));callable=true;
    }
    CHECK(callable);
}
static void execute(XrXirArtifact **artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);uint32_t function=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==3 && !memcmp(module->functions[f].name,"run",3)) { CHECK(function==UINT32_MAX);function=f; }
    CHECK(function!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(artifact,&program)==XR_XIR_OK && !*artifact);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576;config.value_limit=1048576;config.call_limit=1048576;config.work_limit=1000000;
    XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],function,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
}
static XrXirStatus operation(const XrXirCompileContext *context,const char *text,uint32_t count,uint32_t markers,bool run) {
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket wire={0};
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;XrXirStatus status=source(context,text,&result,&diagnostic);
    if (status!=XR_XIR_OK) { CHECK(!result.checked && !result.snapshot);goto done; }
    query(&result,count,markers);signatures(xr_xir_compile_artifact_module(result.checked),count);
    status=xr_xir_compile_checked_write(result.checked,&wire,NULL);
    xr_xir_compile_source_result_free(&result);
    if (status!=XR_XIR_OK) { CHECK(!wire.bytes && !wire.length);goto done; }
    status=xr_xir_compile_checked_read(context,wire.bytes,wire.length,&received,NULL);
    memset(wire.bytes,0xa5,wire.length);xr_xir_compile_checked_packet_free(&wire);
    if (status!=XR_XIR_OK) { CHECK(!received);goto done; }
    status=xr_xir_compile_specialize(received,&closed,NULL, NULL);xr_xir_compile_artifact_free(received);received=NULL;
    if (status!=XR_XIR_OK) { CHECK(!closed);goto done; }
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL);xr_xir_compile_artifact_free(closed);closed=NULL;
    if (status!=XR_XIR_OK) { CHECK(!lowered);goto done; }
    signatures(xr_xir_compile_artifact_module(lowered),count);status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status==XR_XIR_OK && run) execute(&lowered);
done:
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(received);
    xr_xir_compile_checked_packet_free(&wire);xr_xir_compile_source_result_free(&result);return status;
}
static void positive(const char *text,uint32_t count,uint32_t markers) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=stats(&context);
    CHECK(operation(&context,text,count,markers,true)==XR_XIR_OK && stats(&context).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void negative(const char *text,XrXirStatus expected) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline=stats(&context);
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source(&context,text,&result,&diagnostic);
    if (status!=expected) fprintf(stderr,"Source refusal status%u expected%u for %s",(unsigned)status,(unsigned)expected,text);
    CHECK(status==expected && !result.checked && !result.snapshot);
    CHECK(stats(&context).live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
#include "ref_closure_resources.h"
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--resources")) { resources(true);return 0; }
    if (argc==2 && !strcmp(argv[1],"--budget-controls")) { resources(false);return 0; }
    CHECK(argc==1);
    positive(combined,2,1);
    positive("export fn run()->i64{fn inc(value:ref i64)->i64{value+=2;return value};var x=40;return inc(ref x)}\n",1,1);
    positive("export fn run()->i64{var x=40;const f=(value:ref i64)->{value+=2;return value};return f(ref x)}\n",1,1);
    positive("export fn run()->i64{var x=21;const f=fn(value:ref i64)->i64{return value*2};return f(ref x)+x-21}\n",1,1);
    positive("export fn run()->i64{const n=2;var x=40;const f=fn(value:ref i64)->i64{value+=n;return value};return f(ref x)}\n",1,1);
    positive("export fn run()->i64{var n=40;var x=0;const f=fn(value:ref i64)->i64{n+=1;value+=1;return n+value};return f(ref x)}\n",1,1);
    positive("fn maker(step:i64)->fn(ref i64)->i64{return fn(value:ref i64)->i64{value+=step;return value}}\nexport fn run()->i64{var x=40;const f=maker(2);return f(ref x)}\n",1,1);
    negative("export fn run()->i64{const f:fn(i64)->i64=fn(value:ref i64)->i64{return value};return 42}\n",XR_XIR_BAD_TYPE);
    negative("export fn run()->i64{var x=40;const f=fn(value:ref i64)->i64{return value};return f(x)}\n",XR_XIR_BAD_TYPE);
    negative("export fn run()->i64{const x=40;const f=fn(value:ref i64)->i64{return value};return f(ref x)}\n",XR_XIR_BAD_TYPE);
    negative("export fn run()->i64{const f=fn(value:move i64)->i64{return value};return 42}\n",XR_XIR_BAD_TYPE);
    negative("fn escape(value:ref i64)->fn()->i64{return fn()->i64{return value}}\nexport fn run()->i64{var x=40;const f=escape(ref x);return f()}\n",XR_XIR_BAD_VALUE);
    fprintf(stderr,"Source ref closure explicit/context/nested/arrow/capture parameters use authentic prefix/suffix Cells, element queries and exact source markers; producer/packet/Program death, full receiving and two real VM instances42 passed; mode/place/move/scoped-capture refusals physical0\n");return 0;
}
