/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_materialization.c - Real private literal bodies and independent receivers
 *
 * KEY CONCEPT:
 *   Ordinary Source supplies genuine declaration-owned bodies. A new Built
 *   input binds private fixed-data edges and repeats complete ordinary checks.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "materialization76_golden.h"
static XrXirStatus p11_resource_status(XrCompileResourceStatus status) {
    switch(status){
    case XR_COMPILE_RESOURCE_OK:return XR_XIR_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT:return XR_XIR_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY:return XR_XIR_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_OVERFLOW:return XR_XIR_OVERFLOW;
    case XR_COMPILE_RESOURCE_CANCELLED:return XR_XIR_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE:return XR_XIR_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE:return XR_XIR_CLOCK_FAILURE;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirCtfeStatus p11_ctfe_from_xir(XrXirStatus status) {
    switch(status){
    case XR_XIR_OK:return XR_XIR_CTFE_OK;
    case XR_XIR_MEMORY_LIMIT:return XR_XIR_CTFE_MEMORY_LIMIT;
    case XR_XIR_OUT_OF_MEMORY:return XR_XIR_CTFE_OUT_OF_MEMORY;
    case XR_XIR_OVERFLOW:return XR_XIR_CTFE_OVERFLOW;
    case XR_XIR_BUDGET:return XR_XIR_CTFE_COMPLEXITY_LIMIT;
    case XR_XIR_CANCELLED:return XR_XIR_CTFE_CANCELLED;
    case XR_XIR_DEADLINE:return XR_XIR_CTFE_DEADLINE;
    case XR_XIR_CLOCK_FAILURE:return XR_XIR_CTFE_CLOCK_FAILURE;
    default:return XR_XIR_CTFE_BAD_ARTIFACT;
    }
}
static XrXirStatus p11_xir_from_ctfe(XrXirCtfeStatus status) {
    switch(status){
    case XR_XIR_CTFE_OK:return XR_XIR_OK;
    case XR_XIR_CTFE_MEMORY_LIMIT:return XR_XIR_MEMORY_LIMIT;
    case XR_XIR_CTFE_OUT_OF_MEMORY:return XR_XIR_OUT_OF_MEMORY;
    case XR_XIR_CTFE_OVERFLOW:return XR_XIR_OVERFLOW;
    case XR_XIR_CTFE_COMPLEXITY_LIMIT:case XR_XIR_CTFE_STEP_LIMIT:case XR_XIR_CTFE_FRAME_LIMIT:
    case XR_XIR_CTFE_OUTPUT_LIMIT:case XR_XIR_CTFE_DEPTH_LIMIT:return XR_XIR_BUDGET;
    case XR_XIR_CTFE_CANCELLED:return XR_XIR_CANCELLED;
    case XR_XIR_CTFE_DEADLINE:return XR_XIR_DEADLINE;
    case XR_XIR_CTFE_CLOCK_FAILURE:return XR_XIR_CLOCK_FAILURE;
    default:return XR_XIR_BAD_STRUCTURE;
    }
}
static const XrCompileResourceLimits caps={XR_COMPILE_MEMORY_FINITE,UINT64_C(8388608),true};
typedef struct PreparedMaterialization {
    XrXirSourceResult source;
    XrXirModule module;
    XrXirDeclarations declarations;
    XrXirFunctionIdentity *identities;
    XrXirMaterializationBinding edges[2];
    uint32_t inner,outer,run;
} PreparedMaterialization;
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static void context_new(XrXirCompileContext *context) {
    *context=(XrXirCompileContext){0};CHECK(xr_compile_resources_new(&caps,&context->resources)==XR_COMPILE_RESOURCE_OK);
    context->limits=xr_xir_compile_default_limits();
}
static uint32_t named(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length==strlen(name) && !memcmp(body->name,name,body->name_length)) { CHECK(found==UINT32_MAX);found=f; }
    }
    if (found==UINT32_MAX) for (uint32_t f=0;f<module->function_count;++f)
        fprintf(stderr,"function %u %.*s\n",f,(int)module->functions[f].name_length,module->functions[f].name);
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirStatus source_operation(const XrXirCompileContext *context,unsigned mode,XrXirLinkageKind kind,XrXirSourceResult *result) {
    char root[512],library[1024];
    CHECK(snprintf(root,sizeof(root),"import {Outer,Inner} from \"./private\"\nexport fn run()->i64{%s}\n%s",
        mode>=3?"const value=comptime Outer.outer();return value.get()":"return Outer.outer().get()",
        mode==1?"fn probe()->fn()->Inner{return Inner.inner}\n":"")>0);
    CHECK(snprintf(library,sizeof(library),"%sexport struct Inner{private value:string;static inner()->Inner{return Inner{value:%s}};get()->i64{return len(this.value)}}\n"
        "export struct Outer{private secret:Inner;static outer()->Outer{return Outer{secret:Inner.inner()}};get()->i64{return this.secret.get()+40}}\n",
        mode==2?"fn computed()->string{return \"ab\"}\n":"",mode==2?"computed()":"\"ab\"")>0);
    if (mode==4) CHECK(snprintf(library,sizeof(library),
        "export struct Inner{private value:Array<string>;static inner()->Inner{return Inner{value:[\"a\",\"b\"]}};get()->i64{return len(this.value)}}\n"
        "export struct Outer{private secret:Inner;static outer()->Outer{return Outer{secret:Inner.inner()}};get()->i64{return this.secret.get()+40}}\n")>0);
    if (mode==5) CHECK(snprintf(library,sizeof(library),
        "export struct Inner<T>{private value:T;static inner(value:T)->Inner<T>{return Inner<T>{value:value}};get()->T{return this.value}}\n"
        "export struct Outer{private secret:Inner<string>;static outer()->Outer{return Outer{secret:Inner<string>.inner(\"ab\")}};get()->i64{return len(this.secret.get())+40}}\n")>0);
    if (mode==6) CHECK(snprintf(library,sizeof(library),
        "export struct Inner{private value:string;static inner()->Inner{return Inner{value:\"ab\"}}}\n"
        "export struct Outer{private secret:Array<Inner>;static outer()->Outer{var values:Array<Inner>=[];for(var i=0;i<32;i++){values.push(Inner.inner())};return Outer{secret:values}};get()->i64{return len(this.secret)+10}}\n")>0);
    if (mode==7) {
        char types[256]={0},values[256]={0};size_t type_bytes=0,value_bytes=0;
        for (unsigned i=0;i<32;++i) {
            int a=snprintf(types+type_bytes,sizeof(types)-type_bytes,"%sInner",i?",":"");
            int b=snprintf(values+value_bytes,sizeof(values)-value_bytes,"%svalue",i?",":"");
            CHECK(a>0 && b>0 && (size_t)a<sizeof(types)-type_bytes && (size_t)b<sizeof(values)-value_bytes);
            type_bytes+=(size_t)a;value_bytes+=(size_t)b;
        }
        CHECK(snprintf(library,sizeof(library),
            "export struct Inner{private value:string;static inner()->Inner{return Inner{value:\"ab\"}};get()->i64{return len(this.value)}}\n"
            "export struct Outer{private secret:(%s);static outer()->Outer{const value=Inner.inner();return Outer{secret:(%s)}};get()->i64{return this.secret.0.get()+40}}\n",types,values)>0);
    }
    if (mode==8) {
        CHECK(snprintf(root,sizeof(root),"import {Inner} from \"./private\"\n"
            "export fn run()->i64{const value=comptime Inner<string>.inner(\"ab\");return len(value.get())+40+later()}\n"
            "fn later()->i64{const pair:(i64,string)=comptime (0,\"ok\");return pair.0}\n")>0);
        CHECK(snprintf(library,sizeof(library),
            "export struct Inner<T>{private value:(T,T);static inner(value:T)->Inner<T>{return Inner<T>{value:(value,value)}};get()->T{return this.value.0}}\n")>0);
    }
    XrCompilerSession *session=NULL;XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK) return p11_resource_status(created);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    char entry[1024],private_path[1024];
    CHECK(snprintf(entry,sizeof(entry),"%s/root.xr",MATERIALIZATION_ROOT)>0);
    CHECK(snprintf(private_path,sizeof(private_path),"%s/private.xr",MATERIALIZATION_ROOT)>0);
    XrModuleOverlayInput inputs[2]={{authority,"root.xr",entry,root,strlen(root)},
        {authority,"private.xr",private_path,library,strlen(library)}};
    XrXirSourceRequest request={session,entry,&authority,context,NULL,NULL,kind,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,inputs,2,result,&diagnostic,NULL);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"ordinary Source status=%u at%d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);memset(root,0xa5,sizeof(root));memset(library,0xa5,sizeof(library));
    return status;
}
static void ordinary_source(const XrXirCompileContext *context,unsigned mode,XrXirSourceResult *result) {
    CHECK(source_operation(context,mode,XR_XIR_PROGRAM,result)==XR_XIR_OK);
}
static XrXirMaterializationBinding edge(const XrXirModule *module,uint32_t caller,uint32_t callee) {
    const XrXirFunction *body=&module->functions[caller];uint32_t found=UINT32_MAX;
    for (uint32_t i=0;i<body->instruction_count;++i)
        if (body->instructions[i].op==XR_XIR_CALL && body->instructions[i].immediate==callee) { CHECK(found==UINT32_MAX);found=i; }
    CHECK(found!=UINT32_MAX);const XrXirTypeNode *type=xr_xir_type_node(module->types,module->functions[callee].result);
    CHECK(type && type->kind==XR_XIR_TYPE_NOMINAL);
    return (XrXirMaterializationBinding){caller,found,callee,type->nominal.declaration};
}
static void prepare(const XrXirCompileContext *context,unsigned mode,PreparedMaterialization *input) {
    *input=(PreparedMaterialization){0};ordinary_source(context,mode,&input->source);
    input->module=*xr_xir_compile_artifact_module(input->source.checked);input->declarations=*input->module.declarations;
    input->inner=named(&input->module,"inner");input->outer=named(&input->module,"outer");input->run=named(&input->module,"run");
    void *memory=NULL;CHECK(xr_compile_resources_calloc(context->resources,input->module.function_count,sizeof(*input->identities),&memory)==XR_COMPILE_RESOURCE_OK);
    input->identities=memory;memcpy(memory,input->declarations.functions,(size_t)input->module.function_count*sizeof(*input->identities));
    uint32_t helpers[2]={input->inner,input->outer};
    for (unsigned i=0;i<2;++i) { input->identities[helpers[i]].method_kind=XR_XIR_MATERIALIZER;input->identities[helpers[i]].exported=0; }
    input->module.stage=XR_XIR_BUILT;input->module.provenance=NULL;
    input->declarations.functions=input->identities;input->module.declarations=&input->declarations;
    input->edges[0]=edge(&input->module,input->run,input->outer);input->edges[1]=edge(&input->module,input->outer,input->inner);
    if (input->edges[0].caller>input->edges[1].caller) {
        XrXirMaterializationBinding saved=input->edges[0];input->edges[0]=input->edges[1];input->edges[1]=saved;
    }
    CHECK(input->identities[input->run].module!=input->identities[input->outer].module);
}
static void prepared_free(PreparedMaterialization *input) {
    xr_compile_resources_free(input->identities);xr_xir_compile_source_result_free(&input->source);
}
static XrXirStatus checked(const XrXirCompileContext *context,const PreparedMaterialization *input,
    const XrXirMaterializationBinding *edges,uint32_t count,XrXirArtifact **output) {
    const XrXirConstruction *source=input->source.checked->construction;
    XrXirConstructionSpec spec={source->rows,source->count,edges,count,NULL};XrXirConstruction *owner=NULL;
    XrXirStatus status=xr_xir_compile_construction_new_v4(context,input->module.types,&spec,xr_xir_compile_construction_slots(source),&owner);
    XrXirDiagnostic diagnostic={0};
    if (status==XR_XIR_OK) status=xr_xir_compile_check_v2(context,&input->module,owner,output,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"checking status=%u function=%u block=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_construction_free(owner);return status;
}
static XrXirStatus pipeline(const XrXirCompileContext *context,const PreparedMaterialization *input,XrXirArtifact **output) {
    XrXirArtifact *first=NULL,*read=NULL,*closed=NULL;XrXirCheckedPacket wire={0};
    XrXirStatus status=checked(context,input,input->edges,2,&first);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(first,&wire,NULL);
    xr_xir_compile_artifact_free(first);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,wire.bytes,wire.length,&read,NULL);
    xr_xir_compile_checked_packet_free(&wire);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(read,&closed,NULL);
    xr_xir_compile_artifact_free(read);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(closed,&wire,NULL);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,wire.bytes,wire.length,&closed,NULL);
    xr_xir_compile_checked_packet_free(&wire);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target, NULL,output,NULL);
    xr_xir_compile_artifact_free(closed);
    return status;
}
static void compiler_entries(const XrXirArtifact *artifact,uint32_t helper,uint32_t run) {
    XrXirCtfeRequest request={.artifact=artifact,.function=helper,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,artifact->checked_identity,sizeof(request.checked_identity));
    XrXirValue output={0};XrXirCtfeStatus status=xr_xir_compile_ctfe_leaf(&request,&output);
    fprintf(stderr,"Private materializer direct CTFE leaf status=%u output_type=%u\n",(unsigned)status,output.type);
    CHECK(status==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.reserved && !output.payload);
    XrXirCtfeCallRequest calls={request,XR_XIR_CTFE_MAX_CALL_DEPTH};
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_NOT_SUPPORTED && !output.type && !output.reserved && !output.payload);
    calls.evaluation.function=run;
    CHECK(xr_xir_compile_ctfe_calls(&calls,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_I64 && output.payload==42);
    xr_xir_value_drop(&output);
    XrXirRunContext runtime={0};
    CHECK(xr_xir_compile_vm_run(artifact,request.function,&runtime,NULL,0,&output)==XR_XIR_RUN_BAD_ARTIFACT && !output.type && !output.payload);
}
static void execute(XrXirArtifact **artifact,const char *entry,uint32_t bindings) {
    const XrXirModule *module=xr_xir_compile_artifact_module(*artifact);uint32_t run=named(module,entry);
    uint32_t inner=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->declarations->functions[f].method_kind==XR_XIR_MATERIALIZER) { inner=f;break; }
    CHECK(inner!=UINT32_MAX);compiler_entries(*artifact,inner,run);
    CHECK(xr_xir_compile_construction_materialization_count((*artifact)->construction)==bindings);
    XrXirProgram *program=NULL;
    XrXirStatus received=xr_xir_compile_vm_program_take(artifact,&program);
    if (received!=XR_XIR_OK) {
        XrCompileResourceStats usage=stats(xr_xir_compile_artifact_context(*artifact));
        fprintf(stderr,"Materializer Program reception status=%u allocated=%llu peak=%llu\n",
            (unsigned)received,(unsigned long long)usage.allocated_bytes,(unsigned long long)usage.peak_bytes);
    }
    CHECK(received==XR_XIR_OK && !*artifact);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.metadata_limit=1048576;config.value_limit=1048576;config.call_limit=1048576;config.work_limit=1000000;
    XrXirInstance *instances[2]={NULL,NULL};
    for (unsigned i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instances[i],inner,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instances[i],run,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
}
static void negative(const XrXirCompileContext *context,PreparedMaterialization *input) {
    XrXirArtifact *output=NULL;
    CHECK(checked(context,input,NULL,0,&output)==XR_XIR_BAD_STRUCTURE && !output);
    uint32_t exported=input->identities[input->inner].exported;input->identities[input->inner].exported=1;
    CHECK(checked(context,input,input->edges,2,&output)==XR_XIR_BAD_STRUCTURE && !output);input->identities[input->inner].exported=exported;
    XrXirMaterializationBinding wrong[2];memcpy(wrong,input->edges,sizeof(wrong));++wrong[0].instruction;
    CHECK(checked(context,input,wrong,2,&output)==XR_XIR_BAD_STRUCTURE && !output);
    memcpy(wrong,input->edges,sizeof(wrong));wrong[0].nominal=wrong[1].nominal;
    CHECK(checked(context,input,wrong,2,&output)==XR_XIR_BAD_STRUCTURE && !output);
}
static void cycle(const XrXirCompileContext *context,PreparedMaterialization *input) {
    const XrXirFunction *original=input->module.functions;void *memory=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,input->module.function_count,sizeof(*original),&memory)==XR_COMPILE_RESOURCE_OK);
    XrXirFunction *functions=memory;memcpy(functions,original,(size_t)input->module.function_count*sizeof(*original));
    XrXirFunction *inner=&functions[input->inner];uint32_t position=inner->instruction_count-1;
    CHECK(inner->instructions[position].op==XR_XIR_RETURN && inner->block_count==1);
    memory=NULL;CHECK(xr_compile_resources_calloc(context->resources,(size_t)inner->instruction_count+1,sizeof(XrXirInstruction),&memory)==XR_COMPILE_RESOURCE_OK);
    XrXirInstruction *instructions=memory;memcpy(instructions,inner->instructions,(size_t)position*sizeof(*instructions));
    instructions[position]=(XrXirInstruction){XR_XIR_CALL,functions[input->outer].result,{0},{0},input->outer,{0}};
    instructions[position+1]=inner->instructions[position];inner->instructions=instructions;++inner->instruction_count;
    XrXirBlock block=*inner->blocks;++block.count;inner->blocks=&block;input->module.functions=functions;
    XrXirArtifact *output=NULL;uint32_t helpers[2]={input->inner,input->outer};
    for (unsigned i=0;i<2;++i) { input->identities[helpers[i]].method_kind=XR_XIR_STATIC_METHOD;input->identities[helpers[i]].exported=1; }
    CHECK(checked(context,input,NULL,0,&output)==XR_XIR_OK && output);xr_xir_compile_artifact_free(output);output=NULL;
    for (unsigned i=0;i<2;++i) { input->identities[helpers[i]].method_kind=XR_XIR_MATERIALIZER;input->identities[helpers[i]].exported=0; }
    XrXirMaterializationBinding bindings[3];memcpy(bindings,input->edges,sizeof(input->edges));
    const XrXirTypeNode *result=xr_xir_type_node(input->module.types,functions[input->outer].result);
    bindings[2]=(XrXirMaterializationBinding){input->inner,position,input->outer,result->nominal.declaration};
    for (unsigned i=1;i<3;++i) {
        XrXirMaterializationBinding row=bindings[i];unsigned next=i;
        while (next && (bindings[next-1].caller>row.caller ||
                (bindings[next-1].caller==row.caller && bindings[next-1].instruction>row.instruction))) {
            bindings[next]=bindings[next-1];--next;
        }
        bindings[next]=row;
    }
    CHECK(checked(context,input,bindings,3,&output)==XR_XIR_BAD_TYPE && !output);
    input->module.functions=original;xr_compile_resources_free(instructions);xr_compile_resources_free(functions);
}
static void faults(const PreparedMaterialization *input,size_t sites) {
    for (size_t fault=0;fault<sites;++fault) {
        XrXirCompileContext context;context_new(&context);XrCompileResources *same=context.resources;
        XrCompileResourceStats baseline=stats(&context);size_t start=source_fixture_compile_attempts;XrXirArtifact *output=NULL;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        CHECK(pipeline(&context,input,&output)==XR_XIR_OUT_OF_MEMORY && !output && source_fixture_compile_injected);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&context).held_bytes==baseline.held_bytes);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(pipeline(&context,input,&output)==XR_XIR_OK && output);xr_xir_compile_artifact_free(output);
        CHECK(context.resources==same && stats(&context).held_bytes==baseline.held_bytes);
        xr_compile_resources_release(context.resources);
        if (!(fault%32) || fault+1==sites) fprintf(stderr,"Materialization compiler FI=%zu/%zu same-owner retry/live\n",fault+1,sites);
    }
}
static void source_materializers(unsigned mode) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult source={0};
    ordinary_source(&context,mode,&source);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    uint32_t run=named(module,"run"),outer=named(module,mode==8?"inner":"outer"),count=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        bool materializer=module->declarations->functions[f].method_kind==XR_XIR_MATERIALIZER;
        count+=materializer;
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            const XrXirInstruction *op=&module->functions[f].instructions[i];
            if (op->op!=XR_XIR_CALL) continue;
            if (materializer) CHECK(module->declarations->functions[op->immediate].method_kind==XR_XIR_MATERIALIZER);
            if (f==run) CHECK(op->immediate!=outer);
        }
    }
    uint32_t expected=(mode==6 || mode==7)?33u:mode==8?1u:2u;
    CHECK(count==expected && xr_xir_compile_construction_materialization_count(source.checked->construction)==expected);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(context.resources);execute(&lowered,"run",expected);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Ordinary comptime Source mode=%u generated %u bound literal functions under actual private declarations; producer dead/two Instance42\n",mode,expected);
}
#include "source_cases.h"
#include "wire_cases.h"
static void independent_wire(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    wire_built(&context);
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(&context,materialization76_golden,sizeof(materialization76_golden),&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Independent materializer wire status=%u function=%u instruction=%u\n",
        (unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(packet.length==sizeof(materialization76_golden) && !memcmp(packet.bytes,materialization76_golden,packet.length));
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    xr_compile_resources_release(context.resources);execute(&lowered,"entry",1);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Independent complete private materialization wire literal/read/write/two Instance42\n");
}
int main(int argc,char **argv) {
    bool full=argc==2 && !strcmp(argv[1],"--resources");
    bool source_full=argc==2 && !strcmp(argv[1],"--source-resources");
    bool source_array_full=argc==2 && !strcmp(argv[1],"--source-array-resources");
    bool source_generic_full=argc==2 && !strcmp(argv[1],"--source-generic-resources");
    bool source_growth_full=argc==2 && !strcmp(argv[1],"--source-growth-resources");
    bool source_type_full=argc==2 && !strcmp(argv[1],"--source-type-resources");
    CHECK(argc==1 || full || source_full || source_array_full || source_generic_full || source_growth_full || source_type_full);
    if (source_full || source_array_full || source_generic_full || source_growth_full || source_type_full) {
        source_faults(source_type_full?8:source_growth_full?7:source_generic_full?5:source_array_full?4:3);return 0;
    }
    XrXirCompileContext producer;context_new(&producer);PreparedMaterialization input;prepare(&producer,0,&input);
    negative(&producer,&input);cycle(&producer,&input);
    XrXirCompileContext receiver;context_new(&receiver);XrCompileResourceStats baseline=stats(&receiver);XrXirArtifact *lowered=NULL;
    size_t begin=source_fixture_compile_attempts;XrXirStatus status=pipeline(&receiver,&input,&lowered);
    if (status!=XR_XIR_OK) fprintf(stderr,"pipeline status=%u\n",(unsigned)status);
    CHECK(status==XR_XIR_OK && lowered);size_t sites=source_fixture_compile_attempts-begin;
    CHECK(sites);fprintf(stderr,"Materialization compiler actual census=%zu held=%llu\n",sites,(unsigned long long)stats(&receiver).held_bytes);
    xr_xir_compile_artifact_free(lowered);lowered=NULL;CHECK(stats(&receiver).held_bytes==baseline.held_bytes);
    if (full) faults(&input,sites);
    CHECK(pipeline(&receiver,&input,&lowered)==XR_XIR_OK && lowered);
    prepared_free(&input);xr_compile_resources_release(producer.resources);xr_compile_resources_release(receiver.resources);
    execute(&lowered,"run",2);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    for (unsigned mode=1;mode<=2;++mode) {
        context_new(&producer);prepare(&producer,mode,&input);XrXirArtifact *rejected=NULL;
        CHECK(checked(&producer,&input,input.edges,2,&rejected)==XR_XIR_BAD_TYPE && !rejected);
        prepared_free(&input);xr_compile_resources_release(producer.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    for (unsigned mode=3;mode<=5;++mode) source_materializers(mode);
    source_materializers(7);
    source_materializers(8);
    source_materializers(6);
    source_library_case(false);source_library_case(true);
    independent_wire();
    fprintf(stderr,"Private fixed-data catalogue, nested owner edges, no export/FnRef/user computation, independent packets and two real Instance42; compiler physical0\n");
    return 0;
}
