/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_received_run_walk_cases.h - Actual complete reception before receivers
 */
#ifndef XIR_CELL_RECEIVED_RUN_WALK_CASES_H
#define XIR_CELL_RECEIVED_RUN_WALK_CASES_H
typedef struct ReceivedWalkInput {
    XrXirCellProofRun run;uint32_t calls,cursor_fault;bool refuse;
} ReceivedWalkInput;
static XrXirStatus received_walk_range(const XrXirCompileContext *context,const XrXirCellProofBodyRead *read,
    XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    ReceivedWalkInput *input=(ReceivedWalkInput *)read->owner;++input->calls;
    CHECK(cursor->function==0 && cursor->next==7);
    if(input->refuse)return XR_XIR_IO;
    if(!xir_compile_work(context,9+sizeof(*output)+2*sizeof(uint32_t)))return XR_XIR_BUDGET;
    *output=input->run;cursor->next=output->first+output->count;
    if(input->cursor_fault==1)++cursor->next;else if(input->cursor_fault==2)++cursor->function;
    return XR_XIR_OK;
}
static bool received_walk_operation(const XrXirCompileContext *context,ReceivedWalkInput *input,
    uint32_t reject,bool duplicate,XrXirStatus *status) {
    XrXirCellProofBodyHead head={.instruction_count=15};XrXirCellProofRunCursor cursor={.next=7};
    XrXirCellProofBodyRead body={.resources=context->resources,.owner=input,.range=received_walk_range};
    CellProofBuild build={.context=context,.body=&body,.status=XR_XIR_OK};XrXirCellProofRun run;
    input->calls=0;complete_walk_calls=0;complete_walk_reject=reject;
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    bool result=cell_proof_run(&build,&head,&cursor,&run);
    if(result)result=duplicate?cell_proof_run_walk(&build,0,&head,&run,complete_walk_row,1):
        cell_proof_received_run_walk(&build,0,&head,&run,complete_walk_row,1);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK &&
        after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    *status=build.status;return result;
}
static void received_walk_faults(const XrXirCompileContext *context,ReceivedWalkInput *input,XrXirInstruction *rows) {
    uint32_t count=input->run.count;XrXirStatus status;
    for(uint32_t i=0;i<count;++i){XrXirOp held=rows[i].op;
        for(unsigned bound=0;bound<2;++bound){rows[i].op=bound?XR_XIR_OP_COUNT:XR_XIR_INVALID;
            CHECK(!received_walk_operation(context,input,UINT32_MAX,false,&status) && status==XR_XIR_BAD_STRUCTURE && !complete_walk_calls && input->calls==1);}
        rows[i].op=held;
        CHECK(!received_walk_operation(context,input,i,false,&status) && status==XR_XIR_IO && complete_walk_calls==i+1);
        CHECK(received_walk_operation(context,input,UINT32_MAX,false,&status) && status==XR_XIR_OK && complete_walk_calls==count);
    }
    input->refuse=true;
    CHECK(!received_walk_operation(context,input,UINT32_MAX,false,&status) && status==XR_XIR_IO && !complete_walk_calls && input->calls==1);
    input->refuse=false;
    for(unsigned mode=0;mode<16;++mode){XrXirCellProofRun held=input->run;
        if(mode<10)input->run.varying=UINT32_C(1)<<mode;
        else if(mode<12)input->run.count=mode==10?1:9;
        else if(mode==12)input->run.operation=NULL;else if(mode==13)input->run.count=0;
        else input->cursor_fault=mode-13;
        CHECK(!received_walk_operation(context,input,UINT32_MAX,false,&status) && status==XR_XIR_BAD_STRUCTURE && !complete_walk_calls && input->calls==1);
        input->run=held;input->cursor_fault=0;
    }
}
static void received_cell_run_walk_controls(const XrXirCompileContext *context) {
    for(uint32_t count=2;count<=8;++count){XrXirInstruction source[8]={0},rows[8]={0},held[8];
        for(uint32_t i=0;i<count;++i)source[i]=rows[i]=complete_walk_expected[i]=mask_expected((i*i*67+i*19)&1023);
        memcpy(held,rows,sizeof(rows));memset(source,0xCE,sizeof(source));
        ReceivedWalkInput input={.run={.first=7,.count=count,.operation=&rows[0],.literal_tail=&rows[1]}};
        XrXirStatus status;XrCompileResourceStats before,after,oracle;
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(received_walk_operation(context,&input,UINT32_MAX,false,&status) && status==XR_XIR_OK && input.calls==1 && complete_walk_calls==count);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(received_walk_operation(context,&input,UINT32_MAX,true,&status) && status==XR_XIR_OK && complete_walk_calls==count);
        CHECK(xr_compile_resources_stats(context->resources,&oracle)==XR_COMPILE_RESOURCE_OK &&
            oracle.work-after.work-(after.work-before.work)==2*(uint64_t)count);
        received_walk_faults(context,&input,rows);CHECK(!memcmp(held,rows,sizeof(rows)));
        uint64_t exact=after.work-before.work+1;
        for(uint64_t cap=1;cap<=exact;++cap){XrCompileResourceLimits limits={8388608,4194304,cap};XrXirCompileContext finite={0};
            CHECK(xr_compile_resources_new(&limits,&finite.resources)==XR_COMPILE_RESOURCE_OK);finite.limits=context->limits;
            CHECK(received_walk_operation(&finite,&input,UINT32_MAX,false,&status)==(cap==exact) && status==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
            if(cap<exact)CHECK(!received_walk_operation(&finite,&input,UINT32_MAX,false,&status) && status==XR_XIR_BUDGET && !complete_walk_calls && input.calls==1);
            xr_compile_resources_release(finite.resources);
        }
    }
    fprintf(stderr,"Received Cell run lengths2..8: actual complete range callback before all receivers, independent ten-word oracle after producer bytes die, every invalid/upper opcode and16 shape/cursor faults before receivers, real range IO and all receiver IO prefixes/same finite-owner retries, original full-walk oracle and exact2*n work difference, no allocation and every work-unit cap through exact PASS\n");
}
#endif // XIR_CELL_RECEIVED_RUN_WALK_CASES_H
