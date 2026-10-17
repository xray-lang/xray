/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_range_field_cases.h - Actual complete range descriptor fields
 *
 * KEY CONCEPT:
 *   Literal physical fixtures grant no Checked or semantic permission.
 */
#ifndef XIR_CELL_RANGE_FIELD_CASES_H
#define XIR_CELL_RANGE_FIELD_CASES_H
typedef struct CellRangeFields {
    EffectBodyRun runs[2];EffectBodyFunction function;EffectBodyMaterial material;
    EffectCellBodyAdapter adapter;XrXirCellProofBodyRead read;
} CellRangeFields;

static void cell_range_fields_fixture(const XrXirCompileContext *context,CellRangeFields *f,
    uint32_t mask,uint32_t literal) {
    memset(f,0,sizeof(*f));memset(f->runs,0xA5,sizeof(f->runs));
    const uint32_t deltas[10]={2,6,10,UINT32_MAX,18,32,4,4,40,62};
    f->runs[0].first=0;f->runs[0].count=literal?literal:3;
    f->runs[0].mask=literal?EFFECT_BODY_LITERAL_ROWS:mask;f->runs[0].base=mask_expected(0);
    for(uint32_t w=0;w<10;++w)if(mask&(1u<<w))f->runs[0].delta[w]=deltas[w];
    if(literal)for(uint32_t i=1;i<literal;++i)f->runs[0].literal_tail[i-1]=mask_expected((i*i*67+i*19)&1023);
    f->runs[1].first=f->runs[0].count;f->runs[1].count=1;f->runs[1].mask=0;
    f->runs[1].base=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->function=(EffectBodyFunction){.instruction_count=f->runs[0].count+1,.runs=f->runs,.run_count=2,.run_capacity=2};
    f->material=(EffectBodyMaterial){.resources=context->resources,.references=1,.functions=&f->function,
        .storage_kind=EFFECT_BODY_RECORDS_OWNED};
    f->material.domain=(XirMetadataDomain){.types=&f->material.terms.types,.function_count=1};
    f->adapter=(EffectCellBodyAdapter){.work=context,.request=effect_body_owned_request(context,&f->material)};
    f->read=(XrXirCellProofBodyRead){.domain=f->material.domain,.resources=context->resources,.owner=&f->adapter,
        .head=effect_cell_body_head,.instruction=effect_cell_body_instruction,.type=effect_cell_body_type,
        .shape=effect_cell_body_shape,.span=effect_cell_body_span,.range=effect_cell_body_range};
}
static XrXirStatus cell_range_fields_read(const XrXirCompileContext *context,CellRangeFields *f,
    XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    CellRangeFields held;memcpy(&held,f,sizeof(held));
    XrXirCellProofRunCursor old_cursor=*cursor;XrXirCellProofRun old_output;memcpy(&old_output,output,sizeof(old_output));
    XrCompileResourceStats before=cell_borrow_stats(context);
    XrXirStatus status=effect_cell_body_range(context,&f->read,cursor,output);
    XrCompileResourceStats after=cell_borrow_stats(context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes && !memcmp(f,&held,sizeof(held)));
    if(status!=XR_XIR_OK)CHECK(!memcmp(output,&old_output,sizeof(old_output)) && !memcmp(cursor,&old_cursor,sizeof(old_cursor)));
    return status;
}
static void cell_range_fields_oracle(const XrXirCompileContext *context,CellRangeFields *f,
    const XrXirCellProofRun *output,uint32_t mask,uint32_t literal) {
    CHECK(output->first==0 && output->count==(literal?literal:3) && output->varying==(literal?0:mask) &&
        output->operation==&f->runs[0].base && output->literal_tail==(literal?f->runs[0].literal_tail:NULL));
    for(uint32_t w=0;w<10;++w)CHECK(output->delta[w]==(!literal && (mask&(1u<<w))?f->runs[0].delta[w]:UINT32_C(0xCECECECE)));
    CellProofBuild build={.context=context,.status=XR_XIR_OK};
    for(uint32_t i=0;i<output->count;++i){XrXirInstruction storage={0};
        const XrXirInstruction *row=cell_proof_run_operation(&build,0,output,i,&storage);
        XrXirInstruction expected=literal?mask_expected(i?(i*i*67+i*19)&1023:0):
            i==2?mask_third(mask):mask_expected(i?mask:0);
        CHECK(row && build.status==XR_XIR_OK && mask_same(row,&expected));
    }
}
static void cell_range_fields_work(uint32_t mask,uint32_t literal) {
    XrXirCompileContext context=cell_borrow_owner(4000000);CellRangeFields f;
    cell_range_fields_fixture(&context,&f,mask,literal);XrXirCellProofRunCursor cursor={0};XrXirCellProofRun output;
    memset(&output,0xCE,sizeof(output));uint64_t start=cell_borrow_stats(&context).work;
    CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_OK);
    uint64_t exact=cell_borrow_stats(&context).work-start+1;cell_range_fields_oracle(&context,&f,&output,mask,literal);
    xr_compile_resources_release(context.resources);
    for(uint64_t cap=1;cap<=exact;++cap){context=cell_borrow_owner(cap);cell_range_fields_fixture(&context,&f,mask,literal);
        cursor=(XrXirCellProofRunCursor){0};memset(&output,0xCE,sizeof(output));
        CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cap<exact)CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_BUDGET);
        else CHECK(cursor.next==f.runs[0].count && cursor.run==1);
        xr_compile_resources_release(context.resources);
    }
}
static void cell_range_fields_faults(const XrXirCompileContext *context) {
    for(unsigned fault=0;fault<24;++fault){CellRangeFields f;cell_range_fields_fixture(context,&f,1023,0);
        XrXirCellProofRunCursor cursor={0};XrXirCellProofRun output;memset(&output,0xCE,sizeof(output));
        if(fault<10)f.runs[0].delta[fault]=0;
        else if(fault==10)f.runs[0].mask|=1024;else if(fault==11)f.runs[0].count=2;
        else if(fault==12)f.runs[0].first=1;else if(fault==13)f.runs[0].count=0;
        else if(fault==14)f.runs[0].base.op=XR_XIR_INVALID;else if(fault==15)f.runs[0].base.op=XR_XIR_OP_COUNT;
        else if(fault==16)++f.runs[1].first;else if(fault==17)f.function.run_capacity=1;
        else if(fault==18)f.function.instruction_count=3;else if(fault==19)cursor.function=1;
        else if(fault==20)cursor.next=1;else if(fault==21)cursor.run=3;
        else if(fault==22)f.read.domain.function_count=2;else f.read.range=NULL;
        CHECK(cell_range_fields_read(context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(cell_range_fields_read(context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);
    }
    for(unsigned fault=0;fault<5;++fault){CellRangeFields f;cell_range_fields_fixture(context,&f,0,2);
        XrXirCellProofRunCursor cursor={0};XrXirCellProofRun output;memset(&output,0xCE,sizeof(output));
        if(!fault)f.read.domain.types=NULL;else if(fault==1)f.read.domain.declarations=(void *)&f;
        else if(fault==2)f.read.domain.generics=(void *)&f;else if(fault==3)f.read.domain.defaults=(void *)&f;
        else f.adapter.request.shape=effect_body_raw_shape;
        CHECK(cell_range_fields_read(context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);
    }
    CellRangeFields f;cell_range_fields_fixture(context,&f,0,0);
    f.function.instruction_count=5;XrXirCellProofRunCursor cursor={.next=3,.run=1};
    XrXirCellProofRun output;memset(&output,0xCE,sizeof(output));
    CHECK(cell_range_fields_read(context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext foreign=cell_borrow_owner(4000000);uint64_t work=cell_borrow_stats(&foreign).work;
    CHECK(cell_range_fields_read(&foreign,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE && cell_borrow_stats(&foreign).work==work);
    xr_compile_resources_release(foreign.resources);
}
static void cell_range_fields_controls(void) {
    XrXirCompileContext context=cell_borrow_owner(4000000);XrCompileResourceStats before=cell_borrow_stats(&context);
    for(uint32_t mask=0;mask<1024;++mask){CellRangeFields f;cell_range_fields_fixture(&context,&f,mask,0);
        uint32_t selected_count=0;for(uint32_t bits=mask;bits;bits&=bits-1)++selected_count;
        XrXirCellProofRunCursor cursor={0};XrXirCellProofRun output;memset(&output,0xCE,sizeof(output));
        uint64_t start=cell_borrow_stats(&context).work;
        CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_OK && cursor.next==3 && cursor.run==1);
        uint64_t received=cell_borrow_stats(&context).work-start;
        cell_range_fields_oracle(&context,&f,&output,mask,0);
        /* The independent physical range reader must ignore last, whereas
         * the selected reader must still reject a decreasing ordinal. */
        for(uint32_t poison=0;poison<3;++poison){uint32_t last=poison==0?0:poison==1?UINT32_MAX:UINT32_C(0xA5A5A5A5);
            EffectBodyRecordCursor physical={0,0,last};const EffectBodyRun *ref=NULL;
            start=cell_borrow_stats(&context).work;
            CHECK(effect_body_range_ref(&f.adapter.request.read,&f.function,&physical,&ref)==XR_XIR_OK &&
                ref==&f.runs[0] && physical.first==3 && physical.run==1 && physical.last==last);
            uint64_t overhead=18+2*sizeof(uint32_t)+1+sizeof(uint32_t)+sizeof(void *)+
                5*sizeof(uint32_t)+2*sizeof(void *)+13*(uint64_t)selected_count;
            CHECK(received==cell_borrow_stats(&context).work-start+overhead);
        }
        EffectBodyRecordCursor selected={0,0,UINT32_MAX};const EffectBodyRun *ref=NULL;
        CHECK(effect_body_record_cursor_ref(&f.adapter.request.read,&f.function,2,&selected,&ref)==XR_XIR_OK);
        EffectBodyRecordCursor held=selected;const EffectBodyRun *sentinel=ref;
        CHECK(effect_body_record_cursor_ref(&f.adapter.request.read,&f.function,1,&selected,&ref)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&selected,&held,sizeof(held)) && ref==sentinel);
        CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_OK && cursor.next==4 && cursor.run==2 &&
            output.first==3 && output.count==1 && output.operation==&f.runs[1].base);
        CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);
    }
    for(uint32_t count=2;count<=8;++count){CellRangeFields f;cell_range_fields_fixture(&context,&f,0,count);
        XrXirCellProofRunCursor cursor={0};XrXirCellProofRun output;memset(&output,0xCE,sizeof(output));
        CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_OK && cursor.next==count && cursor.run==1);
        cell_range_fields_oracle(&context,&f,&output,0,count);
        for(uint32_t i=0;i<count;++i)for(unsigned upper=0;upper<2;++upper){XrXirInstruction *row=i?&f.runs[0].literal_tail[i-1]:&f.runs[0].base;
            XrXirOp held=row->op;row->op=upper?XR_XIR_OP_COUNT:XR_XIR_INVALID;cursor=(XrXirCellProofRunCursor){0};
            CHECK(cell_range_fields_read(&context,&f,&cursor,&output)==XR_XIR_BAD_STRUCTURE);row->op=held;
        }
    }
    cell_range_fields_faults(&context);cell_range_fields_work(0,0);cell_range_fields_work(1023,0);cell_range_fields_work(0,2);cell_range_fields_work(0,8);
    XrCompileResourceStats after=cell_borrow_stats(&context);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes && after.work<4000000);
    fprintf(stderr,"Cell range fields:1024 affine masks, independent ten-word oracle and exact active-field fee oracle, poisoned inactive deltas, literal lengths2..8 and every invalid/upper opcode,29 shape/domain/identity faults plus EOF/foreign ledger, complete range cursor ignores three last poisons while selected decreasing ordinal still refuses, every work unit empty/full/literal2/literal8 and same finite-owner retries, descriptor/cursor atomic refusal and physical bytes unchanged, no allocation work%llu cap4000000 PASS\n",(unsigned long long)after.work);
    xr_compile_resources_release(context.resources);
}
#endif // XIR_CELL_RANGE_FIELD_CASES_H
