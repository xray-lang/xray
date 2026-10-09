/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_contract.c - Literal physical-use derivation
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"

static void rp_direct_use(void) {
    RootParameterMark physical = rp_mark();
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    RootParameterFixture fixture; rp_fixture(&fixture,&context);
    XrXirArtifact *checked = NULL; XrXirEffects *effects = NULL;
    XrXirDiagnostic diagnostic = {0};
    fixture.module.stage = XR_XIR_BUILT;
    CHECK(xir_fixture_check(&context, &fixture.module, &checked, &diagnostic) == XR_XIR_OK && checked);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects) == XR_XIR_OK && effects);
    const XrXirFunctionEffectContract *contract = xir_effects_contract(effects,0);
    CHECK(contract && contract->parameter_count == 1);
    CHECK(contract->parameters[0].kind == 1 && contract->parameters[0].uses == 1);
    const XrXirRootEffects *root = xr_xir_effects_root(effects,0);
    CHECK(root && !root->requires_root && root->unresolved);
    memset(&fixture,0x95,sizeof(fixture));
    CHECK(contract->parameters[0].kind == 1 && contract->parameters[0].uses == 1);
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked);
    rp_owner_free(&context,baseline); rp_balanced(physical);
}

static void rp_capture_use(bool escape, bool copy) {
    RootParameterMark physical = rp_mark();
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    RootParameterFixture fixture; rp_fixture(&fixture,&context);
    XrXirInstruction factory[4] = {0};
    factory[0] = (XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,
        .args={0,1},.immediate=1};
    uint32_t value = 1, count = 1;
    if (copy) { factory[count++] = (XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={1,0}}; value = 2; }
    if (!escape) { factory[count++] = (XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=value}; value = count; }
    factory[count++] = (XrXirInstruction){.op=XR_XIR_RETURN,.args={value,0}};
    uint32_t operand = 0;
    XrXirBlock block = {.count=count};
    XrXirFunction functions[4] = {0};
    functions[0] = (XrXirFunction){.name="factory",.name_length=7,.parameters=&fixture.parameter,
        .parameter_count=1,.result=escape?(XrXirType)256:XR_XIR_I64,.blocks=&block,.block_count=1,
        .instructions=factory,.instruction_count=count,.operands=&operand,.operand_count=1};
    functions[1] = fixture.function;
    XrXirEffectParameter parameters[2] = {{0,0},{1,1}};
    parameters[0].kind = escape ? 0u : 1u;
    parameters[0].uses = escape ? (copy ? 85u : 81u) : (copy ? 21u : 17u);
    XrXirFunctionEffectContract contracts[4] = {0};
    contracts[0] = (XrXirFunctionEffectContract){.parameter_count=1,.parameters=&parameters[0]};
    XrXirRootTerm capture_term={1,0};
    if (!escape) contracts[0].formula=(XrXirRootFormula){0,1,&capture_term};
    contracts[1] = fixture.contract; contracts[1].parameters=&parameters[1];
    XrXirInstruction initializer={.op=XR_XIR_RETURN};XrXirBlock init_block={.count=1};
    functions[2]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&init_block,.block_count=1,.instructions=&initializer,.instruction_count=1};
    XrXirInstruction entry[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}};XrXirBlock entry_block={.count=2};
    functions[3]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&entry_block,.block_count=1,.instructions=entry,.instruction_count=2};
    XrXirSourceModule source={"probe",5,NULL,0,2};XrXirFunctionIdentity identities[4]={{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=3};
    contracts[2].formula.constant_mask=4;
    XrXirProvenance evidence = {.kind=1,.contracts=contracts,.contract_count=4};
    XrXirModule module = fixture.module;
    module.stage=XR_XIR_BUILT;module.functions=functions;module.function_count=4;module.provenance=&evidence;
    module.declarations=&declarations;
    XrXirArtifact *checked = NULL; XrXirEffects *effects = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(&context, &module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"capture escape%u copy%u status%u f%u b%u i%u\n",
        (unsigned)escape,(unsigned)copy,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status == XR_XIR_OK && checked);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects) == XR_XIR_OK && effects);
    const XrXirFunctionEffectContract *derived = xir_effects_contract(effects,0);
    CHECK(derived && derived->parameters[0].kind == (escape ? 0u : 1u));
    CHECK(derived->parameters[0].uses == (escape ? (copy ? 85u : 81u) : (copy ? 21u : 17u)));
    CHECK(derived->formula.constant_mask==0 && derived->formula.term_count==(escape?0u:1u));
    if (!escape) CHECK(derived->formula.terms[0].kind==1 && derived->formula.terms[0].index==0);
    const XrXirRootEffects *root = xr_xir_effects_root(effects,0);
    CHECK(root && !root->requires_root && root->unresolved == !escape);
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked);
    rp_owner_free(&context,baseline); rp_balanced(physical);
}

