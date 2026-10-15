#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_closed_carriers_controls.h - Closed outer values have no error atoms
 *
 * KEY CONCEPT:
 *   Empty container facts never authorize a callable or erase extracted errors.
 *   Complete admission and real resource failures remain independent proofs.
 */
#ifndef XIR_ERROR_CLOSED_CARRIERS_CONTROLS_H
#define XIR_ERROR_CLOSED_CARRIERS_CONTROLS_H
enum { CC_ARRAY=258,CC_FN=259,CC_UNKNOWN_FN=260,CC_NULLABLE=261,CC_TUPLE=262,
    CC_ATOMIC=263,CC_STRUCT=264,CC_CLASS=265,CC_TASK=266,CC_ERROR_NULLABLE=267,
    CC_ARRAY_CELL=268,CC_ARRAY_TASK=269 };
typedef struct ClosedCarrierFixture {
    EdgeFixture edge;
    XrXirTypeNode nodes[14];
    XrXirNominalDeclaration declarations[3];
    XrXirNominalField field;
    XrXirType field_type,parameter;
    XrXirCallableParameter tuple[2];
} ClosedCarrierFixture;
static void cc_types(ClosedCarrierFixture *f) {
    EdgeFixture *e=&f->edge;
    memcpy(f->nodes,e->nodes,sizeof(e->nodes));
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)EE_ENUM};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)EE_ENUM};
    f->tuple[0]=(XrXirCallableParameter){(XrXirType)EE_ENUM,0};
    f->tuple[1]=(XrXirCallableParameter){XR_XIR_I64,0};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->tuple,.parameter_count=2};
    f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
    f->field_type=(XrXirType)EE_ENUM;
    f->field=(XrXirNominalField){.name={"payload",7},.type=(XrXirType)EE_ENUM};
    f->declarations[0]=e->nominal;
    f->declarations[1]=(XrXirNominalDeclaration){.module={"edges",5},.name={"Value",5},
        .exported=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->declarations[2]=(XrXirNominalDeclaration){.module={"edges",5},.name={"Object",6},
        .exported=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nodes[8]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=1,.fields=&f->field_type,.field_count=1}};
    f->nodes[9]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=2,.fields=&f->field_type,.field_count=1}};
    f->nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};
    f->nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_ERROR};
    f->nodes[12]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)CC_ARRAY};
    f->nodes[13]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=(XrXirType)CC_ARRAY};
    e->types.nodes=f->nodes;e->types.count=14;
    e->nominals.declarations=f->declarations;e->nominals.count=3;
}
static void cc_fixture(ClosedCarrierFixture *f,unsigned mode) {
    memset(f,0,sizeof(*f));EdgeFixture *e=&f->edge;
    ee_fixture(e,mode ? EE_JUMP : EE_BULK,false);cc_types(f);
    f->parameter=(XrXirType)CC_ARRAY;
    e->functions[1].parameters=&f->parameter;e->functions[1].parameter_count=1;
    if (!mode) {
        for (uint32_t i=0;i<512;++i)
            e->subject[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)CC_ARRAY,.args={0}};
        e->subject[544].args[0]=513;pc_reverse_blocks(e);return;
    }
    XrXirOp extract=XR_XIR_NULLABLE_UNWRAP;XrXirType result=(XrXirType)EE_ENUM;
    if (mode==1) f->parameter=(XrXirType)CC_NULLABLE;
    else if (mode==2) { f->parameter=(XrXirType)CC_TUPLE;extract=XR_XIR_TUPLE_FIELD; }
    else if (mode==3) { f->parameter=(XrXirType)CC_STRUCT;extract=XR_XIR_STRUCT_GET; }
    else if (mode==4) { f->parameter=(XrXirType)CC_CLASS;extract=XR_XIR_CLASS_GET; }
    else if (mode==5) { f->parameter=(XrXirType)CC_ERROR_NULLABLE;result=XR_XIR_ERROR; }
    else if (mode==6 || mode==7) {
        f->parameter=(XrXirType)(mode==6 ? CC_FN : CC_UNKNOWN_FN);
        e->subject[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT};e->subject[1]=e->init;
        e->blocks[0]=(XrXirBlock){.count=2};
        e->functions[1].block_count=1;e->functions[1].instruction_count=2;return;
    } else {
        CHECK(mode==8);f->parameter=(XrXirType)CC_ARRAY_CELL;
        e->subject[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CC_ARRAY,.args={0}};
        e->subject[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        e->subject[2]=(XrXirInstruction){.op=XR_XIR_ARRAY_GET,.type=(XrXirType)EE_ENUM,.args={1,2}};
        e->subject[3]=(XrXirInstruction){.op=XR_XIR_THROW,.args={3}};
        e->blocks[0]=(XrXirBlock){.count=4};
        e->functions[1].block_count=1;e->functions[1].instruction_count=4;return;
    }
    e->subject[0]=(XrXirInstruction){.op=extract,.type=result,.args={0}};
    e->subject[1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
    e->blocks[0]=(XrXirBlock){.count=2};
    e->functions[1].block_count=1;e->functions[1].instruction_count=2;
}
static void cc_classification(void) {
    ClosedCarrierFixture f;cc_fixture(&f,0);EdgeFixture *e=&f.edge;
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    ErrorFlow flow={0};flow.body=effect_body_raw_request(&c,&e->module);
    flow.function_index=1;flow.remaining=&c;
    CHECK(effect_body_request_head(&flow.body,1,&flow.head)==XR_XIR_OK);flow.function=&flow.head;
    XrXirType classification_type=(XrXirType)UINT32_MAX;bool classified_copy=false;uint32_t classified_source=UINT32_MAX;
    for (uint32_t t=0;t<14;++t) {
        f.parameter=(XrXirType)(256+t);bool active=true;uint64_t before=pc_stats(&c).work;
        CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK);
        CHECK(classification_type==f.parameter && !classified_copy && classified_source==UINT32_MAX);
        CHECK(active==(t==0 || t==1 || t==10 || t==12 || t==13));
        CHECK(pc_stats(&c).work-before==((t==0 || t==8 || t==9) ? 7u : 6u));
    }
    f.parameter=(XrXirType)CC_ARRAY;
    XrXirTypeNode saved=f.nodes[2];
    for (unsigned mode=0;mode<4;++mode) {
        f.nodes[2]=saved;
        if (!mode) f.nodes[2].parameter_span=1;
        else if (mode==1) f.nodes[2].kind=999;
        else if (mode==2) f.nodes[2].flags=1;
        else f.nodes[2].nominal.field_count=1;
        bool active=false;CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    }
    f.nodes[2]=saved;f.parameter=(XrXirType)CC_STRUCT;
    XrXirNominalIdentity identities[3]={{0}};
    for (uint32_t d=0;d<3;++d) identities[d].kind=f.declarations[d].kind;
    e->nominals.identities=identities;
    bool active=false;CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    e->nominals.declarations=NULL;
    CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && !active);
    identities[1].kind=XR_XIR_NOMINAL_ENUM;
    CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    identities[1].kind=999;
    CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    e->nominals.identities=NULL;
    CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    f.parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    f.parameter=XR_XIR_ERROR;CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    f.parameter=(XrXirType)UINT32_MAX;CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==XR_XIR_OK && active);
    CHECK(pc_stats(&c).live_bytes==baseline);pc_release(&c,baseline);
}
static void cc_classification_limits(void) {
    for (unsigned nominal=0;nominal<2;++nominal) for (unsigned enough=0;enough<2;++enough) {
        ClosedCarrierFixture f;cc_fixture(&f,0);
        f.parameter=(XrXirType)(nominal ? CC_STRUCT : CC_ARRAY);
        XrCompileResourceLimits caps=pc_caps();XrXirCompileContext c=pc_owner(caps);
        uint64_t baseline=pc_stats(&c).live_bytes,remaining=nominal ? 7u : 6u;
        ErrorFlow flow={0};flow.body=effect_body_raw_request(&c,&f.edge.module);
        flow.function_index=1;flow.remaining=&c;
        CHECK(effect_body_request_head(&flow.body,1,&flow.head)==XR_XIR_OK);flow.function=&flow.head;
        XrXirType classification_type=(XrXirType)UINT32_MAX;bool classified_copy=false;uint32_t classified_source=UINT32_MAX;
        if (!enough) --remaining;
        uint64_t used=pc_stats(&c).work;
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-remaining)==XR_COMPILE_RESOURCE_OK);
        bool active=true;uint64_t before=pc_stats(&c).work;size_t allocations=attempts;
        CHECK(error_row_active(&flow,0,&active,&classification_type,&classified_copy,&classified_source)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(active==(enough==0) && attempts==allocations);
        CHECK(classification_type==(enough ? f.parameter : (XrXirType)UINT32_MAX));
        CHECK(!classified_copy && classified_source==UINT32_MAX);
        CHECK(pc_stats(&c).work-before==(enough ? nominal+6u : nominal+5u));pc_release(&c,baseline);
    }
}
static void cc_private(void) {
    ClosedCarrierFixture f;cc_fixture(&f,0);
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
    pc_private_begin(&c,&f.edge,&e,&terms,&flow);uint64_t before=pc_stats(&c).work;
    CHECK(error_function(&flow,1)==XR_XIR_OK && e.outcomes_work.errors[1]==4);
    CHECK(pc_stats(&c).work-before<UINT64_C(25000));
    CHECK(flow.values==546 && flow.active_count==1 && flow.active[0]==513);
    CHECK(flow.stride==1 && e.outcomes_work.words==1 && flow.storage_capacity==4692);
    CHECK(flow.slots[0]==UINT32_MAX && flow.slots[513]==0 && flow.zero[0]==0);
    for (uint32_t v=0;v<513;++v) CHECK(!error_value(&flow,flow.work,v) && error_read(&flow,flow.work,v)[0]==0);
    memset(flow.work,0xa5,sizeof(uint64_t));uint64_t poison=flow.work[0];
    XrXirInstruction storage;const XrXirInstruction *actual=NULL;
    CHECK(error_operation(&flow,flow.function_index,0,&storage,&actual)==XR_XIR_OK);
    CHECK(error_instruction(&flow,0,actual)==XR_XIR_OK && flow.work[0]==poison && flow.zero[0]==0);
    pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
}
/* The bitmap is a local transfer fact, not a substitute for raw or owned
 * admission. Inactive calls and writes must still update their real facts. */
