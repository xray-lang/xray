/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_loop_fixture.h - Owned finite CFG with parallel PHI backedge copies
 */
#ifndef XIR_CTFE_LOOP_FIXTURE_H
#define XIR_CTFE_LOOP_FIXTURE_H
#include "xir_construction_fixture.h"
static XrXirStatus ctfe_phi_build(const XrXirCompileContext *context,
    CtfeFixture *output, CtfeChainTrace *trace) {
    if (!context || !context->resources || !output || output->context.resources || output->lowered)
        return XR_XIR_BAD_STRUCTURE;
    CtfeFixture result={0};result.context=*context;
    CHECK(xr_compile_resources_stats(context->resources,&result.owner_baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirInstruction ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},2,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_JUMP,XR_XIR_UNIT,{0},{1},0,{0}},
        {XR_XIR_PHI,XR_XIR_I64,{0,4},{0},0,{0}},
        {XR_XIR_PHI,XR_XIR_I64,{4,4},{0},0,{0}},
        {XR_XIR_PHI,XR_XIR_I64,{8,4},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},3,{0}},
        {XR_XIR_LT_INT,XR_XIR_BOOL,{6,7},{0},0,{0}},
        {XR_XIR_BRANCH,XR_XIR_UNIT,{8},{2,3},0,{0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{6,0},{0},0,{0}},
        {XR_XIR_JUMP,XR_XIR_UNIT,{0},{1},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},10,{0}},
        {XR_XIR_MUL_INT,XR_XIR_I64,{4,12},{0},0,{0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{13,5},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{14},{0},0,{0}}};
    XrXirBlock blocks[]={{0,4,0,0},{4,6,0,0},{10,2,0,0},{12,4,0,0}};
    uint32_t operands[]={0,0,2,5,0,1,2,4,0,2,2,10};
    XrXirFunction fn={"swap",4,NULL,0,XR_XIR_I64,blocks,4,ops,16,operands,12};
    char module_name[]="ctfe-phi";
    XrXirSourceModule source={module_name,8,NULL,0,1};
    XrXirFunctionIdentity identities[]={{.module=0,.exported=1},{.module=0}};
    XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirBlock init_block={0,1,0,0};
    XrXirFunction functions[]={fn,{"init",4,NULL,0,XR_XIR_UNIT,&init_block,1,&init,1,NULL,0}};
    XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,0,NULL, NULL};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .declarations=ctfe_resource_no_declarations?NULL:&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*copy=NULL,*closed=NULL;
    XrXirCheckedPacket packet={0};XrXirStatus status;
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    ctfe_chain_mark(trace,CTFE_CHAIN_SESSION);
    ctfe_chain_mark(trace,CTFE_CHAIN_SOURCE);
    status=xir_fixture_check(context,&built,&checked,NULL);
    if (status!=XR_XIR_OK) { CHECK(!checked);goto done; }
    for (uint32_t pass=0;pass<2;++pass) {
        ctfe_chain_mark(trace,pass?CTFE_CHAIN_WRITE2:CTFE_CHAIN_WRITE1);
        status=xr_xir_compile_checked_write(checked,&packet,NULL);
        if (status!=XR_XIR_OK) { CHECK(!packet.bytes && !packet.length);goto done; }
        xr_xir_compile_artifact_free(checked);checked=NULL;
        if (!pass) {
            /* Destroy every raw producer buffer after the first owned packet is written. */
            memset(ops,0xa5,sizeof(ops));memset(blocks,0xa5,sizeof(blocks));memset(operands,0xa5,sizeof(operands));
            memset(module_name,0xa5,sizeof(module_name));source=(XrXirSourceModule){0};memset(identities,0xa5,sizeof(identities));
            declarations=(XrXirDeclarations){0};fn=(XrXirFunction){0};memset(functions,0xa5,sizeof(functions));init=(XrXirInstruction){0};init_block=(XrXirBlock){0};built=(XrXirModule){0};
        }
        ctfe_chain_mark(trace,pass?CTFE_CHAIN_READ2:CTFE_CHAIN_READ1);
        status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&copy,NULL);
        memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
        if (status!=XR_XIR_OK) { CHECK(!copy);goto done; }
        ctfe_chain_mark(trace,pass?CTFE_CHAIN_VERIFY2:CTFE_CHAIN_VERIFY1);
        status=xr_xir_compile_artifact_verify(copy,NULL);
        if (status!=XR_XIR_OK) goto done;
        checked=copy;copy=NULL;
    }
    ctfe_chain_mark(trace,CTFE_CHAIN_SPECIALIZE);
    status=xr_xir_compile_specialize(checked,&closed,NULL);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if (status!=XR_XIR_OK) { CHECK(!closed);goto done; }
    ctfe_chain_mark(trace,CTFE_CHAIN_CLOSED_VERIFY);
    status=xr_xir_compile_artifact_verify(closed,NULL);
    if (status!=XR_XIR_OK) goto done;
    ctfe_chain_mark(trace,CTFE_CHAIN_CLOSED_WRITE);
    status=xr_xir_compile_checked_write(closed,&packet,NULL);
    if (status!=XR_XIR_OK) { CHECK(!packet.bytes && !packet.length);goto done; }
    ctfe_chain_mark(trace,CTFE_CHAIN_HASH);
    if (xr_compile_resources_work(context->resources,packet.length)!=XR_COMPILE_RESOURCE_OK) { status=XR_XIR_BUDGET;goto done; }
    xr_sha256(packet.bytes,packet.length,result.identity);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    ctfe_chain_mark(trace,CTFE_CHAIN_LOWER);
    status=xr_xir_compile_lower(closed,&target,&result.lowered,NULL);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    if (status!=XR_XIR_OK) { CHECK(!result.lowered);goto done; }
    ctfe_chain_mark(trace,CTFE_CHAIN_LOWER_VERIFY);
    status=xr_xir_compile_artifact_verify(result.lowered,NULL);
    if (status==XR_XIR_OK) { *output=result;result.lowered=NULL; }
done:
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"PHI producer status%u phase=%s\n",(unsigned)status,source_fixture_compile_phase);
    ctfe_chain_mark(trace,CTFE_CHAIN_CLEANUP);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(copy);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(result.lowered);return status;
}
#endif // XIR_CTFE_LOOP_FIXTURE_H
