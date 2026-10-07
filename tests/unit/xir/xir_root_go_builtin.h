/* Independent const-value versus const-place worker qualification. */
#ifndef XIR_ROOT_GO_BUILTIN_H
#define XIR_ROOT_GO_BUILTIN_H
static XrXirStatus go_shape_check(const XrXirCompileContext *context, unsigned mode,
    bool spawn, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_NO_SUSPEND}};
    XrXirTypes types={nodes,3,NULL,NULL};
    XrXirInstruction init[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_ARRAY_NEW,(XrXirType)257,{0,1},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction entry[]={{XR_XIR_GO,(XrXirType)256,{0},{0},2,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    if(!spawn)entry[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}};
    XrXirInstruction worker[]={{mode==1?XR_XIR_SLOT_PLACE:XR_XIR_SLOT_LOAD,(XrXirType)257,{0},{0},0,{0}},
        {XR_XIR_ARRAY_LEN,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}},{0}};
    XrXirInstruction leaf[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},5,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    uint32_t worker_count=3;
    if(mode>=3){
        uint32_t first=mode==4?1u:0u;
        worker[first]=(XrXirInstruction){XR_XIR_FUNCTION_REF,(XrXirType)258,{0},{0},3,{0}};
        worker[first+1]=(XrXirInstruction){XR_XIR_CALL_INDIRECT,XR_XIR_I64,{0},{0},first,{0}};
        worker[first+2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{first+1},{0},0,{0}};
        worker_count=first+3;
    }
    uint32_t operand=0;XrXirBlock four={0,4,0,0},three={0,3,0,0},two={0,2,0,0},body={0,worker_count,0,0};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&four,1,init,4,&operand,1},
        {"entry",5,NULL,0,XR_XIR_I64,&three,1,entry,3,NULL,0},
        {"worker",6,NULL,0,XR_XIR_I64,&body,1,worker,worker_count,NULL,0},
        {"leaf",4,NULL,0,XR_XIR_I64,&two,1,leaf,2,NULL,0}};
    XrXirFunctionIdentity identities[4]={{0}};identities[3].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirSourceModule source={"root",4,NULL,0,0};XrXirSlot slot={0,(XrXirType)257,mode==2||mode==4};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule built={XR_XIR_BUILT,functions,4,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xr_xir_compile_check(context,&built,output,diagnostic);
}
#endif
