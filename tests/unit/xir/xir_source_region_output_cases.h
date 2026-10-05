/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static void source_region_output_cases(void) {
    size_t bytes=0,offset=99;
    CHECK(source_region_span(&bytes,0,sizeof(XrXirInstruction),_Alignof(XrXirInstruction),&offset));
    CHECK(!bytes && !offset);
    CHECK(source_region_span(&bytes,1,sizeof(XrXirInstruction),_Alignof(XrXirInstruction),&offset));
    CHECK(offset%_Alignof(XrXirInstruction)==0);
    size_t block_offset=0;
    CHECK(source_region_span(&bytes,1,sizeof(XrXirBlock),_Alignof(XrXirBlock),&block_offset));
    CHECK(block_offset%_Alignof(XrXirBlock)==0);size_t output_bytes=bytes;
    bytes=SIZE_MAX;CHECK(!source_region_span(&bytes,1,1,2,&offset) && bytes==SIZE_MAX);
    bytes=SIZE_MAX-1;CHECK(!source_region_span(&bytes,1,4,1,&offset) && bytes==SIZE_MAX-1);
    bytes=0;CHECK(!source_region_span(&bytes,2,SIZE_MAX/2+1,1,&offset) && !bytes);
    bytes=1;CHECK(source_region_span(&bytes,0,4,4,&offset) && bytes==1 && offset==1);
    uint64_t scratch=3*sizeof(uint32_t),payload=sizeof(SourceMemory)+output_bytes;
    uint64_t storage=sizeof(XrCompileResources)+2*sizeof(CompileAllocation)+scratch+payload;
    uint64_t work=12+scratch+payload;
    for(unsigned mode=0;mode<6;++mode){
        XrCompileResourceLimits caps={storage,storage,work};
        if(mode==1)--caps.allocated_bytes;if(mode==2)--caps.live_bytes;if(mode==3)--caps.work;
        SourceContext ctx={0};CHECK(allocation_private_context(&ctx,caps));
        XrXirFunction function={0};function.result=XR_XIR_UNIT;
        SourceInstructionRecipe recipe={0};recipe.instruction.op=XR_XIR_RETURN;
        SourceBlockRecipe block={0};block.block.count=1;
        SourceFunction body={0};body.count=1;body.block_count=1;body.recipes=&recipe;body.blocks=&block;
        ctx.functions=&function;ctx.bodies=&body;
        size_t start=source_fixture_compile_attempts;
        source_fixture_compile_fail_at=mode>=4?start+mode-4:SIZE_MAX;
        source_fixture_compile_injected=false;
        bool success=source_region_emit(&ctx,&function,false);CHECK(success==(mode==0));
        XrCompileResourceStats fees={0};CHECK(xr_compile_resources_stats(ctx.compile.resources,&fees)==XR_COMPILE_RESOURCE_OK);
        if(!mode){
            CHECK(ctx.memory && !ctx.memory->next && source_fixture_compile_attempts==start+2);
            CHECK(function.instructions==(XrXirInstruction *)(ctx.memory+1));
            CHECK(function.blocks==(XrXirBlock *)((unsigned char *)(ctx.memory+1)+block_offset));
            CHECK(function.instruction_count==1 && function.block_count==1);
            CHECK(!function.operands && !function.operand_count);
            CHECK(fees.allocated_bytes==storage && fees.peak_bytes==storage && fees.work==work);
            CHECK(fees.live_bytes==sizeof(XrCompileResources)+sizeof(CompileAllocation)+payload);
        }else{
            CHECK(ctx.diagnostic.status==(mode>=4?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET));
            CHECK(!function.instructions && !function.blocks && !function.operands);
        }
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if(mode>=4)allocation_pure_point("region_output",mode-4);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
}