static void rp_reject_forged_use(void) {
    RootParameterMark physical = rp_mark();
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    RootParameterFixture fixture; rp_fixture(&fixture,&context);
    fixture.module.stage=XR_XIR_BUILT;
    static const uint32_t invalid_uses[] = {0,2,4,8,16,32,64,128,256};
    for (size_t n=0;n<sizeof(invalid_uses)/sizeof(invalid_uses[0]);++n) {
        fixture.effect_parameter.uses=invalid_uses[n];
        XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_check(&context, &fixture.module, &checked, &diagnostic)==XR_XIR_BAD_TYPE && !checked);
        CHECK(rp_stats(&context).live_bytes==baseline);
    }
    fixture.effect_parameter.uses=1;fixture.effect_parameter.kind=0;
    /* A fixed bound has a constant formula. Only the independent physical
     * use derivation may reject this otherwise well-shaped classification. */
    fixture.contract.formula=(XrXirRootFormula){.constant_mask=8};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_check(&context, &fixture.module, &checked, &diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(rp_stats(&context).live_bytes==baseline);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

/* Independent literal work derives from one relation lookup and two real
 * parameter visits. Identity never excuses malformed arrays or shape scans. */
static void rp_equal_barrier(void) {
    static const uint32_t invalid_flags[] = {0,1,10,24};
    RootParameterMark physical = rp_mark();
    XrXirCompileContext context = rp_owner(rp_caps());
    uint64_t baseline = rp_stats(&context).live_bytes;
    XrXirCallableParameter parameters[2] = {{XR_XIR_I64,0},{XR_XIR_STRING,0}};
    XrXirTypeNode nodes[2] = {0};
    nodes[0] = (XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=8,.result=XR_XIR_I64,
        .parameters=parameters,.parameter_count=2};
    nodes[1] = nodes[0];
    XrXirTypes types = {.nodes=nodes,.count=2};
    uint64_t before = rp_stats(&context).work;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)256)==XR_XIR_OK);
    CHECK(rp_stats(&context).work-before==3 && rp_stats(&context).live_bytes==baseline);
    nodes[0].parameters=NULL;nodes[0].parameter_count=1;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)256)==XR_XIR_BAD_TYPE);
    nodes[0].parameters=parameters;nodes[0].parameter_count=0;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)256)==XR_XIR_BAD_TYPE);
    nodes[0]=nodes[1];
    for (size_t n=0;n<sizeof(invalid_flags)/sizeof(invalid_flags[0]);++n) {
        nodes[0].flags=invalid_flags[n];
        CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)256)==XR_XIR_BAD_TYPE);
    }
    nodes[0]=nodes[1];nodes[1].result=XR_XIR_BOOL;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)257)==XR_XIR_BAD_TYPE);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    XrCompileResourceLimits caps=rp_caps();caps.work=2;
    context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)256,(XrXirType)256)==XR_XIR_BUDGET);
    CHECK(rp_stats(&context).work==2 && rp_stats(&context).live_bytes==baseline);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void rp_binding_check(RootParameterBindingFixture *fixture,
    const XrXirCompileContext *context, XrXirStatus expected) {
    uint64_t live=rp_stats(context).live_bytes;
    RootParameterMark physical=rp_mark();
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context, &fixture->module, &checked, &diagnostic);
    if (status!=expected) fprintf(stderr,"binding expected%u actual%u f%u b%u i%u\n",
        expected,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==expected && (expected==XR_XIR_OK ? checked!=NULL : checked==NULL));
    xr_xir_compile_artifact_free(checked);
    CHECK(rp_stats(context).live_bytes==live);rp_balanced(physical);
}

