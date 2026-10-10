/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_conditional_helpers.c - Literal contextual dependency probes
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_compile_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#define XR_XIR_EFFECT_CONTEXT_TESTS 1
#include "xir/xxir_effects.c"
#include "xir_root_conditional_raw_fixture.h"
#include "xir_construction_fixture.h"
#include "xir_invocation_core_cases.h"
#include "xir_invocation_rotation_cases.h"
#include "xir_invocation_deferred_cases.h"
#include "xir_invocation_lowered_context_cases.h"
#include "xir_invocation_publish_cases.h"
#include "xir_lowered_snapshot_cases.h"
#include "xir_invocation_returned_producer_cases.h"

static XrXirStatus conditional_classify(const XrXirCompileContext *context, uint32_t mode, bool oracle) {
    ConditionalRawFixture f;conditional_raw_fixture(&f,mode);
    XrXirStatus status=xr_xir_compile_implementations_verify(context,&f.module);
    EffectContextResolved *resolved=NULL;EffectOrdinaryContexts *contexts=NULL;EffectContextOwner *execution=NULL;
    EffectContextRequest request={&f.module,&f.types,NULL,0,2,0};uint32_t selected=UINT32_MAX;
    if (status==XR_XIR_OK) status=effect_context_resolve(context,&request,&resolved);
    if (status==XR_XIR_OK) {
        if (oracle) CHECK(resolved && resolved->known && resolved->target==1 &&
            resolved->family==1 && resolved->argument_count==1 && resolved->arguments[0]==(XrXirType)256);
        status=effect_ordinary_classify(context,&f.module,resolved,&contexts,&selected);
    }
    if (status==XR_XIR_OK) {
        /* A real caller edge supplies the precise actual while every original
         * declaration/advertisement remains untouched. Build the complete
         * execution owner, including its deep seed and full domain bridge. */
        XrXirFunction functions[5];memcpy(functions,f.functions,sizeof(f.functions));
        XrXirFunctionIdentity identities[5]={0};memcpy(identities,f.identities,sizeof(f.identities));
        XrXirGeneric generics[5]={0};memcpy(generics,f.generics,sizeof(f.generics));
        XrXirType parameters[2]={(XrXirType)256,(XrXirType)259};uint32_t operands[2]={0,1};
        XrXirInstruction actual_ops[2]={{.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,2},.immediate=2},
            {.op=XR_XIR_RETURN,.args={2,0}}};
        functions[4]=(XrXirFunction){.name="precise",.name_length=7,.parameters=parameters,.parameter_count=2,
            .result=XR_XIR_I64,.instructions=actual_ops,.instruction_count=2,.operands=operands,.operand_count=2};
        XrXirDeclarations declarations=f.declarations;declarations.functions=identities;
        XrXirModule module=f.module;module.functions=functions;module.function_count=5;
        module.generics=generics;module.declarations=&declarations;
        status=effect_context_owner_build(context,&module,NULL,&execution);
        if (status==XR_XIR_OK) {
            uint32_t caller_index=(uint32_t)execution->dense->functions[4].instructions[0].immediate;
            CHECK(caller_index<execution->dense->count);
            uint32_t ordinary=execution->dense->nodes[caller_index].ordinary;
            uint32_t target=(uint32_t)execution->ordinary->functions[ordinary].instructions[0].immediate;
            const XrXirFunction *caller=&execution->dense->functions[caller_index];
            XrXirInstruction op=caller->instructions[0];uint8_t fixed[4]={0};
            execution->dense->terms.remaining=context;
            EffectDenseRequest dense={.contexts=execution->ordinary,.execution=execution->dense,
                .selected=target,.caller_index=caller_index,.actual_types=&execution->dense->terms.types,
                .caller=caller,.instruction=&op,.fixed=fixed,.value_count=4};
            EffectDenseVector vector={0};status=effect_dense_build(context,&dense,&vector);
            if (status==XR_XIR_OK && oracle) {
                CHECK(vector.terms==&execution->dense->terms && vector.count==2 &&
                    vector.physical[0]==(XrXirType)256 &&
                    vector.physical[1]==(mode==1 || mode==3 ? (XrXirType)257 : (XrXirType)259));
                CHECK(execution->dense->terms.types.nominals!=f.types.nominals &&
                    execution->dense->terms.types.interfaces!=f.types.interfaces);
                const XrXirType *occupied=vector.physical;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_STRUCTURE &&
                    vector.terms==&execution->dense->terms && vector.physical==occupied && vector.count==2);
                vector=(EffectDenseVector){0};fixed[1]=1;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_OK && vector.physical[1]==(XrXirType)257);
                vector=(EffectDenseVector){0};fixed[1]=0;
                dense.actual_types=&f.types;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_STRUCTURE && !vector.terms && !vector.physical && !vector.count);
                XrXirTypes foreign=execution->dense->terms.types;dense.actual_types=&foreign;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_STRUCTURE && !vector.terms && !vector.physical && !vector.count);
                dense.actual_types=&execution->dense->terms.types;
                XrXirFunction foreign_caller=*caller;dense.caller=&foreign_caller;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_STRUCTURE && !vector.terms && !vector.physical && !vector.count);
                dense.caller=caller;dense.execution=execution->ordinary;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_STRUCTURE && !vector.terms && !vector.physical && !vector.count);
                dense.execution=execution->dense;
                XrXirType *actual_parameters=(XrXirType *)caller->parameters;
                XrXirType saved=actual_parameters[0];actual_parameters[0]=XR_XIR_I64;
                CHECK(effect_dense_build(context,&dense,&vector)==XR_XIR_BAD_TYPE && !vector.terms && !vector.physical && !vector.count);
                actual_parameters[0]=saved;
            }
        }
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(contexts && contexts->count==5 && selected==4);
        const XrXirFunctionEffectContract *base=&contexts->uses.contracts[1];
        const XrXirFunctionEffectContract *closed=&contexts->uses.contracts[selected];
        CHECK(base->parameters[1].kind==2 && base->parameters[1].uses==2);
        CHECK(closed->parameters[1].kind==(mode==1 || mode==3 ? 0u : 1u));
        CHECK(closed->parameters[1].uses==(mode==1 || mode==3 ? 8u : 2u));
        const XrXirInstruction *requirement=&contexts->functions[selected].instructions[0];
        CHECK(requirement->op==XR_XIR_CALL && requirement->immediate==0 && !requirement->targets[0]);
        CHECK(contexts->terms.types.nominals && contexts->terms.types.interfaces &&
            contexts->terms.types.nominals->declarations[0].name.bytes!=f.nominal.name.bytes &&
            contexts->terms.types.interfaces->declarations[0].methods!=f.interface.methods);
        memset(&f,0,sizeof(f));
        CHECK(!memcmp(contexts->terms.types.nominals->declarations[0].name.bytes,"Runner",6));
        CHECK(contexts->nodes[selected].declaration==1 && contexts->nodes[selected].arguments[0]==(XrXirType)256);
        CHECK(contexts->uses.contracts[0].parameters[1].kind==(mode==1 || mode==3 ? 0u : 1u));
    }
    effect_context_owner_free(execution);effect_ordinary_free(contexts);effect_context_free(resolved);return status;
}

static XrXirStatus conditional_scalar(const XrXirCompileContext *context, bool oracle) {
    XrXirTypeNode nodes[4]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=8},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=4},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=12}};
    XrXirTypes types={nodes,4,NULL,NULL};EffectTerms terms={.remaining=context};
    XrXirStatus status=effect_terms_seed(&terms,&types);
    XrXirInstruction ops[6]={
        {.op=XR_XIR_COPY,.type=(XrXirType)256,.args={0,0}},
        {.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)256,.args={2,0}},
        {.op=XR_XIR_LOCAL_WRITE,.args={3,1}},
        {.op=XR_XIR_LOCAL_READ,.type=(XrXirType)256,.args={3,0}},
        {.op=XR_XIR_PHI,.type=(XrXirType)256,.args={0,4}},
        {.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={0,0}}};
    uint32_t operands[4]={0,2,1,1};
    XrXirType parameters[2]={(XrXirType)257,(XrXirType)258};
    XrXirFunction function={.parameters=parameters,.parameter_count=2,.instructions=ops,
        .instruction_count=6,.operands=operands,.operand_count=4};
    XrXirType declared[8]={(XrXirType)256,(XrXirType)256,(XrXirType)256,(XrXirType)256,
        XR_XIR_UNIT,(XrXirType)256,(XrXirType)256,(XrXirType)256};
    XrXirType values[8]={(XrXirType)257,(XrXirType)258,(XrXirType)256,(XrXirType)256,
        XR_XIR_UNIT,(XrXirType)256,(XrXirType)256,(XrXirType)256};
    uint8_t fixed[8]={0};
    EffectScalarFlow flow={&terms,&function,NULL,declared,values,fixed,8,false,NULL};
    if (status==XR_XIR_OK) status=effect_scalar_flow(&flow);
    if (status==XR_XIR_OK && oracle) {
        CHECK(values[2]==(XrXirType)257 && values[3]==(XrXirType)258 &&
            values[5]==(XrXirType)258 && values[6]==(XrXirType)258 && values[7]==(XrXirType)256);
        CHECK(ops[0].type==(XrXirType)257 && ops[3].type==(XrXirType)258 && ops[4].type==(XrXirType)258);
        CHECK(ops[5].op==XR_XIR_FUNCTION_WEAKEN && ops[5].type==(XrXirType)256);
        CHECK(fixed[7] && !fixed[2] && !fixed[6]);
    }
    effect_terms_free(&terms);return status;
}

