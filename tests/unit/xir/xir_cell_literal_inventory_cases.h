/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_literal_inventory_cases.h - Current literal inventory and type reads
 */
#ifndef XIR_CELL_LITERAL_INVENTORY_CASES_H
#define XIR_CELL_LITERAL_INVENTORY_CASES_H
typedef struct LiteralInventoryInput {
    XrXirInstruction rows[8];XrXirType types[8];
    XrXirCellProofBodyHead head;XrXirCellProofRun run;
    uint32_t calls,reject,mismatch;
} LiteralInventoryInput;
typedef struct LiteralInventoryResult {
    XrXirStatus status;uint32_t projects;bool tracked;
} LiteralInventoryResult;
static XrXirStatus literal_inventory_type(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t value,XrXirType *output) {
    LiteralInventoryInput *input=(LiteralInventoryInput *)owner;uint32_t i=input->calls++;
    CHECK(f==0 && value==input->head.parameter_count+input->run.first+i && i<input->run.count);
    if(i==input->reject)return XR_XIR_IO;
    if(!xir_compile_work(context,7+sizeof(*output)))return XR_XIR_BUDGET;
    *output=i==input->mismatch?XR_XIR_UNIT:input->types[i];return XR_XIR_OK;
}
/* This oracle keeps the original complete selected-row core for every row.
 * It does not call the inventory loop under test or reuse any tracked result. */
