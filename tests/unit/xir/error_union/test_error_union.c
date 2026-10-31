/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_error_union.c - Error metadata, complete membership and owned receiving
 */
#include "xir/xxir_types.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_type_inference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#if defined(XR_UNION_ALLOCATOR)
#include "base/xmalloc.h"
static bool fault_next;
static size_t fault_calls,fault_live;
static void *physical_alloc(size_t bytes) {
    ++fault_calls;if (fault_next) {fault_next=false;return NULL;}
    void *memory=xr_malloc(bytes);if (memory) ++fault_live;return memory;
}
static void physical_free(void *memory) {if (memory) {CHECK(fault_live);--fault_live;}xr_free(memory);}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) physical_alloc(bytes)
#define xr_free(memory) physical_free(memory)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#endif
static void *owned(const XrXirCompileContext *context,size_t count,size_t size) {
    void *memory=NULL;CHECK(xr_compile_resources_calloc(context->resources,count,size,&memory)==XR_COMPILE_RESOURCE_OK);
    return memory;
}
static uint64_t held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static void ordinary_union_rejected(const XrXirCompileContext *context,const XrXirTypes *types,XrXirType expression) {
    XrXirFunction function={.error_type=XR_XIR_NEVER};
    XrXirModule module={.functions=&function,.function_count=1,.types=types};
    CHECK(xr_xir_type_in_context(&module,0,XR_XIR_I64));CHECK(!xr_xir_type_in_context(&module,0,expression));
    CHECK(xr_xir_compile_type_constraints(context,&module,0,expression,(XrXirConstraint){0})==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_constraints(context,&module,0,expression,
        (XrXirConstraint){.markers=XR_XIR_CONSTRAINT_ERROR})==XR_XIR_OK);
}
typedef struct InferenceOwner {
    const XrXirCompileContext *context;
    XrXirTypes types;
    XrXirCallableParameter *members;
    XrXirStatus failure;
    uint32_t calls;
    bool physical_failure;
} InferenceOwner;
static XrXirStatus inference_intern(void *context,const XrXirType *members,uint32_t count,XrXirType *output) {
    InferenceOwner *owner=context;++owner->calls;
    if (owner->failure!=XR_XIR_OK) return owner->failure;
#if defined(XR_UNION_ALLOCATOR)
    if (owner->physical_failure) fault_next=true;
#endif
    XrXirErrorUnion normalized={0};
    XrXirStatus status=xr_xir_compile_error_union(owner->context,&owner->types,members,count,&normalized);
    if (status!=XR_XIR_OK) return status;
    CHECK(normalized.count==299 && normalized.members && !normalized.scalar);
    uint32_t previous=owner->types.count;
    XrXirTypeNode *grown=owned(owner->context,(size_t)previous+1,sizeof(*grown));
    memcpy(grown,owner->types.nodes,(size_t)previous*sizeof(*grown));
    grown[previous]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=normalized.members,.parameter_count=normalized.count};
    XrXirTypeNode *retired=(XrXirTypeNode *)owner->types.nodes;
    memset(retired,0xa5,(size_t)previous*sizeof(*retired));xr_compile_resources_free(retired);
    owner->types.nodes=grown;owner->types.count=previous+1;owner->members=normalized.members;
    *output=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+previous);return XR_XIR_OK;
}
static void inference_owner_cases(const XrXirCompileContext *context,const XrXirTypes *types) {
    uint64_t baseline=held(context);
    unsigned modes=3;
#if defined(XR_UNION_ALLOCATOR)
    modes=4;
#endif
    for (unsigned mode=0;mode<modes;++mode) {
        InferenceOwner owner={.context=context,.types=*types,.physical_failure=mode==3};
        if (mode==1) owner.failure=XR_XIR_CANCELLED;else if (mode==2) owner.failure=XR_XIR_OUT_OF_MEMORY;
        XrXirTypeNode *nodes=owned(context,306,sizeof(*nodes));
        memcpy(nodes,types->nodes,(size_t)types->count*sizeof(*nodes));owner.types.nodes=nodes;owner.types.count=306;
        XrXirCallableParameter parameter={XR_XIR_I64,0};
        nodes[304]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE,.element=XR_XIR_CONSTRUCTED_TYPE_BASE+302,
            .result=XR_XIR_I64,.parameters=&parameter,.parameter_count=1,.parameter_span=1};
        nodes[305]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE,.element=XR_XIR_CONSTRUCTED_TYPE_BASE+300,
            .result=XR_XIR_I64,.parameters=&parameter,.parameter_count=1};
        XrXirInferenceRequest request={.types=&owner.types,.own_count=1,
            .union_owner={&owner,&owner.types,inference_intern}};
        XrXirInferenceState *state=NULL;CHECK(xr_xir_compile_inference_begin(context,&request,&state)==XR_XIR_OK);
        XrXirInferencePair pair={XR_XIR_CONSTRUCTED_TYPE_BASE+304,XR_XIR_CONSTRUCTED_TYPE_BASE+305};
        XrXirStatus expected=mode==0 ? XR_XIR_OK : mode==1 ? XR_XIR_CANCELLED : XR_XIR_OUT_OF_MEMORY;
        CHECK(xr_xir_compile_inference_observe(state,&owner.types,pair)==expected && owner.calls==1);
        XrXirType solution=XR_XIR_BOOL;
        XrXirStatus final_status=xr_xir_compile_inference_finalize(state,&owner.types,&solution,1);
        if (final_status!=expected) fprintf(stderr,"Inference owner mode%u finalize%u expected%u nodes%u\n",mode,final_status,expected,owner.types.count);
        CHECK(final_status==expected);
        if (mode==0) {
            const XrXirTypeNode *node=xr_xir_type_node(&owner.types,solution);
            CHECK(owner.types.nodes!=nodes && node && node->kind==XR_XIR_TYPE_UNION && node->parameter_count==299);
            CHECK(node->parameters[0].type==XR_XIR_CONSTRUCTED_TYPE_BASE+1 &&
                node->parameters[298].type==XR_XIR_CONSTRUCTED_TYPE_BASE+299);
        } else {
            CHECK(solution==XR_XIR_BOOL && !owner.members && owner.types.count==306);
            CHECK(xr_xir_compile_inference_observe(state,&owner.types,pair)==expected && owner.calls==1);
#if defined(XR_UNION_ALLOCATOR)
            CHECK(!fault_next);
#endif
        }
        xr_xir_compile_inference_dispose(state);xr_compile_resources_free(owner.members);
        xr_compile_resources_free((void *)owner.types.nodes);CHECK(held(context)==baseline);
    }
}
static void type_cases(void) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t baseline=held(&context);
    XrXirNominalDeclaration *declarations=owned(&context,300,sizeof(*declarations));
    XrXirTypeNode *nodes=owned(&context,304,sizeof(*nodes)),*reversed=owned(&context,301,sizeof(*reversed));
    char (*names)[12]=owned(&context,300,sizeof(*names));
    XrXirType *members=owned(&context,302,sizeof(*members));
    XrXirNominalVariant variant={{"Bad",3},0,0};
    XrXirNominalTable table={.declarations=declarations,.count=300};
    XrXirTypes types={.nodes=nodes,.count=300,.nominals=&table};
    for (uint32_t i=0;i<300;++i) {
        int length=snprintf(names[i],sizeof(names[i]),"E%u",i);CHECK(length>0 && length<12);
        declarations[i]=(XrXirNominalDeclaration){.module={"union",5},.name={names[i],(uint32_t)length},
            .kind=XR_XIR_NOMINAL_ENUM,.variants=&variant,.variant_count=1};
        nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=i}};
        reversed[299-i]=nodes[i];
        members[299-i]=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
    }
    members[300]=XR_XIR_NEVER;members[301]=members[299];
    XrXirErrorUnion normalized={0};
    CHECK(xr_xir_compile_error_union(&context,&types,members,302,&normalized)==XR_XIR_OK);
    CHECK(normalized.count==300 && !normalized.scalar && normalized.members[299].type==XR_XIR_CONSTRUCTED_TYPE_BASE+299);
    nodes[300]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=normalized.members,.parameter_count=300};
    reversed[300]=nodes[300];
    XrXirErrorUnion prefix={0};
    CHECK(xr_xir_compile_error_union(&context,&types,members+1,299,&prefix)==XR_XIR_OK && prefix.count==299);
    nodes[301]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=prefix.members,.parameter_count=299};
    XrXirCallableParameter generic_members[2]={{XR_XIR_CONSTRUCTED_TYPE_BASE,0},{XR_XIR_TYPE_PARAMETER_BASE,0}};
    nodes[302]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=generic_members,.parameter_count=2,.parameter_span=1};
    XrXirCallableParameter pair[2]={{XR_XIR_CONSTRUCTED_TYPE_BASE,0},{XR_XIR_CONSTRUCTED_TYPE_BASE+1,0}};
    nodes[303]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=pair,.parameter_count=2};
    types.count=304;XrXirTypes receiving={.nodes=reversed,.count=301,.nominals=&table};
    uint64_t populated=held(&context);
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(&context,&receiving)==XR_XIR_OK);
    normalized.members[299].mode=XR_PARAM_REF;
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_BAD_TYPE);
    normalized.members[299].mode=0;
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
    puts("Raw UNION: nonzero descriptor passing mode rejected and restored PASS");
    XrXirType all=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+300),shorter=(XrXirType)(all+1);
    XrXirType last=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+299),first=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_error_bound_accepts(&types,last,all));CHECK(!xr_xir_error_bound_accepts(&types,last,shorter));
    CHECK(!xr_xir_error_bound_accepts(&types,XR_XIR_I64,XR_XIR_ERROR));
    CHECK(xr_xir_compile_type_substitution_matches_between(&context,&types,&receiving,NULL,0,all,all)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches_between(&context,&types,&receiving,NULL,0,shorter,all)==XR_XIR_BAD_TYPE);
    XrXirErrorMatch subset={&types,&receiving,NULL,0,all,first,NULL,NULL};
    CHECK(xr_xir_compile_error_substitution_accepts(&context,&subset)==XR_XIR_OK);
    subset.bound=shorter;CHECK(xr_xir_compile_error_substitution_accepts(&context,&subset)==XR_XIR_BAD_TYPE);
    XrXirType arguments[4]={XR_XIR_NEVER,first,XR_XIR_ERROR,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+303)};
    XrXirType expected[4]={first,first,XR_XIR_ERROR,arguments[3]};
    for (unsigned i=0;i<4;++i)
        CHECK(xr_xir_compile_type_substitution_matches_between(&context,&types,&types,&arguments[i],1,
            (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+302),expected[i])==XR_XIR_OK);
    XrXirConstraint bound={.markers=XR_XIR_CONSTRAINT_ERROR};
    XrXirGeneric generic={.constraints=&bound,.parameter_count=1};XrXirFunction function={.error_type=XR_XIR_NEVER};
    XrXirModule module={.types=&types,.functions=&function,.function_count=1,.generics=&generic};
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
    CHECK(xr_xir_compile_type_markers_prove(&context,&proof,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+302),XR_XIR_CONSTRAINT_ERROR)==XR_XIR_OK);
    bound.markers=0;
    CHECK(xr_xir_compile_type_markers_prove(&context,&proof,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+302),XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    XrXirType invalid[2]={XR_XIR_ERROR,XR_XIR_I64};XrXirErrorUnion refused={0};
    CHECK(xr_xir_compile_error_union(&context,&types,invalid,2,&refused)==XR_XIR_BAD_TYPE && !refused.members && !refused.scalar);
    XrXirLayout layout={77,88};XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_layout(&context,&types,all,&target,XR_XIR_LAYOUT_STORAGE,&layout)==XR_XIR_BAD_LAYOUT);
    CHECK(layout.size==77 && layout.alignment==88 && !xr_xir_type_is_owned(&types,all));
    ordinary_union_rejected(&context,&types,all);
    inference_owner_cases(&context,&types);
    CHECK(held(&context)==populated);