static void rp_binding_candidates(bool capture) {
    RootParameterMark physical=rp_mark();
    XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,&context,capture);
    rp_binding_check(&fixture,&context,XR_XIR_OK);
    fixture.contracts[0].bindings=NULL;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_STRUCTURE);
    rp_binding_fixture(&fixture,&context,capture);fixture.binding.value=0;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    rp_binding_fixture(&fixture,&context,capture);fixture.values[1].mode=1;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_STRUCTURE);
    rp_binding_fixture(&fixture,&context,capture);fixture.values[1].declared_type=(XrXirType)257;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    rp_binding_fixture(&fixture,&context,capture);
    fixture.contracts[0].binding_count=0;fixture.contracts[0].bindings=NULL;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    rp_binding_fixture(&fixture,&context,capture);
    fixture.apply.effect_parameter.kind=0;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    /* The real fixed barrier fixes the physical formal before invoking its
     * advertised Fn8 result. Metadata cannot legalize a caller's raw COPY. */
    rp_binding_fixture(&fixture,&context,capture);
    XrXirInstruction fixed_ops[3]={
        {.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={0,0}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock fixed_block={.count=3};
    XrXirRootValueIdentity fixed_value={0,1,(XrXirType)256};
    fixture.functions[1].instructions=fixed_ops;fixture.functions[1].instruction_count=3;
    fixture.functions[1].blocks=&fixed_block;
    fixture.contracts[1].formula=(XrXirRootFormula){.constant_mask=8};
    fixture.contracts[1].values=&fixed_value;fixture.contracts[1].value_count=1;
    fixture.apply.effect_parameter=(XrXirEffectParameter){0,8};
    if (!capture) fixture.contracts[0].formula.constant_mask=8;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    fixture.apply.effect_parameter.kind=1;
    rp_binding_check(&fixture,&context,XR_XIR_BAD_TYPE);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void rp_terminal_literals(void) {
    static const uint32_t bounds[]={2,4,8,12};
    for (size_t n=0;n<4;++n) {
        RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
        uint64_t live=rp_stats(&c).live_bytes;RootParameterFixture f;rp_fixture(&f,&c);
        f.callable.flags=bounds[n];f.module.stage=XR_XIR_BUILT;
        XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
        CHECK(xir_fixture_check(&c, &f.module, &checked, NULL)==XR_XIR_OK && checked);
        CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
        xr_xir_compile_artifact_free(checked);checked=NULL;memset(&f,0x9c,sizeof(f));
        CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,0,&trace)==XR_XIR_OK && trace);
        XrXirRootCauseTrace *occupied=trace;XrCompileResourceStats before=rp_stats(&c);
        CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,0,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==trace);
        CHECK(rp_stats(&c).work==before.work && rp_stats(&c).allocation_count==before.allocation_count);
        for (unsigned unknown=0;unknown<2;++unknown) {
            uint32_t bit=unknown?8u:4u;const XrXirRootEffectWitness *borrowed=unknown?
                xr_xir_effects_unresolved_witness(effects,0):xr_xir_effects_root_witness(effects,0);
            if (!(bounds[n]&bit)) { CHECK(!borrowed);continue; }
            CHECK(borrowed && borrowed->cause==8 && borrowed->instruction==0 &&
                borrowed->callee==UINT32_MAX && borrowed->slot==0 && borrowed->distance==0);
            XrXirRootEffectWitness original=*borrowed;
            for (unsigned field=0;field<5;++field) {
                XrXirRootEffectWitness *w=(XrXirRootEffectWitness *)borrowed;
                if (field==0) w->cause=XR_XIR_ROOT_CAUSE_CONTEXT_CALL;
                if (field==1) w->instruction=1;
                if (field==2) w->callee=0;
                if (field==3) w->slot=1;
                if (field==4) w->distance=1;
                XrXirRootCauseTrace *rejected=NULL;
                CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,0,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
                *w=original;
            }
        }
        xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);memset(&f,0x9c,sizeof(f));
        const XrXirRootEffects *facts=xr_xir_root_cause_trace_facts(trace);
        CHECK(facts && facts->requires_root==!!(bounds[n]&4) && facts->unresolved==!!(bounds[n]&8));
        for (unsigned unknown=0;unknown<2;++unknown) {
            uint32_t count=UINT32_MAX,bit=unknown?8u:4u;
            const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,unknown!=0,&count);
            CHECK(count==((bounds[n]&bit)?1u:0u));
            if (count) CHECK(step && step[0].function==0 && step[0].instruction==0 && step[0].cause==8 &&
                step[0].callee==UINT32_MAX && step[0].slot==0 && step[0].distance==0);
            else CHECK(!step);
        }
        CHECK(rp_stats(&c).live_bytes>live);
        xr_compile_resources_release(c.resources);memset(&c,0xe2,sizeof(c));
        CHECK(xr_xir_root_cause_trace_facts(trace)->unresolved==!!(bounds[n]&8));
        xr_xir_compile_root_cause_trace_free(trace);rp_balanced(physical);
    }
}

