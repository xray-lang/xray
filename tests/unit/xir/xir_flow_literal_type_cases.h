/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_flow_literal_type_cases.h - Complete physical rows and ordered writes
 */
#ifndef XIR_FLOW_LITERAL_TYPE_CASES_H
#define XIR_FLOW_LITERAL_TYPE_CASES_H
typedef struct FlowLiteralFixture {
    XrXirInstruction expected[8],flat[10];EffectBodyRun runs[2];XrXirType parameters[2],projected[12];
    const XrXirInstruction *references[10];uint32_t heads[12],next[10];
    EffectBodyFunction body;EffectBodyMaterial material;EffectInvocationNode node;
    EffectInvocationWorkspace owner;EffectBodyFunctionHead head;EffectInvocationFlow flow;
} FlowLiteralFixture;
static void flow_literal_reset(FlowLiteralFixture *f,bool references) {
    memset(f->projected,0xa5,sizeof(f->projected));memset(f->flat,0xa5,sizeof(f->flat));
    for(uint32_t i=0;i<10;++i)f->references[i]=NULL;
    memset(f->heads,0x5a,sizeof(f->heads));memset(f->next,0x5a,sizeof(f->next));
    f->flow=(EffectInvocationFlow){.owner=&f->owner,.function=&f->head,
        .values=f->head.parameter_count+f->head.instruction_count,.types=f->projected,
        .instructions=references?NULL:f->flat,.instruction_refs=references?f->references:NULL,
        .instruction_storage_count=references?0:f->head.instruction_count,
        .write_heads=f->heads,.write_next=f->next,.body=effect_body_owned_request(f->owner.work,&f->material)};
}
/* These ten-word rows exercise physical projection only. They grant no
 * checked body, Class identity or execution permission. */
static void flow_literal_fixture(const XrXirCompileContext *context,FlowLiteralFixture *f,uint32_t count) {
    memset(f,0,sizeof(*f));XrXirInstruction source[8]={0};f->parameters[0]=XR_XIR_I64;f->parameters[1]=XR_XIR_F64;
    for(uint32_t i=0;i<count;++i){XrXirOp opcode=i%4==1?XR_XIR_LOCAL_WRITE:i%4==2?XR_XIR_SCALAR_LOCAL_WRITE:
            i%4==3?XR_XIR_OWNED_LOCAL_WRITE:XR_XIR_CONST_INT;
        source[i]=(XrXirInstruction){.op=opcode,.type=i%4?XR_XIR_UNIT:XR_XIR_I64,
            .args={i%2,2+(i?i-1:0)},.targets={11*i+7,UINT32_MAX-i},
            .immediate=INT64_MIN+(int64_t)i*307,.type_arguments={300+i,700-i}};
        f->expected[i]=source[i];
    }
    f->runs[0]=(EffectBodyRun){.count=count,.mask=EFFECT_BODY_LITERAL_ROWS,.base=source[0]};
    for(uint32_t i=1;i<count;++i)f->runs[0].literal_tail[i-1]=source[i];memset(source,0xCE,sizeof(source));
    f->body=(EffectBodyFunction){.parameters=f->parameters,.parameter_count=2,.instruction_count=count,
        .runs=f->runs,.run_count=1,.run_capacity=2};
    f->material=(EffectBodyMaterial){.resources=context->resources,.references=1,.functions=&f->body};
    f->material.domain=(XirMetadataDomain){.types=&f->material.terms.types,.function_count=1};
    f->owner=(EffectInvocationWorkspace){.work=context,.final={.material=&f->material,.nodes=&f->node,.count=1}};
    f->head=(EffectBodyFunctionHead){.parameters=f->parameters,.parameter_count=2,.instruction_count=count};flow_literal_reset(f,true);
}
static XrXirStatus flow_literal_execute(FlowLiteralFixture *f) {
    const XrXirCompileContext *context=f->owner.work;RootParameterMark physical=rp_mark();size_t attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(context);XrXirStatus status=effect_invocation_flow_types(&f->flow);
    CHECK(rp_attempts==attempts && rp_stats(context).allocated_bytes==before.allocated_bytes && rp_stats(context).live_bytes==before.live_bytes);
    CHECK(status==XR_XIR_OK?f->flow.types_ready:!f->flow.types_ready);rp_balanced(physical);return status;
}
/* Build the independent index in forward order using explicit tails. The
 * provider constructs its links by reverse traversal and head insertion. */
