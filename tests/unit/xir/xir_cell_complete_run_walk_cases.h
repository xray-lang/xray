/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_complete_run_walk_cases.h - Complete native intake and original roles
 */
#ifndef XIR_CELL_COMPLETE_RUN_WALK_CASES_H
#define XIR_CELL_COMPLETE_RUN_WALK_CASES_H
static XrXirInstruction complete_walk_expected[8];
static uint32_t complete_walk_calls,complete_walk_reject;
static bool complete_walk_row(CellProofBuild *build,uint32_t f,uint32_t i,
    const XrXirCellProofBodyHead *head,const XrXirInstruction *operation) {
    CHECK(f==0 && head->instruction_count==15 && i==7+complete_walk_calls);
    if(complete_walk_calls++==complete_walk_reject)return cell_proof_error(build,f,i,XR_XIR_IO);
    if(!cell_proof_work(build,2))return false;
    CHECK(mask_same(operation,&complete_walk_expected[i-7]));return true;
}
static bool complete_walk_operation(const XrXirCompileContext *context,const XrXirCellProofRun *run,
    uint32_t reject,XrXirStatus *status) {
    XrXirCellProofBodyHead head={.instruction_count=15};CellProofBuild build={.context=context,.status=XR_XIR_OK};
    complete_walk_calls=0;complete_walk_reject=reject;
    bool result=cell_proof_run_walk(&build,0,&head,run,complete_walk_row,1);*status=build.status;return result;
}
static void complete_walk_roles(const XrXirCompileContext *context) {
    XrXirInstruction values[8];for(uint32_t i=0;i<8;++i)values[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    values[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,1}};
    values[3]=(XrXirInstruction){.op=XR_XIR_INVOKE,.type=XR_XIR_I64,.immediate=1};
    values[5]=(XrXirInstruction){.op=XR_XIR_CLEANUP_REGISTER,.type=XR_XIR_UNIT,.immediate=1};
    XrXirCellProofRun run={.count=8,.operation=&values[0],.literal_tail=&values[1]};XrXirCellProofBodyHead head={.instruction_count=8};
    CellProofFunction functions[2]={{.count=8,.stored=8},{.begin=8,.count=1,.parameters=1,.stored=1}};
    uint8_t tracked[9]={0},uses[9]={0},original[9]={0};tracked[8]=1;
    XrXirFunctionIdentity identities[2]={{0},{.cleanup_owner=1}};XrXirDeclarations declarations={.functions=identities};
    XrXirTypes types={0};XrXirCellProofBodyRead body={.domain={.types=&types,.declarations=&declarations,.function_count=2}};
    XrXirCellProvenance proof={.function_count=2,.value_count=9,.functions=functions,.tracked=tracked};
    CellProofBuild build={.context=context,.body=&body,.proof=&proof,.status=XR_XIR_OK,.uses=uses};
    CHECK(cell_proof_run_walk(&build,0,&head,&run,cell_proof_role_receive,1) && uses[8]==2);
    build.uses=original;for(uint32_t i=0;i<8;++i){XrXirInstruction storage;
        const XrXirInstruction *operation=cell_proof_run_operation(&build,0,&run,i,&storage);
        CHECK(operation && cell_proof_role_row(&build,0,i,operation));}
    CHECK(!memcmp(uses,original,sizeof(uses)));
    for(uint32_t damaged=0;damaged<8;++damaged){XrXirOp held=values[damaged].op;values[damaged].op=XR_XIR_INVALID;
        memset(uses,0,sizeof(uses));build.status=XR_XIR_OK;build.uses=uses;
        CHECK(!cell_proof_run_walk(&build,0,&head,&run,cell_proof_role_receive,1) && build.status==XR_XIR_BAD_STRUCTURE);
        for(uint32_t i=0;i<9;++i)CHECK(!uses[i]);values[damaged].op=held;}
    for(unsigned mode=0;mode<4;++mode){XrXirInstruction call=values[1],invoke=values[3];uint32_t owner=identities[1].cleanup_owner;
        if(!mode)values[3].immediate=2;else if(mode==1)identities[1].cleanup_owner=2;
        else if(mode==2)values[1].args[1]=2;else values[1].op=XR_XIR_FUNCTION_REF;
        memset(uses,0,sizeof(uses));build.status=XR_XIR_OK;build.uses=uses;
        CHECK(!cell_proof_run_walk(&build,0,&head,&run,cell_proof_role_receive,1) &&
            build.status==(mode==3?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
        values[1]=call;values[3]=invoke;identities[1].cleanup_owner=owner;
        build.status=XR_XIR_OK;CHECK(cell_proof_run_walk(&build,0,&head,&run,cell_proof_role_receive,1));
    }
}
static void complete_cell_run_walk_controls(const XrXirCompileContext *context) {
    for(uint32_t count=2;count<=8;++count){XrXirInstruction source[8],values[8];
        for(uint32_t i=0;i<count;++i)source[i]=values[i]=complete_walk_expected[i]=mask_expected((i*i*67+i*19)&1023);
        XrXirCellProofRun run={.first=7,.count=count,.operation=&values[0],.literal_tail=&values[1]};memset(source,0xce,sizeof(source));
        XrXirStatus status;CHECK(complete_walk_operation(context,&run,UINT32_MAX,&status) && status==XR_XIR_OK && complete_walk_calls==count);
        for(uint32_t damaged=0;damaged<count;++damaged){XrXirOp held=values[damaged].op;values[damaged].op=XR_XIR_INVALID;
            CHECK(!complete_walk_operation(context,&run,UINT32_MAX,&status) && status==XR_XIR_BAD_STRUCTURE && !complete_walk_calls);
            values[damaged].op=held;}
        for(uint32_t reject=0;reject<count;++reject){CHECK(!complete_walk_operation(context,&run,reject,&status) &&
                status==XR_XIR_IO && complete_walk_calls==reject+1);
            CHECK(complete_walk_operation(context,&run,UINT32_MAX,&status) && complete_walk_calls==count);}
        for(unsigned mode=0;mode<12;++mode){XrXirCellProofRun bad=run;
            if(mode<10)bad.varying=UINT32_C(1)<<mode;else bad.count=mode==10?1:9;
            CHECK(!complete_walk_operation(context,&bad,UINT32_MAX,&status) && status==XR_XIR_BAD_STRUCTURE && !complete_walk_calls);}
        XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(complete_walk_operation(context,&run,UINT32_MAX,&status));CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        uint64_t exact=after.work-before.work+1;
        for(uint64_t cap=1;cap<=exact;++cap){XrCompileResourceLimits limits={8388608,4194304,cap};XrXirCompileContext finite={0};
            CHECK(xr_compile_resources_new(&limits,&finite.resources)==XR_COMPILE_RESOURCE_OK);finite.limits=xr_xir_compile_default_limits();
            CHECK(complete_walk_operation(&finite,&run,UINT32_MAX,&status)==(cap==exact) && status==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
            if(cap<exact){CHECK(!complete_walk_operation(&finite,&run,UINT32_MAX,&status) && status==XR_XIR_BUDGET && !complete_walk_calls);}
            xr_compile_resources_release(finite.resources);}
    }
    complete_walk_roles(context);
    fprintf(stderr,"Complete Cell run walk lengths2..8: independent native ten-word oracle after producer death, every invalid literal opcode before any receiver, all callback IO prefixes and same finite-owner retry, all masks/count bounds and every work-unit cap through exact with exhausted-owner retry; original CALL/INVOKE/cleanup/REF role receiver and zero-actual target gates match scalar oracle PASS\n");
}
#endif // XIR_CELL_COMPLETE_RUN_WALK_CASES_H
