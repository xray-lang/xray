#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_root_fixture.h - Literal upper bounds with authentic target bodies
 */
#ifndef XIR_CALLABLE_ROOT_FIXTURE_H
#define XIR_CALLABLE_ROOT_FIXTURE_H
/* The caller owns the ledger. All local descriptor storage is copied by the
 * ordinary Built-to-Checked path, never retained as fixture stack pointers. */
static XrXirStatus bound_fixture(const XrXirCompileContext *context, uint32_t advertised,
    uint32_t referenced, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirInstruction unit={.op=XR_XIR_RETURN};
    const XrXirType cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+5);
    XrXirInstruction init[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CELL_NEW,.type=cell_type,.args={0,0}},
        {.op=XR_XIR_SLOT_INIT,.args={1,0}},unit};
    XrXirInstruction pure[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=3}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction root[]={
        {.op=XR_XIR_SLOT_LOAD,.type=cell_type},
        {.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0,0}}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction reference[]={
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+advertised),.immediate=referenced},unit};
    XrXirInstruction root_reference[]={
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),.immediate=2},unit};
    XrXirInstruction indirect[]={
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction relay[]={
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=9}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction erased[]={
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.immediate=3},
        {.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),.args={0,0}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1}, {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock two={.count=2},three={.count=3},four={.count=4};
    XrXirType parameter[]={
        XR_XIR_CONSTRUCTED_TYPE_BASE,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+4)};
    uint32_t operand=0;
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_UNIT,.blocks=&four,.block_count=1,.instructions=init,.instruction_count=4,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="main",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=pure,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="root",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&three,.block_count=1,.instructions=root,.instruction_count=3,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="pure",.name_length=4,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=pure,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="reference",.name_length=9,.parameters=NULL,.parameter_count=0,.result=XR_XIR_UNIT,.blocks=&two,.block_count=1,.instructions=reference,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="rootReference",.name_length=13,.parameters=NULL,.parameter_count=0,.result=XR_XIR_UNIT,.blocks=&two,.block_count=1,.instructions=root_reference,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="indirectNone",.name_length=12,.parameters=&parameter[0],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=indirect,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="indirectRoot",.name_length=12,.parameters=&parameter[1],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=indirect,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="indirectUnknown",.name_length=15,.parameters=&parameter[2],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=indirect,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="indirectMixed",.name_length=13,.parameters=&parameter[3],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=indirect,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="relay",.name_length=5,.parameters=&parameter[3],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=relay,.instruction_count=2,.operands=&operand,.operand_count=1,.error_type=XR_XIR_NEVER},
        {.name="erased",.name_length=6,.parameters=NULL,.parameter_count=0,.result=XR_XIR_I64,.blocks=&four,.block_count=1,.instructions=erased,.instruction_count=4,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER},
        {.name="promisedUnknown",.name_length=15,.parameters=&parameter[4],.parameter_count=1,.result=XR_XIR_I64,.blocks=&two,.block_count=1,.instructions=indirect,.instruction_count=2,.operands=NULL,.operand_count=0,.error_type=XR_XIR_NEVER}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=4},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=8},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=12},
        {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=9},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}};
    XrXirTypes types={nodes,6,NULL,NULL};
    XrXirFunctionIdentity identities[13]={{0}};
    identities[12].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirSourceModule source={"root",4,NULL,0,0, 0,XR_XIR_MODULE_FRESH_STORAGE};
    XrXirSlot slot={0,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+5),1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule built={XR_XIR_BUILT,functions,13,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &built, output, diagnostic);
}
#endif // XIR_CALLABLE_ROOT_FIXTURE_H