static void flow_literal_oracle(const FlowLiteralFixture *f,uint32_t count,bool references) {
    uint32_t heads[12],next[10],tails[12];for(uint32_t v=0;v<12;++v)heads[v]=tails[v]=UINT32_MAX;
    for(uint32_t i=0;i<10;++i)next[i]=UINT32_MAX;
    for(uint32_t p=0;p<2;++p)CHECK(f->projected[p]==f->parameters[p]);
    for(uint32_t i=0;i<count;++i){const XrXirInstruction *row=references?f->references[i]:&f->flat[i];
        CHECK(row && conditional_compact_same(row,&f->expected[i]) && f->projected[2+i]==f->expected[i].type);
        if(references)CHECK(row==(i?&f->runs[0].literal_tail[i-1]:&f->runs[0].base));
        if(i%4){uint32_t destination=f->expected[i].args[0];
            if(tails[destination]!=UINT32_MAX)next[tails[destination]]=i;else heads[destination]=i;tails[destination]=i;}
    }
    for(uint32_t v=0;v<count+2;++v)CHECK(f->heads[v]==heads[v]);
    for(uint32_t i=0;i<count;++i)CHECK(f->next[i]==next[i]);
}
static void flow_literal_faults(const XrXirCompileContext *context,uint32_t count) {
    FlowLiteralFixture f;flow_literal_fixture(context,&f,count);
    for(uint32_t i=0;i<count;++i){XrXirInstruction *row=i?&f.runs[0].literal_tail[i-1]:&f.runs[0].base;XrXirOp held=row->op;
        for(unsigned bound=0;bound<2;++bound){row->op=bound?XR_XIR_OP_COUNT:XR_XIR_INVALID;flow_literal_reset(&f,true);
            CHECK(flow_literal_execute(&f)==XR_XIR_BAD_STRUCTURE);for(uint32_t j=0;j<count;++j)CHECK(!f.references[j]);}
        row->op=held;
    }
    EffectBodyRun run=f.runs[0];EffectBodyFunction body=f.body;EffectBodyFunctionHead head=f.head;
    for(unsigned mode=0;mode<20;++mode){
        if(mode<10)f.runs[0].mask|=UINT32_C(1)<<mode;
        else if(mode<12)f.runs[0].count=mode==10?1:9;
        else if(mode==12)f.runs[0].first=1;else if(mode==13)f.body.runs=NULL;
        else if(mode==14)f.body.run_capacity=0;else if(mode==15)f.body.run_count=0;
        else if(mode==16){f.body.run_count=2;f.runs[1]=(EffectBodyRun){.first=count,.count=1,.base=f.expected[count-1]};}
        else if(mode==17){++f.body.instruction_count;++f.head.instruction_count;}
        else if(mode==18)++f.body.parameter_count;
        flow_literal_reset(&f,true);if(mode==19)++f.flow.values;
        CHECK(flow_literal_execute(&f)==XR_XIR_BAD_STRUCTURE);for(uint32_t j=0;j<10;++j)CHECK(!f.references[j]);
        f.runs[0]=run;f.body=body;f.head=head;
    }
}
static void flow_literal_type_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    for(uint32_t count=2;count<=8;++count){FlowLiteralFixture f;flow_literal_fixture(&context,&f,count);
        XrCompileResourceStats before=rp_stats(&context);CHECK(flow_literal_execute(&f)==XR_XIR_OK);uint64_t exact=rp_stats(&context).work-before.work+1;
        flow_literal_oracle(&f,count,true);flow_literal_reset(&f,false);
        CHECK(flow_literal_execute(&f)==XR_XIR_OK);flow_literal_oracle(&f,count,false);flow_literal_faults(&context,count);
        for(uint64_t cap=1;cap<=exact;++cap){XrCompileResourceLimits limits=rp_caps();limits.work=cap;XrXirCompileContext finite=rp_owner(limits);
            uint64_t finite_baseline=rp_stats(&finite).live_bytes;flow_literal_fixture(&finite,&f,count);
            CHECK(flow_literal_execute(&f)==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
            if(cap<exact){flow_literal_reset(&f,true);CHECK(flow_literal_execute(&f)==XR_XIR_BUDGET);}
            else flow_literal_oracle(&f,count,true);
            rp_owner_free(&finite,finite_baseline);
        }
    }
    rp_owner_free(&context,baseline);rp_balanced(physical);
    fprintf(stderr,"Flow literal types lengths2..8: independent complete ten-word oracle after producer bytes die, actual native refs/types and original full scalar decoder, all ordered LOCAL/SCALAR/OWNED writes against independent forward-tail oracle, every invalid/upper opcode and20 shape/neighbor/EOF faults before instruction refs, no types_ready on refusal, no allocation/physical baseline and every work-unit cap through exact/same finite-owner retry PASS\n");
}
#endif // XIR_FLOW_LITERAL_TYPE_CASES_H
