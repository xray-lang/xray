/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effect_source_terminal_ordinary.c - Current Source and detached causes
 *
 * KEY CONCEPT:
 *   One real callback separates pure execution from a genuine module Cell read.
 *   Current Source must match, and copied cause values outlive their producers.
 */
#include "xir_construction_fixture.h"
#include "xir_effect_input_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_view_access_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1); } } while (0)

XR_FUNC uint64_t xir_test_effect_input_clone_calls(void);
XR_FUNC bool xir_test_effect_input_compare_cancelled(void);
XR_FUNC void xir_test_effect_input_arm(uint32_t fault);
XR_FUNC bool xir_test_effect_input_cancel(const void *data);
XR_FUNC uint64_t xir_test_effect_input_live(void);
XR_FUNC uint64_t xir_test_effect_input_oom_attempts(void);

typedef struct OrdinarySourceFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;XrXirType parameter;
    XrXirInstruction run[3],apply[2],pure[2],root[3],init[4];
    uint32_t operand;XrXirBlock blocks[5];XrXirFunction functions[5];
    XrXirEffectParameter role;XrXirRootTerm term;XrXirRootValueIdentity reference;
    XrXirFunctionEffectContract contracts[5];XrXirProvenance evidence;
    XrXirFunctionIdentity identities[5];XrXirSourceModule source;XrXirSlot slot;
    XrXirDeclarations declarations;XrXirModule module;
} OrdinarySourceFixture;

static void ordinary_fixture(OrdinarySourceFixture *f,bool root) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};f->parameter=(XrXirType)256;
    f->run[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=root?3:2};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,1}};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->apply[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    f->apply[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->root[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257,.immediate=0};
    f->root[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0,0}};
    f->root[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)257,.args={0,0}};
    f->init[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1,0},.immediate=0};
    f->init[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,
        .instructions=f->run,.instruction_count=3,.operands=&f->operand,.operand_count=1};
    f->functions[1]=(XrXirFunction){.name="apply",.name_length=5,.result=XR_XIR_I64,
        .parameters=&f->parameter,.parameter_count=1,.instructions=f->apply,.instruction_count=2};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->pure,.instruction_count=2};
    f->functions[3]=(XrXirFunction){.name="root",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->root,.instruction_count=3};
    f->functions[4]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=f->init,.instruction_count=4};
    for (uint32_t i=0;i<5;++i) {
        f->blocks[i]=(XrXirBlock){.count=f->functions[i].instruction_count};
        f->functions[i].blocks=&f->blocks[i];f->functions[i].block_count=1;
        f->functions[i].error_type=XR_XIR_NEVER;
    }
    f->role=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,XR_XIR_EFFECT_USE_INVOKE};
    f->term=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,0};
    f->reference=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->contracts[0]=(XrXirFunctionEffectContract){
        .formula={.constant_mask=root?XR_XIR_CALLABLE_ROOT_REQUIRED:0},.values=&f->reference,.value_count=1};
    f->contracts[1]=(XrXirFunctionEffectContract){.parameters=&f->role,.parameter_count=1,
        .formula={.terms=&f->term,.term_count=1}};
    f->contracts[3].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[4].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=5};
    f->source=(XrXirSourceModule){.name="ordinary",.name_length=8,.initializer=4};
    f->slot=(XrXirSlot){.type=(XrXirType)257,.mutable=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .slots=&f->slot,.slot_count=1,.entry_function=0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.provenance=&f->evidence,.linkage_kind=XR_XIR_PROGRAM};
}

static void ordinary_trace(const XrXirRootCauseTrace *trace,bool root) {
    const XrXirRootEffects *facts=xr_xir_root_cause_trace_facts(trace);
    CHECK(facts && facts->requires_root==root && !facts->unresolved);
    uint32_t count=UINT32_MAX;
    const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,true,&count);
    CHECK(!count && !steps);
    steps=xr_xir_root_cause_trace_steps(trace,false,&count);
    if (!root) { CHECK(!count && !steps);return; }
    static const uint32_t functions[]={0,1,3},instructions[]={1,0,0};
    CHECK(count==3 && steps);
    for (uint32_t i=0;i<3;++i) {
        CHECK(steps[i].function==functions[i] && steps[i].instruction==instructions[i] && steps[i].distance==2-i);
        CHECK(steps[i].callee==(i==2?UINT32_MAX:functions[i+1]));
        CHECK(steps[i].slot==(i==2?0u:UINT32_MAX));
        CHECK(steps[i].cause==(i==2?XR_XIR_ROOT_CAUSE_MUTABLE_SLOT:XR_XIR_ROOT_CAUSE_CALL));
    }
}

