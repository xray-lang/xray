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
    XrXirInstruction init[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_SLOT_INIT,.args={0,0}},unit};
    XrXirInstruction pure[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=3}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction root[]={
        {.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={0,0}}};
    XrXirInstruction reference[]={
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+advertised),.immediate=referenced},unit};
    XrXirInstruction root_reference[]={
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),.immediate=2},unit};
    XrXirInstruction indirect[]={
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction relay[]={
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=9}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirInstruction erased[]={
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),.immediate=3},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0}, {.op=XR_XIR_RETURN,.args={1,0}}};
    XrXirBlock two={.count=2},three={.count=3};
    XrXirType parameter[]={
        XR_XIR_CONSTRUCTED_TYPE_BASE,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+4)};
    uint32_t operand=0;
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&three,1,init,3,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"root",4,NULL,0,XR_XIR_I64,&two,1,root,2,NULL,0},
        {"pure",4,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"reference",9,NULL,0,XR_XIR_UNIT,&two,1,reference,2,NULL,0},
        {"rootReference",13,NULL,0,XR_XIR_UNIT,&two,1,root_reference,2,NULL,0},
        {"indirectNone",12,&parameter[0],1,XR_XIR_I64,&two,1,indirect,2,NULL,0},
        {"indirectRoot",12,&parameter[1],1,XR_XIR_I64,&two,1,indirect,2,NULL,0},
        {"indirectUnknown",15,&parameter[2],1,XR_XIR_I64,&two,1,indirect,2,NULL,0},
        {"indirectMixed",13,&parameter[3],1,XR_XIR_I64,&two,1,indirect,2,NULL,0},
        {"relay",5,&parameter[3],1,XR_XIR_I64,&two,1,relay,2,&operand,1},
        {"erased",6,NULL,0,XR_XIR_I64,&three,1,erased,3,NULL,0},
        {"promisedUnknown",15,&parameter[4],1,XR_XIR_I64,&two,1,indirect,2,NULL,0}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=4},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=8},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=12},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=9}};
    XrXirTypes types={nodes,5,NULL,NULL};
    XrXirFunctionIdentity identities[13]={{0}};
    identities[12].promises=XR_XIR_FUNCTION_NO_SUSPEND;
    XrXirSourceModule source={"root",4,NULL,0,0};
    XrXirSlot slot={0,XR_XIR_I64,1};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=&slot,.slot_count=1,.entry_function=1};
    XrXirModule built={XR_XIR_BUILT,functions,13,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,NULL};
    return xir_fixture_check(context, &built, output, diagnostic);
}
#endif // XIR_CALLABLE_ROOT_FIXTURE_H
