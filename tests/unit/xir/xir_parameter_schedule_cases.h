/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_parameter_schedule_cases.h - Independent parameter-origin closure oracle
 */
#ifndef XIR_PARAMETER_SCHEDULE_CASES_H
#define XIR_PARAMETER_SCHEDULE_CASES_H

/* Arbitrary finite row graphs test transfer closure, never IR stage authority.
 * Checked/inferred literals separately cover full Final publication. */
static void parameter_schedule_rows(void) {
    const uint32_t sizes[5]={4,16,64,2,32};
    for (uint32_t order=0;order<3;++order) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;EffectGraph graph={0};
        for (uint32_t scale=0;scale<5;++scale) {
            uint32_t count=sizes[scale],map[64],inputs[64][2],operands[256];
            uint64_t expected[66]={1,2};XrXirInstruction ops[64]={0};
            for (uint32_t i=0;i<count;++i) map[i]=order==0?i:order==1?count-1-i:(i*5+1)%count;
            for (uint32_t i=0;i<count;++i) {
                uint32_t at=map[i];inputs[at][0]=i?2+map[i-1]:0;
                inputs[at][1]=i+1<count?2+map[i+1]:1;
                operands[4*at]=0;operands[4*at+1]=inputs[at][0];
                operands[4*at+2]=1;operands[4*at+3]=inputs[at][1];
                ops[at]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)256,.args={4*at,4}};
            }
            /* Descending plain closure shares no production scheduling state. */
            bool changed=true;
            while (changed) {
                changed=false;
                for (uint32_t j=count;j;--j) {
                    uint32_t i=j-1;uint64_t joined=expected[2+i]|
                        expected[inputs[i][0]]|expected[inputs[i][1]];
                    if (joined!=expected[2+i]) changed=true;
                    expected[2+i]=joined;
                }
            }
            XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
                .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
            XrXirTypes types={&callable,1,NULL,NULL};XrXirType parameters[2]={256,256};
            XrXirFunction function={.parameters=parameters,.parameter_count=2,.instructions=ops,
                .instruction_count=count,.operands=operands,.operand_count=4*count};
            XrXirModule module={.types=&types,.functions=&function,.function_count=1};
            XrXirEffectParameter uses[2]={0};EffectParameterFlow flow={.work=&context,.parameters=uses};
            flow.body=effect_body_raw_request(&context,&module);
            CHECK(effect_body_request_head(&flow.body,0,&flow.head)==XR_XIR_OK);flow.function=&flow.head;
            CHECK(effect_parameter_rows(&flow,&graph)==XR_XIR_OK);
            CHECK(effect_parameter_solve(&flow)==XR_XIR_OK);
            for (uint32_t v=0;v<count+2;++v) CHECK(flow.rows[v]==expected[v]);
            for (uint32_t p=0;p<2;++p) CHECK(uses[p].uses==XR_XIR_EFFECT_USE_COPY);
            CHECK(effect_parameter_solve(&flow)==XR_XIR_OK);
            /* Poison retained capacity before the next grow/shrink. */
            memset(graph.flow_rows,0xa5,graph.flow_capacity);
        }
        effect_graph_free(&graph);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

/* Owned parameter equations include recursive, earlier and later consumers.
 * Independent reachability to an escaping payload predicts each final role. */
