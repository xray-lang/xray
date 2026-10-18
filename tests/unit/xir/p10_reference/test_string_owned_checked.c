/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_owned_checked.c - Exact owned String instruction admission
 */
#include "../xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
static void checked_case(unsigned selected) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U16}};
    XrXirTypes types={nodes,2,NULL,NULL};uint32_t operands[]={0,1,2};
    XrXirInstruction done={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction body[]={
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}},
        {XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},2,{0}},
        {XR_XIR_STRING_REPLACE,XR_XIR_STRING,{0,3},{0},0,{0}},
        {XR_XIR_STRING_COPY_BYTES,(XrXirType)256,{3},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0,{0}}};
    XrXirBlock blocks[]={{0,1,0,0},{0,7,0,0}};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&blocks[0],1,&done,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&blocks[1],1,body,7,operands,3}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirSourceModule source={"root",4,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirLiteral literals[]={{"aba",3},{"a",1},{"x",1}};
    XrXirDeclarations declarations={&source,1,identities,NULL,0,literals,3,0,1,NULL};
    XrXirModule module={XR_XIR_BUILT,functions,2,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    switch(selected) {
        case 1: body[3].immediate=1;break;
        case 2: body[3].immediate=2;break;
        case 3: body[3].immediate=-1;break;
        case 4: body[3].args[1]=2;break;
        case 5: body[3].targets[0]=1;break;
        case 6: body[3].type_arguments[0]=XR_XIR_I64;break;
        case 7: body[3].type=XR_XIR_BOOL;break;
        case 8: body[1]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}};break;
        case 9: body[4].type=(XrXirType)257;break;
        case 10: body[4].args[1]=1;break;
        case 11: body[4].immediate=1;break;
        case 12: body[4].targets[0]=1;break;
        case 13: body[4].type_arguments[0]=XR_XIR_U8;break;
        case 14: body[4].args[0]=5;break;
        default: break;
    }
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirArtifact *checked=NULL,*decoded=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner.context,&module,&checked,&diagnostic);
    if(selected<=1) {
        if(status!=XR_XIR_OK) fprintf(stderr,"mode=%u status=%u at %u:%u\n",selected,status,diagnostic.function,diagnostic.instruction);
        CHECK(status==XR_XIR_OK && checked);XrXirCheckedPacket packet={0};
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);checked=NULL;
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_artifact_verify(decoded,NULL)==XR_XIR_OK);
        const XrXirModule *received=xr_xir_compile_artifact_module(decoded);
        CHECK(received->functions[1].instructions[3].op==XR_XIR_STRING_REPLACE);
        CHECK(received->functions[1].instructions[3].immediate==selected);
        CHECK(received->functions[1].instructions[4].op==XR_XIR_STRING_COPY_BYTES);
        xr_xir_compile_artifact_free(decoded);xr_xir_compile_checked_packet_free(&packet);
    } else {
        XrXirStatus expected=selected==7 || selected==8 || selected==9 || selected==14?
            XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
        if(status!=expected) fprintf(stderr,"shape=%u status=%u expected=%u\n",selected,status,expected);
        CHECK(status==expected && !checked);
        printf("owned instruction shape=%u rejected status=%u\n",selected,status);
    }
    source_fixture_owner_free(&owner);
}
int main(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==91,"Owned String semantic identity");
    _Static_assert(XR_XIR_STRING_COPY_BYTES==201 && XR_XIR_STRING_REPLACE==202,"Append-only owned String opcodes");
    for(unsigned selected=0;selected<15;++selected) checked_case(selected);
    puts("Owned String first/all Checked roundtrip and exact operand/type/mode admission PASS");return 0;
}
