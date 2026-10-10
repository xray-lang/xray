/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_generic_callable_context.c - Authentic open callable scope
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"

/* keep/nested are the actual wide Source's failing generic definitions,
 * without its unrelated modules. Neither UNKNOWN nor a private bottom grants
 * execution here; concrete specialization still receives full public gates. */
static const char source_generic_definition_text[] =
    "fn identity<T>(value:T)->T{return value}\n"
    "fn keep<A,B>(callback:fn(A)->B)->fn(A)->B{"
    "const retain=identity<fn(A)->B>;return retain(callback)}\n"
    "fn nested<A,B>(transform:fn(fn(A)->B)->fn(A)->B,callback:fn(A)->B)->fn(A)->B{"
    "const apply=invoke<fn(A)->B,fn(A)->B>;return apply(transform,callback)}\n"
    "fn invoke<A,B>(callback:fn(A)->B,value:A)->B{return callback(value)}\n"
    "fn resultShape<A,B>(callback:fn(A)->A)->fn(A)->A{return callback}\n"
    "fn refShape<A,B>(callback:fn(ref A)->B)->fn(ref A)->B{return callback}\n"
    "fn atomicShape<T:AtomicValue>(callback:fn(Atomic<T>)->T)->fn(Atomic<T>)->T{return callback}\n"
    "fn plain(value:i64)->i64{return value+1}\n"
    "export fn run()->i64{const callback=keep<i64,i64>(plain);"
    "const again=nested<i64,i64>(keep<i64,i64>,callback);return again(41)}\nprint(run())\n";

/* These Source controls traverse the complete existing public checker.
 * The definition-local structural helper is never a replacement for real
 * result-role/Unit-use, generic-call or body recipe admission. */
static void source_generic_definition_source_negative(const XrXirCompileContext *context,const char *text) {
    XrCompileResourceStats baseline=library_compile_stats(context);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-generic-definition-negative",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult result={0};
    XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    CHECK(status==XR_XIR_BAD_TYPE && !result.checked);
    xr_xir_compile_source_result_free(&result);xr_compile_resources_free(failure);
    CHECK(library_compile_stats(context).live_bytes==baseline.live_bytes);
}

static uint32_t source_generic_definition_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}

/* The invalid views are explicit negative input. They borrow this real
 * Source pool and allocate their copied generic table on this same ledger.
 * A failed comparison must preserve all input descriptors and live bytes. */
static void source_generic_definition_negative(const XrXirArtifact *artifact,uint32_t keep) {
    const XrXirModule *module=&artifact->module;const XrXirCompileContext *context=&artifact->context;
    XrXirType callback=module->functions[keep].parameters[0];
    XirEffectCallableBound request={module->types,module->types,NULL,0,callback,callback};
    XrCompileResourceStats baseline=library_compile_stats(context);
    CHECK(xir_effect_callable_bound_matches(context,&request)==XR_XIR_BAD_TYPE);
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_OK);
    uint32_t shape=source_generic_definition_function(module,"resultShape");
    request.actual=module->functions[shape].parameters[0];
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_TYPE);
    shape=source_generic_definition_function(module,"refShape");
    request.actual=module->functions[shape].parameters[0];
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_TYPE);
    request.actual=callback;
    CHECK(xir_effect_callable_bound_matches_definition(context,NULL,keep,&request)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effect_callable_bound_matches_definition(context,module,UINT32_MAX,&request)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirTypes foreign=*module->types;request.destination=&foreign;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_STRUCTURE);
    request.destination=module->types;request.arguments=&callback;request.argument_count=1;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_STRUCTURE);
    request.arguments=NULL;request.argument_count=0;
    uint64_t bytes=(uint64_t)module->function_count*sizeof(*module->generics);
    CHECK(bytes<=SIZE_MAX);XrXirGeneric *generics=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)bytes,(void **)&generics)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(context->resources,bytes)==XR_COMPILE_RESOURCE_OK);
    memcpy(generics,module->generics,(size_t)bytes);
    XrXirModule invalid=*module;invalid.generics=generics;
    XrXirConstraint constraints[2]={module->generics[keep].constraints[0],module->generics[keep].constraints[1]};
    constraints[0].markers=~XR_XIR_CONSTRAINT_MASK;generics[keep].constraints=constraints;
    size_t attempts=source_program_compile_attempts;
    CHECK(xir_effect_callable_bound_matches_definition(context,&invalid,keep,&request)==XR_XIR_BAD_TYPE);
    CHECK(source_program_compile_attempts==attempts); /* constraint refusal never reaches identity allocation */
    generics[keep]=module->generics[keep];generics[keep].constraints=NULL;
    attempts=source_program_compile_attempts;
    CHECK(xir_effect_callable_bound_matches_definition(context,&invalid,keep,&request)==XR_XIR_BAD_STRUCTURE);
    CHECK(source_program_compile_attempts==attempts);
    generics[keep]=module->generics[keep];
    request.declared=request.actual=(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+2);
    attempts=source_program_compile_attempts;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_TYPE);
    CHECK(source_program_compile_attempts==attempts); /* out-of-scope bare binder: no shape/identity allocation */
    shape=source_generic_definition_function(module,"atomicShape");
    /* The declared callback is valid under keep. The second query must still
     * rebuild Atomic(PARAM0)'s missing constraint; prior pair storage grants
     * no facts. A rejected request releases its bytes before same-owner retry. */
    XrCompileResourceStats sequence_baseline=library_compile_stats(context);
    XrXirType atomic_callback=module->functions[shape].parameters[0];
    request.declared=callback;request.actual=atomic_callback;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_TYPE);
    CHECK(library_compile_stats(context).live_bytes==sequence_baseline.live_bytes);
    request.actual=callback;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_OK);
    CHECK(library_compile_stats(context).live_bytes==sequence_baseline.live_bytes);
    request.declared=request.actual=atomic_callback;
    CHECK(xir_effect_callable_bound_matches_definition(context,module,shape,&request)==XR_XIR_OK);
    /* Its Atomic(PARAM0) is invalid under keep's unconstrained PARAM0. Full
     * type-use proof must refuse even though declared and actual IDs agree. */
    CHECK(xir_effect_callable_bound_matches_definition(context,module,keep,&request)==XR_XIR_BAD_TYPE);
    xr_compile_resources_free(generics);
    CHECK(library_compile_stats(context).live_bytes==baseline.live_bytes);
    CHECK(module->generics[keep].parameter_count==2 && !module->generics[keep].constraints[0].markers);
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,callback);
    CHECK(signature && signature->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED && signature->parameter_span==2);
}

