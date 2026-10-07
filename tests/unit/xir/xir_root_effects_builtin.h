/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_effects_builtin.h - Independent default and no-suspend authority graphs
 */
#ifndef XIR_ROOT_EFFECTS_BUILTIN_H
#define XIR_ROOT_EFFECTS_BUILTIN_H
static XrXirArtifact *root_builtin(unsigned mode) {
    XrXirInstruction init[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{0},{0},0,{0}},{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction pure[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction rooted[]={{XR_XIR_SLOT_LOAD,XR_XIR_I64,{0},{0},0,{0}},pure[1]};
    XrXirInstruction owner={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction caller[]={{XR_XIR_CALL_DEFAULT,XR_XIR_I64,{0},{4,0},0,{0}},pure[1]};
    XrXirInstruction dynamic[]={{XR_XIR_CALL_INDIRECT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction reference[]={{XR_XIR_FUNCTION_REF,XR_XIR_CONSTRUCTED_TYPE_BASE,{0},{0},8,{0}},pure[1]};
    XrXirBlock one={0,1,0,0},two={0,2,0,0},three={0,3,0,0};
    XrXirType i64=XR_XIR_I64,callable=XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&three,1,init,3,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"rooted",6,NULL,0,XR_XIR_I64,&two,1,rooted,2,NULL,0},
        {"pure",4,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"owner",5,&i64,1,XR_XIR_I64,&one,1,&owner,1,NULL,0},
        {"caller",6,NULL,0,XR_XIR_I64,&two,1,caller,2,NULL,0},
        {"dynamic",7,&callable,1,XR_XIR_I64,&two,1,dynamic,2,NULL,0},
        {"reference",9,NULL,0,callable,&two,1,reference,2,NULL,0},
        {"refTarget",9,NULL,0,XR_XIR_I64,&two,1,rooted,2,NULL,0}};
    XrXirFunctionIdentity identities[9]={{0}};
    identities[8].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirSourceModule source={"root",4,NULL,0,0};XrXirSlot slot={0,XR_XIR_I64,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_NO_SUSPEND};
    XrXirTypes types={&node,1,NULL,NULL};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,4,0,mode==1?3u:2u};
    XrXirDefaultTable defaults={&binding,1};
    XrXirModule built={XR_XIR_BUILT,functions,9,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,&defaults};
    if(mode==2)dynamic[0].op=XR_XIR_OP_COUNT;
    if(mode==3)caller[0].immediate=7;
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check(&effect_context,&built,&checked,&diagnostic);
    if(mode>=2){CHECK(status==XR_XIR_BAD_STRUCTURE&&!checked);return NULL;}
    if(status!=XR_XIR_OK)fprintf(stderr,"builtin root fixture mode%u status%u f%u b%u i%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);return checked;
}
static XrXirArtifact *root_group(void) {
    XrXirInstruction ret={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction init[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},8,{0}},
        {XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,{0,2},{0},(INT64_C(1)<<32)|2,{0}},ret};
    XrXirInstruction other[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},4,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{0},{0},0,{0}},ret};
    XrXirInstruction entry[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},ret};
    uint32_t payload[]={0,1},dependency=1;
    XrXirBlock two={0,2,0,0},three={0,3,0,0},four={0,4,0,0};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&four,1,init,4,payload,2},
        {"entry",5,NULL,0,XR_XIR_I64,&two,1,entry,2,NULL,0},
        {"other",5,NULL,0,XR_XIR_UNIT,&three,1,other,3,NULL,0}};
    XrXirSourceModule modules[]={{"root",4,&dependency,1,0},{"other",5,NULL,0,2}};
    XrXirFunctionIdentity ids[3]={{0},{0},{0}};ids[2].module=1;
    XrXirSlot slots[]={{1,XR_XIR_I64,0},{0,XR_XIR_I64,0},{0,XR_XIR_I64,0}};
    XrXirDeclarations d={.modules=modules,.module_count=2,.functions=ids,.slots=slots,.slot_count=3,.entry_function=1};
    XrXirModule built={XR_XIR_BUILT,functions,3,&d,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check(&effect_context,&built,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"group root fixture status%u f%u b%u i%u\n",
        status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);return checked;
}
#endif // XIR_ROOT_EFFECTS_BUILTIN_H