static XrXirStatus conditional_forest(const XrXirCompileContext *context, bool oracle) {
    XrCompileResourceStats begin=rp_stats(context);
    uint32_t phase=0;
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,4);
    XrXirStatus status=xr_xir_compile_implementations_verify(context,&fixture.module);
    EffectContextResolved *resolved=NULL;EffectOrdinaryContexts *contexts=NULL;
    EffectContextForest *forest=NULL;XrXirRootCauseTrace *trace=NULL;
    EffectContextRequest request={&fixture.module,&fixture.types,NULL,0,2,0};
    uint32_t selected=UINT32_MAX;
    if (status==XR_XIR_OK) { phase=1;status=effect_context_resolve(context,&request,&resolved); }
    if (status==XR_XIR_OK)
        { phase=2;status=effect_ordinary_classify(context,&fixture.module,resolved,&contexts,&selected); }
    if (status==XR_XIR_OK) { phase=3;status=effect_context_forest_seal(context,&fixture.module,contexts,&forest); }
    effect_ordinary_free(contexts);effect_context_free(resolved);memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) { phase=4;status=effect_context_forest_trace(context,forest,2,&trace); }
    if (status==XR_XIR_OK && oracle) {
        uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,false,&count);
        CHECK(forest && forest->count==5 && selected==4 && count==3 && steps);
        CHECK(steps[0].function==2 && steps[0].instruction==0 && steps[0].callee==1 &&
            steps[0].slot==UINT32_MAX && steps[0].distance==2 && steps[0].cause==6);
        CHECK(steps[1].function==1 && steps[1].instruction==0 && steps[1].callee==0 &&
            steps[1].slot==UINT32_MAX && steps[1].distance==1 && steps[1].cause==5);
        CHECK(steps[2].function==0 && steps[2].instruction==1 && steps[2].callee==UINT32_MAX &&
            steps[2].slot==0 && steps[2].distance==0 && steps[2].cause==1);
        CHECK(forest->terms.types.nominals && forest->terms.types.interfaces &&
            !memcmp(forest->terms.types.nominals->declarations[0].name.bytes,"Runner",6) &&
            !memcmp(forest->terms.types.interfaces->declarations[0].name.bytes,"Apply",5));
        CHECK(forest->certificates[2].next==4 && forest->certificates[2*forest->count+4].next==0);
        CHECK(forest->nodes[4].argument_count==1 && forest->nodes[4].arguments[0]==(XrXirType)256 &&
            forest->nodes[4].parameter_count==2 && forest->nodes[4].physical_types[0]==(XrXirType)256 &&
            forest->nodes[4].physical_types[1]==(XrXirType)257);
        XrXirRootCauseTrace *rejected=NULL;
        XrXirRootEffectWitness saved=forest->witnesses[2*forest->count+4];
        forest->witnesses[2*forest->count+4].callee=1;
        CHECK(effect_context_forest_trace(context,forest,2,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
        forest->witnesses[2*forest->count+4]=saved;
        EffectContextCertificate certificate=forest->certificates[2];
        forest->certificates[2].next=1;
        CHECK(effect_context_forest_trace(context,forest,2,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
        forest->certificates[2]=certificate;
        forest->witnesses[2*forest->count+4].cause=9;
        CHECK(effect_context_forest_trace(context,forest,2,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
        forest->witnesses[2*forest->count+4]=saved;
    }
    effect_context_forest_free(forest);
    if (status==XR_XIR_OK && oracle) {
        uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,false,&count);
        CHECK(count==3 && steps[1].cause==5 && steps[2].cause==1);
        CHECK(xr_xir_root_cause_trace_facts(trace)->requires_root);
    }
    xr_xir_compile_root_cause_trace_free(trace);
    if (oracle && status!=XR_XIR_OK) {
        XrCompileResourceStats end=rp_stats(context);
        fprintf(stderr,"CONDITIONAL_FOREST phase=%u status=%u allocated=%llu/%llu work=%llu/%llu live=%llu peak=%llu\n",
            phase,(uint32_t)status,(unsigned long long)begin.allocated_bytes,(unsigned long long)end.allocated_bytes,
            (unsigned long long)begin.work,(unsigned long long)end.work,
            (unsigned long long)end.live_bytes,(unsigned long long)end.peak_bytes);
    }
    return status;
}

static void conditional_shape_candidates(const XrXirCompileContext *context) {
    ConditionalRawFixture f;conditional_raw_fixture(&f,0);
    f.forward_parameters[1]=(XrXirType)259;
    XrXirInstruction ops[3]={
        {.op=XR_XIR_COPY,.type=(XrXirType)259,.args={1,0}},
        {.op=XR_XIR_CALL_REQUIREMENT,.type=XR_XIR_I64,.args={0,2}},
        {.op=XR_XIR_RETURN,.args={3,0}}};
    f.forward_operands[1]=2;f.functions[1].instructions=ops;f.functions[1].instruction_count=3;
    XrXirEffectParameter method_parameters[2]={{0,0},{1,1}},forward_parameters[2]={{0,0},{2,0}};
    XrXirRootValueIdentity value={0,3,(XrXirType)257};
    XrXirEffectCallBinding binding={4,1,1,2};
    XrXirFunctionEffectContract contracts[4]={
        {.parameter_count=2,.parameters=method_parameters},
        {.parameter_count=2,.parameters=forward_parameters,.values=&value,.value_count=1,
         .bindings=&binding,.binding_count=1},
        {.parameter_count=2,.parameters=method_parameters},{0}};
    XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=4};
    f.module.provenance=&evidence;
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,1)==XR_XIR_OK);
    CHECK(forward_parameters[1].kind==2);
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,0)==XR_XIR_BAD_TYPE);
    binding.value=1;
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,1)==XR_XIR_BAD_TYPE);
    binding.value=2;binding.family=1;
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,1)==XR_XIR_BAD_TYPE);
    binding.family=4;ops[0].op=XR_XIR_FUNCTION_WEAKEN;
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,1)==XR_XIR_BAD_STRUCTURE);
    ops[0].op=XR_XIR_COPY;value.declared_type=(XrXirType)259;
    CHECK(xir_effect_call_binding_candidate(context,&f.module,1,1,1)==XR_XIR_BAD_TYPE);
}

static void conditional_domain_rejections(const XrXirCompileContext *context) {
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,0);
    XrXirNominalDeclaration other=fixture.nominal;other.name=(XrXirLiteral){"Other",5};
    XrXirNominalTable names={&other,1,NULL};
    XrXirTypes foreign=fixture.types;foreign.nominals=&names;
    EffectContextRequest request={&fixture.module,&foreign,NULL,0,2,0};
    EffectContextResolved *resolved=NULL;
    CHECK(effect_context_resolve(context,&request,&resolved)==XR_XIR_BAD_TYPE && !resolved);
    EffectOrdinaryContexts *contexts=NULL;uint32_t selected=UINT32_MAX;
    request.actual_types=&fixture.types;
    CHECK(effect_context_resolve(context,&request,&resolved)==XR_XIR_OK);
    CHECK(effect_ordinary_classify(context,&fixture.module,resolved,&contexts,&selected)==XR_XIR_OK);
    fixture.types.count=5;
    CHECK(effect_ordinary_roots(context,&fixture.module,contexts)==XR_XIR_BAD_STRUCTURE);
    CHECK(!contexts->uses.root && !contexts->uses.root_witnesses);
    effect_ordinary_free(contexts);effect_context_free(resolved);
}

/* Raw owner probes exercise dependency closure only. Literal Source gates
 * independently pass complete admission and serialized final correspondence. */
static XrXirStatus conditional_dense_owner(const XrXirCompileContext *context,
    uint32_t mode, bool oracle) {
    ConditionalRawFixture f;conditional_raw_fixture(&f,mode);
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5]={0};XrXirGeneric generics[5]={0};
    memcpy(functions,f.functions,sizeof(f.functions));memcpy(identities,f.identities,sizeof(f.identities));
    memcpy(generics,f.generics,sizeof(f.generics));
    XrXirType parameters[2]={(XrXirType)256,(XrXirType)259};uint32_t operands[2]={0,1};
    XrXirInstruction ops[2]={{.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,2},.immediate=2},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    functions[4]=(XrXirFunction){.name="entry",.name_length=5,.parameters=parameters,.parameter_count=2,
        .result=XR_XIR_I64,.instructions=ops,.instruction_count=2,.operands=operands,.operand_count=2};
    f.declarations.functions=identities;f.declarations.entry_function=4;
    f.module.functions=functions;f.module.function_count=5;f.module.generics=generics;
    XrXirProvenance template={.kind=XR_XIR_EVIDENCE_TEMPLATE};f.module.provenance=&template;
    EffectContextOwner *owner=NULL;XrXirStatus status=effect_context_owner_build(context,&f.module,NULL,&owner);
    XrXirRootCauseTrace *trace=NULL;
    if (status==XR_XIR_OK && oracle) {
        const XrXirRootEffects *facts=&owner->forest->facts[4];
        if (facts->requires_root!=(mode==3 || mode==4) || facts->unresolved!=(mode==1)) {
            fprintf(stderr,"dense owner mode%u root%u unresolved%u\n",mode,facts->requires_root,facts->unresolved);
            for (uint32_t n=0;n<owner->dense->count;++n) {
                const XrXirFunction *body=&owner->dense->functions[n];
                fprintf(stderr," context%u decl%u ordinary%u root%u unknown%u",n,owner->dense->nodes[n].declaration,
                    owner->dense->nodes[n].ordinary,owner->dense->uses.root[n].requires_root,owner->dense->uses.root[n].unresolved);
                for (uint32_t p=0;p<body->parameter_count;++p)
                    fprintf(stderr," p%u:%u/k%u",p,body->parameters[p],owner->dense->uses.contracts[n].parameters[p].kind);
                for (uint32_t i=0;i<body->instruction_count;++i)
                    fprintf(stderr," i%u:%u/t%u/to%lld",i,body->instructions[i].op,body->instructions[i].type,(long long)body->instructions[i].immediate);
                fputc('\n',stderr);
            }
        }
        CHECK(facts->requires_root==(mode==3 || mode==4) && facts->unresolved==(mode==1));
        XrXirEffects effects={.resources=context->resources,.count=5,.contexts=owner};
        XrXirEffectArgument physical[2]={{0,(XrXirType)256},{1,(XrXirType)259}};
        XrXirOrigin origin={4,NULL,0,physical,2};
        XirEffectContextInput request={&f.module,&f.types,&origin,0,NULL,0};XirEffectContextView selected={0};
        CHECK(xir_effects_context_select(context,&effects,&request,&selected)==XR_XIR_OK);
        CHECK(selected.declaration==2 && selected.parameter_count==2 && !selected.argument_count);
        CHECK(selected.physical_types[0]==(XrXirType)256 &&
            selected.physical_types[1]==(mode==1 || mode==3 ? (XrXirType)257 : (XrXirType)259));
        physical[0].type=XR_XIR_I64;
        CHECK(xir_effects_context_select(context,&effects,&request,&selected)==XR_XIR_BAD_TYPE);
        physical[0].type=(XrXirType)256;f.types.count=5;
        CHECK(xir_effects_context_select(context,&effects,&request,&selected)==XR_XIR_BAD_STRUCTURE);
        f.types.count=6;f.binding.function=2;
        CHECK(xir_effects_context_select(context,&effects,&request,&selected)==XR_XIR_BAD_STRUCTURE);
        f.binding.function=0;
        CHECK(xir_effects_context_select(context,&effects,&request,&selected)==XR_XIR_OK);
    }
    if (status==XR_XIR_OK) {
        memset(&f,0,sizeof(f));memset(functions,0,sizeof(functions));memset(identities,0,sizeof(identities));
        status=effect_context_forest_trace(context,owner->forest,4,&trace);
    }
    if (status==XR_XIR_OK && oracle && mode==4) {
        uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,false,&count);
        CHECK(count==4 && steps[0].function==4 && steps[0].cause==6 && steps[0].distance==3);
        CHECK(steps[1].function==2 && steps[1].cause==6 && steps[1].distance==2);
        CHECK(steps[2].function==1 && steps[2].cause==5 && steps[2].callee==0 && steps[2].distance==1);
        CHECK(steps[3].function==0 && steps[3].cause==1 && steps[3].slot==0 && !steps[3].distance);
    }
    effect_context_owner_free(owner);
    if (status==XR_XIR_OK && oracle) CHECK(xr_xir_root_cause_trace_facts(trace)->requires_root==(mode==3 || mode==4));
    xr_xir_compile_root_cause_trace_free(trace);return status;
}

