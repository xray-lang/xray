#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_trace_fixture.h - Literal direct, default, cleanup and recursive graphs
 */
#ifndef XIR_ROOT_TRACE_FIXTURE_H
#define XIR_ROOT_TRACE_FIXTURE_H
static XrXirArtifact *trace_fixture(const XrXirCompileContext *context) {
    XrXirInstruction done = {.op=XR_XIR_RETURN};
    XrXirInstruction init[] = {
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_SLOT_INIT,.args={0},.immediate=0},
        {.op=XR_XIR_SLOT_INIT,.args={0},.immediate=1},done};
    XrXirInstruction pure[] = {{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},done};
    XrXirInstruction read[] = {{.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64},done};
    XrXirInstruction default_call[] = {
        {.op=XR_XIR_CALL_DEFAULT,.type=XR_XIR_I64,.targets={4,0}},done};
    XrXirInstruction indirect[] = {
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction mixed[] = {
        {.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction relay[] = {
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=7}, {.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction scc_a[] = {
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=10},
        {.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64}, {.op=XR_XIR_RETURN,.args={1}}};
    XrXirInstruction scc_b[] = {{.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=9},done};
    XrXirInstruction tie[] = {
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=10},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=5},done};
    XrXirInstruction cleanup_owner[] = {
        {.op=XR_XIR_CLEANUP_REGISTER,.targets={1},.immediate=13},
        {.op=XR_XIR_CLEANUP_LEAVE,.targets={2}},done};
    XrXirInstruction const_read[] = {{.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64,.immediate=1},done};
    XrXirInstruction reference[] = {
        {.op=XR_XIR_FUNCTION_REF,.type=XR_XIR_CONSTRUCTED_TYPE_BASE,.immediate=9},done};
    XrXirBlock one={.count=1},two={.count=2},three={.count=3},four={.count=4};
    XrXirBlock cleanup_blocks[]={{.count=1},{.first=1,.count=1,.frontier=1},{.first=2,.count=1}};
    XrXirType scalar=XR_XIR_I64,callable=XR_XIR_CONSTRUCTED_TYPE_BASE;
    uint32_t relay_operand=0;
    XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,&four,1,init,4,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"read",4,NULL,0,XR_XIR_I64,&two,1,read,2,NULL,0},
        {"pure",4,NULL,0,XR_XIR_I64,&two,1,pure,2,NULL,0},
        {"defaultOwner",12,&scalar,1,XR_XIR_I64,&one,1,&done,1,NULL,0},
        {"defaultCall",11,NULL,0,XR_XIR_I64,&two,1,default_call,2,NULL,0},
        {"indirect",8,&callable,1,XR_XIR_I64,&two,1,indirect,2,NULL,0},
        {"mixed",5,&callable,1,XR_XIR_I64,&three,1,mixed,3,NULL,0},
        {"relay",5,&callable,1,XR_XIR_I64,&two,1,relay,2,&relay_operand,1},
        {"sccA",4,NULL,0,XR_XIR_I64,&three,1,scc_a,3,NULL,0},
        {"sccB",4,NULL,0,XR_XIR_I64,&two,1,scc_b,2,NULL,0},
        {"tie",3,NULL,0,XR_XIR_I64,&three,1,tie,3,NULL,0},
        {"cleanupOwner",12,NULL,0,XR_XIR_UNIT,cleanup_blocks,3,cleanup_owner,3,NULL,0},
        {"cleanup",7,NULL,0,XR_XIR_UNIT,&two,1,read,2,NULL,0},
        {"constRead",9,NULL,0,XR_XIR_I64,&two,1,const_read,2,NULL,0},
        {"reference",9,NULL,0,callable,&two,1,reference,2,NULL,0}};
    XrXirFunctionIdentity identities[16]={{0}};
    identities[13].cleanup_owner=13;
    XrXirSourceModule source={"root",4,NULL,0,0};
    XrXirSlot slots[]={{0,XR_XIR_I64,1},{0,XR_XIR_I64,0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .slots=slots,.slot_count=2,.entry_function=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    XrXirTypes types={&node,1,NULL,NULL};
    XrXirDefaultBinding binding={XR_XIR_DEFAULT_PARAMETER,4,0,2};
    XrXirDefaultTable defaults={&binding,1};
    XrXirModule built={XR_XIR_BUILT,functions,16,&declarations,NULL,&types,NULL,XR_XIR_PROGRAM,&defaults};
    XrXirArtifact *checked=NULL; XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context, &built, &checked, &diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"trace fixture status%u f%u b%u i%u\n",
        status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked); return checked;
}
static XrXirEffects *trace_summary(const XrXirCompileContext *context) {
    XrXirArtifact *checked=trace_fixture(context); XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    xr_xir_compile_artifact_free(checked); return effects;
}
#endif // XIR_ROOT_TRACE_FIXTURE_H
