/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_typed_values_source_cases.h - Slot closure and malformed state rejection
 */
static XrXirStatus library_state_typed_values_map_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;SourceContext ctx={0};ctx.compile=*context;ctx.slot_count=5;
    XrXirSlot slots[3]={{0,XR_XIR_I64,1},{1,XR_XIR_I64,1},{1,XR_XIR_STRING,1}};
    XrXirDeclarations declarations={.slots=slots,.slot_count=3};
    XrXirModule library={.declarations=&declarations};uint32_t modules[2]={UINT32_MAX,4};
    SourceLibraryMap map={.modules=modules,.module_count=2};
    if (!source_library_slot_map(&ctx,&library,&map,2)) {xr_compile_resources_free(map.slots);return ctx.diagnostic.status;}
    CHECK(map.slots[0]==UINT32_MAX && map.slots[1]==2 && map.slots[2]==3 && map.next_slot==4);
    XrXirInstruction group={XR_XIR_SLOT_GROUP_INIT,XR_XIR_UNIT,{0},{0},(INT64_C(1)<<32)|2,{0}},output={0};
    if (!source_library_instruction(&ctx,&map,&group,&output)) {xr_compile_resources_free(map.slots);return ctx.diagnostic.status;}
    CHECK((uint64_t)output.immediate==((UINT64_C(2)<<32)|2));
    XrXirInstruction sentinel=output;group.immediate=(INT64_C(0)<<32)|2;
    bool accepted=source_library_instruction(&ctx,&map,&group,&output);
    if (ctx.diagnostic.status==XR_XIR_BUDGET) {xr_compile_resources_free(map.slots);return XR_XIR_BUDGET;}
    CHECK(!accepted && ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !memcmp(&sentinel,&output,sizeof(output)));
    ctx.diagnostic=(XrXirSourceDiagnostic){0};group.immediate=(INT64_C(1)<<32)|2;map.slots[2]=4;
    accepted=source_library_instruction(&ctx,&map,&group,&output);
    if (ctx.diagnostic.status==XR_XIR_BUDGET) {xr_compile_resources_free(map.slots);return XR_XIR_BUDGET;}
    CHECK(!accepted && ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !memcmp(&sentinel,&output,sizeof(output)));
    xr_compile_resources_free(map.slots);return XR_XIR_OK;
}
/* A nonidentity map exposes stale producer ordinals independently of Source
 * inventory order. This relocation probe is not an alternate Checked checker. */
static XrXirStatus library_state_typed_values_type_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;SourceContext ctx={0};ctx.compile=*context;ctx.slot_count=1;
    ctx.slots=source_scratch(&ctx,1,sizeof(*ctx.slots),false);
    if (!ctx.slots) return ctx.diagnostic.status;
    const XrXirType input=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType mapped=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+7);
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    XrXirTypes types={.nodes=&node,.count=1};XrXirSlot slot={0,input,1};
    XrXirDeclarations declarations={.slots=&slot,.slot_count=1};
    XrXirModule library={.types=&types,.declarations=&declarations};
    uint32_t modules[1]={3},slots[1]={0};
    SourceLibraryUnit unit={.types=&mapped,.type_count=1};
    SourceLibraryMap map={.unit=&unit,.modules=modules,.module_count=1,.slots=slots,.slot_count=1};
    bool accepted=source_library_slots(&ctx,&library,&map);
    if (!accepted) {xr_compile_resources_free(ctx.slots);return ctx.diagnostic.status;}
    CHECK(ctx.slots[0].type==mapped && ctx.slots[0].module==3 && ctx.slots[0].mutable);
    XrXirSlot sentinel=ctx.slots[0];mapped=(XrXirType)UINT32_MAX;
    accepted=source_library_slots(&ctx,&library,&map);
    if (ctx.diagnostic.status==XR_XIR_BUDGET) {xr_compile_resources_free(ctx.slots);return XR_XIR_BUDGET;}
    CHECK(!accepted && ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !memcmp(ctx.slots,&sentinel,sizeof(sentinel)));
    ctx.diagnostic=(XrXirSourceDiagnostic){0};node.kind=XR_XIR_TYPE_CELL;mapped=input;
    accepted=source_library_slots(&ctx,&library,&map);
    if (ctx.diagnostic.status==XR_XIR_BUDGET) {xr_compile_resources_free(ctx.slots);return XR_XIR_BUDGET;}
    CHECK(!accepted && ctx.diagnostic.status==XR_XIR_BAD_STAGE && !memcmp(ctx.slots,&sentinel,sizeof(sentinel)));
    xr_compile_resources_free(ctx.slots);return XR_XIR_OK;
}
static void library_state_typed_values_negative(const XrXirSourceRequest *request,const char *name,XrXirLinkageKind kind) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,name)>0);
    XrXirSourceRequest negative=*request;negative.entry_path=path;negative.linkage_kind=kind;
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
    XrXirStatus status=xr_xir_compile_source_check(&negative,&result,&diagnostic,NULL);
    if (status==XR_XIR_OK) fprintf(stderr,"unexpected accepted private-state negative %s\n",name);
    CHECK(status==XR_XIR_BAD_STRUCTURE || status==XR_XIR_BAD_TYPE);
    CHECK(!result.checked && !result.snapshot && diagnostic.message[0]);
    if (!strncmp(name,"worker_",7)) CHECK(status==XR_XIR_BAD_TYPE &&
        !strcmp(diagnostic.message,"GO target requires the current instance root execution"));
    xr_xir_compile_source_result_free(&result);
    CHECK(source_program_compile_live==live && source_program_compile_bytes==bytes);
    printf("Library private state negative %s rejected status%u reason%s\n",name,status,diagnostic.message);
}
static void library_state_typed_values_source_negatives(const XrXirSourceRequest *request) {
    const char *programs[]={"worker_read.xr","worker_write.xr","worker_snapshot.xr","private_read.xr"};
    for (uint32_t i=0;i<4;++i) library_state_typed_values_negative(request,programs[i],XR_XIR_PROGRAM);
}
static void library_state_typed_values_bad_slots(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->declarations->slot_count==7 && module->declarations->slots[0].mutable &&
        xr_xir_type_node(module->types,module->declarations->slots[0].type));
    for (uint32_t mode=0;mode<3;++mode) {
        XrXirModule malformed=*module;malformed.stage=XR_XIR_BUILT;
        XrXirDeclarations declarations=*module->declarations;XrXirSlot slots[7];
        memcpy(slots,declarations.slots,sizeof(slots));declarations.slots=slots;malformed.declarations=&declarations;
        if (mode==0) slots[0].mutable=2;
        else if (mode==1) slots[0].module=declarations.module_count;
        else slots[0].mutable=0;
        XrXirArtifact *output=NULL;
        size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
        XrXirStatus status=xr_xir_compile_check_v2(context,&malformed,xr_xir_compile_artifact_construction(artifact),&output,NULL);
        XrXirStatus expected=mode==2 ? XR_XIR_BAD_TYPE : XR_XIR_BAD_STRUCTURE;
        if (status!=expected || output)
            fprintf(stderr,"typed state malformed mode%u status%u output%u\n",mode,status,output ? 1u : 0u);
        CHECK(status==expected && !output);
        CHECK(source_program_compile_live==live && source_program_compile_bytes==bytes);
    }
}