static XrXirStatus conditional_refinement(const XrXirCompileContext *context, bool oracle) {
    ConditionalRawFixture f;conditional_raw_fixture(&f,0);
    XrXirEffects effects={.resources=context->resources,.count=4};
    XrXirStatus status=xir_effects_refinement_capture(context,&f.module,&effects);
    EffectOrdinaryContexts *ordinary=NULL;
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,&f.module,&ordinary);
    if (status==XR_XIR_OK) {
        f.method_parameters[1]=(XrXirType)259;
        ordinary->terms.remaining=context;
        status=effect_refinement_apply(ordinary,&f.module,effects.refinement);
        ordinary->terms.remaining=NULL;
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(ordinary->functions[0].parameters[1]==(XrXirType)257);
        CHECK(effect_refinement_match(context,&f.module,effects.refinement)==XR_XIR_OK);
        f.nodes[1].result=XR_XIR_BOOL;
        /* The interface's complete signature domain rejects this child first. */
        CHECK(effect_refinement_match(context,&f.module,effects.refinement)==XR_XIR_BAD_TYPE);
        f.nodes[1].result=XR_XIR_I64;f.identities[0].cleanup_owner=3;
        CHECK(effect_refinement_match(context,&f.module,effects.refinement)==XR_XIR_BAD_STRUCTURE);
        f.identities[0].cleanup_owner=0;f.nominal.name=(XrXirLiteral){"Other",5};
        CHECK(effect_refinement_match(context,&f.module,effects.refinement)==XR_XIR_BAD_TYPE);
        f.nominal.name=(XrXirLiteral){"Runner",6};f.generics[1].parameter_count=0;
        CHECK(effect_refinement_match(context,&f.module,effects.refinement)==XR_XIR_BAD_STRUCTURE);
        f.generics[1].parameter_count=1;
    }
    effect_ordinary_free(ordinary);effect_refinement_free(effects.refinement);return status;
}

static void conditional_projection_rejections(const XrXirCompileContext *context) {
    ConditionalRawFixture f;conditional_raw_fixture(&f,0);
    XrXirNominalField field={{"payload",7},(XrXirType)257,0};
    XrXirNominalVariant variant={{"Some",4},0,1};
    f.nominal.kind=XR_XIR_NOMINAL_ENUM;f.nominal.fields=&field;f.nominal.field_count=1;
    f.nominal.variants=&variant;f.nominal.variant_count=1;
    EffectTerms terms={.remaining=context};CHECK(effect_terms_seed(&terms,&f.types)==XR_XIR_OK);
    XrXirNominalDeclaration projected=f.nominal;XrXirNominalTable table={&projected,1,NULL};
    XrXirTypeNode nodes[6];memcpy(nodes,f.nodes,sizeof(nodes));
    XrXirType payload=(XrXirType)257;nodes[0].nominal.fields=&payload;nodes[0].nominal.field_count=1;
    XrXirTypes actual={nodes,6,&table,NULL};
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_OK);
    projected.module=(XrXirLiteral){"other",5};CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.module=f.nominal.module;projected.name=(XrXirLiteral){"Other",5};
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.name=f.nominal.name;projected.kind=XR_XIR_NOMINAL_STRUCT;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.kind=XR_XIR_NOMINAL_ENUM;projected.parameter_count=1;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.parameter_count=0;projected.native.native_id=1;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.native.native_id=0;projected.native.source_fingerprint[0]=1;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    projected.native.source_fingerprint[0]=0;nodes[1].result=XR_XIR_BOOL;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    nodes[1].result=XR_XIR_I64;payload=(XrXirType)259;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    payload=(XrXirType)257;variant.field_count=0;
    CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_BAD_TYPE);
    variant.field_count=1;CHECK(effect_terms_projected_domain(&terms,&actual)==XR_XIR_OK);
    effect_terms_free(&terms);
}

/* Two static registration sites may share one lexical declaration. The real
 * capture operands select separate contexts when their complete bounds differ. */
static XrXirStatus conditional_cleanup_owner(const XrXirCompileContext *context, bool oracle) {
    XrXirTypeNode nodes[3]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=9},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=3},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=5}};
    XrXirTypes types={nodes,3,NULL,NULL};
    XrXirType declared[2]={(XrXirType)256,(XrXirType)256};
    XrXirType actual[2]={(XrXirType)257,(XrXirType)258};uint32_t operands[2]={0,1};
    XrXirInstruction registrations[3]={
        {.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1,0},.immediate=1},
        {.op=XR_XIR_CLEANUP_REGISTER,.args={1,1},.targets={2,0},.immediate=1},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction cleanup[4]={
        {.op=XR_XIR_INVOKE_INDIRECT,.type=XR_XIR_I64,.targets={1,2}},
        {.op=XR_XIR_RETURN},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction entry[2]={{.op=XR_XIR_CALL,.args={0,2}}, {.op=XR_XIR_RETURN}};
    XrXirInstruction done={.op=XR_XIR_RETURN};
    XrXirBlock owner_block={.count=3},entry_block={.count=2},init_block={.count=1};
    XrXirBlock branches[3]={{.count=1},{.first=1,.count=1},{.first=2,.count=2}};
    XrXirFunction functions[4]={
        {.name="owner",.name_length=5,.parameters=declared,.parameter_count=2,.result=XR_XIR_UNIT,
         .blocks=&owner_block,.block_count=1,.instructions=registrations,.instruction_count=3,
         .operands=operands,.operand_count=2},
        {.name="cleanup",.name_length=7,.parameters=declared,.parameter_count=1,.result=XR_XIR_UNIT,
         .blocks=branches,.block_count=3,.instructions=cleanup,.instruction_count=4},
        {.name="entry",.name_length=5,.parameters=actual,.parameter_count=2,.result=XR_XIR_UNIT,
         .blocks=&entry_block,.block_count=1,.instructions=entry,.instruction_count=2,
         .operands=operands,.operand_count=2},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&init_block,.block_count=1,
         .instructions=&done,.instruction_count=1}};
    XrXirFunctionIdentity identities[4]={0};identities[1].cleanup_owner=1;
    XrXirSourceModule source={"cleanup-context",15,NULL,0,3};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=2};
    XrXirProvenance template={.kind=XR_XIR_EVIDENCE_TEMPLATE};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=4,.declarations=&declarations,
        .types=&types,.provenance=&template,.linkage_kind=XR_XIR_PROGRAM};
    EffectContextOwner *owner=NULL;XrXirStatus status=effect_context_owner_build(context,&module,NULL,&owner);
    if (status==XR_XIR_OK && oracle) {
        XrXirEffects effects={.resources=context->resources,.count=4,.contexts=owner};
        XrXirEffectArgument physical[2]={{0,(XrXirType)257},{1,(XrXirType)258}};
        XrXirOrigin origin={0,NULL,0,physical,2};
        XirEffectContextInput input={&module,&types,&origin,0,NULL,0};XirEffectContextView first={0},second={0};
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_OK);
        input.instruction=1;CHECK(xir_effects_context_select(context,&effects,&input,&second)==XR_XIR_OK);
        CHECK(first.declaration==1 && second.declaration==1 && first.parameter_count==1 && second.parameter_count==1);
        CHECK(first.physical_types[0]==(XrXirType)257 && !first.requires_root && !first.unresolved);
        CHECK(second.physical_types[0]==(XrXirType)258 && second.requires_root && !second.unresolved);
        CHECK(first.function!=second.function);
        XrXirEffectArgument capture={0,(XrXirType)257};XrXirOrigin child={1,NULL,0,&capture,1};
        input.origin=&child;input.instruction=UINT32_MAX;input.owners=&origin;input.owner_count=1;
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_OK && !first.requires_root);
        /* Missing owner, another actual owner, and a forged declaration cannot
         * reuse the independently selected body/capture certificate. */
        input.owners=NULL;input.owner_count=0;
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_BAD_TYPE);
        input.owners=&origin;input.owner_count=1;physical[1].type=(XrXirType)257;
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_BAD_TYPE);
        physical[1].type=(XrXirType)258;child.function=2;
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_BAD_TYPE);
        child.function=1;capture.type=(XrXirType)256;
        CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_BAD_TYPE);
    }
    effect_context_owner_free(owner);return status;
}

