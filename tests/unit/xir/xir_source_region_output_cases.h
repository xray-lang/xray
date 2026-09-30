/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_region_output_cases.h - One owner for sealed function array views
 *
 * KEY CONCEPT:
 *   The arena charges actual storage; temporary maps refund on every exit.
 */
static void source_region_output_cases(void) {
    size_t bytes=0,offset=99;
    CHECK(source_region_span(&bytes,0,sizeof(XrXirInstruction),_Alignof(XrXirInstruction),&offset));
    CHECK(!bytes && !offset);
    CHECK(source_region_span(&bytes,1,sizeof(XrXirInstruction),_Alignof(XrXirInstruction),&offset));
    CHECK(offset%_Alignof(XrXirInstruction)==0);
    size_t block_offset=0;
    CHECK(source_region_span(&bytes,1,sizeof(XrXirBlock),_Alignof(XrXirBlock),&block_offset));
    CHECK(block_offset%_Alignof(XrXirBlock)==0);
    size_t output_bytes=bytes;
    bytes=SIZE_MAX;
    CHECK(!source_region_span(&bytes,1,1,2,&offset) && bytes==SIZE_MAX);
    bytes=SIZE_MAX-1;
    CHECK(!source_region_span(&bytes,1,4,1,&offset) && bytes==SIZE_MAX-1);
    bytes=0;
    CHECK(!source_region_span(&bytes,2,SIZE_MAX/2+1,1,&offset) && !bytes);
    bytes=1;
    CHECK(source_region_span(&bytes,0,4,4,&offset) && bytes==1 && offset==1);
    for (unsigned mode=0;mode<6;++mode) {
        SourceContext ctx={0}; ctx.budget=xr_xir_default_budget();
        XrXirFunction function={0}; function.result=XR_XIR_UNIT;
        SourceInstructionRecipe recipe={0}; recipe.instruction.op=XR_XIR_RETURN;
        SourceBlockRecipe block={0}; block.block.count=1;
        SourceFunction body={0}; body.count=1;body.block_count=1;
        body.recipes=&recipe;body.blocks=&block;
        ctx.functions=&function;ctx.bodies=&body;
        uint64_t scratch=3*sizeof(uint32_t),work=9;
        ctx.budget.scratch_bytes=scratch;ctx.budget.work=work;
        ctx.budget.metadata_bytes=sizeof(SourceMemory)+output_bytes;
        if (mode==1) --ctx.budget.metadata_bytes;
        if (mode==2) --ctx.budget.scratch_bytes;
        if (mode==3) --ctx.budget.work;
        uint64_t before_scratch=ctx.budget.scratch_bytes,before_work=ctx.budget.work;
        attempts=0;fail_at=mode>=4 ? mode-4 : SIZE_MAX;
        bool success=source_region_emit(&ctx,&function,false);
        CHECK(success==(mode==0));
        CHECK(ctx.budget.scratch_bytes==before_scratch);
        if (!mode) {
            CHECK(ctx.memory && !ctx.memory->next && attempts==2);
            CHECK(ctx.allocated==sizeof(SourceMemory)+output_bytes);
            CHECK(function.instructions==(XrXirInstruction *)(ctx.memory+1));
            CHECK(function.blocks==(XrXirBlock *)((unsigned char *)(ctx.memory+1)+block_offset));
            CHECK(function.instruction_count==1 && function.block_count==1);
            CHECK(!function.operands && !function.operand_count && !ctx.budget.work);
        } else {
            CHECK(ctx.diagnostic.status==(mode>=4 ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET));
            CHECK(!function.instructions && !function.blocks && !function.operands && !ctx.memory);
            CHECK(ctx.budget.work==(mode==2 || mode==3 ? before_work : 0));
        }
        while(ctx.memory) {SourceMemory *next=ctx.memory->next;xr_free(ctx.memory);ctx.memory=next;}
        CHECK(!live);
    }
    attempts=0;fail_at=SIZE_MAX;
}