#if defined(XR_UNION_ALLOCATOR)
    size_t live=fault_live,calls=fault_calls;fault_next=true;
    CHECK(xr_xir_compile_error_union(&context,&types,members,302,&refused)==XR_XIR_OUT_OF_MEMORY);
    CHECK(!fault_next && fault_calls==calls+1 && fault_live==live && held(&context)==populated && !refused.scalar && !refused.members);
#endif
    XrXirType pair_types[2]={first,(XrXirType)(first+1)};XrXirErrorUnion measured={0};
    CHECK(xr_xir_compile_error_union(&context,&types,pair_types,2,&measured)==XR_XIR_OK);
    uint64_t charge=held(&context)-populated;CHECK(charge>2*sizeof(XrXirCallableParameter));xr_compile_resources_free(measured.members);
    xr_compile_resources_free(prefix.members);xr_compile_resources_free(normalized.members);
    xr_compile_resources_free(members);xr_compile_resources_free(names);xr_compile_resources_free(nodes);
    xr_compile_resources_free(reversed);xr_compile_resources_free(declarations);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==baseline);
    xr_compile_resources_release(context.resources);
    limits=(XrCompileResourceLimits){.memory_mode=XR_COMPILE_MEMORY_FINITE,.memory_limit=baseline+1};context.resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirNominalDeclaration finite_declarations[2]={
        {.module={"union",5},.name={"Left",4},.kind=XR_XIR_NOMINAL_ENUM,.variants=&variant,.variant_count=1},
        {.module={"union",5},.name={"Right",5},.kind=XR_XIR_NOMINAL_ENUM,.variants=&variant,.variant_count=1}};
    XrXirNominalTable finite_table={.declarations=finite_declarations,.count=2};
    XrXirTypeNode finite_nodes[2]={{.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}}};
    XrXirTypes finite_types={.nodes=finite_nodes,.count=2,.nominals=&finite_table};
    XrXirType finite_members[2]={XR_XIR_CONSTRUCTED_TYPE_BASE,XR_XIR_CONSTRUCTED_TYPE_BASE+1};
    CHECK(xr_xir_compile_error_union(&context,&finite_types,finite_members,2,&refused)==XR_XIR_MEMORY_LIMIT && !refused.members && !refused.scalar);
    CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==baseline);
    xr_compile_resources_release(context.resources);
    for (int delta=-1;delta<=1;++delta) {
        limits.memory_limit=baseline+charge;if (delta<0) --limits.memory_limit;else limits.memory_limit+=(uint64_t)delta;context.resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=xr_xir_compile_error_union(&context,&finite_types,finite_members,2,&refused);
        CHECK(status==(delta<0 ? XR_XIR_MEMORY_LIMIT : XR_XIR_OK));
        if (delta>=0) {CHECK(refused.count==2 && refused.members);xr_compile_resources_free(refused.members);refused=(XrXirErrorUnion){0};}
        CHECK(held(&context)==baseline);xr_compile_resources_release(context.resources);
    }
}
#ifndef XR_UNION_TYPES_ONLY
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_source_generation.h"
#include "xir/xxir_generic.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdarg.h>
static void append(char *text,size_t capacity,size_t *length,const char *format,...) {
    va_list arguments;va_start(arguments,format);
    int n=vsnprintf(text+*length,capacity-*length,format,arguments);va_end(arguments);
    CHECK(n>=0 && (size_t)n<capacity-*length);*length+=(size_t)n;
}
static char *source_300(void) {
    size_t length=0;char *text=xr_malloc(65536);CHECK(text);text[0]=0;
    for (unsigned i=0;i<300;++i) append(text,65536,&length,"enum E%u{Bad}\n",i);
    append(text,65536,&length,"fn value() throws(");
    for (unsigned i=0;i<300;++i) append(text,65536,&length,"%sE%u",i?"|":"",i);
    append(text,65536,&length,")->i64{throw E299.Bad}\nexport fn answer()->i64{try{return value()}catch(e:E299){return 42}catch(e){return -1}}\nprint(answer())\n");
    return text;
}
static void packet_members(const XrXirCompileContext *context,const XrXirModule *module,
    uint32_t expected,const char *tail) {
    if (!expected) return;
    uint32_t value=UINT32_MAX;const XrXirTypeNode *set=NULL;XrXirType last=XR_XIR_UNIT;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==5 && !memcmp(module->functions[f].name,"value",5)) {
            CHECK(value==UINT32_MAX);value=f;set=xr_xir_type_node(module->types,module->functions[f].error_type);
        }
    CHECK(value!=UINT32_MAX && set && set->kind==XR_XIR_TYPE_UNION && set->parameter_count==expected);
    for (uint32_t p=0;p<set->parameter_count;++p) {
        XrXirType type=set->parameters[p].type;const XrXirTypeNode *node=xr_xir_type_node(module->types,type);
        CHECK(node && xr_xir_type_is_enum(module->types,type));
        const XrXirNominalDeclaration *d=&module->types->nominals->declarations[node->nominal.declaration];
        if (d->name.length==strlen(tail) && !memcmp(d->name.bytes,tail,d->name.length)) last=type;
    }
    CHECK(last!=XR_XIR_UNIT && set->parameters[expected-1].type==last);
    XrXirModule raw=*module;XrXirTypes types=*module->types;
    XrXirTypeNode *nodes=owned(context,(size_t)types.count+1,sizeof(*nodes));
    memcpy(nodes,types.nodes,(size_t)types.count*sizeof(*nodes));
    XrXirType prefix=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+types.count);
    nodes[types.count]=(XrXirTypeNode){.kind=XR_XIR_TYPE_UNION,.parameters=set->parameters,.parameter_count=expected-1};
    types.nodes=nodes;++types.count;raw.types=&types;
    XrXirFunction *functions=owned(context,module->function_count,sizeof(*functions));
    memcpy(functions,module->functions,(size_t)module->function_count*sizeof(*functions));raw.functions=functions;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xir_compile_local_errors_verify(context,&raw,NULL)==XR_XIR_OK);
    XrXirType original=functions[value].error_type;functions[value].error_type=prefix;
    CHECK(xir_compile_local_errors_verify(context,&raw,NULL)==XR_XIR_BAD_TYPE);
    functions[value].error_type=original;CHECK(xir_compile_local_errors_verify(context,&raw,NULL)==XR_XIR_OK);
    xr_compile_resources_free(functions);xr_compile_resources_free(nodes);
}
typedef struct Output {char bytes[128];size_t length;} Output;
static bool run_vm;
static XrXirOutputStatus output_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(out->bytes)-out->length);
    memcpy(out->bytes+out->length,bytes,length);out->length+=length;return XR_XIR_OUTPUT_OK;
}
static uint32_t function_named(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    return UINT32_MAX;
}
static XrXirValue run_entry(XrXirInstance *instance,uint32_t entry,XrXirCallStatus expected) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status!=expected) fprintf(stderr,"VM entry%u status%u expected%u\n",entry,status,expected);
    CHECK(status==expected);XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==expected);return result;
}
static void late_error(XrXirValue *error) {
    const char expected[]="owned é\0界";XrXirValue concrete={0};XrXirEnumBorrow value={0};
    concrete=*error;if (error->type==XR_XIR_ERROR) CHECK(xr_xir_error_borrow(error,&concrete));
    CHECK(xr_xir_enum_borrow(&concrete,&value)==XR_XIR_VALUE_OK);
    CHECK(value.name.length==5 && !memcmp(value.name.bytes,"First",5));
    CHECK(value.member.length==3 && !memcmp(value.member.bytes,"Bad",3) && value.field_count==1);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value.fields[0],&bytes,&length));
    CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));xr_xir_value_drop(error);
}
static void vm_case(XrXirArtifact **checked,uint32_t entry,int64_t expected) {
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};XrXirArtifact *lowered=NULL;
    CHECK(xr_xir_compile_lower(*checked,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(*checked);*checked=NULL;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t answer=function_named(module,"answer"),escaped=function_named(module,"escaped");CHECK(answer!=UINT32_MAX);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    XrXirValue value=run_entry(instance,entry,XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
    char stdout_expected[40];int count=snprintf(stdout_expected,sizeof(stdout_expected),"%lld\n",(long long)expected);
    CHECK(count>0 && output.length==(size_t)count && !memcmp(output.bytes,stdout_expected,output.length));
    value=run_entry(instance,answer,XR_XIR_CALL_RETURNED);CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);
    if (escaped!=UINT32_MAX) value=run_entry(instance,escaped,XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    if (escaped!=UINT32_MAX) late_error(&value);
    puts("VM: exact output/answer, Program and Instance retired, owned UTF8/NUL Error PASS");
}
static void wire_word(uint8_t *bytes,uint32_t value) {
    for (unsigned n=0;n<4;++n) bytes[n]=(uint8_t)(value>>(8*n));
}
static void wire_digest(uint8_t *bytes,size_t length) {
    XrSHA256Context context;xr_sha256_init(&context);xr_sha256_update(&context,bytes,32);
    xr_sha256_update(&context,bytes+64,length-64);xr_sha256_final(&context,bytes+32);
}
static void checked_wire_boundary(const XrXirCompileContext *context,uint8_t *packet,size_t length,
    const XrXirModule *module,uint32_t members) {
    uint64_t baseline=held(context);CHECK(length>=64 && XR_XIR_CHECKED_SCHEMA==39 && XR_XIR_CHECKED_CONTRACT==109);
    CHECK(packet[12]==109 && !packet[13] && !packet[14] && !packet[15]);
    uint8_t digest[32];memcpy(digest,packet+32,sizeof(digest));
    XrXirArtifact *output=NULL;wire_word(packet+12,103);wire_digest(packet,length);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(held(context)==baseline);wire_word(packet+12,104);wire_digest(packet,length);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(held(context)==baseline);wire_word(packet+12,105);wire_digest(packet,length);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(held(context)==baseline);wire_word(packet+12,106);wire_digest(packet,length);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(held(context)==baseline);wire_word(packet+12,107);wire_digest(packet,length);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(held(context)==baseline);wire_word(packet+12,109);memcpy(packet+32,digest,sizeof(digest));
    if (!members) return;
    uint32_t value=function_named(module,"value");CHECK(value!=UINT32_MAX);
    const XrXirTypeNode *node=xr_xir_type_node(module->types,module->functions[value].error_type);
    CHECK(node && node->kind==XR_XIR_TYPE_UNION && node->parameter_count==members);
    size_t bytes=12+(size_t)members*4;uint8_t *pattern=owned(context,bytes,1);
    wire_word(pattern,XR_XIR_TYPE_UNION);wire_word(pattern+4,node->parameter_span);wire_word(pattern+8,members);
    for (uint32_t p=0;p<members;++p) wire_word(pattern+12+(size_t)p*4,(uint32_t)node->parameters[p].type);
    size_t position=SIZE_MAX;
    for (size_t p=64;p<=length && bytes<=length-p;++p)
        if (!memcmp(packet+p,pattern,bytes)) {position=p;break;}
    CHECK(position!=SIZE_MAX);
    uint64_t populated=held(context);
    const uint32_t invalid[]={0,16,XR_XIR_TYPE_TUPLE};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        wire_word(packet+position,invalid[i]);wire_digest(packet,length);
        CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)!=XR_XIR_OK && !output);
        CHECK(held(context)==populated);
    }
    wire_word(packet+position,XR_XIR_TYPE_UNION);memcpy(packet+32,digest,sizeof(digest));
    XrXirTypes narrowed=*module->types;XrXirModule raw=*module;
    XrXirTypeNode *nodes=owned(context,narrowed.count,sizeof(*nodes));
    memcpy(nodes,narrowed.nodes,(size_t)narrowed.count*sizeof(*nodes));narrowed.nodes=nodes;raw.types=&narrowed;
    uint32_t index=(uint32_t)module->functions[value].error_type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    nodes[index].parameter_count=members-1;
    CHECK(xr_xir_compile_types_structure_verify(context,&narrowed)==XR_XIR_OK);
    CHECK(xir_compile_local_errors_verify(context,&raw,NULL)==XR_XIR_BAD_TYPE);xr_compile_resources_free(nodes);
    size_t cut=position+12+(size_t)(members-1)*4;CHECK(cut+4<=length);
    uint8_t *shortened=owned(context,length-4,1);
    memcpy(shortened,packet,cut);memcpy(shortened+cut,packet+cut+4,length-cut-4);
    for (unsigned n=0;n<8;++n) shortened[24+n]=(uint8_t)((uint64_t)(length-4-64)>>(8*n));
    wire_digest(shortened,length-4);
    XrXirStatus physical=xr_xir_compile_checked_read(context,shortened,length-4,&output,NULL);
    CHECK(physical!=XR_XIR_OK && !output);
    wire_word(shortened+position+8,members-1);wire_digest(shortened,length-4);
    XrXirStatus semantic=xr_xir_compile_checked_read(context,shortened,length-4,&output,NULL);
    CHECK(semantic==XR_XIR_BAD_TYPE && !output);xr_compile_resources_free(shortened);CHECK(held(context)==populated);
    printf("Checked106 UNION%u: missing physical tail status%u, canonical prefix/body status%u, valid SHA PASS\n",
        members,(unsigned)physical,(unsigned)semantic);
    CHECK(xr_xir_compile_checked_read(context,packet,length,&output,NULL)==XR_XIR_OK && output);
    xr_xir_compile_artifact_free(output);xr_compile_resources_free(pattern);CHECK(held(context)==baseline);
}
static void source_case(const char *text,uint32_t members,const char *tail,XrXirStatus expected,int64_t answer) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext producer={0},receiver={0};producer.limits=receiver.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&producer.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t baseline=held(&producer);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(producer.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"error-union",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&producer,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    size_t length=strlen(text);char *input=xr_malloc(length+1);CHECK(input);memcpy(input,text,length+1);
    XrXirSourceText source={NULL,input,length};XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build_text(&request,&source,&product,&diagnostic);
    memset(input,0xa5,length+1);xr_free(input);xr_compile_session_free(session);
    if (status!=expected && diagnostic.failure.report) {
        XrFrontendReportInfo info={0};CHECK(xr_frontend_report_info(diagnostic.failure.report,&info));
        for (uint32_t r=0;r<info.roots;++r) {
            XrFrontendRoot root={0};CHECK(xr_frontend_report_root(diagnostic.failure.report,r,&root));
            fprintf(stderr,"source status%u stage%u %d:%d %s\n",status,diagnostic.stage,root.primary.line,root.primary.column,root.message);
        }
    }
    CHECK(status==expected && (expected==XR_XIR_OK ? product!=NULL : product==NULL));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    if (product) {
        CHECK(xr_compile_resources_new(&limits,&receiver.resources)==XR_COMPILE_RESOURCE_OK);
        uint64_t received_baseline=held(&receiver);XrXirSourceProductPacketView view={0};
        uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
        CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&view)==XR_XIR_OK);
        uint8_t *packet=owned(&producer,view.length,1);memcpy(packet,view.bytes,view.length);
        XrXirArtifact *checked=NULL;
        CHECK(xr_xir_compile_checked_read(&receiver,packet,view.length,&checked,NULL)==XR_XIR_OK && checked);
        checked_wire_boundary(&receiver,packet,view.length,xr_xir_compile_artifact_module(checked),members);
        memset(packet,0xa5,view.length);xr_compile_resources_free(packet);xr_xir_compile_source_product_free(product);
        CHECK(held(&producer)==baseline);xr_compile_resources_release(producer.resources);producer.resources=NULL;
        packet_members(&receiver,xr_xir_compile_artifact_module(checked),members,tail);
        if (run_vm) vm_case(&checked,entry,answer);
        xr_xir_compile_artifact_free(checked);CHECK(held(&receiver)==received_baseline);xr_compile_resources_release(receiver.resources);
        printf("Source packet members%u bytes%zu: producer retired, complete tail and raw CFG boundary PASS\n",members,length);
    } else {CHECK(held(&producer)==baseline);xr_compile_resources_release(producer.resources);}
}
static XrXirProgram *generation_program(const XrXirSourceGenerationView *view,uint32_t *entry,uint32_t *escaped) {
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(view->result->checked,&closed,NULL, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    *entry=module->declarations->entry_function;*escaped=function_named(module,"escaped");
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);return program;
}
static void history_case(bool erased) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t baseline=held(&context);
    XrXirSourceGenerationConfig source_config={context,"error-union-history",XR_SOURCE_STDLIB,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&source_config,&owner)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
    const char *inputs[]={"enum First{Bad{message:string}}\nenum Second{Bad}\n"
        "fn make() throws(First|Second)->i64{throw First.Bad{message:\"owned é\\0界\"}}\nconst f=make\n",
        "enum Shift{Tag}\nconst shifted=Shift.Tag\nexport fn escaped() throws(First|Second)->i64{return try make()}\n"};
    uint32_t escaped=UINT32_MAX;
    for (unsigned i=0;i<2;++i) {
        size_t length=strlen(inputs[i]);char *bytes=xr_malloc(length+1);CHECK(bytes);memcpy(bytes,inputs[i],length+1);
        if (erased) {
            char *at=strstr(bytes,"First|Second");CHECK(at);memmove(at+5,at+12,strlen(at+12)+1);memcpy(at,"Error",5);length-=7;
        }
        XrXirSourceText input={NULL,bytes,length};uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_generation_prepare_bindings(owner,&input,&serial,&diagnostic);
        memset(bytes,0xa5,length+1);xr_free(bytes);
        if (status!=XR_XIR_OK) fprintf(stderr,"history%u Source%u %d:%d %s\n",i,status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_OK && serial==i+1);
        XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
        uint32_t entry=UINT32_MAX;XrXirProgram *program=generation_program(&view,&entry,&escaped);
        status=xr_xir_compile_source_generation_bind_program(owner,serial,program, NULL);
        if (status!=XR_XIR_OK) fprintf(stderr,"history%u bind%u\n",i,status);
        CHECK(status==XR_XIR_OK);uint64_t runtime_serial=0;
        CHECK(xr_xir_source_generation_prepare_instance(owner,serial,instance,&runtime_serial)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);XrXirValue value=run_entry(instance,entry,XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_generation_publish(instance,runtime_serial)==XR_XIR_CALL_READY);
        CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    }
    CHECK(escaped!=UINT32_MAX);xr_xir_compile_source_generations_free(owner);
    XrXirValue error=run_entry(instance,escaped,XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);late_error(&error);
    CHECK(held(&context)==baseline);xr_compile_resources_release(context.resources);
    puts("Source history: authentic callable union receiving, retired producers and late owned Error PASS");
}
static void inferred_source_cases(void) {
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(B|A)->i64{throw B.Bad}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
}
static void inferred_constraints_cases(void) {
    source_case("enum B{Bad{payload:fn()->i64}}\nenum A{Bad}\nfn value() throws(B|A)->i64{return 42}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum B{Bad{payload:fn()->i64}}\nenum A{Bad}\nfn value() throws(B|A)->i64{return 42}\n"
        "fn wrap<E:Error & Sendable>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(B|A)->i64{throw B.Bad}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "fn folded()->i64{try{return wrap(value)}catch(e:B){return 42}catch(e){return -1}}\n"
        "export fn answer()->i64{return comptime folded()}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
}
static void inferred_residual_cases(void) {
    source_case("enum A{Bad}\nfn value() throws(A)->i64{throw A.Bad}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e:A){return 42}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nfn value()->i64{return 42}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e:A){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum B{Bad}\nenum A{Bad}\nenum C{Bad}\nfn value() throws(B|A|C)->i64{throw C.Bad}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e:C){return 42}catch(e){return -1}}\nprint(answer())\n",3,"C",XR_XIR_OK,42);
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(Error)->i64{throw B.Bad}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(value)}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(B|A)->i64{throw B.Bad}\n"
        "fn wrap<E:Error,F:Error>(anchor:F,callback:fn() throws(E|F)->i64) throws(E|F)->i64{return try callback()}\n"
        "export fn answer()->i64{try{return wrap(A.Bad,value)}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(B|A)->i64{throw B.Bad}\n"
        "struct Box<T>{value:T;wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}}\n"
        "export fn answer()->i64{const box=Box<i64>{value:1};try{return box.wrap(value)}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nfn value()->i64{return 42}\n"
        "fn wrap<E:Error>(callback:fn() throws(E|A)->i64) throws(E|A)->i64{return try callback()}\nwrap(value)\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum B{Bad}\nenum A{Bad}\nfn value() throws(B|A)->i64{throw B.Bad}\n"
        "fn wrap<E:Error,F:Error>(callback:fn() throws(E|F)->i64) throws(E|F)->i64{return try callback()}\ntry wrap(value)\n",0,NULL,XR_XIR_BAD_TYPE,0);
}
static void generic_source_cases(void) {
    source_case("enum A{Bad}\nenum B{Bad}\nfn worker() throws(A|B)->i64{throw B.Bad}\n"
        "export fn answer()->i64{const task:Task<i64,A|B>=go worker();try{return try await task}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\nfn values()->Iterator<i64,A|B>{yield 1;throw B.Bad}\n"
        "export fn answer()->i64{const it=values();try{return try it.next()+try it.next()}catch(e:B){return 42}catch(e){return -1}}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\nenum Tag<E:Error>{Empty}\n"
        "export fn answer()->i64{const n=Tag<Never>.Empty;const a=Tag<A>.Empty;const u=Tag<A|B>.Empty;return 42}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\nfinal class Packet<E:Error>{value:i64;init(value){this.value=value}}\n"
        "export fn answer()->i64{return Packet<Never>(12).value+Packet<A>(10).value+Packet<A|B>(20).value}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
    source_case("final class Packet{value:i64;init(value){this.value=value}}\nconst bad=Packet<i64>(1)\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nenum B{Bad}\ninterface Reader<E:Error>{read() throws(E)->i64}\n"
        "struct Box<E:Error> implements Reader<E>{value:i64;read() throws(E)->i64{return this.value}}\n"
        "export fn answer()->i64{const n=Box<Never>{value:12};const a=Box<A>{value:10};const u=Box<A|B>{value:20};"
        "try{return n.read()+a.read()+u.read()}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\nstruct Box<T>{value:T;pick<E:Error>() throws(E|A)->i64{throw A.Bad}}\n"
        "export fn answer()->i64{const box=Box<i64>{value:7};try{return box.pick<A|B>()}catch(e:A){return 42}catch(e){return -1}}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\ninterface Reader<T>{pick<E:Error>(value:T) throws(E|A)->i64}\n"
        "struct Box implements Reader<i64>{pick<E:Error>(value:i64) throws(E|A)->i64{throw A.Bad}}\n"
        "fn use<T:Reader<i64>>(box:T) throws(A|B)->i64{return try box.pick<A|B>(7)}\n"
        "export fn answer()->i64{try{return use(Box{})}catch(e:A){return 42}catch(e){return -1}}\nprint(answer())\n",0,NULL,XR_XIR_OK,42);
    source_case("enum A{Bad}\nenum B{Bad}\ninterface Reader{read() throws(A)->i64}\n"
        "struct Box implements Reader{read() throws(A|B)->i64{throw B.Bad}}\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\ninterface Reader{pick<T>(value:T)->T}\n"
        "struct Box implements Reader{pick<T>(value:T)->T{return value}}\n"
        "fn use<T:Reader>(box:T){box.pick<A|A>(A.Bad)}\nuse(Box{})\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nstruct Box<E:Error>{value:i64;pick<T>()->i64{return this.value}}\n"
        "const box=Box<A>{value:1}\nbox.pick<A|A>()\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nfn bad<T>(){}\nbad<A|A>()\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nenum Tag<E:Error>{Empty}\nfn bad() throws(Never){throw Tag<Never>.Empty}\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum Tag<E:Error>{Empty}\nconst bad=Tag<Error|i64>.Empty\n",0,NULL,XR_XIR_BAD_TYPE,0);
}
static char *read_source(const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>=0 && !fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)size+1);CHECK(text);CHECK(fread(text,1,(size_t)size,file)==(size_t)size && !fclose(file));text[size]=0;return text;
}
static void emit_case(const char *path,const char *output) {
    char *text=!strcmp(path,"#300") ? source_300() : read_source(path);size_t length=strlen(text);
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);uint64_t baseline=held(&context);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"error-union-emit",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceText source={NULL,text,length};XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_product_build_text(&request,&source,&product,&diagnostic)==XR_XIR_OK);
    memset(text,0xa5,length);xr_free(text);xr_compile_session_free(session);
    const XrXirModule *module=xr_xir_compile_source_product_module(product);
    uint32_t escaped=function_named(module,"escaped"),answer=function_named(module,"answer");CHECK(answer!=UINT32_MAX);
    XrXirCSource code={0};CHECK(xr_xir_compile_source_product_emit(product,"error_union",SIZE_MAX,&code)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    FILE *file=fopen(output,"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
    CHECK(fprintf(file,"\nconst uint32_t error_union_escaped=%uu,error_union_answer=%uu;\n",escaped,answer)>0 && !fclose(file));
    xr_xir_compile_c_source_free(&code);CHECK(held(&context)==baseline);xr_compile_resources_release(context.resources);
}
static void ctfe_source_case(const char *original,uint32_t members,const char *tail) {
    const char suffix[]="print(answer())\n";
    size_t length=strlen(original),suffix_length=sizeof(suffix)-1;
    CHECK(length>=suffix_length && !memcmp(original+length-suffix_length,suffix,suffix_length));
    char *text=xr_malloc(length+64);CHECK(text);memcpy(text,original,length-suffix_length);
    const char folded[]="const folded=comptime answer()\nprint(folded)\n";
    memcpy(text+length-suffix_length,folded,sizeof(folded));
    source_case(text,members,tail,XR_XIR_OK,42);xr_free(text);
}
static void source_cases(const char *fixture) {
    char *text=read_source(fixture);
    source_case(text,7,"Seventh",XR_XIR_OK,42);ctfe_source_case(text,7,"Seventh");xr_free(text);
    text=source_300();source_case(text,300,"E299",XR_XIR_OK,42);ctfe_source_case(text,300,"E299");xr_free(text);
    source_case("fn bad() throws(Error|i64)->i64{return 0}\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("fn bad<E>() throws(Error|E)->i64{return 0}\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nenum B{Bad}\nfn bad(value:A|B){}\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nfn bad<T>(){}\nbad<A|Never>()\n",0,NULL,XR_XIR_BAD_TYPE,0);
    source_case("enum A{Bad}\nenum B{Bad}\nfn wrap<E:Error>() throws(E|A)->i64{throw A.Bad}\n"
        "export fn answer()->i64{var n=0;try{n+=wrap<Never>()}catch(e:A){n+=1};try{n+=wrap<A>()}catch(e:A){n+=10};"
        "try{n+=wrap<Error>()}catch(e){n+=100};try{n+=wrap<A|B>()}catch(e:A){n+=1000}catch(e:B){n+=0};return n}\nprint(answer())\n",
        0,NULL,XR_XIR_OK,1111);
}
#endif
int main(int argc,char **argv) {
    type_cases();puts("Error union: 300th member, substitution, receiving, proof, layout and held failure PASS");
#ifndef XR_UNION_TYPES_ONLY
    if (argc==2 && !strcmp(argv[1],"--infer")) {run_vm=true;inferred_source_cases();inferred_residual_cases();inferred_constraints_cases();return 0;}
    if (argc==4 && !strcmp(argv[1],"--emit")) {emit_case(argv[2],argv[3]);return 0;}
    if (argc==2 && (!strcmp(argv[1],"--history") || !strcmp(argv[1],"--history-erased"))) {history_case(!strcmp(argv[1],"--history-erased"));return 0;}
    CHECK(argc==3 && (!strcmp(argv[1],"--packet-only") || !strcmp(argv[1],"--vm")));
    run_vm=!strcmp(argv[1],"--vm");source_cases(argv[2]);generic_source_cases();
#else
    (void)argc;(void)argv;
#endif
#if defined(XR_UNION_ALLOCATOR)
    CHECK(!fault_live);
#endif
    return 0;
}