static XrXirStatus conditional_cell_owner(const XrXirCompileContext *context, bool oracle) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT}};
    XrXirTypes types={nodes,2,NULL,NULL};XrXirType cell=(XrXirType)256,unit_cell=(XrXirType)257;
    XrXirInstruction read[2]={{.op=XR_XIR_CELL_READ,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction local[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CELL_NEW,.type=(XrXirType)256},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1}},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction shared[3]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)256},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1}}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction initialize[3]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9},
        {.op=XR_XIR_SLOT_INIT}, {.op=XR_XIR_RETURN}};
    XrXirInstruction read_unit[2]={{.op=XR_XIR_CELL_READ}, {.op=XR_XIR_RETURN}};
    XrXirInstruction local_unit[3]={{.op=XR_XIR_CELL_NEW,.type=(XrXirType)257},
        {.op=XR_XIR_CALL,.args={0,1},.immediate=4}, {.op=XR_XIR_RETURN}};
    uint32_t local_operand=1,shared_operand=0;
    XrXirBlock blocks[6]={{.count=2},{.count=4},{.count=3},{.count=3},{.count=2},{.count=3}};
    XrXirFunction functions[6]={
        {.name="read",.name_length=4,.parameters=&cell,.parameter_count=1,.result=XR_XIR_I64,
         .blocks=&blocks[0],.block_count=1,.instructions=read,.instruction_count=2},
        {.name="local",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,
         .instructions=local,.instruction_count=4,.operands=&local_operand,.operand_count=1},
        {.name="shared",.name_length=6,.result=XR_XIR_I64,.blocks=&blocks[2],.block_count=1,
         .instructions=shared,.instruction_count=3,.operands=&shared_operand,.operand_count=1},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[3],.block_count=1,
         .instructions=initialize,.instruction_count=3},
        {.name="readUnit",.name_length=8,.parameters=&unit_cell,.parameter_count=1,.result=XR_XIR_UNIT,
         .blocks=&blocks[4],.block_count=1,.instructions=read_unit,.instruction_count=2},
        {.name="localUnit",.name_length=9,.result=XR_XIR_UNIT,.blocks=&blocks[5],.block_count=1,
         .instructions=local_unit,.instruction_count=3,.operands=&shared_operand,.operand_count=1}};
    XrXirFunctionIdentity identities[6]={0};XrXirSourceModule source={"cell-effects",12,NULL,0,3};
    XrXirSlot slot={0,(XrXirType)256,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=6,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;EffectContextOwner *owner=NULL;
    XrXirStatus status=xr_xir_compile_effects_infer_verified(context,&module,&effects);
    XrXirProvenance template={.kind=XR_XIR_EVIDENCE_TEMPLATE};module.provenance=&template;
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,&module,NULL,&owner);
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects->root[0].unresolved && !effects->root[0].requires_root);
        CHECK(!effects->root[1].unresolved && !effects->root[1].requires_root);
        CHECK(effects->root[2].requires_root && !effects->root[2].unresolved);
        CHECK(effects->root[4].unresolved && !effects->root[4].requires_root);
        CHECK(!effects->root[5].unresolved && !effects->root[5].requires_root);
        CHECK(effects->contracts[0].parameters[0].kind==XR_XIR_EFFECT_PARAMETER_FIXED);
        CHECK(effects->contracts[0].formula.term_count==1 &&
            effects->contracts[0].formula.terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER &&
            effects->contracts[0].formula.terms[0].index==0);
        XirEffectEntryRootView entry={0};
        CHECK(xir_effects_entry_root(context,effects,0,&entry)==XR_XIR_OK &&
            !entry.intrinsic_mask && entry.parameter_count==1 && entry.parameters[0]==0);
        CHECK(xir_effects_entry_root(context,effects,1,&entry)==XR_XIR_OK &&
            !entry.intrinsic_mask && !entry.parameter_count && !entry.parameters);
        CHECK(xir_effects_entry_root(context,effects,2,&entry)==XR_XIR_OK &&
            entry.intrinsic_mask==XR_XIR_CALLABLE_ROOT_REQUIRED && !entry.parameter_count);
        CHECK(xir_effects_entry_root(context,effects,4,&entry)==XR_XIR_OK &&
            !entry.intrinsic_mask && entry.parameter_count==1 && entry.parameters[0]==0);
        entry.intrinsic_mask=UINT32_MAX;
        CHECK(xir_effects_entry_root(context,effects,6,&entry)==XR_XIR_BAD_STRUCTURE &&
            entry.intrinsic_mask==UINT32_MAX);
        CHECK(owner->dense->count==6);
        XrXirEffects selected_effects={.resources=context->resources,.count=6,.contexts=owner};
        XrXirOrigin local_origin={.function=1},shared_origin={.function=2};
        XirEffectContextInput input={&module,&types,&local_origin,2,NULL,0};
        XirEffectContextView local_view={0},shared_view={0};
        CHECK(xir_effects_context_select(context,&selected_effects,&input,&local_view)==XR_XIR_OK);
        input.origin=&shared_origin;input.instruction=1;
        CHECK(xir_effects_context_select(context,&selected_effects,&input,&shared_view)==XR_XIR_OK);
        CHECK(local_view.function==shared_view.function && local_view.declaration==0 && shared_view.declaration==0);
        CHECK(!local_view.requires_root && !local_view.unresolved && shared_view.requires_root && !shared_view.unresolved);
        memset(nodes,0,sizeof(nodes));memset(functions,0,sizeof(functions));
        CHECK(xir_effects_entry_root(context,effects,0,&entry)==XR_XIR_OK &&
            !entry.intrinsic_mask && entry.parameter_count==1 && entry.parameters[0]==0);
        status=xr_xir_compile_root_cause_trace_copy(context,effects,0,&trace);
        if (status==XR_XIR_OK) {
            uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,true,&count);
            CHECK(count==1 && steps && steps[0].cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER && steps[0].slot==0);
        }
    }
    effect_context_owner_free(owner);
    xr_xir_compile_effects_free(effects);
    if (status==XR_XIR_OK && oracle) CHECK(xr_xir_root_cause_trace_facts(trace)->unresolved);
    xr_xir_compile_root_cause_trace_free(trace);return status;
}

static XrXirStatus conditional_cell_payload(const XrXirCompileContext *context, bool oracle) {
    XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=2},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}};
    XrXirTypes types={nodes,2,NULL,NULL};
    XrXirInstruction local[5]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2},
        {.op=XR_XIR_CELL_NEW,.type=(XrXirType)257},
        {.op=XR_XIR_CELL_READ,.type=(XrXirType)256,.args={1,0}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2}, {.op=XR_XIR_RETURN,.args={3,0}}};
    XrXirInstruction shared[4]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257},
        {.op=XR_XIR_CELL_READ,.type=(XrXirType)256},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1}, {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction pure[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction initialize[3]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2},
        {.op=XR_XIR_SLOT_INIT}, {.op=XR_XIR_RETURN}};
    XrXirBlock blocks[4]={{.count=5},{.count=4},{.count=2},{.count=3}};
    XrXirFunction functions[4]={
        {.name="local",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[0],.block_count=1,
         .instructions=local,.instruction_count=5},
        {.name="shared",.name_length=6,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,
         .instructions=shared,.instruction_count=4},
        {.name="pure",.name_length=4,.result=XR_XIR_I64,.blocks=&blocks[2],.block_count=1,
         .instructions=pure,.instruction_count=2},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[3],.block_count=1,
         .instructions=initialize,.instruction_count=3}};
    XrXirFunctionIdentity identities[4]={0};XrXirSourceModule source={"cell-payload",12,NULL,0,3};
    XrXirSlot slot={0,(XrXirType)257,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=0};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=4,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirEffects *effects=NULL;XrXirStatus status=xr_xir_compile_effects_infer_verified(context,&module,&effects);
    if (status==XR_XIR_OK && oracle) {
        CHECK(!effects->root[0].requires_root && !effects->root[0].unresolved);
        CHECK(effects->root[1].requires_root && !effects->root[1].unresolved);
        XirEffectEntryRootView entry={0};
        CHECK(xir_effects_entry_root(context,effects,1,&entry)==XR_XIR_OK &&
            entry.intrinsic_mask==XR_XIR_CALLABLE_ROOT_REQUIRED && !entry.parameter_count);
        CHECK(xr_xir_callable_signature(&types,shared[1].type)->flags==2);
        CHECK(xr_xir_callable_signature(&types,local[2].type)->flags==2);
    }
    xr_xir_compile_effects_free(effects);return status;
}


/* Empty descriptor pools and zero-sized payload tables are real domains. The
 * same consumers must still reject nonempty data without its authentic owner. */
static void conditional_empty_domains(const XrXirCompileContext *context) {
    EffectTerms terms={.remaining=context};
    XrXirInstruction construct={.op=XR_XIR_STRUCT_NEW};
    XrXirFunction body={.instructions=&construct,.instruction_count=1};
    uint8_t fixed=0;XrXirType declared=XR_XIR_UNIT,values=XR_XIR_UNIT;
    EffectScalarFlow flow={&terms,&body,NULL,&declared,&values,&fixed,1,false,NULL};
    CHECK(effect_scalar_escape(&flow,0)==XR_XIR_OK);
    construct.args[0]=1;CHECK(effect_scalar_escape(&flow,0)==XR_XIR_BAD_STRUCTURE);
    construct.args[0]=0;construct.args[1]=1;CHECK(effect_scalar_escape(&flow,0)==XR_XIR_BAD_STRUCTURE);
    construct.args[1]=0;
    effect_terms_free(&terms);

    XrXirInstruction read[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction call[2]={{.op=XR_XIR_CALL,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction done={.op=XR_XIR_RETURN};
    XrXirBlock blocks[3]={{.count=2},{.count=2},{.count=1}};
    XrXirFunction functions[3]={
        {.name="read",.name_length=4,.result=XR_XIR_I64,.blocks=&blocks[0],.block_count=1,
         .instructions=read,.instruction_count=2},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,
         .instructions=call,.instruction_count=2},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[2],.block_count=1,
         .instructions=&done,.instruction_count=1}};
    XrXirFunctionIdentity identities[3]={0};XrXirSourceModule module_identity={"empty",5,NULL,0,2};
    XrXirDeclarations declarations={.modules=&module_identity,.module_count=1,
        .functions=identities,.entry_function=1};
    XrXirProvenance template={.kind=XR_XIR_EVIDENCE_TEMPLATE};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=3,
        .declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM,.provenance=&template};
    EffectContextOwner *owner=NULL;
    CHECK(effect_context_owner_build(context,&module,NULL,&owner)==XR_XIR_OK);
    XrXirEffects effects={.resources=context->resources,.count=3,.contexts=owner};
    XrXirOrigin origin={.function=1};XirEffectContextInput input={&module,NULL,&origin,0,NULL,0};
    XirEffectContextView selected={0};
    CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_OK && selected.declaration==0);
    call[0].immediate=1;selected.declaration=UINT32_MAX;
    CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_STRUCTURE &&
        selected.declaration==UINT32_MAX);
    effect_context_owner_free(owner);

    ConditionalRawFixture nominal;conditional_raw_fixture(&nominal,0);owner=NULL;
    CHECK(effect_context_owner_build(context,&nominal.module,NULL,&owner)==XR_XIR_OK);
    effects=(XrXirEffects){.resources=context->resources,.count=4,.contexts=owner};
    origin=(XrXirOrigin){.function=2};input=(XirEffectContextInput){&nominal.module,NULL,&origin,UINT32_MAX,NULL,0};
    selected.declaration=UINT32_MAX;
    CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE &&
        selected.declaration==UINT32_MAX);
    effect_context_owner_free(owner);
}