static void ordinary_current(const XrXirCompileContext *context,XirFixtureEffectInput input,
    const XrXirEffects *effects) {
    CHECK(input.module && input.construction);
    CHECK(xir_fixture_effects_refresh(context,input,effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_fixture_effects_terminal(context,input,effects)==XR_XIR_OK);
    if(!xr_xir_effects_read(effects,0).root.requires_root) {
        XrXirOrigin caller={.function=0},target={.function=2};
        XirEffectCaptureRequest request={.source=input.module,.types=input.module->types,
            .effects=effects,.caller=&caller,.target=&target,.actual=&input.module->functions[0],
            .instruction=0,.construction=input.construction};
        XirEffectCaptureBound result={UINT32_MAX,true};
        CHECK(xir_effect_capture_bound_input(context,&request,&result)==XR_XIR_OK && result.refined && result.flags==XR_XIR_CALLABLE_ROOT_NONE);
        request.construction=NULL;result=(XirEffectCaptureBound){UINT32_MAX,true};
        CHECK(xir_effect_capture_bound_input(context,&request,&result)==XR_XIR_BAD_STRUCTURE && result.flags==UINT32_MAX && result.refined);
    }
    XrXirModule current=*input.module;XrXirFunction functions[5];XrXirInstruction rows[2];
    CHECK(current.function_count==5 && current.functions[2].instruction_count==2);
    memcpy(functions,current.functions,sizeof(functions));memcpy(rows,functions[2].instructions,sizeof(rows));
    current.functions=functions;functions[2].instructions=rows;
    XirFixtureEffectInput changed={&current,input.construction};
    CHECK(rows[0].op==XR_XIR_CONST_INT && rows[0].immediate==7);
    rows[0].immediate=8;
    CHECK(xir_fixture_effects_terminal(context,changed,effects)==XR_XIR_BAD_STRUCTURE);
    rows[0].immediate=7;
    CHECK(xir_fixture_effects_terminal(context,changed,effects)==XR_XIR_OK);
    memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
}

static void ordinary_slots(const XrXirConstruction *construction) {
    const XrXirSlotFacts *slots=xr_xir_compile_construction_slots(construction);
    CHECK(slots && slots->count==1 && slots->rows);
    CHECK(slots->rows[0].slot==0 && !slots->rows[0].exported && slots->rows[0].name_length==7);
    CHECK(slots->rows[0].name && !memcmp(slots->rows[0].name,"fixture",7));
}

static void ordinary_missing_slot(const XrXirCompileContext *context,const OrdinarySourceFixture *fixture) {
    XrXirConstruction *missing=NULL;XrXirArtifact *rejected=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_construction(context,&fixture->types,&missing)==XR_XIR_OK && missing);
    CHECK(xr_xir_compile_check_v2(context,&fixture->module,missing,&rejected,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(!rejected && diagnostic.status==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_construction_free(missing);
}

static void ordinary_current_facts(const XrXirCompileContext *context,
    XirFixtureEffectInput input,const XrXirEffects *effects) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_fixture_effects_terminal(context,(XirFixtureEffectInput){input.module,NULL},effects)==XR_XIR_BAD_STRUCTURE);
    XrXirDiagnostic diagnostic={0};
    CHECK(xir_effects_parameters_match_input(context,(XirEffectInput){input.module,NULL,NULL},effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effects_local_errors_verify_input(context,(XirEffectInput){input.module,NULL,NULL},effects,&diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effects_parameters_match_input(context,(XirEffectInput){input.module,input.construction,NULL},effects)==XR_XIR_OK);
    CHECK(xir_effects_local_errors_verify_input(context,(XirEffectInput){input.module,input.construction,NULL},effects,&diagnostic)==XR_XIR_OK);

    XrXirEffects *rejected=NULL;
    CHECK(xir_fixture_effects_infer(context,(XirFixtureEffectInput){input.module,NULL},&rejected)==XR_XIR_BAD_STRUCTURE && !rejected);
    const XrXirSlotFacts *original=xr_xir_compile_construction_slots(input.construction);
    CHECK(original && original->count==1);
    XrXirConstructionSpec spec={0};XrXirConstruction *alternate=NULL;
    XrXirSlotFact row=original->rows[0];row.name="changed";
    XrXirSlotFacts changed={&row,1};
    CHECK(xr_xir_compile_construction_new_v4(context,input.module->types,&spec,&changed,&alternate,NULL)==XR_XIR_OK);
    CHECK(xir_fixture_effects_terminal(context,(XirFixtureEffectInput){input.module,alternate},effects)==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_construction_free(alternate);alternate=NULL;
    row=original->rows[0];row.exported=1;
    CHECK(xr_xir_compile_construction_new_v4(context,input.module->types,&spec,&changed,&alternate,NULL)==XR_XIR_OK);
    CHECK(xir_fixture_effects_terminal(context,(XirFixtureEffectInput){input.module,alternate},effects)==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_construction_free(alternate);alternate=NULL;
    XrXirSlotFacts empty={0};
    CHECK(xr_xir_compile_construction_new_v4(context,input.module->types,&spec,&empty,&alternate,NULL)==XR_XIR_OK);
    CHECK(xir_fixture_effects_terminal(context,(XirFixtureEffectInput){input.module,alternate},effects)==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_construction_free(alternate);
    CHECK(xir_fixture_effects_terminal(context,input,effects)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
}

static void ordinary_control(const XrXirCompileContext *context,XirFixtureEffectInput input) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    uint64_t live=xir_test_effect_input_live();
    for(uint32_t mode=1;mode<=4;++mode) {
        XrCompileRequestControl control={0};
        XrCompileRequestOptions options={xir_test_effect_input_cancel,NULL,
            mode==2 || mode==3?XR_COMPILE_DEADLINE_AFTER:XR_COMPILE_DEADLINE_NONE,
            mode==2 || mode==3?UINT64_C(10000000000):0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        xir_test_effect_input_arm(mode);
        XrXirEffects *rejected=NULL;uint64_t attempts=xir_test_effect_input_oom_attempts();
        XrXirStatus expected=mode==1?XR_XIR_CANCELLED:mode==2?XR_XIR_DEADLINE:mode==3?XR_XIR_CLOCK_FAILURE:XR_XIR_OUT_OF_MEMORY;
        CHECK(xr_xir_compile_effects_infer_verified_input(context,
            (XirEffectInput){input.module,input.construction,&control},&rejected)==expected && !rejected);
        xir_test_effect_input_arm(0);
        if(mode==4)CHECK(xir_test_effect_input_oom_attempts()==attempts+1);
        else CHECK(xr_compile_request_control_poll(&control)==(mode==1?XR_COMPILE_RESOURCE_CANCELLED:
            mode==2?XR_COMPILE_RESOURCE_DEADLINE:XR_COMPILE_RESOURCE_CLOCK_FAILURE));
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
        CHECK(xir_test_effect_input_live()==live);
        XrCompileRequestControl fresh={0};XrCompileRequestOptions fresh_options={0};
        CHECK(xr_compile_request_control_init(&fresh_options,&fresh)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_effects_infer_verified_input(context,
            (XirEffectInput){input.module,input.construction,&fresh},&rejected)==XR_XIR_OK && rejected);
        xr_xir_compile_effects_free(rejected);
        CHECK(xir_test_effect_input_live()==live);
    }
}

static void ordinary_case(bool root) {
    /* Ordinary functional admission retains no cumulative allocation quota. */
    XrCompileResourceLimits caps={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    OrdinarySourceFixture fixture;ordinary_fixture(&fixture,root);
    ordinary_missing_slot(&context,&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
    XrXirConstruction *construction=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_module_construction(&context,&fixture.module,&construction)==XR_XIR_OK && construction);
    ordinary_slots(construction);
    XrXirStatus status=xr_xir_compile_check_v2(&context,&fixture.module,construction,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"ordinary root%u check%u diagnostic%u f%u b%u i%u\n",
        (unsigned)root,(unsigned)status,(unsigned)diagnostic.status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    xr_xir_compile_construction_free(construction);
    XirFixtureEffectInput input={xr_xir_compile_artifact_module(checked),xr_xir_compile_artifact_construction(checked)};
    ordinary_slots(input.construction);
    uint64_t clones=xir_test_effect_input_clone_calls();
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    CHECK(xir_test_effect_input_clone_calls()==clones+1);
    clones=xir_test_effect_input_clone_calls();
    XrXirEffectFacts actual=xr_xir_effects_read(effects,0);
    CHECK(actual.status==XR_XIR_OK && actual.root.requires_root==root && !actual.root.unresolved);
    ordinary_current(&context,input,effects);
    ordinary_current_facts(&context,input,effects);
    CHECK(xir_test_effect_input_clone_calls()==clones);
    ordinary_control(&context,input);
    CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,0,&trace)==XR_XIR_OK && trace);
    ordinary_trace(trace,root);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL;
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,&diagnostic)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    input=(XirFixtureEffectInput){xr_xir_compile_artifact_module(received),xr_xir_compile_artifact_construction(received)};
    CHECK(xir_fixture_effects_terminal(&context,input,effects)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(received);
    memset(&fixture,0xCE,sizeof(fixture));input=(XirFixtureEffectInput){0};
    xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
    ordinary_trace(trace,root);xr_xir_compile_root_cause_trace_free(trace);
}

static void ordinary_control_relays(const XrXirCompileContext *context,const XrXirArtifact *checked) {
    XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK && closed);
    CHECK(closed->module.provenance && closed->module.provenance->source &&
        closed->module.provenance->source->source_facts);
    XrCompileResourceStats frame={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&frame)==XR_COMPILE_RESOURCE_OK);
    XrCompileRequestOptions options={xir_test_effect_input_cancel,NULL,XR_COMPILE_DEADLINE_NONE,0};
    XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    xir_test_effect_input_arm(5);
    CHECK(xir_artifact_verify(closed,NULL,&control)==XR_XIR_CANCELLED);
    CHECK(xir_test_effect_input_compare_cancelled());
    xir_test_effect_input_arm(0);
    CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_CANCELLED);
    XrCompileRequestControl fresh={0};
    CHECK(xr_compile_request_control_init(&options,&fresh)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_artifact_verify(closed,NULL,&fresh)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==frame.held_bytes);
    xr_xir_compile_artifact_free(closed);
}

static void ordinary_view_control(void) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_EFFECT_INPUT_WORK};
    XrXirSourceRequest request={session,XR_EFFECT_INPUT_WORK "/relay.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    static const char text[]="class Options{value:i64;init(){this.value=7}}\n"
        "fn use(v:Slice<u8>,options:Options=Options())->i64{return len(v)+options.value}\n"
        "export fn storage_receiver()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view)-9}";
    XrXirSourceText source={"relay.xr",text,sizeof(text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check_text(&request,&source,&result,&diagnostic,NULL)==XR_XIR_OK && result.checked);
    xr_compile_session_free(session);
    XrCompileResourceStats frame={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
    XrCompileRequestOptions options={xir_test_effect_input_cancel,NULL,XR_COMPILE_DEADLINE_NONE,0};
    XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    uint64_t clones=xir_test_effect_input_clone_calls();
    xir_test_effect_input_arm(1);
    XirEffectInput input={xr_xir_compile_artifact_module(result.checked),xr_xir_compile_artifact_construction(result.checked),&control};
    CHECK(xir_compile_view_cell_access_verified_input(&context,input,NULL)==XR_XIR_CANCELLED);
    CHECK(xir_test_effect_input_clone_calls()==clones+1);
    xir_test_effect_input_arm(0);
    CHECK(xr_compile_request_control_poll(&control)==XR_COMPILE_RESOURCE_CANCELLED);
    XrCompileRequestControl fresh={0};
    CHECK(xr_compile_request_control_init(&options,&fresh)==XR_COMPILE_RESOURCE_OK);input.control=&fresh;
    CHECK(xir_compile_view_cell_access_verified_input(&context,input,NULL)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==frame.held_bytes);
    xr_xir_compile_source_result_free(&result);xr_compile_resources_release(context.resources);
}

static void ordinary_zero_slots(void) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_EFFECT_INPUT_WORK};
    XrXirSourceRequest request={session,XR_EFFECT_INPUT_WORK "/zero.xr",&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    static const char text[]="print(42)\n";
    XrXirSourceText source={"zero.xr",text,sizeof(text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check_text(&request,&source,&result,&diagnostic,NULL)==XR_XIR_OK && result.checked);
    xr_compile_session_free(session);session=NULL;
    XirFixtureEffectInput input={xr_xir_compile_artifact_module(result.checked),xr_xir_compile_artifact_construction(result.checked)};
    const XrXirSlotFacts *slots=xr_xir_compile_construction_slots(input.construction);
    CHECK(slots && slots->count==0);
    XrXirEffects *effects=NULL;uint64_t clones=xir_test_effect_input_clone_calls();
    CHECK(xir_fixture_effects_infer(&context,input,&effects)==XR_XIR_OK && effects);
    CHECK(xir_test_effect_input_clone_calls()==clones+1);
    CHECK(xir_fixture_effects_body_match(&context,(XirFixtureEffectInput){input.module,NULL},effects)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_fixture_effects_body_match(&context,input,effects)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);ordinary_control_relays(&context,result.checked);xr_xir_compile_source_result_free(&result);
    xr_compile_resources_release(context.resources);
}

int main(void) {
    ordinary_case(false);ordinary_case(true);ordinary_zero_slots();ordinary_view_control();
    CHECK(xir_test_effect_input_live()==0);
    puts("ordinary Source current input and detached causes PASS");return 0;
}
