/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_single_run_reader_cases.h - Current run values and complete Class membership
 */
#ifndef XIR_SINGLE_RUN_READER_CASES_H
#define XIR_SINGLE_RUN_READER_CASES_H

static bool single_run_complete_same(const XrXirInstruction *a,const XrXirInstruction *b) {
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] && a->args[1]==b->args[1] &&
        a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] && a->immediate==b->immediate &&
        a->type_arguments[0]==b->type_arguments[0] && a->type_arguments[1]==b->type_arguments[1];
}
static void single_run_base_copy_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    EffectBodyRun run={.first=7,.count=4,
        .base={.op=XR_XIR_COPY,.type=0x76543210u,.args={0x11223344u,0x89abcdefu},
            .targets={0xfedcba98u,0x55667788u},.immediate=INT64_MIN+123,
            .type_arguments={0x80000001u,0x7fffffffu}}};
    for (uint32_t w=0;w<10;++w) run.delta[w]=w+1;
    for (uint32_t mask=0;mask<1024;++mask) {
        run.mask=mask;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output={0};
        CHECK(effect_body_instruction_ref_value(&read,&run,7,&output)==XR_XIR_OK &&
            single_run_complete_same(&output,&run.base));

    }
    /* Selected delta validation uses the same actual sparse mask walk for
     * rows and Cell spans, without a compiler accounting callback. */
    const uint32_t masks[3]={0,1,1023};
    for (uint32_t i=0;i<3;++i) {
        run.mask=masks[i];EffectBodyRead read={.owner=&context,.execution=NULL};
        CHECK(effect_body_instruction_ref_prepare(&read,&run,7,false)==XR_XIR_OK);
    }
    run.mask=1023;
    for (uint32_t mode=0;mode<12;++mode) {
        EffectBodyRun bad=run;
        if (mode<10) bad.delta[mode]=0;
        else bad.base.op=mode==10?XR_XIR_INVALID:XR_XIR_OP_COUNT;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        CHECK(effect_body_instruction_ref_value(&read,&bad,7,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    EffectBodyRead read={.owner=&context,.execution=NULL};
    XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
    CHECK(effect_body_instruction_ref_prepare(&read,&run,8,true)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_body_instruction_ref_value(&read,NULL,7,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    CHECK(effect_body_instruction_ref_value(NULL,&run,7,&output)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&output,&sentinel,sizeof(output)));
    CHECK(effect_body_instruction_ref_value(&read,&run,7,NULL)==XR_XIR_BAD_STRUCTURE);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    puts("base-row copy: all1024 masks, complete ten fields,12 malformed, current owner and sparse spans passed");
}
/* Three-run controls include both unselected physical neighbors in every read. */
static void three_run_reader_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    EffectBodyRun runs[3]={0};
    for (uint32_t r=0;r<3;++r) {
        runs[r]=(EffectBodyRun){.first=2*r,.count=r==2?4u:2u,.mask=(1u<<6)|(1u<<7)|(1u<<8),
            .base={.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={17+12*r,23+r},
                .targets={31+r,47+r},.immediate=-11,.type_arguments={0x7ffffff0u+r,0x80000001u}}};
        runs[r].delta[6]=17;runs[r].delta[7]=0xfffffffcu;runs[r].delta[8]=9;
    }
    EffectBodyFunction function={.instruction_count=8,.runs=runs,.run_count=3,.run_capacity=3};
    unsigned malformed=0;EffectBodyRun sentinel_ref={0};
    for (uint32_t i=0;i<8;++i) {
        uint32_t selected=i<2?0u:i<4?1u:2u,offset=i-2*selected;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        const EffectBodyRun *ref=NULL;
        CHECK(effect_body_record_ref(&read,&function,i,&ref)==XR_XIR_OK && ref==&runs[selected]);

        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        XrXirInstruction expected={.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={17+12*selected,23+selected},
            .targets={31+selected,47+selected},.immediate=0,
            .type_arguments={0x7ffffff0u+selected+9*offset,0x80000001u}};
        uint64_t immediate=(uint64_t)high<<32|low;
        memcpy(&expected.immediate,&immediate,sizeof(immediate));
        CHECK(single_run_complete_same(&output,&expected));

    }
    for (uint32_t r=0;r<3;++r) for (uint32_t mode=0;mode<6;++mode) for (uint32_t i=0;i<8;++i) {
        EffectBodyRun bad[3]={runs[0],runs[1],runs[2]};function.runs=bad;
        if (mode==0) ++bad[r].first;
        else if (mode==1) bad[r].count=0;
        else if (mode==2) bad[r].mask|=1u<<10;
        else if (mode==3) bad[r].delta[7]=0;
        else if (mode==4) bad[r].count=1;
        else ++bad[r].count;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        const EffectBodyRun *ref=&sentinel_ref;
        CHECK(effect_body_record_ref(&read,&function,i,&ref)==XR_XIR_BAD_STRUCTURE && ref==&sentinel_ref);
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));++malformed;
    }
    rp_owner_free(&context,baseline);rp_balanced(physical);
    printf("three-run all8 rows/ten fields, %u malformed neighbor reads, owned output invariants PASS\n",malformed);
}

