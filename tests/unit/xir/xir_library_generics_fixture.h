/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_generics_fixture.h - Source-free private generic closure
 */
#ifndef XIR_LIBRARY_GENERICS_FIXTURE_H
#define XIR_LIBRARY_GENERICS_FIXTURE_H
/* There is deliberately no AST or source file for either library. */
static XrXirStatus generic_library_fixture(const XrXirCompileContext *context,
    const char *canonical, bool constrained, bool invalid, XrXirArtifact **output) {
    const XrXirType t=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirConstraint constraint={.markers=constrained?XR_XIR_CONSTRAINT_SENDABLE:0};
    XrXirInstruction helper[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction init[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction body[]={
        {XR_XIR_CALL,t,{0,1},{0},0,{0,1}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction bad[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirBlock one={0,1,0,0},two={0,2,0,0};
    uint32_t operand=0;
    XrXirFunction functions[]={
        {"helper",6,&t,1,t,&one,1,helper,1,NULL,0},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,init,1,NULL,0},
        {"identity",8,&t,1,t,&two,1,invalid?bad:body,2,invalid?NULL:&operand,invalid?0:1}};
    XrXirGeneric generics[]={
        {&constraint,1,NULL,0,NULL},{0},{&constraint,1,invalid?NULL:&t,invalid?0:1,NULL}};
    XrXirFunctionIdentity identities[3]={{0},{0},{.exported=1}};
    XrXirSourceModule module={canonical,(uint32_t)strlen(canonical),NULL,0,1};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,
        .declarations=&declarations,.generics=generics,.linkage_kind=XR_XIR_LIBRARY};
    return xr_xir_compile_check(context,&built,output,NULL);
}

/* Genuine unsupported families are checked before Catalog admission. */
static XrXirStatus generic_library_admission_fixture(const XrXirCompileContext *context,
    const char *canonical, unsigned family, XrXirArtifact **output) {
    XrXirType t=family==0?(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE:(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    XrXirInterfaceDeclaration declaration={.module={canonical,(uint32_t)strlen(canonical)},.name={"Boundary",8},.exported=1};
    XrXirInterfaceTable interfaces={&declaration,1};
    XrXirTypes types={.nodes=family==0?&node:NULL,.count=family==0?1:0,.interfaces=family==1?&interfaces:NULL};
    XrXirInterfaceApplication application={0,NULL,0};
    XrXirConstraint constraint={.interfaces=family==1?&application:NULL,.interface_count=family==1?1:0};
    uint32_t kind=XR_XIR_BINDER_RESULT_VARIABLE;
    XrXirGeneric generic={&constraint,1,NULL,0,family==2?&kind:NULL};
    XrXirGeneric generics[]={family==0?(XrXirGeneric){0}:generic,{0}};
    XrXirInstruction instructions[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block={0,1,0,0};
    XrXirFunction functions[]={
        {"identity",8,&t,1,t,&block,1,instructions,1,NULL,0},
        {"$init",5,NULL,0,XR_XIR_UNIT,&block,1,instructions,1,NULL,0}};
    XrXirFunctionIdentity identities[]={{.exported=1},{0}};
    XrXirSourceModule module={canonical,(uint32_t)strlen(canonical),NULL,0,1};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,.functions=identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,
        .types=family==2?NULL:&types,.declarations=&declarations,.generics=family==0?NULL:generics,.linkage_kind=XR_XIR_LIBRARY};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check(context,&built,output,&diagnostic);
    fprintf(stderr,"admission family%u status%u function%u block%u instruction%u\n",family,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}

#endif // XIR_LIBRARY_GENERICS_FIXTURE_H
