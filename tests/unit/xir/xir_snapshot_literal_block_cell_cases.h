/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_literal_block_cell_cases.h - Independent complete Cell literal data
 */
#ifndef XIR_SNAPSHOT_LITERAL_BLOCK_CELL_CASES_H
#define XIR_SNAPSHOT_LITERAL_BLOCK_CELL_CASES_H
static void mask_literal_cell_blocks(const XrXirCompileContext *context) {
    for(uint32_t count=2;count<=8;++count){EffectBodyRun run;memset(&run,0xa5,sizeof(run));
        XrXirInstruction producer[8],expected[8];
        for(uint32_t i=0;i<count;++i)producer[i]=expected[i]=mask_expected((i*i*67+i*19)&1023);
        run.first=7;run.count=count;run.mask=EFFECT_BODY_LITERAL_ROWS;run.base=producer[0];
        for(uint32_t i=1;i<count;++i)run.literal_tail[i-1]=producer[i];memset(producer,0xce,sizeof(producer));
        XrXirCellProofRun range={.first=7,.count=count,.operation=&run.base,.literal_tail=run.literal_tail};
        const XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
        for(uint32_t i=0;i<count;++i){CellProofBuild build={.context=context,.status=XR_XIR_OK};XrXirInstruction storage=sentinel;
            const XrXirInstruction *output=cell_proof_run_operation(&build,0,&range,7+i,&storage);
            CHECK(build.status==XR_XIR_OK && output==(i?&run.literal_tail[i-1]:&run.base) &&
                mask_same(output,&expected[i]) && mask_same(&storage,&sentinel));}
        for(uint32_t damaged=0;damaged<count;++damaged){XrXirInstruction *row=damaged?&run.literal_tail[damaged-1]:&run.base;
            XrXirOp held=row->op;row->op=XR_XIR_INVALID;
            for(uint32_t i=0;i<count;++i){CellProofBuild build={.context=context,.status=XR_XIR_OK};XrXirInstruction storage=sentinel;
                CHECK(!cell_proof_run_operation(&build,0,&range,7+i,&storage) && build.status==XR_XIR_BAD_STRUCTURE &&
                    mask_same(&storage,&sentinel));}row->op=held;}
        for(unsigned mode=0;mode<12;++mode){XrXirCellProofRun bad=range;
            if(mode<10)bad.varying=UINT32_C(1)<<mode;else bad.count=mode==10?1:9;
            CellProofBuild build={.context=context,.status=XR_XIR_OK};XrXirInstruction storage=sentinel;
            CHECK(!cell_proof_run_operation(&build,0,&bad,7,&storage) && build.status==XR_XIR_BAD_STRUCTURE &&
                mask_same(&storage,&sentinel));}
    }
    fprintf(stderr,"Cell complete literal lengths2..8: full ten-word native oracle after producer death, every damaged opcode before any selected row, all varying-mask bits and invalid counts preserve output PASS\n");
}
#endif // XIR_SNAPSHOT_LITERAL_BLOCK_CELL_CASES_H
