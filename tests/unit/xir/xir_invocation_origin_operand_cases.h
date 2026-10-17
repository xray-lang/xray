/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_origin_operand_cases.h - Current numeric origin operand columns
 *
 * KEY CONCEPT:
 *   Literal alias flags are independent of callable or Program permission.
 *   Changing canonical operands after projection must still be observed.
 */
#ifndef XIR_INVOCATION_ORIGIN_OPERAND_CASES_H
#define XIR_INVOCATION_ORIGIN_OPERAND_CASES_H

static void invocation_origin_operand_literals(void) {
    static const XrXirOp operations[]={XR_XIR_COPY,XR_XIR_SCALAR_COPY,XR_XIR_OWNED_RETAIN,
        XR_XIR_LOCAL_NEW,XR_XIR_SCALAR_LOCAL_NEW,XR_XIR_OWNED_LOCAL_NEW,
        XR_XIR_LOCAL_READ,XR_XIR_SCALAR_LOCAL_READ,XR_XIR_OWNED_LOCAL_READ,
        XR_XIR_LOCAL_WRITE,XR_XIR_SCALAR_LOCAL_WRITE,XR_XIR_OWNED_LOCAL_WRITE,XR_XIR_PHI};
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    for(size_t n=0;n<sizeof(operations)/sizeof(*operations);++n) {
        bool write=n>=9 && n<12,phi=operations[n]==XR_XIR_PHI;
        XrXirType parameters[6]={XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64};
        uint32_t operands[6]={0,0,0,1,0,2};XrXirInstruction instructions[3];
        EffectPhysicalRecord records[3];
        for(uint32_t i=0;i<3;++i) {
            instructions[i]=(XrXirInstruction){.op=operations[n],.type=write?XR_XIR_UNIT:XR_XIR_I64,
                .args={phi?2*i:i,phi?2:write?i+3:0},.targets={11,17},.immediate=INT64_C(0x100000002),
                .type_arguments={23,29}};
            records[i]=(EffectPhysicalRecord){6+i,EFFECT_PHYSICAL_TRANSFER};
        }
        XrXirFunction source={.instructions=instructions,.instruction_count=3};
        EffectBodyFunction body={.parameters=parameters,.parameter_count=6,.instruction_count=3,
            .operands=operands,.operand_count=6};
        CHECK(effect_body_records_copy(&context,&source,&body)==XR_XIR_OK && body.run_count==1);
        EffectPhysicalFunction physical={.parameters=6,.instructions=3,.values=9,.records=records,.count=3,.capacity=3};
        EffectPhysicalDomain domain={.resources=context.resources,.functions=&physical,.count=1};
        EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&body,.physical=&domain};
        material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
        EffectInvocationNode node={0};EffectInvocationWorkspace owner={.work=&context,
            .final={.material=&material,.physical=&domain,.nodes=&node,.count=1}};
        EffectBodyFunctionHead head={.parameters=parameters,.parameter_count=6,.instruction_count=3,
            .operands=operands,.operand_count=6};
        EffectInvocationFlow flow={.owner=&owner,.function=&head,.body=effect_body_owned_request(&context,&material)};
        CHECK(effect_invocation_flow_storage(&flow)==XR_XIR_OK && effect_invocation_flow_types(&flow)==XR_XIR_OK);
        uint64_t seed=write?UINT64_C(8):UINT64_C(1),expected=write?UINT64_C(9):UINT64_C(65);
        flow.fixed[0]=seed;flow.changed=false;
        CHECK(effect_invocation_origins_pass(&flow)==XR_XIR_OK && flow.changed && flow.fixed[0]==expected && !flow.certified[0]);
        EffectBodyRun *run=(EffectBodyRun *)body.runs;XrXirInstruction *base=(XrXirInstruction *)&run->base;
        uint32_t first=base->args[0];base->args[0]=9;flow.fixed[0]=seed;flow.changed=false;
        CHECK(flow.types_ready && flow.instructions[0].args[0]==first);
        CHECK(effect_invocation_origins_pass(&flow)==(phi?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_VALUE) &&
            !flow.changed && flow.fixed[0]==seed);
        base->args[0]=first;
        uint32_t mask=run->mask,delta=run->delta[4];run->mask|=UINT32_C(16);run->delta[4]=0;
        CHECK(effect_invocation_origins_pass(&flow)==XR_XIR_BAD_STRUCTURE && !flow.changed && flow.fixed[0]==seed);
        run->mask=mask;run->delta[4]=delta;
        conditional_generation_instruction_reads=0;conditional_generation_instruction_refused=false;
        flow.body.instruction=conditional_generation_counted_instruction;
        CHECK(effect_invocation_origins_pass(&flow)==XR_XIR_OK && flow.changed && flow.fixed[0]==expected &&
            conditional_generation_instruction_reads==3 && !flow.certified[0]);
        xr_compile_resources_free(flow.memory);xr_compile_resources_free(body.runs);
        CHECK(rp_stats(&context).live_bytes==baseline);
    }
    rp_owner_free(&context,baseline);rp_balanced(initial);
    puts("Origin operands: all13 transfer opcodes, three varying rows, current-source rejection and delegated reads PASS");
}
#endif // XIR_INVOCATION_ORIGIN_OPERAND_CASES_H