/* Complete raw checking and the owned checked revalidation precede the
 * independent effect inference. No dependency probe grants stage authority. */
static XrXirStatus conditional_zero_checked(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirEffects **output) {
    XrXirModule built=*module;built.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;
    XrXirStatus status=xir_fixture_check(context,&built,&checked,NULL);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_v2(context,owned,
        xr_xir_compile_artifact_construction(checked),NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_infer_verified(context,owned,output);
    xr_xir_compile_artifact_free(checked);return status;
}

/* Opaque containers do not seed callable origins. Extracting their real
 * callable payload and executing it still retains its unknown advertisement. */
static XrXirStatus conditional_zero_carrier(const XrXirCompileContext *context,
    uint32_t mode, bool oracle) {
    XrXirCallableParameter field={(XrXirType)256,0};
    XrXirTypeNode nodes[6]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
         .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
         .flags=XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=&field,.parameter_count=1},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT}};
    XrXirTypes types={nodes,6,NULL,NULL};
    XrXirType parameter=(XrXirType)(258+mode);
    XrXirOp extract=mode==0 ? XR_XIR_NULLABLE_UNWRAP :
        mode==1 ? XR_XIR_ARRAY_GET : XR_XIR_TUPLE_FIELD;
    XrXirInstruction ops[4]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=extract,.type=(XrXirType)256,.args={0,mode==1 ? 1u : 0u}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_RETURN,.args={3,0}}};
    XrXirBlock block={.count=4};
    XrXirFunction function={.name="extract",.name_length=7,.parameters=&parameter,
        .parameter_count=1,.result=XR_XIR_I64,.instructions=ops,.instruction_count=4,
        .blocks=&block,.block_count=1};
    XrXirModule module={.stage=XR_XIR_CHECKED,.types=&types,.functions=&function,
        .function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    XrXirEffects *effects=NULL;
    XrXirStatus status=conditional_zero_checked(context,&module,&effects);
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects->root[0].unresolved && !effects->root[0].requires_root);
        CHECK(effects->contracts[0].formula.constant_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED &&
            !effects->contracts[0].formula.term_count && !effects->contracts[0].formula.terms);
        XirEffectEntryRootView view={0};
        CHECK(xir_effects_entry_root(context,effects,0,&view)==XR_XIR_OK &&
            view.intrinsic_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED && !view.parameter_count);
    }
    xr_xir_compile_effects_free(effects);effects=NULL;
    if (status!=XR_XIR_OK || !oracle) return status;
    /* Merely extracting the same payload does not execute its body. */
    ops[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    status=conditional_zero_checked(context,&module,&effects);
    if (status==XR_XIR_OK) CHECK(!effects->root[0].unresolved && !effects->root[0].requires_root &&
        !effects->contracts[0].formula.constant_mask && !effects->contracts[0].formula.term_count);
    xr_xir_compile_effects_free(effects);effects=NULL;
    if (status!=XR_XIR_OK) return status;
    /* Exhaustive opcode admission precedes every zero-origin decision. */
    XrXirOp invalid[3]={XR_XIR_INVALID,XR_XIR_OP_COUNT,(XrXirOp)(XR_XIR_OP_COUNT+19)};
    for (uint32_t i=0;i<3;++i) {
        ops[1].op=invalid[i];
        CHECK(xr_xir_compile_effects_infer_verified(context,&module,&effects)==XR_XIR_BAD_STRUCTURE && !effects);
    }
    ops[1].op=extract;
    ops[2]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    CHECK(xr_xir_compile_effects_infer_verified(context,&module,&effects)==XR_XIR_BAD_STRUCTURE && !effects);
    return XR_XIR_OK;
}

static XrXirStatus conditional_zero_module_fn(const XrXirCompileContext *context,bool oracle) {
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
         .flags=XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}};
    XrXirTypes types={nodes,2,NULL,NULL};
    XrXirInstruction load[2]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257}, {.op=XR_XIR_RETURN}};
    XrXirInstruction pure[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN}};
    XrXirInstruction init[4]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=1},
        {.op=XR_XIR_CELL_NEW,.type=(XrXirType)257},
        {.op=XR_XIR_SLOT_INIT,.args={1,0}}, {.op=XR_XIR_RETURN}};
    XrXirBlock blocks[3]={{.count=2},{.count=2},{.count=4}};
    XrXirFunction functions[3]={
        {.name="load",.name_length=4,.result=XR_XIR_UNIT,.instructions=load,.instruction_count=2,
         .blocks=&blocks[0],.block_count=1},
        {.name="pure",.name_length=4,.result=XR_XIR_I64,.instructions=pure,.instruction_count=2,
         .blocks=&blocks[1],.block_count=1},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.instructions=init,.instruction_count=4,
         .blocks=&blocks[2],.block_count=1}};
    XrXirFunctionIdentity identities[3]={[1]={.promises=XR_XIR_FUNCTION_NO_SUSPEND}};
    XrXirSourceModule source={"zero-module-fn",14,NULL,0,2};
    XrXirSlot slot={0,(XrXirType)257,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_CHECKED,.types=&types,.functions=functions,
        .function_count=3,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirEffects *effects=NULL;
    XrXirStatus status=conditional_zero_checked(context,&module,&effects);
    if (status==XR_XIR_OK && oracle) {
        EffectParameterFlow flow={.work=context,.module=&module,.function=&functions[0]};
        bool needed=true;
        CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && !needed);
        CHECK(effects->root[0].requires_root && !effects->root[0].unresolved &&
            effects->contracts[0].formula.constant_mask==XR_XIR_CALLABLE_ROOT_REQUIRED &&
            !effects->contracts[0].formula.term_count);
        CHECK(!effects->root[1].requires_root && !effects->root[1].unresolved);
        CHECK(effects->root[2].requires_root && !effects->root[2].unresolved);
        XirEffectEntryRootView view={0};
        CHECK(xir_effects_entry_root(context,effects,0,&view)==XR_XIR_OK &&
            view.intrinsic_mask==XR_XIR_CALLABLE_ROOT_REQUIRED && !view.parameter_count);
    }
    xr_xir_compile_effects_free(effects);return status;
}

/* Two real generic calls have no physical parameters. Their complete empty
 * vectors still retain separate ordinary identities, exact owners and the
 * full raw/owned validation. Neither UNKNOWN nor a missing nonempty vector
 * receives a different authority rule from this zero-arity probe. */
static XrXirStatus conditional_zero_arity(const XrXirCompileContext *context,bool oracle) {
    XrXirInstruction read[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction calls[3]={
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=0,.type_arguments={0,1}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=0,.type_arguments={1,1}},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction done={.op=XR_XIR_RETURN};
    XrXirBlock blocks[3]={{.count=2},{.count=3},{.count=1}};
    XrXirFunction functions[3]={
        {.name="zero",.name_length=4,.result=XR_XIR_I64,.blocks=&blocks[0],.block_count=1,
         .instructions=read,.instruction_count=2},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&blocks[1],.block_count=1,
         .instructions=calls,.instruction_count=3},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[2],.block_count=1,
         .instructions=&done,.instruction_count=1}};
    XrXirType ordinary[2]={XR_XIR_I64,XR_XIR_F64};XrXirConstraint constraint={0};
    XrXirGeneric generics[3]={{.constraints=&constraint,.parameter_count=1},
        {.arguments=ordinary,.argument_count=2},{0}};
    XrXirFunctionIdentity identities[3]={0};XrXirSourceModule identity={"zero-arity",10,NULL,0,2};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,
        .functions=identities,.entry_function=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,
        .declarations=&declarations,.generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;EffectContextOwner *owner=NULL;
    XrXirStatus status=xir_fixture_check(context,&module,&checked,NULL);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_v2(context,owned,
        xr_xir_compile_artifact_construction(checked),NULL);
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,owned,NULL,&owner);
    if (status==XR_XIR_OK) {
        XrXirEffects effects={.resources=context->resources,.count=3,.contexts=owner};
        XrXirOrigin origin={.function=0,.arguments=&ordinary[0],.argument_count=1};
        XirEffectContextInput input={owned,owned->types,&origin,UINT32_MAX,NULL,0};
        XirEffectContextView first={0},second={0};
        status=xir_effects_context_select(context,&effects,&input,&first);
        if (status==XR_XIR_OK) {
            origin.arguments=&ordinary[1];
            status=xir_effects_context_select(context,&effects,&input,&second);
        }
        if (status==XR_XIR_OK && oracle) {
            CHECK(first.declaration==0 && second.declaration==0 && !first.parameter_count && !second.parameter_count);
            CHECK(first.argument_count==1 && second.argument_count==1 && first.arguments[0]==XR_XIR_I64 &&
                second.arguments[0]==XR_XIR_F64 && first.function!=second.function);
            CHECK(!first.requires_root && !first.unresolved && !second.requires_root && !second.unresolved);
            XrXirType wrong=XR_XIR_I32;origin.arguments=&wrong;XirEffectContextView rejected={.declaration=UINT32_MAX};
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE &&
                rejected.declaration==UINT32_MAX);
            origin.arguments=ordinary;origin.argument_count=2;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            origin.argument_count=1;origin.function=2;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            origin.function=0;XrXirEffectArgument extra={0,XR_XIR_I64};
            origin.effect_arguments=&extra;origin.effect_argument_count=1;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            origin.effect_arguments=NULL;origin.effect_argument_count=0;
            XrXirOrigin false_owner={.function=1};input.owners=&false_owner;input.owner_count=1;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            input.owners=NULL;input.owner_count=0;
            CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_OK);
        }
    }
    xr_xir_compile_artifact_free(checked);memset(functions,0,sizeof(functions));memset(generics,0,sizeof(generics));
    if (status==XR_XIR_OK) {
        XrXirRootCauseTrace *trace=NULL;status=effect_context_forest_trace(context,owner->forest,1,&trace);
        if (status==XR_XIR_OK && oracle) {
            uint32_t count=UINT32_MAX;
            CHECK(!xr_xir_root_cause_trace_steps(trace,false,&count) && !count);
            CHECK(!owner->forest->facts[1].requires_root && !owner->forest->facts[1].unresolved);
        }
        xr_xir_compile_root_cause_trace_free(trace);
    }
    effect_context_owner_free(owner);return status;
}

