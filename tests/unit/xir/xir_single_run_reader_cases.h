/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_single_run_reader_cases.h - Complete small-run values and atomic refusals
 */
#ifndef XIR_SINGLE_RUN_READER_CASES_H
#define XIR_SINGLE_RUN_READER_CASES_H

typedef struct SingleRunReadProbe { uint64_t work;size_t calls,refuse; } SingleRunReadProbe;
static bool single_run_consume(const void *owner,uint64_t units) {
    SingleRunReadProbe *probe=(SingleRunReadProbe *)owner;
    if (probe->calls++==probe->refuse) return false;
    CHECK(units<=UINT64_MAX-probe->work);probe->work+=units;return true;
}
static void single_run_reader_controls(void) {
    EffectBodyRun run={.first=0,.count=8,.mask=(1u<<6)|(1u<<7)|(1u<<8),
        .base={.op=XR_XIR_COPY,.type=XR_XIR_I64,.args={17,23},.targets={31,47},
            .immediate=-11,.type_arguments={0x7ffffff0u,0x80000001u}}};
    run.delta[6]=17;run.delta[7]=0xfffffffcu;run.delta[8]=9;
    EffectBodyFunction function={.instruction_count=8,.runs=&run,.run_count=1,.run_capacity=1};
    uint64_t total=0;size_t refusal_sites=0;
    for (uint32_t constant=0;constant<2;++constant) for (uint32_t i=0;i<8;++i) {
        run.mask=constant?0:(1u<<6)|(1u<<7)|(1u<<8);
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
        XrXirInstruction output={0};
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t offset=constant?0:i;
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.op==XR_XIR_COPY && output.type==XR_XIR_I64 && output.args[0]==17 && output.args[1]==23 &&
            output.targets[0]==31 && output.targets[1]==47 &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low) &&
            output.type_arguments[0]==0x7ffffff0u+9*offset && output.type_arguments[1]==0x80000001u);
        CHECK(probe.work && probe.calls);total+=probe.work;size_t sites=probe.calls;
        for (size_t fail=0;fail<sites;++fail) {
            XrXirInstruction sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
            probe=(SingleRunReadProbe){.refuse=fail};
            CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_BUDGET &&
                probe.calls==fail+1 && !memcmp(&output,&sentinel,sizeof(output)));
            ++refusal_sites;
        }
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
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        CHECK(effect_body_instruction_get(&read,&changed,0,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    EffectBodyRun pair[2]={run,run};pair[0].count=4;pair[1].first=4;pair[1].count=4;
    pair[1].base.args[0]=29;function.runs=pair;function.run_count=function.run_capacity=2;
    const uint32_t selected[4]={0,3,4,7};
    for (uint32_t trial=0;trial<4;++trial) {
        uint32_t i=selected[trial],offset=i%4;
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
        XrXirInstruction output;
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.args[0]==(i<4?17u:29u) &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low));
        size_t sites=probe.calls;total+=probe.work;
        for (size_t fail=0;fail<sites;++fail) {
            XrXirInstruction sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
            probe=(SingleRunReadProbe){.refuse=fail};
            CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_BUDGET &&
                probe.calls==fail+1 && !memcmp(&output,&sentinel,sizeof(output)));
            ++refusal_sites;
        }
    }
    for (uint32_t mode=0;mode<6;++mode) {
        EffectBodyRun bad[2]={pair[0],pair[1]};EffectBodyFunction changed=function;changed.runs=bad;
        if (mode==0) bad[0].count=3;
        else if (mode==1) {bad[1].first=3;bad[1].count=5;}
        else if (mode==2) bad[1].count=3;
        else if (mode==3) bad[1].delta[6]=0;
        else if (mode==4) bad[1].base.op=XR_XIR_INVALID;
        else bad[0].mask|=1u<<10;
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
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
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
        XrXirInstruction output;
        CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_OK);
        uint32_t low=0xfffffff5u+17*offset,high=0xffffffffu+0xfffffffcu*offset;
        CHECK(output.args[0]==(i<2?17u:i<4?29u:53u) &&
            (uint64_t)output.immediate==((uint64_t)high<<32|low));
        size_t sites=probe.calls;total+=probe.work;
        for (size_t fail=0;fail<sites;++fail) {
            XrXirInstruction sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
            probe=(SingleRunReadProbe){.refuse=fail};
            CHECK(effect_body_instruction_get(&read,&function,i,&output)==XR_XIR_BUDGET &&
                probe.calls==fail+1 && !memcmp(&output,&sentinel,sizeof(output)));
            ++refusal_sites;
        }
    }
    for (uint32_t mode=0;mode<6;++mode) {
        EffectBodyRun bad[3]={triple[0],triple[1],triple[2]};EffectBodyFunction changed=function;changed.runs=bad;
        if (mode==0) bad[0].count=3;
        else if (mode==1) bad[1].first=3;
        else if (mode==2) bad[1].count=3;
        else if (mode==3) bad[1].delta[7]=0;
        else if (mode==4) bad[2].count=3;
        else bad[1].mask|=1u<<10;
        SingleRunReadProbe probe={.refuse=SIZE_MAX};EffectBodyRead read={&probe,single_run_consume};
        XrXirInstruction output,sentinel;memset(&sentinel,0x5a,sizeof(sentinel));output=sentinel;
        /* The selected middle row touches both deliberately malformed joins.
         * A bounded getter is not the separate complete owner admission. */
        CHECK(effect_body_instruction_get(&read,&changed,2,&output)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&output,&sentinel,sizeof(output)));
    }
    printf("SINGLE_RUN_READER full_field_cases=16 boundary_cases=10 malformed=22 refused_callbacks=%zu actual_work=%llu\n",
        refusal_sites,(unsigned long long)total);
}
#endif
