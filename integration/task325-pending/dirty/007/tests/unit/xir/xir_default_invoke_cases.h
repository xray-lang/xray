/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_invoke_cases.h - Default authority across explicit error edges
 *
 * KEY CONCEPT:
 *   Owner metadata is not a value operand; only the proper successor owns results.
 */
#ifndef XIR_DEFAULT_INVOKE_CASES_H
#define XIR_DEFAULT_INVOKE_CASES_H
#include "xir_construction_fixture.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_effects.h"
static void default_invoke_case(unsigned mode,bool generic) {
    XrXirInstruction ret={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction entry[]={
        {XR_XIR_INVOKE_DEFAULT,XR_XIR_I64,{2,0},{1,2},0,{0}},
        {XR_XIR_INVOKE_RESULT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},0,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{4},{0},0,{0}}};
    XrXirInstruction helper[]={
        {XR_XIR_ENUM_NEW,XR_XIR_CONSTRUCTED_TYPE_BASE,{0},{0},0,{0}},
        {XR_XIR_THROW,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock one={0,1,0,0},two={0,2,0,0},blocks[]={{0,1,0,0},{1,2,0,0},{3,3,0,0}};
    XrXirType i64=XR_XIR_I64;
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&ret,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,blocks,3,entry,6,NULL,0},
        {"owner",5,&i64,1,XR_XIR_I64,&one,1,&ret,1,NULL,0},
        {"default",7,NULL,0,XR_XIR_I64,&two,1,helper,2,NULL,0}};
    XrXirSourceModule source={"root",4,NULL,0,0};
    XrXirFunctionIdentity ids[4]={{0},{0},{0},{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=ids,.entry_function=1};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,2,0,3};
    XrXirDefaultTable table={&binding,1};
    XrXirNominalVariant variant={{"Failed",6},0,0};
    XrXirNominalDeclaration nominal={.module={"root",4},.name={"Failure",7},.kind=XR_XIR_NOMINAL_ENUM,.variants=&variant,.variant_count=1};
    XrXirNominalTable nt={&nominal,1,NULL};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL};
    XrXirTypes types={&node,1,&nt,NULL};
    XrXirType args[]={XR_XIR_I64,XR_XIR_STRING,XR_XIR_BOOL,XR_XIR_F64};
    XrXirConstraint constraints[4]={{0},{0},{0},{0}};
    XrXirGeneric generics[4]={{0},{NULL,0,args,4, NULL},{constraints,4,NULL,0, NULL},{constraints,4,NULL,0, NULL}};
    if(generic)entry[0].type_arguments[1]=4;
    XrXirModule module={XR_XIR_BUILT,functions,4,&declarations,generic?generics:NULL,&types,NULL,XR_XIR_PROGRAM,&table};
    XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
    switch(mode){
    case 0:expected=XR_XIR_OK;break;
    case 1:entry[0].targets[1]=1;break;
    case 2:entry[0].targets[0]=0;break;
    case 3:entry[0].targets[0]=3;break;
    case 4:entry[1].immediate=1;break;
    case 5:entry[3].op=XR_XIR_INVOKE_RESULT;break;
    case 6:entry[3].type=XR_XIR_I64;break;
    case 7:entry[0].immediate=3;break;
    case 8:entry[0].args[0]=3;break;
    case 9:entry[0].args[1]=1;break;
    case 10:entry[0].type_arguments[1]=2;break;
    case 11:entry[2]=(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{2,0},0,{0}};break;
    case 12:entry[1].type=XR_XIR_BOOL;break;
    case 13:module.stage=XR_XIR_LOWERED;expected=XR_XIR_BAD_STAGE;break;
    case 14:entry[0].targets[0]=2;entry[0].targets[1]=1;break;
    case 15:entry[5]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{3},{0},0,{0}};expected=XR_XIR_OK;break;
    }
    XrXirArtifact *checked=NULL;XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_check(suite_context, &module, &checked, &d);
    if(status!=expected)fprintf(stderr,"invoke mode%u generic%u status%u expected%u f%u i%u\n",mode,generic,status,expected,d.function,d.instruction);
    CHECK(status==expected && ((checked!=NULL)==(expected==XR_XIR_OK)));
    if(!checked)return;
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked, &effects)==XR_XIR_OK);
    CHECK(xr_xir_effects_error(effects,3,XR_XIR_CONSTRUCTED_TYPE_BASE,0));
    CHECK(xr_xir_effects_error(effects,1,XR_XIR_CONSTRUCTED_TYPE_BASE,0)==(mode==15));
    CHECK(!xr_xir_effects_error_unknown(effects,1));xr_xir_compile_effects_free(effects);
    XrXirArtifact *special=NULL,*read=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked, &special, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    XrXirInstruction *call=(XrXirInstruction *)special->module.functions[1].instructions;
    CHECK(call->op==XR_XIR_INVOKE && !call->args[0] && !call->args[1] && call->targets[0]==1 && call->targets[1]==2);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(special, &packet, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(suite_context, packet.bytes, packet.length, &read, NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(read);read=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(special, &target, &lowered, NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);lowered=NULL;
    XrXirInstruction saved=*call;
    call->args[0]=2;CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_STRUCTURE);*call=saved;
    call->op=XR_XIR_CALL;CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_STRUCTURE);*call=saved;
    call->targets[0]=2;call->targets[1]=1;CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_STRUCTURE);*call=saved;
    call->immediate=0;CHECK(xr_xir_compile_artifact_verify(special, NULL)!=XR_XIR_OK);*call=saved;
    if(generic){XrXirType *tuple=(XrXirType *)special->module.provenance->origins[call->immediate].arguments;
        XrXirType old=tuple[0];tuple[0]=XR_XIR_STRING;
        CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_BAD_TYPE);tuple[0]=old;}
    CHECK(xr_xir_compile_artifact_verify(special, NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(special);special=NULL;
}
static void default_invoke_cases(void){
    for(unsigned mode=0;mode<16;++mode){default_invoke_case(mode,false);default_invoke_case(mode,true);}
    puts("default invoke: 32 CFG/authority cases; caught/rethrow error sets and provenance PASS");
}
#endif // XIR_DEFAULT_INVOKE_CASES_H