static bool literal_inventory_scalar(CellProofBuild *build,LiteralInventoryInput *input,bool *tracked) {
    if(!cell_proof_work(build,6))return false;
    for(uint32_t i=input->run.first;i-input->run.first<input->run.count;++i){
        XrXirType type=cell_proof_type(build,0,input->head.parameter_count+i);
        if(build->status!=XR_XIR_OK || !cell_proof_work(build,3))return false;
        XrXirInstruction storage;const XrXirInstruction *row=cell_proof_run_operation(build,0,&input->run,i,&storage);
        if(!row)return false;
        if(row->type!=type)return cell_proof_error(build,0,i,XR_XIR_BAD_TYPE);
        if(!cell_proof_inventory_instruction(build,0,i,type,row->op,tracked))return false;
    }
    return true;
}
static bool literal_inventory_operation(const XrXirCompileContext *context,LiteralInventoryInput *input,
    LiteralInventoryResult *output,bool scalar) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};XrXirTypes types={.nodes=&node,.count=1};
    XrXirCellProofBodyRead body={.owner=input,.type=literal_inventory_type,.domain={.types=&types}};
    XrXirCellProvenance proof={.project_count=11};CellProofBuild build={.context=context,.body=&body,.proof=&proof,.status=XR_XIR_OK};
    input->calls=0;bool tracked=false;XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    bool result=scalar?literal_inventory_scalar(&build,input,&tracked):cell_proof_inventory_run(&build,0,&input->head,&input->run,&tracked);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK &&
        after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    *output=(LiteralInventoryResult){build.status,proof.project_count,tracked};return result;
}
static void literal_inventory_fixture(LiteralInventoryInput *input,uint32_t count) {
    memset(input,0,sizeof(*input));input->reject=input->mismatch=UINT32_MAX;
    input->head=(XrXirCellProofBodyHead){.parameter_count=3,.instruction_count=15};
    for(uint32_t i=0;i<count;++i){
        XrXirOp opcode=i%3==1?XR_XIR_CELL_PROJECT:i%3==2?XR_XIR_SLOT_PLACE:XR_XIR_CONST_INT;
        input->types[i]=i%3==1?(XrXirType)256:XR_XIR_I64;
        input->rows[i]=(XrXirInstruction){.op=opcode,.type=input->types[i],.args={i,7*i},.immediate=(int64_t)37*i-19};
    }
    input->run=(XrXirCellProofRun){.first=7,.count=count,.operation=&input->rows[0],.literal_tail=&input->rows[1]};
}
static void literal_inventory_faults(const XrXirCompileContext *context,LiteralInventoryInput *input) {
    LiteralInventoryResult output;uint32_t count=input->run.count;XrXirInstruction held[8];memcpy(held,input->rows,sizeof(held));
    for(uint32_t i=0;i<count;++i){XrXirOp opcode=input->rows[i].op;
        input->rows[i].op=XR_XIR_INVALID;
        CHECK(!literal_inventory_operation(context,input,&output,false) && output.status==XR_XIR_BAD_STRUCTURE &&
            input->calls==1 && !output.tracked && output.projects==11);
        input->reject=0;
        CHECK(!literal_inventory_operation(context,input,&output,false) && output.status==XR_XIR_IO && input->calls==1);
        input->reject=UINT32_MAX;input->rows[i].op=opcode;
        input->reject=i;CHECK(!literal_inventory_operation(context,input,&output,false) && output.status==XR_XIR_IO && input->calls==i+1);
        input->reject=UINT32_MAX;input->mismatch=i;
        CHECK(!literal_inventory_operation(context,input,&output,false) && output.status==XR_XIR_BAD_TYPE && input->calls==i+1);
        input->mismatch=UINT32_MAX;
        CHECK(literal_inventory_operation(context,input,&output,false) && input->calls==count);
    }
    for(unsigned mode=0;mode<14;++mode){XrXirCellProofRun held_run=input->run;
        if(mode<10)input->run.varying=UINT32_C(1)<<mode;
        else if(mode<12)input->run.count=mode==10?1:9;
        else if(mode==12)input->run.first=15;else input->run.count=9;
        CHECK(!literal_inventory_operation(context,input,&output,false) && output.status==XR_XIR_BAD_STRUCTURE &&
            !output.tracked && output.projects==11);
        input->run=held_run;
    }
    CHECK(!memcmp(held,input->rows,sizeof(held)));
}
static void cell_literal_inventory_controls(const XrXirCompileContext *context) {
    XrCompileResourceLimits limits={8388608,4194304,4000000};
    for(uint32_t count=2;count<=8;++count){LiteralInventoryInput input;literal_inventory_fixture(&input,count);
        LiteralInventoryResult output,scalar;XrCompileResourceStats before,after,oracle;
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(literal_inventory_operation(context,&input,&output,false) && output.status==XR_XIR_OK && input.calls==count);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(literal_inventory_operation(context,&input,&scalar,true) && scalar.status==output.status &&
            scalar.tracked==output.tracked && scalar.projects==output.projects && output.tracked && output.projects==11+(count+1)/3);
        CHECK(xr_compile_resources_stats(context->resources,&oracle)==XR_COMPILE_RESOURCE_OK);
        CHECK(oracle.work-after.work-(after.work-before.work)==(uint64_t)2*count*(count-1));
        literal_inventory_faults(context,&input);uint64_t exact=after.work-before.work+1;
        for(uint64_t cap=1;cap<=exact;++cap){XrXirCompileContext finite={0};limits.work=cap;
            CHECK(xr_compile_resources_new(&limits,&finite.resources)==XR_COMPILE_RESOURCE_OK);finite.limits=context->limits;
            CHECK(literal_inventory_operation(&finite,&input,&output,false)==(cap==exact) &&
                output.status==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
            if(cap<exact){XrCompileResourceStats retry_before,retry_after;
                CHECK(xr_compile_resources_stats(finite.resources,&retry_before)==XR_COMPILE_RESOURCE_OK);
                uint32_t calls=cap-retry_before.work>=6?1u:0u;
                CHECK(!literal_inventory_operation(&finite,&input,&output,false) && output.status==XR_XIR_BUDGET &&
                    input.calls==calls && !output.tracked && output.projects==11);
                CHECK(xr_compile_resources_stats(finite.resources,&retry_after)==XR_COMPILE_RESOURCE_OK &&
                    retry_after.work-retry_before.work==(calls?6u:0u));
            }
            xr_compile_resources_release(finite.resources);
        }
    }
    fprintf(stderr,"Literal Cell inventory lengths2..8: original scalar core oracle, every independent type callback and IO/type refusal, every bad opcode before inventory effects with first type IO priority, mask/count/head bounds, native bytes unchanged, no allocation and every work-unit cap through exact with exhausted-owner retry PASS\n");
}
#endif // XIR_CELL_LITERAL_INVENTORY_CASES_H
