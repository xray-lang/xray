/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * wire_cases.h - Ordinary Built input compared with independent literal fields
 */
static void wire_built(const XrXirCompileContext *context) {
    XrXirInstruction init={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    XrXirInstruction entry[]={
        {XR_XIR_CALL,(XrXirType)256,{0},{0},2,{0}},
        {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},3,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction make[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},19,{0}},
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},23,{0}},
        {XR_XIR_STRUCT_NEW,(XrXirType)256,{0,2},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirInstruction sum[]={
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_STRUCT_GET,XR_XIR_I64,{0},{0},1,{0}},
        {XR_XIR_ADD_INT,XR_XIR_I64,{1,2},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}}};
    XrXirBlock blocks[]={{0,1,0,0},{0,3,0,0},{0,4,0,0}};
    uint32_t operands[]={0,1};XrXirType pair=(XrXirType)256,field_types[]={XR_XIR_I64,XR_XIR_I64};
    XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&init,1,NULL,0},
        {"entry",5,NULL,0,XR_XIR_I64,blocks+1,1,entry,3,operands,1},
        {"make",4,NULL,0,(XrXirType)256,blocks+2,1,make,4,operands,2},
        {"sum",3,&pair,1,XR_XIR_I64,blocks+2,1,sum,4,NULL,0}};
    XrXirSourceModule source={"alpha",5,NULL,0,0};
    XrXirFunctionIdentity identities[]={
        {0},{.exported=1},{.nominal_owner=1,.method_kind=XR_XIR_MATERIALIZER},
        {.exported=1,.nominal_owner=1,.method_kind=XR_XIR_STATIC_METHOD}};
    XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    XrXirNominalField fields[]={{{"a",1},XR_XIR_I64,XR_XIR_FIELD_PRIVATE},{{"b",1},XR_XIR_I64,XR_XIR_FIELD_PRIVATE}};
    XrXirNominalDeclaration nominal={{"alpha",5},{"Pair",4},1,NULL,0,fields,2,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0}};
    XrXirNominalTable table={&nominal,1,NULL};
    XrXirTypeNode node={XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,NULL,0,field_types,2}};
    XrXirTypes types={&node,1,&table,NULL};
    XrXirFunctionEffectContract contracts[4]={0};contracts[0].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirEffectParameter parameter={0};contracts[3].parameter_count=1;contracts[3].parameters=&parameter;
    XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=4};
    XrXirModule built={XR_XIR_BUILT,functions,4,&declarations,NULL,&types,&evidence,XR_XIR_PROGRAM,NULL};
    uint32_t defaults[2]={0};XrXirConstructionRow row={0,defaults,2};
    XrXirMaterializationBinding edge={1,0,2,0};XrXirConstructionSpec input={&row,1,&edge,1};
    XrXirConstruction *owner=NULL;CHECK(xr_xir_compile_construction_new_v2(context,&types,&input,&owner)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_declarations_verify(context,&declarations,&types,4,XR_XIR_PROGRAM)==XR_XIR_OK);
    CHECK(xir_construction_verify(context,&built,owner)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check_v2(context,&built,owner,&checked,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Independent Built status=%u function=%u instruction=%u\n",(unsigned)status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    XrXirCompileContext exact=*context;exact.limits.functions=4;XrXirArtifact *bounded=NULL;
    CHECK(xr_xir_compile_check_v2(&exact,&built,owner,&bounded,NULL)==XR_XIR_OK && bounded);
    xr_xir_compile_artifact_free(bounded);bounded=NULL;exact.limits.functions=3;
    CHECK(xr_xir_compile_check_v2(&exact,&built,owner,&bounded,NULL)==XR_XIR_BUDGET && !bounded);
    node.nominal.fields=NULL;node.nominal.field_count=0;
    CHECK(xr_xir_compile_check_v2(context,&built,owner,&bounded,NULL)==XR_XIR_BAD_TYPE && !bounded);
    node.nominal.fields=field_types;node.nominal.field_count=2;
    xr_xir_compile_construction_free(owner);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if (packet.length!=sizeof(materialization74_golden) || memcmp(packet.bytes,materialization74_golden,packet.length)) {
        size_t at=0;while (at<packet.length && at<sizeof(materialization74_golden) && packet.bytes[at]==materialization74_golden[at]) ++at;
        fprintf(stderr,"Independent materializer comparison actual=%zu literal=%zu first=%zu\n",packet.length,sizeof(materialization74_golden),at);
    }
    CHECK(packet.length==sizeof(materialization74_golden) && !memcmp(packet.bytes,materialization74_golden,packet.length));
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
}
