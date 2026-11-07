#include "xir_construction_fixture.h"
/* Independent const-value versus const-place worker qualification. */
#ifndef XIR_ROOT_GO_BUILTIN_H
#define XIR_ROOT_GO_BUILTIN_H
static XrXirStatus go_shape_check(const XrXirCompileContext *context, unsigned mode,
    bool spawn, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_NO_SUSPEND|XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)257}};
    XrXirTypes types={nodes,4,NULL,NULL};
    bool mutable=mode==2||mode==4;
    XrXirInstruction init[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_ARRAY_NEW,(XrXirType)257,{0,1},{0},0,{0}},
        {XR_XIR_SLOT_INIT,XR_XIR_UNIT,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},{0}};
    uint32_t init_count=4;
    if(mutable){
        init[2]=(XrXirInstruction){XR_XIR_CELL_NEW,(XrXirType)259,{1},{0},0,{0}};
        init[3]=(XrXirInstruction){XR_XIR_SLOT_INIT,XR_XIR_UNIT,{2},{0},0,{0}};
        init[4]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
        init_count=5;
    }
    XrXirInstruction entry[]={{XR_XIR_GO,(XrXirType)256,{0},{0},2,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    if(!spawn)entry[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}};
    XrXirInstruction worker[]={{mode==1?XR_XIR_SLOT_PLACE:XR_XIR_SLOT_LOAD,(XrXirType)257,{0},{0},0,{0}},
        {XR_XIR_ARRAY_LEN,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}},{0},{0},{0}};
    XrXirInstruction leaf[]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},5,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    uint32_t worker_count=3;
    if(mutable){
        worker[0]=(XrXirInstruction){XR_XIR_SLOT_LOAD,(XrXirType)259,{0},{0},0,{0}};
        worker[1]=(XrXirInstruction){XR_XIR_CELL_READ,(XrXirType)257,{0},{0},0,{0}};
        worker[2]=(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{1},{0},0,{0}};
        worker[3]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}};
        worker_count=4;
    }
    if(mode>=3){
        uint32_t first=mode==4?2u:0u;
        worker[first]=(XrXirInstruction){XR_XIR_FUNCTION_REF,(XrXirType)258,{0},{0},3,{0}};
        /* A fixed callable bound retains UNKNOWN even for a pure target. */
        worker[first+1]=(XrXirInstruction){XR_XIR_FUNCTION_WEAKEN,(XrXirType)258,{first},{0},0,{0}};
        worker[first+2]=(XrXirInstruction){XR_XIR_CALL_INDIRECT,XR_XIR_I64,{0},{0},first+1,{0}};
        worker[first+3]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{first+2},{0},0,{0}};
        worker_count=first+4;
    }
    uint32_t operand=0;XrXirBlock initial={0,init_count,0,0},three={0,3,0,0},two={0,2,0,0},body={0,worker_count,0,0};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_UNIT,.blocks=&initial,.block_count=1,.instructions=init,.instruction_count=init_count,.operands=&operand,.operand_count=1,.error_type=XR_XIR_NEVER},
        {.name="entry",.name_length=5,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&three,.block_count=1,.instructions=entry,.instruction_count=3,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="worker",.name_length=6,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&body,.block_count=1,.instructions=worker,.instruction_count=worker_count,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="leaf",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=leaf,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER}};
    XrXirFunctionIdentity identities[4]={{0}};identities[3].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirSourceModule source={"root",4,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE};XrXirSlot slot={0,mutable?(XrXirType)259:(XrXirType)257,mutable};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule built={XR_XIR_BUILT,functions,4,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &built, output, diagnostic);
}
#endif
