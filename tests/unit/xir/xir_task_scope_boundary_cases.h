/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * New BUILT bodies borrow the genuine Source declaration and Construction
 * owners only until the complete checker has independently cloned them. */
#include "xir/xxir_internal.h"
#include "xir/xxir_view_prepare_internal.h"
#include "base/xmalloc.h"
typedef struct ScopeCancel {unsigned calls,stop;} ScopeCancel;
static bool scope_cancel_later(const void *pointer) {
    ScopeCancel *state=(ScopeCancel *)pointer;return ++state->calls>=state->stop;
}
static void scope_normalized_controls(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(artifact);
    bool normalized=false;
    for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            normalized|=module->functions[f].instructions[i].op==XR_XIR_VIEW_CLEANUP;
    CHECK(normalized);uint64_t baseline=held(context->resources);
    ScopeCancel state={0,2};XrCompileRequestOptions options={scope_cancel_later,&state,XR_COMPILE_DEADLINE_NONE,0};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirDiagnostic issue={0};
    CHECK(xir_compile_view_prepare_ordinary(context,(XirEffectInput){module,construction,&control},&issue)==XR_XIR_CANCELLED);
    CHECK(issue.function<module->function_count&&issue.instruction<module->functions[issue.function].instruction_count);
    CHECK(held(context->resources)==baseline);
    CHECK(xir_compile_view_prepare_ordinary(context,(XirEffectInput){module,construction,NULL},&issue)==XR_XIR_OK);
    CHECK(held(context->resources)==baseline);
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
            state.calls=0;control=(XrCompileRequestControl){0};
            CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
            CHECK(xir_compile_view_prepare_ordinary(context,(XirEffectInput){&built,construction,&control},&issue)==XR_XIR_CANCELLED);
            CHECK(issue.function==run&&issue.instruction==spawn&&held(context->resources)==baseline);
            CHECK(xir_compile_view_prepare_ordinary(context,(XirEffectInput){&built,construction,NULL},&issue)==XR_XIR_OK);
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
static void scope_typed_exit_controls(const XrXirCompileContext *context) {
    const char text[]="enum Failure{Bad}\nfn worker() throws(Failure){throw Failure.Bad}\n"
        "fn run() throws(Failure){try scope{go worker()}}";
    XrXirSourceResult source={0};CHECK(check(context,text,&source,NULL)==XR_XIR_OK);
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
        if(original->declarations->functions[f].cleanup_owner==run+1){CHECK(helper==UINT32_MAX);helper=f;}
    for(uint32_t i=0;i<original->functions[run].instruction_count;++i)
        if(original->functions[run].instructions[i].op==XR_XIR_SCOPE_BEGIN)group=original->functions[run].instructions[i].type;
    CHECK(helper!=UINT32_MAX&&group!=XR_XIR_UNIT);
    XrXirFunction *functions=xr_malloc((size_t)original->function_count*sizeof(*functions));CHECK(functions);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    XrXirModule built=*original;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.functions=functions;
    XrXirInstruction ops[]={
        {.op=XR_XIR_CLEANUP_REGISTER,.targets={1},.immediate=helper},
        {.op=XR_XIR_SCOPE_BEGIN,.type=group},
        {.op=XR_XIR_SCOPED_GO,.type=group,.args={0,1},.immediate=worker},
        {.op=XR_XIR_SCOPE_JOIN,.args={1},.targets={3,2}},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=3},
        {.op=XR_XIR_THROW,.args={4}},
        {.op=XR_XIR_CLEANUP_LEAVE,.targets={4}},
        {.op=XR_XIR_RETURN}};
    const XrXirBlock blocks[]={{0,1,0,0},{1,3,0,1},{4,2,0,1},{6,1,0,1},{7,1,0,0}};
    uint32_t operands[]={1};functions[run].instructions=ops;functions[run].instruction_count=8;
    functions[run].blocks=blocks;functions[run].block_count=5;functions[run].operands=operands;functions[run].operand_count=1;
    uint64_t baseline=held(context->resources);XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
    CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_OK&&output);
    xr_xir_compile_artifact_free(output);output=NULL;CHECK(held(context->resources)==baseline);
    const XrXirBlock unjoined[]={{0,1,0,0},{1,3,0,1}};
    ops[3]=(XrXirInstruction){.op=XR_XIR_RETURN};functions[run].instruction_count=4;
    functions[run].blocks=unjoined;functions[run].block_count=2;functions[run].error_type=XR_XIR_NEVER;
    CHECK(xr_xir_compile_check_v2(context,&built,construction,&output,&issue)==XR_XIR_BAD_TYPE&&!output);
    CHECK(issue.function==run&&issue.instruction==3&&held(context->resources)==baseline);
    xr_free(functions);xr_xir_compile_source_result_free(&source);
}
static void scope_cleanup_error_controls(const XrXirCompileContext *context) {
    const char *text="enum Body{Failed}\nenum Child{Failed}\n"
        "fn bodyError()throws(Body){throw Body.Failed}\n"
        "fn worker()throws(Child){throw Child.Failed}\n"
        "fn run()throws(Body|Child){try{try scope{go worker();throw Body.Failed}}"
        "catch(error:Body){throw error}catch(error:Child){throw error}}";
    XrXirSourceResult source={0};CHECK(check(context,text,&source,NULL)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    uint32_t run=UINT32_MAX;XrXirType body_error=XR_XIR_UNIT;
    for(uint32_t f=0;f<original->function_count;++f) {
        const XrXirFunction *function=original->functions+f;
        if(function->name_length==3&&!memcmp(function->name,"run",3))run=f;
        if(function->name_length==9&&!memcmp(function->name,"bodyError",9))body_error=function->error_type;
    }
    CHECK(run!=UINT32_MAX&&body_error!=XR_XIR_UNIT);
    const XrXirFunction *actual=original->functions+run;
    uint32_t producer=UINT32_MAX,branch=UINT32_MAX,owner=UINT32_MAX;
    for(uint32_t i=0;i<actual->instruction_count;++i) {
        CHECK(actual->instructions[i].op!=XR_XIR_SCOPE_JOIN);
        if(actual->instructions[i].op==XR_XIR_CLEANUP_ERROR){CHECK(producer==UINT32_MAX);producer=i;}
        if(actual->instructions[i].op==XR_XIR_BRANCH)branch=i;
    }
    CHECK(producer!=UINT32_MAX&&branch!=UINT32_MAX);
    for(uint32_t b=0;b<actual->block_count;++b)
        if(producer>=actual->blocks[b].first&&producer-actual->blocks[b].first<actual->blocks[b].count)owner=b;
    CHECK(owner!=UINT32_MAX);
    uint32_t target=actual->instructions[producer].targets[0],landing=actual->blocks[target].first;
    CHECK(actual->instructions[landing].op==XR_XIR_INVOKE_ERROR);
    XrXirFunction *functions=xr_malloc((size_t)original->function_count*sizeof(*functions));CHECK(functions);
    XrXirInstruction *ops=xr_malloc((size_t)actual->instruction_count*sizeof(*ops));CHECK(ops);
    XrXirBlock *blocks=xr_malloc((size_t)actual->block_count*sizeof(*blocks));CHECK(blocks);
    uint32_t *operands=xr_malloc(((size_t)actual->operand_count+2)*sizeof(*operands));CHECK(operands);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    XrXirModule built=*original;built.stage=XR_XIR_BUILT;built.provenance=NULL;built.functions=functions;
    for(unsigned mode=0;mode<8;++mode) {
        functions[run]=*actual;memcpy(ops,actual->instructions,(size_t)actual->instruction_count*sizeof(*ops));
        memcpy(blocks,actual->blocks,(size_t)actual->block_count*sizeof(*blocks));
        memcpy(operands,actual->operands,(size_t)actual->operand_count*sizeof(*operands));
        functions[run].instructions=ops;functions[run].blocks=blocks;functions[run].operands=operands;
        if(mode==1)functions[run].error_type=body_error;
        if(mode==2){ops[producer].immediate=blocks[owner].frontier;blocks[target].frontier=blocks[owner].frontier;}
        if(mode==3)ops[landing].immediate=0;
        if(mode==4)ops[landing]=(XrXirInstruction){.op=XR_XIR_ERROR_ERASE,.type=XR_XIR_ERROR,.args={ops[producer].args[0]}};
        if(mode==5)ops[branch].targets[1]=target;
        if(mode==6) {
            operands[actual->operand_count]=owner;operands[actual->operand_count+1]=ops[producer].args[0];
            functions[run].operand_count+=2;
            ops[landing]=(XrXirInstruction){.op=XR_XIR_PHI,.type=XR_XIR_ERROR,.args={actual->operand_count,2}};
        }
        if(mode==7)ops[landing].op=XR_XIR_INVOKE_RESULT;
        uint64_t baseline=held(context->resources);XrXirArtifact *output=NULL;XrXirDiagnostic issue={0};
        XrXirStatus expected=mode==0?XR_XIR_OK:(mode==1||mode==4||mode==6)?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xr_xir_compile_check_v2(context,&built,construction,&output,&issue);
        if(status!=expected)fprintf(stderr,"cleanup control %u actual%u expected%u f%u b%u i%u\n",
            mode,status,expected,issue.function,issue.block,issue.instruction);
        CHECK(status==expected&&!!output==(mode==0));
        if(mode)CHECK(issue.function==run);
        xr_xir_compile_artifact_free(output);CHECK(held(context->resources)==baseline);
    }
    xr_free(operands);xr_free(blocks);xr_free(ops);xr_free(functions);xr_xir_compile_source_result_free(&source);
}
static void scope_receiving_controls(const XrXirCompileContext *context) {
    char text[8192];size_t length=0;
    const char *head="fn worker(){}\nfn large(flag:bool){scope{go worker();\n";
    memcpy(text,head,strlen(head));length=strlen(head);
    for(uint32_t i=0;i<80;++i) {
        const char *branch="if(flag){go worker()}\n";
        CHECK(strlen(branch)<sizeof(text)-length);memcpy(text+length,branch,strlen(branch));length+=strlen(branch);
    }
    CHECK(4<sizeof(text)-length);memcpy(text+length,"}}\n",4);
    XrXirSourceResult source={0};CHECK(check(context,text,&source,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source.checked);
    uint32_t large=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==5&&!memcmp(module->functions[f].name,"large",5))large=f;
    CHECK(large!=UINT32_MAX);uint64_t baseline=held(context->resources);
    ScopeCancel state={0,2};XrCompileRequestOptions options={scope_cancel_later,&state,XR_COMPILE_DEADLINE_NONE,0};
    XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirEffects *effects=NULL;XrXirDiagnostic issue={0};
    CHECK(xir_compile_verify_owned_effects_input(context,(XirEffectInput){module,construction,&control},&effects,&issue)==XR_XIR_CANCELLED);
    CHECK(!effects&&issue.function==large&&issue.instruction<module->functions[large].instruction_count&&held(context->resources)==baseline);
    CHECK(xir_compile_verify_owned_effects_input(context,(XirEffectInput){module,construction,NULL},&effects,&issue)==XR_XIR_OK&&effects);
    xr_xir_compile_effects_free(effects);CHECK(held(context->resources)==baseline);
    XirSourceFacts *facts=NULL;CHECK(xir_source_facts_acquire(source.checked,&facts,&issue,NULL)==XR_XIR_OK&&facts);
    state.calls=0;control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_compile_verify_source_effects_input(context,(XirEffectInput){module,construction,&control},xir_source_facts_effects(facts),&issue)==XR_XIR_CANCELLED);
    CHECK(issue.function==large&&issue.instruction<module->functions[large].instruction_count);
    CHECK(xir_compile_verify_source_effects_input(context,(XirEffectInput){module,construction,NULL},xir_source_facts_effects(facts),&issue)==XR_XIR_OK);
    xir_source_facts_release(facts);CHECK(held(context->resources)==baseline);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,&issue,NULL)==XR_XIR_OK&&closed);
    baseline=held(context->resources);state.calls=0;control=(XrCompileRequestControl){0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&control,&lowered,&issue)==XR_XIR_CANCELLED&&!lowered);
    CHECK(held(context->resources)==baseline);
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,&issue)==XR_XIR_OK&&lowered);
    xr_xir_compile_artifact_free(lowered);CHECK(held(context->resources)==baseline);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&source);
}
