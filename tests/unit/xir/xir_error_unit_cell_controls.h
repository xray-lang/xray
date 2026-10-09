/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Unit Cell has no payload operand. Unused index0 must never import the
 * independent Error parameter's facts into owned cells or copied aliases.
 */
#ifndef XIR_ERROR_UNIT_CELL_CONTROLS_H
#define XIR_ERROR_UNIT_CELL_CONTROLS_H
static void pc_unit_cell_fixture(EdgeFixture *f,unsigned mode) {
    ee_fixture(f,EE_JUMP,false);
    bool unit=mode<2,write=(mode&1u)!=0;
    f->nodes[1].element=unit ? XR_XIR_UNIT : (XrXirType)EE_ENUM;
    f->parameters[0]=XR_XIR_ERROR;
    f->parameters[1]=write ? (XrXirType)EE_CELL : (XrXirType)EE_ENUM;
    f->functions[1].parameters=f->parameters;f->functions[1].parameter_count=2;
    f->functions[1].block_count=1;
    if (!write) {
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)EE_CELL,
            .args={unit ? 0u : 1u}};
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_CELL,.args={2}};
        f->subject[2]=f->init;f->functions[1].instruction_count=3;
    } else if (unit) {
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_CELL,.args={1}};
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={2,0}};
        f->subject[2]=f->init;f->functions[1].instruction_count=3;
    } else {
        f->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
        f->subject[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_CELL,.args={1}};
        f->subject[2]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={3,2}};
        f->subject[3]=f->init;f->functions[1].instruction_count=4;
    }
    f->blocks[0]=(XrXirBlock){.count=f->functions[1].instruction_count};
}
static void pc_unit_cell_controls(void) {
    for(unsigned mode=0;mode<4;++mode) {
        EdgeFixture f;pc_unit_cell_fixture(&f,mode);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        XrXirEffects e={0};EffectTerms terms={0};ErrorFlow flow={0};flow.remaining=&c;
        pc_private_begin(&c,&f,&e,&terms,&flow);
        CHECK(error_function(&flow,1)==XR_XIR_OK);
        /* Parameter0 deliberately carries Error's unidentified bit. */
        CHECK(error_read(&flow,flow.work,0)[0]==UINT64_C(2));
        uint32_t first=mode==0 || mode==2 ? 2u : 1u;
        uint32_t alias=mode==3 ? 3u : mode==1 ? 2u : 3u;
        uint64_t expected=mode<2 ? 0u : mode==2 ? UINT64_C(12) : UINT64_C(4);
        CHECK(flow.cell_count==2 && flow.slots[first]!=UINT32_MAX && flow.slots[alias]!=UINT32_MAX);
        CHECK(error_read(&flow,flow.work,first)[0]==expected);
        CHECK(error_read(&flow,flow.work,alias)[0]==expected);
        CHECK(flow.roots[alias]==flow.roots[first] && !e.errors[1]);
        CHECK(flow.zero[0]==0 && flow.zero!=flow.work && flow.zero!=flow.snapshot);
        pc_private_free(&flow,&terms,&e);pc_release(&c,baseline);
    }
    puts("Unit Cell NEW/WRITE ignore absent payload; Error index0 and nonUnit enum aliases preserved");
}
#endif