/* These literal storage subjects admit no Final or effect permission. The
 * expected membership comes from independent raw rows and literal outcomes. */
static XrXirStatus class_scan_membership_case(const XrXirCompileContext *context,
    uint32_t count,uint32_t mode,bool raw) {
    CHECK(count && count<=17 && mode<9);
    InvocationClassRowsFixture domain;invocation_class_rows_fixture(&domain,0);
    EffectBodyRun runs[17]={0};XrXirInstruction instructions[68]={0};
    for (uint32_t r=0;r<count;++r) {
        uint32_t mask=mode<4?mode:0;
        runs[r]=(EffectBodyRun){.first=4*r,.count=4,.mask=mask,
            .base={.op=XR_XIR_COPY,.type=XR_XIR_I64}};
        runs[r].delta[0]=runs[r].delta[1]=1;
        if (r+1==count && mode==5) runs[r].base.type=(XrXirType)256;
        if (r+1==count && mode==6) {runs[r].base.type=(XrXirType)255;runs[r].mask=2;}
        for (uint32_t i=0;i<4;++i) {
            instructions[4*r+i]=runs[r].base;
            if (runs[r].mask&1u) instructions[4*r+i].op=(XrXirOp)((uint32_t)XR_XIR_COPY+i);
            if (runs[r].mask&2u) instructions[4*r+i].type=(XrXirType)((uint32_t)runs[r].base.type+i);
        }
    }
    if (mode==7) --runs[count-1].count;
    if (mode==8) ++runs[count-1].first;
    XrXirType parameter=XR_XIR_I64;
    EffectBodyFunction body={.parameters=&parameter,.parameter_count=1,.instruction_count=4*count,
        .runs=runs,.run_count=count,.run_capacity=count};
    EffectBodyMaterial material={.resources=context->resources,.references=1,.functions=&body};
    material.terms.types=domain.types;
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectInvocationClassFact fact={.value=1+2*count,.mask=EFFECT_CLASS_KNOWN};
    EffectInvocationNode node={.body=0};
    EffectInvocationState state={.classes={.count=mode==4?1u:0u,.facts=mode==4?&fact:NULL}};
    XrXirEffects construction={0};
    EffectInvocationWorkspace workspace={
        .final={.material=&material,.count=1,.capacity=1,.nodes=&node},.states=&state};
    workspace.read=(EffectInvocationRead){.work=context,.final=&workspace.final,
        .roles={.construction=&construction},.discovery=&workspace};
    EffectBodyFunctionHead head={.parameters=&parameter,.parameter_count=1,.instruction_count=4*count};
    XrXirFunction function={.parameters=&parameter,.parameter_count=1,
        .instructions=instructions,.instruction_count=4*count};
    XrXirModule module={.functions=&function,.function_count=1,.types=&material.terms.types};
    EffectInvocationFlow flow={.read=&workspace.read,.function=&head,.values=1+4*count};
    flow.body=raw?effect_body_raw_request(context,&module):effect_body_owned_request(context,&material);
    EffectInvocationNode saved=node;EffectInvocationClassFact saved_fact=fact;
    EffectInvocationState saved_state=state;
    XrXirStatus status=effect_invocation_class_values_match(&flow,100);
    CHECK(!memcmp(&node,&saved,sizeof(node)) && !memcmp(&fact,&saved_fact,sizeof(fact)) &&
        !memcmp(&state,&saved_state,sizeof(state)));
    return status;
}

static void class_scan_membership_controls(void) {
    const uint32_t counts[7]={1,2,3,4,5,9,17};unsigned cases=0;
    for (uint32_t c=0;c<7;++c) for (uint32_t mode=0;mode<9;++mode)
        for (uint32_t raw=0;raw<2;++raw) {
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
            XrCompileResourceStats before=rp_stats(&context);
            XrXirStatus expected=mode<4 || (raw && mode>=7)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            CHECK(class_scan_membership_case(&context,counts[c],mode,raw!=0)==expected);
            CHECK(conditional_memory_unchanged(&context,before));
            rp_owner_free(&context,before.held_bytes);rp_balanced(physical);++cases;
        }
    printf("Class membership: %u literal raw/owned cases, allocation-free PASS\n",cases);
}