static void source_generic_definition_inspect(const XrXirArtifact *artifact) {
    const XrXirModule *module=&artifact->module;
    uint32_t keep=source_generic_definition_function(module,"keep");
    CHECK(module->generics && module->generics[keep].parameter_count==2);
    const XrXirFunction *body=&module->functions[keep];CHECK(body->parameter_count==1);
    const XrXirTypeNode *parameter=xr_xir_callable_signature(module->types,body->parameters[0]);
    CHECK(parameter && parameter->parameter_span==2 && parameter->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    bool actual_indirect=false;
    for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_CALL_INDIRECT) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->args[1]==1 && op->args[0]<body->operand_count);
        XrXirType actual=xr_xir_operand_type(body,body->operands[op->args[0]]);
        const XrXirTypeNode *callee=xr_xir_callable_signature(module->types,
            xr_xir_operand_type(body,(uint32_t)op->immediate));
        CHECK(callee && callee->parameter_count==1 && callee->parameters[0].type==actual);
        CHECK(actual==body->parameters[0]);actual_indirect=true;
    }
    CHECK(actual_indirect);source_generic_definition_negative(artifact,keep);
}

/* Every physical fail-at trial and all six resource boundaries replay this
 * exact eight-stage operation under its one finite owner. Producer death and
 * all public packet, specialization and Lowered proof gates remain complete. */
static XrXirStatus source_generic_definition_operation(const XrXirCompileContext *context,void *opaque) {
    bool inspect=*(bool *)opaque;XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-generic-callable-context",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,source_generic_definition_text,sizeof(source_generic_definition_text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirArtifact *read=NULL,*instance=NULL,*reloaded=NULL,*lowered=NULL;
    XrXirCheckedPacket first={0},second={0};XrXirDiagnostic location={0};uint32_t phase=1;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if(status==XR_XIR_OK && inspect) {
        source_generic_definition_inspect(result.checked);
        source_generic_definition_source_negative(context,
            "fn unused<T:AtomicValue>(a:Atomic<T>,v:T)->T{return a.fetchAdd(v)}\n");
        source_generic_definition_source_negative(context,
            "fn id<T>(x:T)->T{return x}\nconst f=id<()>\n");
    }
    if(status==XR_XIR_OK){phase=2;status=xr_xir_compile_checked_write(result.checked,&first,&location);}
    xr_xir_compile_source_result_free(&result);
    if(status==XR_XIR_OK){phase=3;status=xr_xir_compile_checked_read(context,first.bytes,first.length,&read,&location);}
    if(status==XR_XIR_OK){phase=4;status=xr_xir_compile_specialize(read,&instance,&location);}
    xr_xir_compile_artifact_free(read);read=NULL;
    if(status==XR_XIR_OK){phase=5;status=xr_xir_compile_checked_write(instance,&second,&location);}
    xr_xir_compile_artifact_free(instance);instance=NULL;
    if(status==XR_XIR_OK){phase=6;status=xr_xir_compile_checked_read(context,second.bytes,second.length,&reloaded,&location);}
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK){phase=7;status=xr_xir_compile_lower(reloaded,&target,&lowered,&location);}
    xr_xir_compile_artifact_free(reloaded);reloaded=NULL;
    if(status==XR_XIR_OK){phase=8;status=xr_xir_compile_artifact_verify(lowered,&location);}
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX && status!=XR_XIR_BUDGET)
        fprintf(stderr,"generic-callable-context phase%u status%u %d:%d %s f%u/b%u/i%u\n",phase,(unsigned)status,
            diagnostic.line,diagnostic.column,diagnostic.message,location.function,location.block,location.instruction);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);xr_xir_compile_artifact_free(instance);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&first);xr_xir_compile_checked_packet_free(&second);
    xr_compile_resources_free(failure);return status;
}

int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--compiler")));
    bool inspect=argc==1;
    if(!inspect)library_compile_operation_cases("source-generic-callable-context",source_generic_definition_operation,&inspect);
    else {
        LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        CHECK(source_generic_definition_operation(&owner.context,&inspect)==XR_XIR_OK);library_compile_owner_drop(&owner);
    }
    library_compile_observer_free();return 0;
}
