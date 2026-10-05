/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_invoke_allocations.h - Default authority across explicit error edges
 *
 * KEY CONCEPT:
 *   Owner metadata is not a value operand; only the proper successor owns results.
 */
#ifndef XIR_DEFAULT_INVOKE_ALLOCATIONS_H
#define XIR_DEFAULT_INVOKE_ALLOCATIONS_H
static XrXirStatus default_invoke_allocation_case(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;
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
    entry[0].type_arguments[1]=4;
    XrXirModule module={XR_XIR_BUILT,functions,4,&declarations,generics,&types,NULL,XR_XIR_PROGRAM,&table};

    XrXirArtifact *checked=NULL,*special=NULL,*read=NULL,*lowered=NULL;
    XrXirEffects *effects=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_check(context,&module,&checked,NULL);
    if(status!=XR_XIR_OK)CHECK(!checked);
    if(status==XR_XIR_OK){status=xr_xir_compile_effects_analyze(checked,&effects);if(status!=XR_XIR_OK)CHECK(!effects);}
    if(status==XR_XIR_OK){CHECK(!xr_xir_effects_error(effects,1,XR_XIR_CONSTRUCTED_TYPE_BASE,0));
        status=xr_xir_compile_specialize(checked,&special,NULL);if(status!=XR_XIR_OK)CHECK(!special);}
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_write(special,&packet,NULL);if(status!=XR_XIR_OK)CHECK(!packet.bytes&&!packet.length);}
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(read,&fixture_target,&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(special);xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    return status;
}
static void default_invoke_allocation_cases(void) {
    allocation_compile_operation_cases("default invoke full pipeline",default_invoke_allocation_case,NULL);
}
#endif // XIR_DEFAULT_INVOKE_ALLOCATIONS_H
