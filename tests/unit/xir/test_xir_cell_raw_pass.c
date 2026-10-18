/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cell_raw_pass.c - Current raw physical walks and callback isolation
 */
#include "xir/xxir_verify.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)

typedef struct RawPassFixture {
    XrXirCompileContext context;
    XrXirTypes types;
    XrXirType parameter;
    uint32_t operand;
    XrXirInstruction operations[2];
    XrXirFunction function;
    XrXirModule module;
    XrXirCellProofBodyRead read;
    XrXirCellProofBodyHead head;
    CellProofBuild build;
} RawPassFixture;
static XrCompileResourceStats raw_pass_stats(const RawPassFixture *fixture) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(fixture->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void raw_pass_fixture(RawPassFixture *fixture) {
    memset(fixture,0,sizeof(*fixture));
    XrCompileResourceLimits caps={1048576,1048576,1048576};
    CHECK(xr_compile_resources_new(&caps,&fixture->context.resources)==XR_COMPILE_RESOURCE_OK);
    fixture->context.limits=xr_xir_compile_default_limits();fixture->parameter=XR_XIR_I64;
    fixture->operations[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,
        .args={17,23},.targets={31,47},.immediate=INT64_MIN,.type_arguments={53,59}};
    fixture->operations[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture->function=(XrXirFunction){.parameters=&fixture->parameter,.parameter_count=1,
        .result=XR_XIR_UNIT,.instructions=fixture->operations,.instruction_count=2,
        .operands=&fixture->operand,.operand_count=1};
    fixture->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=&fixture->function,
        .function_count=1,.types=&fixture->types};
    fixture->read=cell_proof_raw_body(&fixture->context,&fixture->module);
    fixture->build=(CellProofBuild){.context=&fixture->context,.body=&fixture->read,.status=XR_XIR_OK};
    CHECK(cell_proof_head(&fixture->build,0,&fixture->head));
}
static void raw_pass_close(RawPassFixture *fixture,uint64_t live) {
    CHECK(raw_pass_stats(fixture).live_bytes==live);
    xr_compile_resources_release(fixture->context.resources);fixture->context.resources=NULL;
}
static unsigned raw_pass_instruction_calls,raw_pass_shape_calls;
static bool raw_pass_refuse;
static XrXirStatus raw_pass_custom_instruction(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    ++raw_pass_instruction_calls;
    if (raw_pass_refuse) return XR_XIR_IO;
    return cell_proof_raw_instruction(context,owner,f,i,storage,output);
}
static XrXirStatus raw_pass_custom_shape(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t i,XrXirCellProofInstructionShape *output) {
    ++raw_pass_shape_calls;
    if (raw_pass_refuse) return XR_XIR_IO;
    return cell_proof_raw_shape(context,owner,f,i,output);
}
static XrXirStatus raw_pass_custom_head(const XrXirCompileContext *context,const void *owner,
    uint32_t f,XrXirCellProofBodyHead *output) {
    return cell_proof_raw_head(context,owner,f,output);
}
static XrXirStatus raw_pass_custom_type(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t v,XrXirType *output) {
    return cell_proof_raw_type(context,owner,f,v,output);
}
static XrXirStatus raw_pass_custom_span(const XrXirCompileContext *context,const void *owner,
    uint32_t f,uint32_t first,uint32_t count,XrXirCellProofSpanCursor *cursor) {
    (void)context;(void)owner;(void)f;(void)first;(void)count;(void)cursor;return XR_XIR_IO;
}
static XrXirStatus raw_pass_custom_range(const XrXirCompileContext *context,const XrXirCellProofBodyRead *read,
    XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    (void)context;(void)read;(void)cursor;(void)output;return XR_XIR_IO;
}
static void raw_pass_current_rows(void) {
    RawPassFixture fixture;raw_pass_fixture(&fixture);
    XrCompileResourceStats before=raw_pass_stats(&fixture);const XrXirInstruction *raw=NULL;
    CHECK(cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw) && raw==fixture.operations);
    CHECK(raw_pass_stats(&fixture).work-before.work==21+sizeof(raw));
    XrXirInstruction storage={0};before=raw_pass_stats(&fixture);
    CHECK(cell_proof_pass_instruction(&fixture.build,0,0,raw,&storage)==&fixture.operations[0]);
    CHECK(raw_pass_stats(&fixture).work-before.work==1+sizeof(raw));
    XrXirCellProofInstructionShape shape={0};before=raw_pass_stats(&fixture);
    CHECK(cell_proof_pass_shape(&fixture.build,0,0,raw,&shape) &&
        shape.op==XR_XIR_CONST_INT && shape.type==XR_XIR_I64);
    CHECK(raw_pass_stats(&fixture).work-before.work==3+sizeof(XrXirOp)+sizeof(shape));
    /* Rebinding after a completed walk observes a new current physical row. */
    fixture.operations[0].op=XR_XIR_CONST_BOOL;fixture.operations[0].type=XR_XIR_BOOL;
    CHECK(cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw));
    CHECK(cell_proof_pass_shape(&fixture.build,0,0,raw,&shape) &&
        shape.op==XR_XIR_CONST_BOOL && shape.type==XR_XIR_BOOL);
    CHECK(raw_pass_stats(&fixture).allocated_bytes==before.allocated_bytes);
    raw_pass_close(&fixture,before.live_bytes);
}
static void raw_pass_binding_rejections(void) {
    for (uint32_t mode=0;mode<15;++mode) {
        RawPassFixture fixture;raw_pass_fixture(&fixture);XrCompileResourceStats before=raw_pass_stats(&fixture);
        XrXirCellProofBodyHead head=fixture.head;XrXirTypes foreign_types={0};
        const XrXirInstruction *raw=fixture.operations;
        if (mode==0) head.parameter_count=0;
        else if (mode==1) head.parameters=NULL;
        else if (mode==2) head.result=XR_XIR_I64;
        else if (mode==3) head.instruction_count=1;
        else if (mode==4) head.operands=NULL;
        else if (mode==5) head.operand_count=0;
        else if (mode==6) fixture.read.domain.types=&foreign_types;
        else if (mode==7) fixture.read.domain.declarations=(const XrXirDeclarations *)&fixture;
        else if (mode==8) fixture.read.domain.generics=(const XrXirGeneric *)&fixture;
        else if (mode==9) fixture.read.domain.defaults=(const XrXirDefaultTable *)&fixture;
        else if (mode==10) fixture.read.domain.function_count=2;
        else if (mode==11) fixture.read.resources=NULL;
        else if (mode==12) fixture.module.functions=NULL;
        else if (mode==13) fixture.function.instructions=NULL;
        else fixture.read.owner=NULL;
        CHECK(!cell_proof_raw_pass(&fixture.build,0,&head,&raw) && !raw && fixture.build.status==XR_XIR_BAD_STRUCTURE);
        CHECK(raw_pass_stats(&fixture).allocated_bytes==before.allocated_bytes);
        raw_pass_close(&fixture,before.live_bytes);
    }
    for (uint32_t opcode=0;opcode<3;++opcode) {
        RawPassFixture fixture;raw_pass_fixture(&fixture);uint64_t live=raw_pass_stats(&fixture).live_bytes;
        const XrXirInstruction *raw=NULL;CHECK(cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw));
        fixture.operations[1].op=opcode==0?XR_XIR_INVALID:opcode==1?XR_XIR_OP_COUNT:(XrXirOp)UINT32_MAX;
        XrXirInstruction storage={0};CHECK(!cell_proof_pass_instruction(&fixture.build,0,1,raw,&storage));
        CHECK(fixture.build.status==XR_XIR_BAD_STRUCTURE && fixture.build.location.function==0 && fixture.build.location.instruction==1);
        fixture.build.status=XR_XIR_OK;XrXirCellProofInstructionShape sentinel={XR_XIR_RETURN,XR_XIR_F64},output=sentinel;
        CHECK(!cell_proof_pass_shape(&fixture.build,0,1,raw,&output) && !memcmp(&output,&sentinel,sizeof(output)));
        CHECK(fixture.build.status==XR_XIR_BAD_STRUCTURE && fixture.build.location.instruction==1);
        raw_pass_close(&fixture,live);
    }
}
static void raw_pass_custom_callbacks(void) {
    for (uint32_t mode=0;mode<6;++mode) {
        RawPassFixture fixture;raw_pass_fixture(&fixture);uint64_t live=raw_pass_stats(&fixture).live_bytes;
        if (mode==0) fixture.read.head=raw_pass_custom_head;
        else if (mode==1) fixture.read.instruction=raw_pass_custom_instruction;
        else if (mode==2) fixture.read.type=raw_pass_custom_type;
        else if (mode==3) fixture.read.shape=raw_pass_custom_shape;
        else if (mode==4) fixture.read.span=raw_pass_custom_span;
        else fixture.read.range=raw_pass_custom_range;
        const XrXirInstruction *raw=fixture.operations;
        CHECK(cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw) && !raw && fixture.build.status==XR_XIR_OK);
        raw_pass_close(&fixture,live);
    }
    for (uint32_t refuse=0;refuse<2;++refuse) {
        RawPassFixture fixture;raw_pass_fixture(&fixture);XrCompileResourceStats before=raw_pass_stats(&fixture);
        fixture.read.instruction=raw_pass_custom_instruction;fixture.read.shape=raw_pass_custom_shape;
        raw_pass_instruction_calls=raw_pass_shape_calls=0;raw_pass_refuse=refuse!=0;
        const XrXirInstruction *raw=fixture.operations;
        CHECK(cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw) && !raw);
        uint64_t start=raw_pass_stats(&fixture).work;
        XrXirInstruction storage={0};const XrXirInstruction *op=cell_proof_pass_instruction(&fixture.build,0,0,raw,&storage);
        CHECK(raw_pass_instruction_calls==1 && (refuse?(!op && fixture.build.status==XR_XIR_IO):(op==fixture.operations && fixture.build.status==XR_XIR_OK)));
        CHECK(raw_pass_stats(&fixture).work-start==1+sizeof(op)+(refuse?0u:3+sizeof(op)));
        fixture.build.status=XR_XIR_OK;
        XrXirCellProofInstructionShape sentinel={XR_XIR_RETURN,XR_XIR_F64},shape=sentinel;
        start=raw_pass_stats(&fixture).work;
        CHECK(cell_proof_pass_shape(&fixture.build,0,0,raw,&shape)==(refuse==0));
        CHECK(raw_pass_shape_calls==1 && (refuse?(fixture.build.status==XR_XIR_IO && !memcmp(&shape,&sentinel,sizeof(shape))):
            (shape.op==XR_XIR_CONST_INT && shape.type==XR_XIR_I64)));
        CHECK(raw_pass_stats(&fixture).work-start==1+sizeof(shape)+(refuse?0u:3+2*sizeof(shape)));
        CHECK(raw_pass_stats(&fixture).allocated_bytes==before.allocated_bytes);
        raw_pass_close(&fixture,before.live_bytes);
    }
    raw_pass_refuse=false;
}
static void raw_pass_budget_cuts(void) {
    for (uint32_t mode=0;mode<3;++mode) for (uint32_t exact=0;exact<2;++exact) {
        RawPassFixture fixture;raw_pass_fixture(&fixture);XrCompileResourceStats before=raw_pass_stats(&fixture);
        const XrXirInstruction *raw=fixture.operations;
        uint64_t fee=mode==0?21+sizeof(raw):mode==1?1+sizeof(raw):
            3+sizeof(XrXirOp)+sizeof(XrXirCellProofInstructionShape);
        uint64_t available=fee-(exact?0u:1u);
        CHECK(xr_compile_resources_work(fixture.context.resources,UINT64_C(1048576)-before.work-available)==XR_COMPILE_RESOURCE_OK);
        before=raw_pass_stats(&fixture);bool ok;
        XrXirInstruction storage={0};XrXirCellProofInstructionShape sentinel={XR_XIR_RETURN,XR_XIR_F64},shape=sentinel;
        if (mode==0) {ok=cell_proof_raw_pass(&fixture.build,0,&fixture.head,&raw);CHECK(exact?raw==fixture.operations:raw==NULL);}
        else if (mode==1) ok=cell_proof_pass_instruction(&fixture.build,0,0,raw,&storage)!=NULL;
        else {ok=cell_proof_pass_shape(&fixture.build,0,0,raw,&shape);CHECK(exact?shape.op==XR_XIR_CONST_INT:!memcmp(&shape,&sentinel,sizeof(shape)));}
        CHECK(ok==(exact!=0) && fixture.build.status==(exact?XR_XIR_OK:XR_XIR_BUDGET));
        XrCompileResourceStats after=raw_pass_stats(&fixture);
        CHECK(after.work-before.work==(exact?fee:1u) && after.allocated_bytes==before.allocated_bytes);
        raw_pass_close(&fixture,before.live_bytes);
    }
}
int main(void) {
    raw_pass_current_rows();raw_pass_binding_rejections();raw_pass_custom_callbacks();raw_pass_budget_cuts();
    puts("raw Cell passes: current physical rows,15 binding rejections,3 invalid opcodes,6 mixed adapters, exact callbacks/IO/old fees,6 work cuts and allocator0 PASS");
    return 0;
}
