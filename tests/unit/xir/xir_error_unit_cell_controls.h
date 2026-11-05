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
    f->module.stage=XR_XIR_BUILT;
}
static void pc_unit_cell_controls(void) {
    for(unsigned mode=0;mode<4;++mode) {
        EdgeFixture f;pc_unit_cell_fixture(&f,mode);
        XrXirCompileContext c=pc_owner(pc_caps());uint64_t baseline=pc_stats(&c).live_bytes;
        f.functions[0].error_type=XR_XIR_NEVER;f.functions[1].error_type=XR_XIR_NEVER;
        f.functions[2].error_type=(XrXirType)EE_ENUM;
        XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&c,&f.module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);pc_release(&c,baseline);
    }
    puts("Unit Cell and copied enum aliases retain complete common admission");
}
#endif