/* The same bit has two independent origins. Actual Fn2 removes the symbolic
 * contribution, so the caller must follow the slot even if PARAMETER wins
 * the callee's advertised shortest choice. Reordering does not rank constants. */
static void rp_constant_competition(bool slot_first) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t live=rp_stats(&c).live_bytes;RootParameterBindingFixture f;
    rp_binding_fixture(&f,&c,false);f.nodes[0].flags=4;
    XrXirInstruction apply[3]={
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={2,0}}};
    if (slot_first) { XrXirInstruction swap=apply[0];apply[0]=apply[1];apply[1]=swap; }
    XrXirBlock block={.count=3};XrXirSlot slot={0,XR_XIR_I64,1};
    f.functions[1].instructions=apply;f.functions[1].instruction_count=3;f.functions[1].blocks=&block;
    XrXirInstruction init[3]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9},
        {.op=XR_XIR_SLOT_INIT,.args={0,0}}, {.op=XR_XIR_RETURN}};
    f.functions[3].instructions=init;f.functions[3].instruction_count=3;f.functions[3].blocks=&block;
    f.declarations.slots=&slot;f.declarations.slot_count=1;
    f.contracts[0].formula.constant_mask=4;f.contracts[1].formula.constant_mask=4;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirRootCauseTrace *caller=NULL,*callee=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&c, &f.module, &checked, &diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"competition order%u status%u f%u b%u i%u\n",
        (unsigned)slot_first,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,0,&caller)==XR_XIR_OK && caller);
    CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,1,&callee)==XR_XIR_OK && callee);
    CHECK(xir_effects_contract(effects,0)->formula.constant_mask==4 &&
        xir_effects_contract(effects,0)->formula.term_count==0);
    XrXirRootFormula *constant=(XrXirRootFormula *)&xir_effects_contract(effects,1)->formula;
    constant->constant_mask=0;XrXirRootCauseTrace *rejected=NULL;
    CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,0,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
    constant->constant_mask=4;
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);memset(&f,0xe3,sizeof(f));
    CHECK(xr_xir_root_cause_trace_facts(caller)->requires_root && !xr_xir_root_cause_trace_facts(caller)->unresolved);
    uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(caller,false,&count);
    CHECK(count==2 && steps && steps[0].function==0 && steps[0].instruction==2 &&
        steps[0].cause==6 && steps[0].callee==1 && steps[0].distance==1 && steps[0].slot==UINT32_MAX);
    CHECK(steps[1].function==1 && steps[1].instruction==(slot_first?0u:1u) && steps[1].cause==1 &&
        steps[1].callee==UINT32_MAX && steps[1].slot==0 && steps[1].distance==0);
    steps=xr_xir_root_cause_trace_steps(callee,false,&count);
    CHECK(count==1 && steps && steps[0].function==1 && steps[0].instruction==0 &&
        steps[0].cause==(slot_first?XR_XIR_ROOT_CAUSE_MUTABLE_SLOT:XR_XIR_ROOT_CAUSE_PARAMETER) && steps[0].callee==UINT32_MAX &&
        steps[0].slot==0 && steps[0].distance==0);
    xr_xir_compile_root_cause_trace_free(caller);xr_xir_compile_root_cause_trace_free(callee);
    rp_owner_free(&c,live);rp_balanced(physical);
}