static void single_run_reader_controls(void) {
    single_run_base_copy_controls();
    three_run_reader_controls();
    class_scan_membership_controls();
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).held_bytes;
    EffectBodyRun run={.first=0,.count=8,.mask=(1u<<6)|(1u<<7)|(1u<<8),
        .base={.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={17,23},.targets={31,47},
            .immediate=-11,.type_arguments={0x7ffffff0u,0x80000001u}}};
    run.delta[6]=17;run.delta[7]=0xfffffffcu;run.delta[8]=9;
    EffectBodyFunction function={.instruction_count=8,.runs=&run,.run_count=1,.run_capacity=1};
    for (uint32_t constant=0;constant<2;++constant) for (uint32_t i=0;i<8;++i) {
        run.mask=constant?0:(1u<<6)|(1u<<7)|(1u<<8);
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output={0};
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t offset=constant?0:i;
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.op==XR_XIR_COPY && output.type==XR_XIR_I64 && output.args[0]==17 && output.args[1]==23 &&
            output.targets[0]==31 && output.targets[1]==47 &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low) &&
            output.type_arguments[0]==0x7ffffff0u+9*offset && output.type_arguments[1]==0x80000001u);

    }
    run.mask=(1u<<6)|(1u<<7)|(1u<<8);
    for (uint32_t mode=0;mode<10;++mode) {
        EffectBodyRun bad=run;EffectBodyFunction changed=function;changed.runs=&bad;
        if (mode==0) bad.first=1;
        else if (mode==1) bad.count=0;
        else if (mode==2) bad.count=7;
        else if (mode==3) bad.count=9;
        else if (mode==4) bad.mask|=1u<<10;
        else if (mode==5) bad.delta[7]=0;
        else if (mode==6) changed.run_capacity=0;
        else if (mode==7) bad.base.op=XR_XIR_INVALID;
        else if (mode==8) changed.instruction_count=7;
        else { changed.instruction_count=bad.count=1; }
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        CHECK(effect_body_instruction_get(&read,&changed,0,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    EffectBodyRun pair[2]={run,run};pair[0].count=4;pair[1].first=4;pair[1].count=4;
    pair[1].base.args[0]=29;function.runs=pair;function.run_count=function.run_capacity=2;
    const uint32_t selected[4]={0,3,4,7};
    for (uint32_t trial=0;trial<4;++trial) {
        uint32_t i=selected[trial],offset=i%4;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output;
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.args[0]==(i<4?17u:29u) &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low));

    }
    for (uint32_t mode=0;mode<6;++mode) {
        EffectBodyRun bad[2]={pair[0],pair[1]};EffectBodyFunction changed=function;changed.runs=bad;
        if (mode==0) bad[0].count=3;
        else if (mode==1) {bad[1].first=3;bad[1].count=5;}
        else if (mode==2) bad[1].count=3;
        else if (mode==3) bad[1].delta[6]=0;
        else if (mode==4) bad[1].base.op=XR_XIR_INVALID;
        else bad[0].mask|=1u<<10;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        CHECK(effect_body_instruction_get(&read,&changed,7,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    EffectBodyRun triple[3]={pair[0],pair[1],pair[1]};
    triple[0].count=2;triple[1].first=2;triple[1].count=2;triple[2].first=4;
    triple[2].base.args[0]=53;function.runs=triple;function.run_count=function.run_capacity=3;
    const uint32_t coordinates[6]={0,1,2,3,4,7};
    for (uint32_t trial=0;trial<6;++trial) {
        uint32_t i=coordinates[trial],offset=i<4?i%2:i-4;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output;
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.args[0]==(i<2?17u:i<4?29u:53u) &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low));

    }
    for (uint32_t mode=0;mode<6;++mode) {
        EffectBodyRun bad[3]={triple[0],triple[1],triple[2]};EffectBodyFunction changed=function;changed.runs=bad;
        if (mode==0) bad[0].count=3;
        else if (mode==1) bad[1].first=3;
        else if (mode==2) bad[1].count=3;
        else if (mode==3) bad[1].delta[7]=0;
        else if (mode==4) bad[2].count=3;
        else bad[1].mask|=1u<<10;
        EffectBodyRead read={.owner=&context,.execution=NULL};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        /* The selected middle row touches both deliberately malformed joins.
         * A bounded getter is not the separate complete owner admission. */
        CHECK(effect_body_instruction_get(&read,&changed,2,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    rp_owner_free(&context,baseline);rp_balanced(physical);
    puts("SINGLE_RUN_READER full_field_cases=16 boundary_cases=10 malformed=22 owned_output=unchanged physical=0/0");
}
#endif