/* A real two-level lexical REGISTER chain has zero captured values at
 * every level. Its generic owner contexts are still complete identities,
 * so omission, reordering or substitution cannot select a sibling body. */
static XrXirStatus conditional_zero_arity_cleanup(const XrXirCompileContext *context,bool oracle) {
    XrXirInstruction outer[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=1,
        .targets={1,0},.type_arguments={0,1}},{.op=XR_XIR_RETURN}};
    XrXirInstruction child[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=2,
        .targets={1,0},.type_arguments={0,1}},{.op=XR_XIR_RETURN}};
    XrXirInstruction done[2]={{.op=XR_XIR_RETURN},{.op=XR_XIR_RETURN}};
    XrXirInstruction entry[4]={{.op=XR_XIR_CALL,.immediate=0,.type_arguments={0,1}},
        {.op=XR_XIR_CALL,.immediate=0,.type_arguments={1,1}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock split[2]={{.count=1},{.first=1,.count=1,.frontier=1}},one={.count=1},four={.count=4};
    XrXirFunction functions[5]={
        {.name="outer",.name_length=5,.blocks=split,.block_count=2,.instructions=outer,.instruction_count=2},
        {.name="child",.name_length=5,.blocks=split,.block_count=2,.instructions=child,.instruction_count=2},
        {.name="nested",.name_length=6,.blocks=&one,.block_count=1,.instructions=&done[0],.instruction_count=1},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&four,.block_count=1,
         .instructions=entry,.instruction_count=4},
        {.name="init",.name_length=4,.blocks=&one,.block_count=1,.instructions=&done[1],.instruction_count=1}};
    XrXirType binder=XR_XIR_TYPE_PARAMETER_BASE,ordinary[2]={XR_XIR_I64,XR_XIR_F64};
    XrXirConstraint constraints[3]={0};
    XrXirGeneric generics[5]={
        {.constraints=&constraints[0],.parameter_count=1,.arguments=&binder,.argument_count=1},
        {.constraints=&constraints[1],.parameter_count=1,.arguments=&binder,.argument_count=1},
        {.constraints=&constraints[2],.parameter_count=1},
        {.arguments=ordinary,.argument_count=2},{0}};
    XrXirFunctionIdentity identities[5]={0};identities[1].cleanup_owner=1;identities[2].cleanup_owner=2;
    XrXirSourceModule identity={"zero-cleanup",12,NULL,0,4};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,.entry_function=3};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=5,.declarations=&declarations,
        .generics=generics,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;EffectContextOwner *owner=NULL;
    XrXirStatus status=xir_fixture_check(context,&module,&checked,NULL);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_v2(context,owned,
        xr_xir_compile_artifact_construction(checked),NULL);
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,owned,NULL,&owner);
    if (status==XR_XIR_OK) {
        XrXirEffects effects={.resources=context->resources,.count=5,.contexts=owner};
        XrXirOrigin nested={.function=2,.arguments=&ordinary[0],.argument_count=1};
        XrXirOrigin parents[2]={{.function=1,.arguments=&ordinary[0],.argument_count=1},
            {.function=0,.arguments=&ordinary[0],.argument_count=1}};
        XirEffectContextInput input={owned,owned->types,&nested,UINT32_MAX,parents,2};
        XirEffectContextView first={0},second={0};
        status=xir_effects_context_select(context,&effects,&input,&first);
        if (status==XR_XIR_OK) {
            nested.arguments=&ordinary[1];parents[0].arguments=parents[1].arguments=&ordinary[1];
            status=xir_effects_context_select(context,&effects,&input,&second);
        }
        if (status==XR_XIR_OK && oracle) {
            CHECK(first.declaration==2 && second.declaration==2 && first.function!=second.function &&
                !first.parameter_count && !second.parameter_count && !first.requires_root && !first.unresolved &&
                !second.requires_root && !second.unresolved);
            nested.arguments=&ordinary[0];parents[0].arguments=parents[1].arguments=&ordinary[0];
            XirEffectContextView rejected={.declaration=UINT32_MAX};input.owners=NULL;input.owner_count=0;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE &&
                rejected.declaration==UINT32_MAX);
            input.owners=parents;input.owner_count=1;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            input.owner_count=2;XrXirOrigin saved=parents[0];parents[0]=parents[1];parents[1]=saved;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            parents[1]=parents[0];parents[0]=saved;parents[1].function=3;
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            parents[1].function=0;parents[1].arguments=&ordinary[1];
            CHECK(xir_effects_context_select(context,&effects,&input,&rejected)==XR_XIR_BAD_TYPE);
            parents[1].arguments=&ordinary[0];
            CHECK(xir_effects_context_select(context,&effects,&input,&first)==XR_XIR_OK);
        }
    }
    xr_xir_compile_artifact_free(checked);memset(functions,0,sizeof(functions));memset(generics,0,sizeof(generics));
    if (status==XR_XIR_OK) {
        XrXirRootCauseTrace *trace=NULL;status=effect_context_forest_trace(context,owner->forest,3,&trace);
        if (status==XR_XIR_OK && oracle) CHECK(!owner->forest->facts[3].requires_root && !owner->forest->facts[3].unresolved);
        xr_xir_compile_root_cause_trace_free(trace);
    }
    effect_context_owner_free(owner);return status;
}

/* Declaration-free ordinary PROGRAM modules retain their valid raw/owned
 * path. An empty lexical collector grants no context or reference permission. */
static XrXirStatus conditional_base_without_declarations(const XrXirCompileContext *context,bool oracle) {
    XrXirInstruction ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirBlock block={.count=2};
    XrXirFunction function={.name="plain",.name_length=5,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=ops,.instruction_count=2};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirStatus status=xir_fixture_check(context,&module,&checked,NULL);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_v2(context,owned,
        xr_xir_compile_artifact_construction(checked),NULL);
    XrXirOrigin *owners=NULL;uint32_t count=0;
    if (status==XR_XIR_OK) status=effect_context_base_owner_origins(context,owned,0,&owners,&count);
    if (status==XR_XIR_OK && oracle) CHECK(!owned->declarations && !owners && !count);
    xr_compile_resources_free(owners);xr_xir_compile_artifact_free(checked);return status;
}

/* Authentic nongeneric nested cleanup callers reconstruct their whole base
 * owner chain. Constructing the ROOT reference does not execute its body. */
static XrXirStatus conditional_base_cleanup_reference(const XrXirCompileContext *context,bool oracle) {
    XrXirStatus plain=conditional_base_without_declarations(context,oracle);
    if (plain!=XR_XIR_OK) return plain;
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
         .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED|XR_XIR_CALLABLE_NO_SUSPEND},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT}};
    XrXirTypes types={nodes,2,NULL,NULL};
    XrXirInstruction outer[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=1,.targets={1,0}},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction child[2]={{.op=XR_XIR_CLEANUP_REGISTER,.immediate=2,.targets={1,0}},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction nested[3]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=4},{.op=XR_XIR_RETURN}};
    XrXirInstruction pure={.op=XR_XIR_RETURN};
    XrXirInstruction shared[2]={{.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257},{.op=XR_XIR_RETURN}};
    XrXirInstruction entry[4]={{.op=XR_XIR_CALL,.immediate=0},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction init[3]={{.op=XR_XIR_CELL_NEW,.type=(XrXirType)257},
        {.op=XR_XIR_SLOT_INIT},{.op=XR_XIR_RETURN}};
    XrXirBlock split[2]={{.count=1},{.first=1,.count=1,.frontier=1}},one={.count=1},two={.count=2},three={.count=3},four={.count=4};
    XrXirFunction functions[7]={
        {.name="outer",.name_length=5,.blocks=split,.block_count=2,.instructions=outer,.instruction_count=2},
        {.name="child",.name_length=5,.blocks=split,.block_count=2,.instructions=child,.instruction_count=2},
        {.name="nested",.name_length=6,.blocks=&three,.block_count=1,.instructions=nested,.instruction_count=3},
        {.name="pure",.name_length=4,.blocks=&one,.block_count=1,.instructions=&pure,.instruction_count=1},
        {.name="shared",.name_length=6,.blocks=&two,.block_count=1,.instructions=shared,.instruction_count=2},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&four,.block_count=1,
         .instructions=entry,.instruction_count=4},
        {.name="init",.name_length=4,.blocks=&three,.block_count=1,.instructions=init,.instruction_count=3}};
    XrXirFunctionIdentity identities[7]={[1]={.cleanup_owner=1},[2]={.cleanup_owner=2},
        [3]={.promises=XR_XIR_FUNCTION_NO_SUSPEND},[4]={.promises=XR_XIR_FUNCTION_NO_SUSPEND}};
    XrXirSourceModule identity={"nested-reference",16,NULL,0,6};
    XrXirSlot slot={0,(XrXirType)257,1};
    XrXirDeclarations declarations={.modules=&identity,.module_count=1,.functions=identities,
        .entry_function=5,.slots=&slot,.slot_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=7,
        .types=&types,.declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;EffectContextOwner *owner=NULL;
    XrXirStatus status=xir_fixture_check(context,&module,&checked,NULL);
    const XrXirModule *owned=xr_xir_compile_artifact_module(checked);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify_v2(context,owned,
        xr_xir_compile_artifact_construction(checked),NULL);
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,owned,NULL,&owner);
    uint32_t pure_mask=UINT32_MAX,root_mask=UINT32_MAX;
    if (status==XR_XIR_OK) {
        XrXirEffects effects={.resources=context->resources,.count=7,.contexts=owner};
        status=xir_effects_reference_root(context,&effects,owned,2,0,&pure_mask);
        if (status==XR_XIR_OK) status=xir_effects_reference_root(context,&effects,owned,2,1,&root_mask);
        uint32_t noncleanup=UINT32_MAX;
        if (status==XR_XIR_OK) status=xir_effects_reference_root(context,&effects,owned,5,1,&noncleanup);
        if (status==XR_XIR_OK && oracle) {
            CHECK(!pure_mask && !noncleanup && root_mask==XR_XIR_CALLABLE_ROOT_REQUIRED);
            CHECK(!owner->forest->facts[2].requires_root && !owner->forest->facts[2].unresolved);
            XrXirOrigin origin={.function=2},parents[3]={{.function=1},{.function=0},{.function=5}};
            XirEffectContextInput input={owned,owned->types,&origin,0,parents,2};
            XirEffectContextView selected={.declaration=UINT32_MAX};
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_OK &&
                selected.declaration==3 && !selected.requires_root && !selected.unresolved);
            selected.declaration=UINT32_MAX;input.owners=NULL;input.owner_count=0;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE &&
                selected.declaration==UINT32_MAX);
            input.owners=parents;input.owner_count=1;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE);
            input.owner_count=2;parents[0].function=0;parents[1].function=1;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE);
            parents[0].function=1;parents[1].function=5;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE);
            parents[1].function=0;input.owner_count=3;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_BAD_TYPE);
            input.owner_count=2;
            CHECK(xir_effects_context_select(context,&effects,&input,&selected)==XR_XIR_OK);
            XrXirModule changed=*owned;XrXirDeclarations altered=*owned->declarations;
            XrXirFunctionIdentity copied[7];memcpy(copied,owned->declarations->functions,sizeof(copied));
            altered.functions=copied;changed.declarations=&altered;
            uint32_t sentinel=UINT32_MAX;changed.declarations=NULL;
            CHECK(xir_effects_reference_root(context,&effects,&changed,2,0,&sentinel)==XR_XIR_BAD_STRUCTURE &&
                sentinel==UINT32_MAX);
            changed.declarations=&altered;copied[2].cleanup_owner=8;
            CHECK(xir_effects_reference_root(context,&effects,&changed,2,0,&sentinel)==XR_XIR_BAD_STRUCTURE &&
                sentinel==UINT32_MAX);
            copied[2].cleanup_owner=2;copied[1].cleanup_owner=3;
            CHECK(xir_effects_reference_root(context,&effects,&changed,2,0,&sentinel)==XR_XIR_BAD_STRUCTURE &&
                sentinel==UINT32_MAX);
            copied[1].cleanup_owner=1;copied[0].cleanup_owner=2;
            CHECK(xir_effects_reference_root(context,&effects,&changed,2,0,&sentinel)==XR_XIR_BAD_STRUCTURE &&
                sentinel==UINT32_MAX);
            copied[0].cleanup_owner=0;XrXirGeneric generics[7]={0};
            generics[1].parameter_count=1;changed.generics=generics;
            CHECK(xir_effects_reference_root(context,&effects,&changed,2,0,&sentinel)==XR_XIR_BAD_TYPE &&
                sentinel==UINT32_MAX);
            XrXirOrigin occupied={0},*out=&occupied;uint32_t out_count=0;
            CHECK(effect_context_base_owner_origins(context,owned,2,&out,&out_count)==XR_XIR_BAD_STRUCTURE &&
                out==&occupied && !out_count);
        }
    }
    xr_xir_compile_artifact_free(checked);memset(functions,0,sizeof(functions));memset(nodes,0,sizeof(nodes));
    if (status==XR_XIR_OK) {
        XrXirRootCauseTrace *trace=NULL;status=effect_context_forest_trace(context,owner->forest,5,&trace);
        if (status==XR_XIR_OK && oracle) CHECK(!owner->forest->facts[5].requires_root && !owner->forest->facts[5].unresolved);
        xr_xir_compile_root_cause_trace_free(trace);
    }
    effect_context_owner_free(owner);return status;
}

