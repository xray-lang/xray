/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_root_interfaces.h - Requirement bounds and authentic implementations
 */
#ifndef XIR_CALLABLE_ROOT_INTERFACES_H
#define XIR_CALLABLE_ROOT_INTERFACES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_implementation.h"
static XrXirStatus bound_implementation(const XrXirCompileContext *context,
    uint32_t flags,bool rooted,XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    XrXirType receiver=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NOMINAL},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=flags}};
    XrXirNominalDeclaration nominal={{"alpha",5},{"Meter",5},1,NULL,0,NULL,0,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}};
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirInterfaceMethod method={{"measure",7},(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),0,0,NULL};
    XrXirInterfaceDeclaration interface={{"alpha",5},{"Measure",7},1,NULL,0,NULL,0,&method,1};
    XrXirInterfaceTable interfaces={&interface,1};XrXirTypes types={nodes,2,&nominals,&interfaces};
    XrXirInstruction init[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7}, {.op=XR_XIR_SLOT_INIT,.args={0,0}}, {.op=XR_XIR_RETURN}};
    XrXirInstruction answer[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction measure[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41}, {.op=XR_XIR_RETURN,.args={1,0}}};
    if(rooted)measure[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64};
    XrXirBlock two={.count=2},three={.count=3};
    XrXirFunction functions[]={
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,answer,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&three,1,init,3,NULL,0},
        {"measure",7,&receiver,1,XR_XIR_I64,&two,1,measure,2,NULL,0}};
    XrXirSourceModule source={"alpha",5,NULL,0,1};
    XrXirFunctionIdentity identities[3]={{0}};
    identities[2].exported=1;identities[2].nominal_owner=1;identities[2].method_kind=XR_XIR_READ_METHOD;
    if(flags&1)identities[2].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirImplementationBinding binding={{0,NULL,0},0,2};
    XrXirImplementation implementation={0,{0,NULL,0},&binding,1};XrXirImplementationTable table={&implementation,1};
    XrXirSlot slot={0,XR_XIR_I64,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=0,.implementations=&table};
    XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &module, output, diagnostic);
}
static XrXirStatus bound_requirement(const XrXirCompileContext *context,uint32_t flags,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=flags};
    XrXirInterfaceMethod method={{"measure",7},XR_XIR_CONSTRUCTED_TYPE_BASE,0,0,NULL};
    XrXirInterfaceDeclaration interface={{"alpha",5},{"Measure",7},1,NULL,0,NULL,0,&method,1};
    XrXirInterfaceTable interfaces={&interface,1};XrXirTypes types={&node,1,NULL,&interfaces};
    XrXirInstruction answer[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirInstruction call[]={{.op=XR_XIR_CALL_REQUIREMENT,.type=XR_XIR_I64,.args={0,1}}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirBlock one={.count=1},two={.count=2};XrXirType parameter=XR_XIR_TYPE_PARAMETER_BASE;uint32_t operand=0;
    XrXirFunction functions[]={
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,answer,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"caller",6,&parameter,1,XR_XIR_I64,&two,1,call,2,&operand,1}};
    XrXirInterfaceApplication application={0,NULL,0};XrXirConstraint constraint={0,&application,1};
    XrXirGeneric generics[3]={{0}};generics[2].constraints=&constraint;generics[2].parameter_count=1;
    XrXirSourceModule source={"alpha",5,NULL,0,1};XrXirFunctionIdentity identities[3]={{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,generics,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &module, output, diagnostic);
}
static XrXirStatus bound_invoke(const XrXirCompileContext *context,uint32_t flags,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=flags};
    XrXirTypes types={&node,1,NULL,NULL};XrXirType parameter=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirInstruction answer[]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirInstruction call[]={
        {.op=XR_XIR_INVOKE_INDIRECT,.type=XR_XIR_I64,.targets={1,2}},
        {.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={2,0}},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7}, {.op=XR_XIR_RETURN,.args={5,0}}};
    XrXirBlock one={.count=1},two={.count=2},blocks[]={{.count=1},{.first=1,.count=2},{.first=3,.count=3}};
    XrXirFunction functions[]={
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,answer,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&init,1,NULL,0},
        {"caller",6,&parameter,1,XR_XIR_I64,blocks,3,call,6,NULL,0}};
    XrXirSourceModule source={"alpha",5,NULL,0,1};XrXirFunctionIdentity identities[3]={{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &module, output, diagnostic);
}
static void bound_interfaces(void) {
    static const uint32_t flags[]={2,4,8,12,3,5,9,13};
    static const bool known[]={false,true,false,true,false,true,false,true};
    static const bool unknown[]={false,false,true,true,false,false,true,true};
    XrXirCompileContext context=bound_owner(bound_caps());XrXirDiagnostic diagnostic={0};
    for(uint32_t n=0;n<8;++n) {
        for(uint32_t rooted=0;rooted<2;++rooted) {
            XrXirArtifact *checked=NULL;bool allowed=!rooted || n%4!=0;
            XrXirStatus status=bound_implementation(&context,flags[n],rooted!=0,&checked,&diagnostic);
            if(status!=(allowed?XR_XIR_OK:XR_XIR_BAD_TYPE))fprintf(stderr,"impl flags%u rooted%u status%u f%u i%u\n",flags[n],rooted,status,diagnostic.function,diagnostic.instruction);
            CHECK(status==(allowed?XR_XIR_OK:XR_XIR_BAD_TYPE) && (checked!=NULL)==allowed);
            if(!allowed)CHECK(diagnostic.function==2);
            xr_xir_compile_artifact_free(checked);
        }
        for(uint32_t invoke=0;invoke<2;++invoke) {
            XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
            XrXirStatus status=invoke?bound_invoke(&context,flags[n],&checked,&diagnostic):bound_requirement(&context,flags[n],&checked,&diagnostic);
            if(status!=XR_XIR_OK)fprintf(stderr,"abstract flags%u invoke%u status%u f%u i%u\n",flags[n],invoke,status,diagnostic.function,diagnostic.instruction);
            CHECK(status==XR_XIR_OK && checked && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
            const XrXirRootEffects *root=xr_xir_effects_root(effects,2);CHECK(root);
            CHECK(root->requires_root==known[n] && root->unresolved==unknown[n]);
            const XrXirRootEffectWitness *k=xr_xir_effects_root_witness(effects,2),*u=xr_xir_effects_unresolved_witness(effects,2);
            CHECK((k!=NULL)==known[n] && (u!=NULL)==unknown[n]);
            if(k)CHECK(k->instruction==0 && k->cause==(invoke?XR_XIR_ROOT_CAUSE_INDIRECT:XR_XIR_ROOT_CAUSE_REQUIREMENT));
            if(u)CHECK(u->instruction==0 && u->cause==(invoke?XR_XIR_ROOT_CAUSE_INDIRECT:XR_XIR_ROOT_CAUSE_REQUIREMENT));
            XrXirRootCauseTrace *trace=NULL;
            CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,2,&trace)==XR_XIR_OK && trace);
            xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
            const XrXirRootEffects *detached=xr_xir_root_cause_trace_facts(trace);
            CHECK(detached && detached->requires_root==known[n] && detached->unresolved==unknown[n]);
            for(uint32_t chain=0;chain<2;++chain) {
                uint32_t count=UINT32_MAX;
                const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,chain!=0,&count);
                bool present=chain?unknown[n]:known[n];
                CHECK(count==(present?1u:0u) && (steps!=NULL)==present);
                if(present)CHECK(steps[0].function==2 && steps[0].instruction==0 &&
                    steps[0].callee==UINT32_MAX && steps[0].slot==UINT32_MAX && steps[0].distance==0 &&
                    steps[0].cause==(invoke?XR_XIR_ROOT_CAUSE_INDIRECT:XR_XIR_ROOT_CAUSE_REQUIREMENT));
            }
            xr_xir_compile_root_cause_trace_free(trace);
        }
    }
    bound_owner_free(&context);
}
#endif // XIR_CALLABLE_ROOT_INTERFACES_H
