/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_identity_ir_cases.h - Independent Channel operation operand rejection controls
 */
#ifndef XIR_CHANNEL_IDENTITY_IR_CASES_H
#define XIR_CHANNEL_IDENTITY_IR_CASES_H
#include "xir_construction_fixture.h"
static void channel_identity_ir_expect(LibraryCompileOwner *owner,unsigned axis,XrXirType element,XrXirStatus expected) {
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CHANNEL,.element=element};XrXirTypes types={&node,1,NULL,NULL};
    XrXirInstruction instructions[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CHANNEL_NEW,.type=(XrXirType)256,.args={0,0}},
        {.op=XR_XIR_CHANNEL_CAPACITY,.type=XR_XIR_I64,.args={1,0}},
        {.op=XR_XIR_CHANNEL_IS_CLOSED,.type=XR_XIR_BOOL,.args={1,0}},
        {.op=XR_XIR_CHANNEL_CLOSE,.type=XR_XIR_UNIT,.args={1,0}},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={2,0}}};
    switch (axis) {
    case 0: instructions[0].op=XR_XIR_CONST_BOOL;instructions[0].type=XR_XIR_BOOL;instructions[0].immediate=1;break;
    case 1: node.kind=XR_XIR_TYPE_ARRAY;break;
    case 2: instructions[2].args[0]=0;break;
    case 3: instructions[3].args[0]=0;break;
    case 4: instructions[4].args[0]=0;break;
    case 5: instructions[2].type=XR_XIR_BOOL;break;
    case 6: instructions[3].type=XR_XIR_I64;break;
    case 7: instructions[4].type=XR_XIR_I64;break;
    case 8: instructions[1].args[0]=UINT32_MAX;break;
    case 9: instructions[1].type=(XrXirType)257;break;
    case 10: instructions[1].type=XR_XIR_STRING;break;
    case 11: instructions[4].immediate=1;break;
    case 12: instructions[2].args[1]=1;break;
    default:break;
    }
    XrXirBlock block={.count=6};XrXirFunction function={.name="channelProof",.name_length=12,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=instructions,.instruction_count=6};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
    XrCompileResourceStats before=library_compile_stats(&owner->context);XrXirArtifact *artifact=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&owner->context,&module,&artifact,&diagnostic);
    if (status!=expected) fprintf(stderr,"Channel IR axis=%u expected=%u actual=%u instruction=%u\n",axis,expected,status,diagnostic.instruction);
    CHECK(status==expected && (status==XR_XIR_OK ? artifact!=NULL : artifact==NULL));xr_xir_compile_artifact_free(artifact);
    CHECK(library_compile_stats(&owner->context).live_bytes==before.live_bytes);
}
static void channel_identity_ir_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    channel_identity_ir_expect(&owner,UINT32_MAX,XR_XIR_I64,XR_XIR_OK);
    channel_identity_ir_expect(&owner,UINT32_MAX,XR_XIR_STRING,XR_XIR_OK);
    channel_identity_ir_expect(&owner,UINT32_MAX,XR_XIR_UNIT,XR_XIR_OK);
    for (unsigned axis=0;axis<13;++axis) channel_identity_ir_expect(&owner,axis,XR_XIR_I64,
        axis==8 ? XR_XIR_BAD_VALUE : axis>=11 ? XR_XIR_BAD_STRUCTURE : XR_XIR_BAD_TYPE);
    library_compile_owner_drop(&owner);puts("Channel identity XIR: three positive element types and thirteen exact rejection axes physical0");
}
#endif // XIR_CHANNEL_IDENTITY_IR_CASES_H