static void conditional_zero_flow_gate(const XrXirCompileContext *context) {
    XrXirCallableParameter field={(XrXirType)256,0};
    XrXirTypeNode nodes[5]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=&field,.parameter_count=1},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}};
    XrXirTypes types={nodes,5,NULL,NULL};
    XrXirType parameter=XR_XIR_I64;
    XrXirInstruction ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN}};
    XrXirFunction function={.parameters=&parameter,.parameter_count=1,
        .instructions=ops,.instruction_count=2,.result=XR_XIR_UNIT};
    XrXirModule module={.types=&types,.functions=&function,.function_count=1};
    EffectParameterFlow flow={.work=context,.module=&module,.function=&function};
    bool needed=true;uint64_t before=rp_stats(context).work;
    CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && !needed);
    CHECK(rp_stats(context).work-before==3);
    parameter=(XrXirType)256;needed=false;
    CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && needed);
    for (uint32_t container=257;container<=260;++container) {
        parameter=(XrXirType)container;needed=true;
        CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && !needed);
        ops[1]=(XrXirInstruction){.op=container==257 ? XR_XIR_NULLABLE_UNWRAP :
            container==258 ? XR_XIR_ARRAY_GET : container==259 ? XR_XIR_TUPLE_FIELD : XR_XIR_CELL_READ,
            .type=(XrXirType)256};
        needed=false;
        CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && needed);
        ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    }
    parameter=XR_XIR_I64;
    XrXirOp calls[]={XR_XIR_CALL,XR_XIR_INVOKE,XR_XIR_CALL_DEFAULT,XR_XIR_INVOKE_DEFAULT,
        XR_XIR_CALL_REQUIREMENT,XR_XIR_CALL_INDIRECT,XR_XIR_INVOKE_INDIRECT,
        XR_XIR_FUNCTION_REF,XR_XIR_FUNCTION_WEAKEN,XR_XIR_GO,XR_XIR_CLEANUP_REGISTER};
    for (uint32_t i=0;i<sizeof(calls)/sizeof(calls[0]);++i) {
        ops[1]=(XrXirInstruction){.op=calls[i],.type=XR_XIR_UNIT};needed=false;
        CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_OK && needed);
    }
    RootParameterMark physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=1;
    XrXirCompileContext limited=rp_owner(caps);uint64_t baseline=rp_stats(&limited).live_bytes;
    flow.work=&limited;needed=false;
    CHECK(effect_formula_needs_value_flow(&flow,&needed)==XR_XIR_BUDGET && needed);
    rp_owner_free(&limited,baseline);rp_balanced(physical);
}


/* This private storage probe does not create a module permission. A complete
 * execution owner below exercises the real capture hook and final proof. */
static XrXirStatus conditional_invocation_bounds(const XrXirCompileContext *context, bool oracle) {
    XrXirTypeNode nodes[3]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_REQUIRED},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_NONE}};
    XrXirTypes types={nodes,3,NULL,NULL};
    XrXirType parameters[3]={(XrXirType)256,(XrXirType)257,(XrXirType)258};
    XrXirInstruction ops[2]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256},
        {.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={3,0}}};
    XrXirFunction function={.parameters=parameters,.parameter_count=3,
        .instructions=ops,.instruction_count=2};
    EffectInvocationDeclaredBounds *bounds=NULL;
    XrXirStatus status=effect_invocation_bounds_new(context,&bounds);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_capture(context,bounds,&types,&function,0,NULL);
    if (status==XR_XIR_OK && oracle) {
        CHECK(bounds && bounds->count==1 && bounds->functions[0].values==5);
        CHECK(effect_invocation_bounds_new(context,&bounds)==XR_XIR_BAD_STRUCTURE);
        uint32_t count=bounds->count;
        CHECK(effect_invocation_bounds_capture(context,bounds,&types,&function,0,NULL)==XR_XIR_BAD_STRUCTURE &&
            bounds->count==count);
        /* A real private reference bottom and later producer mutations cannot
         * alter the receiving owner's original physical advertisement. */
        ops[0].type=(XrXirType)258;
        for (uint32_t p=0;p<3;++p) parameters[p]=(XrXirType)258;
        memset(nodes,0,sizeof(nodes));
        uint32_t expected[5]={XR_XIR_CALLABLE_ROOT_UNRESOLVED,XR_XIR_CALLABLE_ROOT_REQUIRED,
            0,XR_XIR_CALLABLE_ROOT_UNRESOLVED,XR_XIR_CALLABLE_ROOT_UNRESOLVED};
        for (uint32_t v=0;v<5;++v) {
            uint32_t mask=UINT32_MAX;
            CHECK(effect_invocation_bounds_mask(context,bounds,0,v,&mask)==XR_XIR_OK && mask==expected[v]);
        }
        uint32_t unchanged=UINT32_C(0x12345678);
        CHECK(effect_invocation_bounds_mask(context,bounds,1,0,&unchanged)==XR_XIR_BAD_STRUCTURE &&
            unchanged==UINT32_C(0x12345678));
        CHECK(effect_invocation_bounds_mask(context,bounds,0,5,&unchanged)==XR_XIR_BAD_STRUCTURE &&
            unchanged==UINT32_C(0x12345678));
        RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&foreign).live_bytes;
        CHECK(effect_invocation_bounds_mask(&foreign,bounds,0,0,&unchanged)==XR_XIR_BAD_STRUCTURE &&
            unchanged==UINT32_C(0x12345678));
        rp_owner_free(&foreign,baseline);rp_balanced(physical);
    }
    effect_invocation_bounds_free(bounds);
    if (status!=XR_XIR_OK) return status;
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,2);
    EffectContextOwner *execution=NULL;
    status=effect_context_owner_build(context,&fixture.module,NULL,&execution);
    if (status==XR_XIR_OK && oracle) {
        CHECK(execution && execution->declared && execution->declared->count==execution->dense->count);
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,execution->declared,0,2,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(execution->declared->functions[0].values==
            execution->dense->functions[0].parameter_count+execution->dense->functions[0].instruction_count);
    }
    effect_context_owner_free(execution);return status;
}