static void cc_cfg_transfer_fixture(ClosedCarrierFixture *f) {
    cc_fixture(f,0);EdgeFixture *e=&f->edge;
    e->parameters[0]=(XrXirType)CC_ARRAY;e->parameters[1]=(XrXirType)EE_CELL;
    e->functions[1].parameters=e->parameters;e->functions[1].parameter_count=2;
    e->subject[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)CC_ARRAY,.args={0}};
    e->subject[1]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
    e->subject[2]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={1,3}};
    e->subject[3]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
    e->subject[4]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)EE_ENUM,.args={1}};
    e->subject[5]=(XrXirInstruction){.op=XR_XIR_THROW,.args={6}};
    e->blocks[0]=(XrXirBlock){.count=6};e->functions[1].instruction_count=6;
    e->functions[1].block_count=1;
}
static void cc_cfg_continuation(void) {
    for (unsigned effectful=0;effectful<2;++effectful) for (unsigned owned=0;owned<2;++owned) {
        ClosedCarrierFixture f;
        if (effectful) cc_cfg_transfer_fixture(&f);else cc_fixture(&f,0);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f.edge,&e,&terms,&flow);e.outcomes_work.errors[2]=8;
        EffectBodyMaterial *material=NULL;
        if (owned) {
            CHECK(effect_body_material_create(&c,&f.edge.module,&material)==XR_XIR_OK && material);
            flow.body=effect_body_owned_request(&c,material);
        }
        size_t allocations=attempts;
        CHECK(error_function(&flow,1)==XR_XIR_OK && e.outcomes_work.errors[1]==(effectful ? 12u : 4u));
        CHECK(attempts==allocations+1 && flow.zero[0]==0);
        if (!effectful) {
            CHECK(flow.values==546 && flow.active_count==1 && flow.active[0]==513 && flow.stride==1);
            CHECK(flow.storage_capacity==(owned ? 4761u : 4692u));
            if (owned) {
                const uint8_t *bits=flow.reachable+flow.function->block_count;
                for (uint32_t i=0;i<69;++i) CHECK(bits[i]==(i<64 ? 255u : 0u));
                uint32_t first=material->functions[1].runs[0].first;uint64_t saved=e.outcomes_work.errors[1];
                allocations=attempts;uint64_t held=pc_stats(&c).live_bytes;
                material->functions[1].runs[0].first=1;
                CHECK(error_function(&flow,1)==XR_XIR_BAD_STRUCTURE && e.outcomes_work.errors[1]==saved);
                CHECK(attempts==allocations && pc_stats(&c).live_bytes==held);
                material->functions[1].runs[0].first=first;
                f.edge.subject[0].op=XR_XIR_INVALID;f.nodes[2].flags=1;
                CHECK(xir_fixture_verify(&c,&f.edge.module,NULL)==XR_XIR_BAD_STRUCTURE);
                allocations=attempts;held=pc_stats(&c).live_bytes;
                memset(flow.storage,0xff,flow.storage_capacity);
                CHECK(error_function(&flow,1)==XR_XIR_OK && e.outcomes_work.errors[1]==4 && flow.zero[0]==0);
                CHECK(attempts==allocations && pc_stats(&c).live_bytes==held);
            }
        } else {
            /* CALL has no output row, yet contributes variant1 and invalidates
             * the Cell after the concrete variant1 write. */
            CHECK(flow.slots[4]==UINT32_MAX && flow.slots[5]==UINT32_MAX);
            CHECK(error_read(&flow,flow.work,6)[0]==12 && flow.cell_count==1);
            if (owned) CHECK(*(flow.reachable+flow.function->block_count)==1);
        }
        effect_body_material_free(material);pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
}
static void cc_cfg_watchdog(void) {
    for (unsigned enough=0;enough<2;++enough) {
        ClosedCarrierFixture f;cc_fixture(&f,0);
        XrCompileResourceLimits caps=pc_caps();XrXirCompileContext c=pc_owner(caps);
        uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f.edge,&e,&terms,&flow);
        CHECK(error_function(&flow,1)==XR_XIR_OK);
        uint64_t used=pc_stats(&c).work,left=enough ? 4u : 3u;
        CHECK(e.outcomes_work.words==1 && e.outcomes_work.atom_count==2 && used<=caps.work-left);
        CHECK(xr_compile_resources_work(c.resources,caps.work-used-left)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=pc_stats(&c).work,held_live=pc_stats(&c).live_bytes;size_t allocations=attempts;
        CHECK(error_instruction_begin(&flow,0)==(enough ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(pc_stats(&c).work-before==(enough ? 4u : 0u) && e.outcomes_work.errors[1]==4 && flow.zero[0]==0);
        CHECK(error_instruction_begin(&flow,flow.function->instruction_count)==XR_XIR_BAD_STRUCTURE);
        CHECK(pc_stats(&c).live_bytes==held_live && attempts==allocations);
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
}
static void cc_cfg_large_faults(void) {
    static XrXirInstruction instructions[1025];size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        EdgeFixture f;ee_fixture(&f,EE_JUMP,false);
        instructions[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        for (uint32_t i=1;i<1024;++i)
            instructions[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={i-1}};
        instructions[1024]=f.init;f.blocks[0]=(XrXirBlock){.count=1025};
        f.functions[1].parameters=NULL;f.functions[1].parameter_count=0;
        f.functions[1].instructions=instructions;f.functions[1].instruction_count=1025;
        f.functions[1].block_count=1;
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);EffectBodyMaterial *material=NULL;
        CHECK(effect_body_material_create(&c,&f.module,&material)==XR_XIR_OK && material);
        flow.body=effect_body_owned_request(&c,material);
        uint64_t prepared=pc_stats(&c).live_bytes;size_t before=attempts;
        injected=false;fail_at=pass ? before+pass-1 : SIZE_MAX;
        XrXirStatus status=error_function(&flow,1);size_t actual=attempts-before;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites==2); }
        else {
            CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
            CHECK(!flow.storage && !e.outcomes_work.errors[1] && pc_stats(&c).live_bytes==prepared);
            CHECK(error_function(&flow,1)==XR_XIR_OK);
        }
        CHECK(!e.outcomes_work.errors[1] && !flow.active_count && !flow.cell_count && flow.zero[0]==0);
        for (uint32_t v=0;v<1024;++v) CHECK(flow.roots[v]==0 && flow.slots[v]==UINT32_MAX);
        effect_body_material_free(material);pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
    printf("owned COPY classification CFG full OOM sites%zu sameowner retry physical0\n",sites);
}
static XrXirStatus cc_whole(const XrXirCompileContext *c,unsigned mode) {
    ClosedCarrierFixture f;cc_fixture(&f,mode<9 ? mode : 8);
    XrXirInstruction actual[4]={{0}},initialize[4]={{0}};
    XrXirBlock actual_block={0},initialize_block={.count=4};
    XrXirSlot slot={0,(XrXirType)CC_ARRAY_CELL,1};uint32_t operand=0;
    if (mode>=9) {
        CHECK(mode==9 || mode==10);
        EdgeFixture *edge=&f.edge;edge->module.function_count=4;
        if (mode==9) {
            actual[0]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)CC_ARRAY};
            actual[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CC_ARRAY_CELL,.args={0}};
            actual[2]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=1};
            actual[3]=(XrXirInstruction){.op=XR_XIR_RETURN};operand=1;actual_block.count=4;
        } else {
            initialize[0]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)CC_ARRAY};
            /* SLOT_INIT publishes the real physical Cell<Array<Error>>;
             * it cannot accept a logical Array payload in this slot. */
            initialize[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CC_ARRAY_CELL,.args={0}};
            initialize[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1}};
            initialize[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
            edge->functions[0].instructions=initialize;edge->functions[0].instruction_count=4;
            edge->functions[0].blocks=&initialize_block;
            edge->declarations.slots=&slot;edge->declarations.slot_count=1;
            actual[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)CC_ARRAY_CELL};
            actual[1]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=1};
            actual[2]=(XrXirInstruction){.op=XR_XIR_RETURN};actual_block.count=3;
        }
        edge->functions[3]=(XrXirFunction){.name="actual",.name_length=6,.result=XR_XIR_UNIT,
            .blocks=&actual_block,.block_count=1,.instructions=actual,.instruction_count=actual_block.count,
            .operands=&operand,.operand_count=1};
    }
    f.edge.module.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;XrXirEffects *e=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(c, &f.edge.module, &checked, &d);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&e);else CHECK(!checked);
    if (status==XR_XIR_OK) {
        uint64_t bits=!mode ? 4u : mode==5 ? 2u : mode==6 || mode==7 ? 1u : 12u;
        CHECK(e && effect_final_outcomes(e)->words==1 && effect_final_outcomes(e)->atom_count==2 && effect_final_outcomes(e)->errors[1]==bits && effect_final_outcomes(e)->errors[2]==8);
        CHECK(effect_final_outcomes(e)->functions[1].throws==((mode==6 || mode==7) ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_MAY));
        if (effect_roots_read(e)->root[1].requires_root || effect_roots_read(e)->root[1].unresolved!=(mode==7 || mode>=8)) {
            const XrXirRootFormula *formula=&effect_roots_read(e)->contracts[1].formula;
            fprintf(stderr,"closed CONDITIONAL_EXPECTATION_MISMATCH mode=%u requires=%u unresolved=%u constant=%u terms=%u cell_role=%u cell_origin=%u root_cause=%u unknown_cause=%u\n",
                mode,(unsigned)effect_roots_read(e)->root[1].requires_root,(unsigned)effect_roots_read(e)->root[1].unresolved,
                formula->constant_mask,formula->term_count,
                (unsigned)xr_xir_cell_provenance_role(e->cells,1,0),
                xr_xir_cell_provenance_origin(e->cells,1,0),
                effect_roots_read(e)->root_witnesses[1].cause,effect_roots_read(e)->unresolved_witnesses[1].cause);
            for (uint32_t t=0;t<formula->term_count;++t)
                fprintf(stderr,"closed term=%u kind=%u index=%u\n",t,formula->terms[t].kind,formula->terms[t].index);
            fflush(stderr);
        }
        CHECK(!effect_roots_read(e)->root[1].requires_root && effect_roots_read(e)->root[1].unresolved==(mode==7 || mode>=8));
        if (mode>=8) {
            /* An open physical Cell parameter remains conditional regardless
             * of whether authentic incoming local/module callers also exist. */
            const XrXirRootFormula *formula=&effect_roots_read(e)->contracts[1].formula;
            CHECK(!formula->constant_mask && formula->term_count==1 &&
                formula->terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER && formula->terms[0].index==0);
            CHECK(xr_xir_cell_provenance_origin(e->cells,1,0)==XR_XIR_CELL_ORIGIN_SCOPED);
        }
        if (mode>=9) {
            /* The complete checker and effect analyzer close only the real
             * CALL edge: owned local is NONE; published module requires ROOT. */
            CHECK(xr_xir_cell_provenance_role(e->cells,1,0)==XR_XIR_CELL_PROOF_SCOPED_REF);
            CHECK(effect_roots_read(e)->root[3].requires_root==(mode==10) && !effect_roots_read(e)->root[3].unresolved);
            CHECK(effect_final_outcomes(e)->errors[3]==12 && effect_final_outcomes(e)->functions[3].throws==XR_XIR_EFFECT_MAY);
            CHECK(xr_xir_cell_provenance_origin(e->cells,3,operand)==
                (mode==10 ? XR_XIR_CELL_ORIGIN_MODULE : XR_XIR_CELL_ORIGIN_OWNED));
        }
        CHECK(effect_roots_read(e)->root[0].requires_root && !effect_roots_read(e)->root[0].unresolved && !effect_final_outcomes(e)->errors[0]);
    } else {
        CHECK(!e);
        if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr,"closed mode%u status%u f%u b%u i%u\n",mode,status,d.function,d.block,d.instruction);
    }
    xr_xir_compile_effects_free(e);xr_xir_compile_artifact_free(checked);return status;
}
static void cc_faults(unsigned mode) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirStatus status=cc_whole(&c,mode);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites>0 && sites<4096); }
        else CHECK(injected && actual>=pass && status==XR_XIR_OUT_OF_MEMORY);
        CHECK(pc_stats(&c).live_bytes==baseline && cc_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    printf("closed mode%u full admission/inference OOM sites%zu sameownerretry physical0\n",mode,sites);
}
static XrCompileResourceStats cc_measured(XrCompileResourceLimits caps,unsigned mode,XrXirStatus expected) {
    XrXirCompileContext c=pc_owner(caps);uint64_t baseline=pc_stats(&c).live_bytes;
    CHECK(cc_whole(&c,mode)==expected);XrCompileResourceStats s=pc_stats(&c);pc_release(&c,baseline);return s;
}
static void cc_limits(unsigned mode) {
    XrCompileResourceStats a=cc_measured(pc_caps(),mode,XR_XIR_OK);CHECK(a.work<UINT64_C(700000));
    XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=cc_measured(exact,mode,XR_XIR_OK);
    CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    CHECK(exact.allocated_bytes>1 && exact.live_bytes>1 && exact.work>1);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)cc_measured(less,mode,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)cc_measured(less,mode,XR_XIR_BUDGET);
    less=exact;--less.work;(void)cc_measured(less,mode,XR_XIR_BUDGET);
}
static void cc_bad_types(void) {
    XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
    for (unsigned mode=0;mode<5;++mode) {
        ClosedCarrierFixture f;cc_fixture(&f,0);f.edge.module.stage=XR_XIR_BUILT;
        if (!mode) f.nodes[2].parameter_span=1;
        else if (mode==1) f.nodes[3].flags=0;
        else if (mode==2) f.nodes[2].flags=1;
        else if (mode==3) f.edge.subject[0].type=XR_XIR_I64;
        else {
            f.edge.subject[0]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
            f.edge.functions[1].instruction_count=1;f.edge.functions[1].block_count=1;
            f.edge.blocks[0]=(XrXirBlock){.count=1};
        }
        XrXirArtifact *checked=NULL;XrXirStatus expected=mode==2 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE;
        CHECK(xir_fixture_check(&c, &f.edge.module, &checked, NULL)==expected && !checked);
        CHECK(pc_stats(&c).live_bytes==baseline);
    }
    pc_release(&c,baseline);
}
static void pc_closed_carriers_controls(void) {
    cc_classification();cc_classification_limits();cc_private();cc_bad_types();
    cc_cfg_continuation();cc_cfg_watchdog();cc_cfg_large_faults();
    for (unsigned mode=0;mode<11;++mode) {
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        CHECK(cc_whole(&c,mode)==XR_XIR_OK);pc_release(&c,baseline);
    }
    cc_faults(0);cc_faults(3);cc_faults(9);cc_faults(10);
    cc_limits(0);cc_limits(3);cc_limits(9);cc_limits(10);
    puts("closed literals: 546 values/active1/4692 bytes, payload12/2, callable1, root isolation, fresh FI/axes");
}
#endif // XIR_ERROR_CLOSED_CARRIERS_CONTROLS_H
