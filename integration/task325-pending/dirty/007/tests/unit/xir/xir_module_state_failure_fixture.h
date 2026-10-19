/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_failure_fixture.h - Checked initialization failures and owned sticky outcomes
 *
 * KEY CONCEPT:
 *   Whole Programs preserve failure identity and release real owners.
 */

#ifndef XIR_MODULE_STATE_FAILURE_FIXTURE_H
#define XIR_MODULE_STATE_FAILURE_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
static XrXirArtifact *state_failure_fixture(const XrXirCompileContext *context,uint32_t scenario) {
    CHECK(scenario>=2 && scenario<=4);
    XrXirInstruction init[6]={
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    uint32_t count=5;
    if(scenario==2) {
        init[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},40,{0}};
        init[1]=init[4]; count=2;
    } else if(scenario==4) {
        init[2]=(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}};
        init[3]=(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}};
        init[4]=(XrXirInstruction){XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,{2,3},{0},0,{0}};
        count=6;
    }
    XrXirInstruction empty={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction entry[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction text_entry[]={
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_PRINT,XR_XIR_UNIT,{0,1},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    const uint32_t printed=0;
    uint32_t entry_count=scenario==2?2u:4u;
    XrXirBlock blocks[]={{0,count,0,0},{0,1,0,0},{0,entry_count,0,0}};
    XrXirFunction functions[]={
        {"base_init",9,NULL,0,XR_XIR_UNIT,&blocks[0],1,init,count,NULL,0},
        {"left_init",9,NULL,0,XR_XIR_UNIT,&blocks[1],1,&empty,1,NULL,0},
        {"right_init",10,NULL,0,XR_XIR_UNIT,&blocks[1],1,&empty,1,NULL,0},
        {"root_init",9,NULL,0,XR_XIR_UNIT,&blocks[1],1,&empty,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&blocks[2],1,scenario==2?entry:text_entry,entry_count,scenario==2?NULL:&printed,scenario==2?0u:1u}};
    const uint32_t base=0,diamond[]={1,2};
    const XrXirSourceModule modules[]={
        {"base",4,NULL,0,0},{"left",4,&base,1,1},
        {"right",5,&base,1,2},{"root",4,diamond,2,3}};
    const XrXirFunctionIdentity identities[]={
        {.module=0},{.module=1},{.module=2},{.module=3},{.module=3,.exported=true}};
    const XrXirSlot slot={0,scenario==2?XR_XIR_I64:XR_XIR_STRING,false};
    const XrXirLiteral literals[]={{"module-owned-text",17},{"module initializer assertion",28}};
    const XrXirDeclarations declarations={modules,4,identities,&slot,1,literals,2,3,4,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,5,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context, &module, &checked, &diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"state=%u check=%u function=%u block=%u instruction=%u\n",
        scenario,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    XrXirCheckedPacket packet={0}; CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_context(lowered)->resources==context->resources);
    return lowered;
}
#endif