static void parameter_schedule_functions(void) {
    const uint32_t sizes[4]={8,2,16,4};
    for (uint32_t order=0;order<3;++order) for (uint32_t split=0;split<2;++split) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        EffectGraph graph={0};XrXirEffects effects={.resources=context.resources};
        for (uint32_t scale=0;scale<4;++scale) {
            uint32_t count=sizes[scale],map[16],targets[16],operand=0;bool fixed[16]={0};
            XrXirFunction functions[16]={0};XrXirInstruction ops[16][3]={0};
            XrXirBlock blocks[16]={0};XrXirFunctionEffectContract contracts[16]={0};
            XrXirEffectParameter declared[16]={0};
            XrXirType parameter=(XrXirType)256;
            XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
                .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},{.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256}};
            XrXirTypes types={nodes,2,NULL,NULL};
            for (uint32_t i=0;i<count;++i) map[i]=order==0?i:order==1?count-1-i:(i*5+1)%count;
            uint32_t seed=map[count/2];fixed[seed]=true;
            for (uint32_t i=0;i<count;++i) {
                uint32_t at=map[i],next=(i+1)%count;
                if (split && i<count/2) next=(i+1)%(count/2);
                targets[at]=map[next];
                ops[at][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_UNIT,
                    .args={0,1},.immediate=targets[at]};
                ops[at][1]=at==seed?(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)257}:
                    (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
                ops[at][2]=(XrXirInstruction){.op=XR_XIR_RETURN};blocks[at].count=3;
                functions[at]=(XrXirFunction){.name="flow",.name_length=4,.parameters=&parameter,
                    .parameter_count=1,.result=XR_XIR_UNIT,.instructions=ops[at],.instruction_count=3,
                    .blocks=&blocks[at],.block_count=1,.operands=&operand,.operand_count=1};
                declared[at].kind=XR_XIR_EFFECT_PARAMETER_VARIABLE;
                contracts[at]=(XrXirFunctionEffectContract){.parameters=&declared[at],.parameter_count=1};
            }
            bool changed=true;
            while (changed) {
                changed=false;
                for (uint32_t i=count;i;--i) if (fixed[targets[i-1]] && !fixed[i-1]) {
                    fixed[i-1]=true;changed=true;
                }
            }
            XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=count};
            XrXirModule module={.stage=XR_XIR_CHECKED,.types=&types,.functions=functions,
                .function_count=count,.provenance=&evidence,
                .linkage_kind=XR_XIR_PROGRAM};
            effects.count=count;
            XrXirStatus status=effect_parameters_derive(&module,&effects,&graph,&context);
            if (status!=XR_XIR_OK) fprintf(stderr,"parameter schedule order%u split%u count%u status%u work%llu\n",
                order,split,count,(uint32_t)status,(unsigned long long)rp_stats(&context).work);
            CHECK(status==XR_XIR_OK);
            for (uint32_t f=0;f<count;++f) {
                XrXirEffectParameter actual=effects.contracts[f].parameters[0];
                uint32_t kind=fixed[f]?XR_XIR_EFFECT_PARAMETER_FIXED:XR_XIR_EFFECT_PARAMETER_VARIABLE;
                CHECK(actual.kind==kind);
                uint32_t expected=fixed[targets[f]]?XR_XIR_EFFECT_USE_FIXED:XR_XIR_EFFECT_USE_FORWARD;
                if (f==seed) expected|=XR_XIR_EFFECT_USE_STORE;
                CHECK(actual.uses==expected);
            }
            CHECK(graph.parameter_capacity>=count && graph.parameter_first_callers);
            memset(graph.parameter_first_callers,0xa5,(size_t)graph.parameter_capacity*sizeof(uint32_t));
            /* Retire the current contracts before changing their count owner. */
            effect_parameters_free(&effects);graph.material=NULL;graph.physical=NULL;
        }
        effect_graph_free(&graph);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}
static void parameter_schedule_local_write(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;EffectGraph graph={0};
    XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types={&callable,1,NULL,NULL};XrXirType parameters[2]={256,256};
    XrXirInstruction ops[3]={{.op=XR_XIR_COPY,.type=(XrXirType)256},
        {.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={2,0}},
        {.op=XR_XIR_LOCAL_WRITE,.type=XR_XIR_UNIT,.args={0,1}}};
    XrXirFunction function={.parameters=parameters,.parameter_count=2,
        .instructions=ops,.instruction_count=3};
    XrXirModule module={.types=&types,.functions=&function,.function_count=1};
    XrXirEffectParameter uses[2]={0};EffectParameterFlow flow={.work=&context,.parameters=uses};
    flow.body=effect_body_raw_request(&context,&module);
    CHECK(effect_body_request_head(&flow.body,0,&flow.head)==XR_XIR_OK);flow.function=&flow.head;
    CHECK(effect_parameter_rows(&flow,&graph)==XR_XIR_OK && effect_parameter_solve(&flow)==XR_XIR_OK);
    CHECK(flow.rows[0]==3 && flow.rows[1]==2 && flow.rows[2]==3);
    CHECK(uses[0].uses==(XR_XIR_EFFECT_USE_COPY|XR_XIR_EFFECT_USE_FIXED) &&
        uses[1].uses==(XR_XIR_EFFECT_USE_COPY|XR_XIR_EFFECT_USE_FIXED));
    effect_graph_free(&graph);rp_owner_free(&context,baseline);rp_balanced(physical);
}
static void parameter_schedule_controls(void) {
    parameter_schedule_rows();parameter_schedule_functions();parameter_schedule_local_write();
    CHECK(!rp_live && !rp_live_bytes);
    puts("PARAMETER_SCHEDULE_ORACLE rows=15 owned_graphs=24 orders=3 grow_shrink=PASS late_local_write=PASS");
}
#endif