static XrXirStatus conditional_invocation_refinement(const XrXirCompileContext *context, bool oracle) {
    ConditionalRawFixture fixture;conditional_raw_fixture(&fixture,2);
    XrXirEffects effects={.resources=context->resources,.count=4};
    XrXirStatus status=xir_effects_refinement_capture(context,&fixture.module,&effects);
    if (status==XR_XIR_OK) {
        fixture.method_parameters[1]=(XrXirType)259;
        fixture.method_ops[0].type=(XrXirType)259;
    }
    EffectContextOwner *execution=NULL;
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,&fixture.module,effects.refinement,&execution);
    effect_refinement_free(effects.refinement);effects.refinement=NULL;
    memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK && oracle) {
        uint32_t mask=UINT32_MAX;
        CHECK(execution && !execution->refinement);
        CHECK(effect_invocation_bounds_mask(context,execution->declared,0,1,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(effect_invocation_bounds_mask(context,execution->declared,0,2,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    }
    effect_context_owner_free(execution);return status;
}

static XrXirStatus conditional_invocation_generic_bounds(const XrXirCompileContext *context, bool oracle) {
    XrXirCallableParameter parameters[3]={{XR_XIR_TYPE_PARAMETER_BASE,XR_PARAM_READ},
        {XR_XIR_TYPE_PARAMETER_BASE,XR_PARAM_READ},{XR_XIR_I64,XR_PARAM_READ}};
    XrXirTypeNode nodes[3]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_TYPE_PARAMETER_BASE,.parameters=&parameters[0],
         .parameter_count=1,.parameter_span=1,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_TYPE_PARAMETER_BASE,.parameters=&parameters[1],
         .parameter_count=1,.parameter_span=1,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.parameters=&parameters[2],
         .parameter_count=1,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED}};
    XrXirTypes types={nodes,3,NULL,NULL};
    XrXirType parameter=XR_XIR_TYPE_PARAMETER_BASE,arguments[2]={XR_XIR_TYPE_PARAMETER_BASE,XR_XIR_I64};
    uint32_t operand=1;
    XrXirInstruction target={.op=XR_XIR_RETURN,.args={0,0}};
    XrXirInstruction maker[2]={{.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=0,.type_arguments={0,1}},
        {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction entry[4]={{.op=XR_XIR_CALL,.type=(XrXirType)258,.immediate=1,.type_arguments={0,1}},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0,.args={0,1}},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirInstruction initializer={.op=XR_XIR_RETURN};
    XrXirBlock blocks[4]={{.first=0,.count=1},{.first=0,.count=2},
        {.first=0,.count=4},{.first=0,.count=1}};
    XrXirFunction functions[4]={
        {.name="target",.name_length=6,.parameters=&parameter,.parameter_count=1,
         .result=XR_XIR_TYPE_PARAMETER_BASE,.instructions=&target,.instruction_count=1,.blocks=&blocks[0],.block_count=1},
        {.name="maker",.name_length=5,.result=(XrXirType)256,.instructions=maker,.instruction_count=2,
         .blocks=&blocks[1],.block_count=1},
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.instructions=entry,.instruction_count=4,
         .operands=&operand,.operand_count=1,.blocks=&blocks[2],.block_count=1},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.instructions=&initializer,.instruction_count=1,
         .blocks=&blocks[3],.block_count=1}};
    XrXirConstraint constraints[2]={0};
    XrXirGeneric generics[4]={{.constraints=&constraints[0],.parameter_count=1},
        {.constraints=&constraints[1],.parameter_count=1,.arguments=&arguments[0],.argument_count=1},
        {.arguments=&arguments[1],.argument_count=1},{0}};
    XrXirFunctionIdentity identities[4]={0};
    XrXirSourceModule source={.name="bounds",.name_length=6,.initializer=3};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=2};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,
        .declarations=&declarations,.generics=generics,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    XrXirEffects effects={.resources=context->resources,.count=4};
    if (status==XR_XIR_OK) status=xir_effects_refinement_capture(context,&module,&effects);
    if (status==XR_XIR_OK) maker[0].type=(XrXirType)257;
    EffectContextOwner *execution=NULL;
    if (status==XR_XIR_OK) status=effect_context_owner_build(context,&module,effects.refinement,&execution);
    xr_xir_compile_artifact_free(checked);
    effect_refinement_free(effects.refinement);effects.refinement=NULL;
    memset(nodes,0,sizeof(nodes));memset(functions,0,sizeof(functions));
    if (status==XR_XIR_OK && oracle) {
        uint32_t selected=UINT32_MAX,mask=UINT32_MAX;
        CHECK(execution && !execution->refinement);
        CHECK(effect_invocation_bounds_mask(context,execution->declared,1,0,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        for (uint32_t n=0;n<execution->dense->count;++n) {
            const EffectOrdinaryNode *node=&execution->dense->nodes[n];
            if (node->declaration!=1 || node->argument_count!=1 || node->arguments[0]!=XR_XIR_I64) continue;
            CHECK(selected==UINT32_MAX);selected=n;
            const XrXirTypeNode *signature=xr_xir_callable_signature(&execution->dense->terms.types,
                execution->dense->functions[n].instructions[0].type);
            CHECK(signature && !signature->parameter_span && signature->result==XR_XIR_I64 &&
                signature->parameter_count==1 && signature->parameters[0].mode==XR_PARAM_READ &&
                signature->parameters[0].type==XR_XIR_I64);
            CHECK(effect_invocation_bounds_mask(context,execution->declared,n,0,&mask)==XR_XIR_OK &&
                mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        }
        CHECK(selected!=UINT32_MAX);
    }
    effect_context_owner_free(execution);return status;
}

static void conditional_new_literals(void) {
    /* Independent graphs own independent finite ledgers; each pipeline keeps its ledger. */
    for (uint32_t which=25;which<49;++which) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        XrXirStatus status=which<29 ? conditional_invocation_core(&context,which-25,true) :
            which==29 ? conditional_invocation_rotation(&context,false,true) :
            which<32 ? conditional_invocation_deferred(&context,which-30,true) :
            which<34 ? conditional_invocation_lowered_context(&context,which-32,true) :
            which<36 ? conditional_invocation_publish(&context,which-34,true) :
            which<38 ? conditional_invocation_template_publish(&context,which-36,true) :
            which<41 ? conditional_lowered_snapshot(&context,which-38,true) :
            conditional_invocation_returned(&context,which-41,true);
        if (status!=XR_XIR_OK) fprintf(stderr,"conditional new fixture %u status %u\n",which,(uint32_t)status);
        CHECK(status==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    conditional_shape_candidates(&context);conditional_domain_rejections(&context);conditional_empty_domains(&context);
    conditional_zero_flow_gate(&context);
    for (uint32_t mode=0;mode<3;++mode) CHECK(conditional_zero_carrier(&context,mode,true)==XR_XIR_OK);
    CHECK(conditional_zero_module_fn(&context,true)==XR_XIR_OK);
    CHECK(conditional_zero_arity(&context,true)==XR_XIR_OK);
    CHECK(conditional_zero_arity_cleanup(&context,true)==XR_XIR_OK);
    CHECK(conditional_base_cleanup_reference(&context,true)==XR_XIR_OK);
    CHECK(conditional_invocation_bounds(&context,true)==XR_XIR_OK);
    CHECK(conditional_invocation_refinement(&context,true)==XR_XIR_OK);
    CHECK(conditional_invocation_generic_bounds(&context,true)==XR_XIR_OK);
    for (uint32_t mode=0;mode<4;++mode) CHECK(conditional_classify(&context,mode,true)==XR_XIR_OK);
    CHECK(conditional_scalar(&context,true)==XR_XIR_OK);
    CHECK(conditional_forest(&context,true)==XR_XIR_OK);
    for (uint32_t mode=0;mode<5;++mode) CHECK(conditional_dense_owner(&context,mode,true)==XR_XIR_OK);
    CHECK(conditional_refinement(&context,true)==XR_XIR_OK);
    conditional_projection_rejections(&context);
    CHECK(conditional_cleanup_owner(&context,true)==XR_XIR_OK);
    CHECK(conditional_cell_owner(&context,true)==XR_XIR_OK);
    CHECK(conditional_cell_payload(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static XrXirStatus conditional_resource_case(const XrXirCompileContext *context, uint32_t which) {
    return which<4 ? conditional_classify(context,which,false) :
        which==4 ? conditional_scalar(context,false) : which==5 ? conditional_forest(context,false) :
        which<11 ? conditional_dense_owner(context,which-6,false) : which==11 ? conditional_refinement(context,false) :
        which==12 ? conditional_cleanup_owner(context,false) : which==13 ? conditional_cell_owner(context,false) :
        which==14 ? conditional_cell_payload(context,false) :
        which<18 ? conditional_zero_carrier(context,which-15,false) :
        which==18 ? conditional_zero_module_fn(context,false) : which==19 ? conditional_zero_arity(context,false) :
        which==20 ? conditional_zero_arity_cleanup(context,false) :
        which==21 ? conditional_base_cleanup_reference(context,false) :
        which==22 ? conditional_invocation_bounds(context,false) :
        which==23 ? conditional_invocation_refinement(context,false) :
        which==24 ? conditional_invocation_generic_bounds(context,false) :
        which<29 ? conditional_invocation_core(context,which-25,false) :
        which==29 ? conditional_invocation_rotation(context,false,false) :
        which<32 ? conditional_invocation_deferred(context,which-30,false) :
        which<34 ? conditional_invocation_lowered_context(context,which-32,false) :
        which<36 ? conditional_invocation_publish(context,which-34,false) :
        which<38 ? conditional_invocation_template_publish(context,which-36,false) :
        which<41 ? conditional_lowered_snapshot(context,which-38,false) :
        conditional_invocation_returned(context,which-41,false);
}

static void conditional_oom(void) {
    for (uint32_t which=0;which<49;++which) {
        size_t sites=0;
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
            rp_attempts=0;rp_fail_at=pass ? pass-1 : SIZE_MAX;
            XrXirStatus status=conditional_resource_case(&context,which);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts; }
            else CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}

static void conditional_axes(void) {
    for (uint32_t which=0;which<49;++which) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_resource_case(&context,which)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) {
            CHECK(measured[axis]>0);
            for (uint32_t pass=0;pass<2;++pass) {
                XrCompileResourceLimits limits=rp_caps();
                uint64_t value=measured[axis]-(pass ? 0 : 1);
                if (axis==0) limits.allocated_bytes=value;
                else if (axis==1) limits.live_bytes=value;
                else limits.work=value;
                physical=rp_mark();context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
                CHECK(conditional_resource_case(&context,which)==(pass ? XR_XIR_OK : XR_XIR_BUDGET));
                rp_owner_free(&context,baseline);rp_balanced(physical);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--compiler")) { conditional_oom();conditional_axes(); }
    else if (argc==2 && !strcmp(argv[1],"--rotation")) conditional_invocation_rotation_run(false);
    else if (argc==2 && !strcmp(argv[1],"--rotation-pipeline")) conditional_invocation_rotation_run(true);
    else if (argc==2 && !strcmp(argv[1],"--new")) conditional_new_literals();
    else { CHECK(argc==1);conditional_literals();conditional_new_literals(); }
    CHECK(!rp_live && !rp_live_bytes);return 0;
}
