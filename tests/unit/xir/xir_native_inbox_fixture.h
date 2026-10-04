/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_inbox_fixture.h - Verified graphs for generated inbox boundaries
 */
#ifndef XIR_NATIVE_INBOX_FIXTURE_H
#define XIR_NATIVE_INBOX_FIXTURE_H
#include "xir_call_fixture.h"
static XrXirArtifact *native_inbox_fixture(const XrXirCompileContext *context, uint32_t mode) {
    CHECK(mode < 6);
    const XrXirType string_parameter = XR_XIR_STRING;
    const uint32_t operands[] = {0};
    XrXirInstruction root[] = {
        {XR_XIR_CALL,XR_XIR_STRING,{0,1},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},91,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}}};
    XrXirInstruction child[] = {
        {XR_XIR_COPY,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock root_blocks[] = {{0,3,0,0},{3,3,0,0}};
    const XrXirBlock child_block={0,2,0,0}, one={0,1,0,0}, two={0,2,0,0};
    const XrXirInstruction done={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction main_ops[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction cleanup[]={{XR_XIR_OUTPUT,XR_XIR_UNIT,{0},{0},1,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirFunction functions[]={
        {"root",4,&string_parameter,1,XR_XIR_I64,root_blocks,1,root,3,operands,1},
        {"child",5,&string_parameter,1,XR_XIR_STRING,&child_block,1,child,2,NULL,0},
        {"cleanup",7,&string_parameter,1,XR_XIR_UNIT,&two,1,cleanup,2,NULL,0},
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&done,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&two,1,main_ops,2,NULL,0}};
    XrXirInstruction discarded[] = {
        {XR_XIR_INVOKE,XR_XIR_STRING,{0,1},{1,2},1,{0}},
        {XR_XIR_INVOKE_DISCARD,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},91,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{6},{0},0,{0}}};
    const XrXirBlock discarded_blocks[]={{0,1,0,0},{1,3,0,0},{4,3,0,0}};
    if (!mode) {
        functions[0].instructions=discarded; functions[0].instruction_count=7;
        functions[0].blocks=discarded_blocks; functions[0].block_count=3;
    }
    if (mode>=2) {
        root[0].type=XR_XIR_UNIT;
        functions[1].result=XR_XIR_UNIT; functions[1].instructions=&done;
        functions[1].instruction_count=1; functions[1].blocks=&one;
    }
    const XrXirInstruction failing_child[]={
        {XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_DIV_INT,XR_XIR_I64,{2,3},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock failing_block={0,5,0,0};
    if (mode>=3) {
        functions[1].instructions=failing_child; functions[1].instruction_count=5;
        functions[1].blocks=&failing_block;
    }
    const uint32_t protected_operands[]={0,0};
    const XrXirInstruction protected_root[]={
        {XR_XIR_CLEANUP_REGISTER,XR_XIR_UNIT,{0,1},{1},2,{0}},
        {XR_XIR_CALL,XR_XIR_UNIT,{1,1},{0},1,{0}},
        {XR_XIR_CLEANUP_LEAVE,XR_XIR_UNIT,{0},{2},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}},
        {XR_XIR_PANIC_CATCH,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},91,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{7},{0},0,{0}}};
    const XrXirBlock protected_blocks[]={{0,1,0,0},{1,2,3,1},{3,2,0,0},{5,3,0,0}};
    XrXirFunctionIdentity identities[5]={{0}};
    if (mode==4) {
        functions[0].instructions=protected_root; functions[0].instruction_count=8;
        functions[0].blocks=protected_blocks; functions[0].block_count=4;
        functions[0].operands=protected_operands; functions[0].operand_count=2;
        identities[2].cleanup_owner=1;
    }
    /* No cleanup: both the protected CALL and handler have frontier zero. */
    const XrXirInstruction same_frontier_root[]={
        {XR_XIR_JUMP,XR_XIR_UNIT,{0},{1},0,{0}},
        {XR_XIR_CALL,XR_XIR_UNIT,{0,1},{0},1,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}},
        {XR_XIR_PANIC_CATCH,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},91,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{6},{0},0,{0}}};
    const XrXirBlock same_frontier_blocks[]={{0,1,0,0},{1,3,2,0},{4,3,0,0}};
    if (mode==5) {
        functions[0].instructions=same_frontier_root; functions[0].instruction_count=7;
        functions[0].blocks=same_frontier_blocks; functions[0].block_count=3;
    }
    const XrXirSourceModule source={"alpha",5,NULL,0,3};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,4,NULL};
    ErrorFixture error; error_fixture_init(&error,false);
    const XrXirModule built={XR_XIR_BUILT,functions,5,&declarations,NULL,&error.types,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check(context,&built,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"inbox graph mode=%u status=%u function=%u block=%u instruction=%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_context(lowered)->resources==context->resources);
    return lowered;
}
#endif // XIR_NATIVE_INBOX_FIXTURE_H