/* A real Fn4 invocation followed by opaque requirement forwarding is kind2.
 * Lack of a static witness adds UNKNOWN but cannot remove the executed ROOT. */
static void rp_conditional_known(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext c=rp_owner(rp_caps());
    uint64_t live=rp_stats(&c).live_bytes;
    XrXirCallableParameter callback={(XrXirType)256,0};
    XrXirTypeNode nodes[2]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=4},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=8,.parameters=&callback,.parameter_count=1}};
    XrXirInterfaceMethod method={{"forward",7},(XrXirType)257,0,0,NULL};
    XrXirInterfaceDeclaration interface={{"probe",5},{"Forward",7},1,NULL,0,NULL,0,&method,1};
    XrXirInterfaceTable interfaces={&interface,1};XrXirTypes types={nodes,2,NULL,&interfaces};
    XrXirInstruction entry[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0,0}}}, init={.op=XR_XIR_RETURN};
    XrXirInstruction ops[3]={
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CALL_REQUIREMENT,.type=XR_XIR_I64,.args={0,2}}, {.op=XR_XIR_RETURN,.args={3,0}}};
    XrXirBlock one={.count=1},two={.count=2},three={.count=3};
    XrXirType parameters[2]={XR_XIR_TYPE_PARAMETER_BASE,(XrXirType)256};uint32_t operands[2]={0,1};
    XrXirFunction functions[3]={
        {.name="entry",.name_length=5,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=entry,.instruction_count=2},
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&one,.block_count=1,.instructions=&init,.instruction_count=1},
        {.name="conditional",.name_length=11,.result=XR_XIR_I64,.parameters=parameters,.parameter_count=2,
            .blocks=&three,.block_count=1,.instructions=ops,.instruction_count=3,.operands=operands,.operand_count=2}};
    XrXirInterfaceApplication application={0,NULL,0};XrXirConstraint constraint={0,&application,1};
    XrXirGeneric generics[3]={{0}};generics[2].constraints=&constraint;generics[2].parameter_count=1;
    XrXirSourceModule source={"probe",5,NULL,0,1};XrXirFunctionIdentity identities[3]={{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=0};
    XrXirEffectParameter effect_parameters[2]={{0,0},{2,3}};XrXirRootTerm term={2,1};
    XrXirFunctionEffectContract contracts[3]={0};contracts[1].formula.constant_mask=4;
    contracts[2]=(XrXirFunctionEffectContract){.parameter_count=2,.parameters=effect_parameters,.formula={12,1,&term}};
    XrXirProvenance evidence={.kind=1,.contracts=contracts,.contract_count=3};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,.declarations=&declarations,.generics=generics,.types=&types,.provenance=&evidence,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&c, &module, &checked, &diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"conditional status%u f%u b%u i%u\n",status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    const XrXirFunctionEffectContract *contract=xir_effects_contract(effects,2);
    CHECK(contract && contract->parameters[1].kind==2 && contract->parameters[1].uses==3);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_root_cause_trace_copy(&c,effects,2,&trace)==XR_XIR_OK && trace);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_root_cause_trace_facts(trace)->requires_root && xr_xir_root_cause_trace_facts(trace)->unresolved);
    for (unsigned unknown=0;unknown<2;++unknown) {
        uint32_t count=0;const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,unknown!=0,&count);
        CHECK(count==1 && step && step->function==2 && step->instruction==0 &&
            step->cause==(unknown?XR_XIR_ROOT_CAUSE_CONTEXT_CALL:XR_XIR_ROOT_CAUSE_INDIRECT) && step->callee==UINT32_MAX && step->slot==UINT32_MAX && !step->distance);
    }
    xr_xir_compile_root_cause_trace_free(trace);rp_owner_free(&c,live);rp_balanced(physical);
}

int main(void) {
    rp_constant_competition(false);rp_constant_competition(true);rp_conditional_known();
    rp_terminal_literals();
    rp_equal_barrier();
    rp_binding_candidates(false);rp_binding_candidates(true);
    rp_direct_use();
    rp_capture_use(false,false);rp_capture_use(false,true);
    rp_capture_use(true,false);rp_capture_use(true,true);
    rp_reject_forged_use();
    CHECK(!rp_live && !rp_live_bytes);
    return 0;
}
