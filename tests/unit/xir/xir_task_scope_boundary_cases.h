/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * New BUILT bodies borrow the genuine Source declaration and Construction
 * owners only until the complete checker has independently cloned them. */
#include "xir/xxir_internal.h"
#include "base/xmalloc.h"
typedef struct ScopeCancel {unsigned calls,stop;} ScopeCancel;
static bool scope_cancel_later(const void *pointer) {
    ScopeCancel *state=(ScopeCancel *)pointer;return ++state->calls>=state->stop;
}
static void scope_raw_controls(const XrXirCompileContext *context) {
    XrXirSourceResult source={0};
    CHECK(check(context,"fn worker(){}\nfn run(){scope{go worker()}}",&source,NULL)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    uint32_t run=UINT32_MAX,worker=UINT32_MAX,helper=UINT32_MAX;XrXirType group=XR_XIR_UNIT;
    for(uint32_t f=0;f<original->function_count;++f) {
        const XrXirFunction *function=original->functions+f;
        if(function->name_length==3&&!memcmp(function->name,"run",3))run=f;
        if(function->name_length==6&&!memcmp(function->name,"worker",6))worker=f;
    }
    CHECK(run!=UINT32_MAX&&worker!=UINT32_MAX);
    for(uint32_t f=0;f<original->function_count;++f)
        if(original->declarations->functions[f].cleanup_owner==run+1) {CHECK(helper==UINT32_MAX);helper=f;}
    for(uint32_t i=0;i<original->functions[run].instruction_count;++i)
        if(original->functions[run].instructions[i].op==XR_XIR_SCOPE_BEGIN)group=original->functions[run].instructions[i].type;
    CHECK(helper!=UINT32_MAX&&group!=XR_XIR_UNIT);
    XrXirFunction *functions=xr_malloc((size_t)original->function_count*sizeof(*functions));CHECK(functions);
    XrXirFunctionIdentity *identities=xr_malloc((size_t)original->function_count*sizeof(*identities));CHECK(identities);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    memcpy(identities,original->declarations->functions,(size_t)original->function_count*sizeof(*identities));
    XrXirDeclarations declarations=*original->declarations;declarations.functions=identities;
    XrXirModule built=*original;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.functions=functions;built.declarations=&declarations;
    for(uint32_t aliases=0;aliases<=96;aliases+=96) {
        uint32_t count=aliases+8,last=aliases+1,spawn=aliases+2,join=aliases+3;
        XrXirInstruction *ops=xr_malloc((size_t)count*sizeof(*ops));CHECK(ops);memset(ops,0,(size_t)count*sizeof(*ops));
        ops[0]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.targets={1},.immediate=helper};
        ops[1]=(XrXirInstruction){.op=XR_XIR_SCOPE_BEGIN,.type=group};
        for(uint32_t i=2;i<=last;++i)ops[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=group,.args={i-1}};
        uint32_t operands[]={last};
        ops[spawn]=(XrXirInstruction){.op=XR_XIR_SCOPED_GO,.type=group,.args={0,1},.immediate=worker};
        ops[join]=(XrXirInstruction){.op=XR_XIR_SCOPE_JOIN,.args={last},.targets={3,2}};
        ops[join+1]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=join};
        ops[join+2]=(XrXirInstruction){.op=XR_XIR_THROW,.args={join+1}};
        ops[join+3]=(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={4}};
        ops[join+4]=(XrXirInstruction){.op=XR_XIR_RETURN};
        const XrXirBlock blocks[]={{0,1,0,0},{1,aliases+3,0,1},{join+1,2,0,1},{join+3,1,0,1},{join+4,1,0,0}};
        functions[run].instructions=ops;functions[run].instruction_count=count;functions[run].blocks=blocks;
        functions[run].block_count=5;functions[run].operands=operands;functions[run].operand_count=1;
        uint64_t baseline=held(context->resources);XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_OK&&output);
        xr_xir_compile_artifact_free(output);output=NULL;CHECK(held(context->resources)==baseline);
        ScopeCancel state={0,2};XrCompileRequestOptions options={scope_cancel_later,&state,XR_COMPILE_DEADLINE_NONE,0};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=xir_compile_structure_verify_input(context,(XirEffectInput){&built,construction,&control},&issue);
        if(aliases) {
            CHECK(status==XR_XIR_CANCELLED&&state.calls==state.stop&&issue.function==run&&issue.instruction==spawn);
            CHECK(held(context->resources)==baseline&&!output);
            CHECK(xir_compile_structure_verify_input(context,(XirEffectInput){&built,construction,NULL},&issue)==XR_XIR_OK);
        } else CHECK(status==XR_XIR_OK&&state.calls<state.stop);
        CHECK(held(context->resources)==baseline);
        identities[run].promises|=XR_XIR_FUNCTION_NO_SUSPEND;
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_BAD_TYPE&&!output&&issue.reason==XR_XIR_DIAGNOSTIC_NO_SUSPEND);
        identities[run]=original->declarations->functions[run];
        ops[1].immediate=1;
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_BAD_STRUCTURE&&!output);
        ops[1].immediate=0;ops[join].args[0]=spawn;
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_BAD_TYPE&&!output);
        ops[join].args[0]=last;
        const XrXirInstruction impure[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN}};
        const XrXirBlock impure_block={0,2,0,0};
        functions[helper].instructions=impure;functions[helper].instruction_count=2;
        functions[helper].blocks=&impure_block;functions[helper].block_count=1;
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_BAD_STRUCTURE&&!output);
        functions[helper]=original->functions[helper];
        CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_OK&&output);
        xr_xir_compile_artifact_free(output);CHECK(held(context->resources)==baseline);xr_free(ops);
    }
    xr_free(identities);xr_free(functions);xr_xir_compile_source_result_free(&source);
}
