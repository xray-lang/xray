/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_task_i64_source_cases.h - Slot closure and malformed state rejection
 */
static XrXirStatus library_state_task_i64_map_operation(const XrXirCompileContext *context,void *opaque) {
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
static void library_state_task_i64_negative(const XrXirSourceRequest *request,const char *name,XrXirLinkageKind kind) {
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
static void library_state_task_i64_source_negatives(const XrXirSourceRequest *request) {
    const char *programs[]={"worker_read.xr","worker_write.xr","worker_snapshot.xr","private_read.xr","worker_handle.xr","await_parameter_child.xr"};
    for (uint32_t i=0;i<6;++i) library_state_task_i64_negative(request,programs[i],XR_XIR_PROGRAM);
}
static void library_state_task_i64_bad_slots(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->declarations->slot_count==5 && module->declarations->slots[0].mutable);
    for (uint32_t mode=0;mode<3;++mode) {
        XrXirModule malformed=*module;malformed.stage=XR_XIR_BUILT;
        XrXirDeclarations declarations=*module->declarations;XrXirSlot slots[5];
        memcpy(slots,declarations.slots,sizeof(slots));declarations.slots=slots;malformed.declarations=&declarations;
        if (mode==0) slots[0].mutable=2;
        else if (mode==1) slots[0].module=declarations.module_count;
        else slots[0].mutable=0;
        XrXirArtifact *output=NULL;
        size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
        XrXirStatus status=xr_xir_compile_check_v2(context,&malformed,xr_xir_compile_artifact_construction(artifact),&output,NULL);
        CHECK(status==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(source_program_compile_live==live && source_program_compile_bytes==bytes);
    }
}

/* A non-identity relocation demonstrates direct GO/INVOKE mapping and rollback.
 * INVOKE_RESULT and INVOKE_ERROR keep their local origin instruction ordinals. */
static XrXirStatus library_state_task_i64_callee_map(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;SourceContext ctx={0};ctx.compile=*context;XrXirStatus status=XR_XIR_OK;
    uint32_t *functions=source_scratch(&ctx,3,sizeof(*functions),false);
    if (!functions) return ctx.diagnostic.status;
    functions[0]=7;functions[1]=11;functions[2]=19;
    SourceLibraryMap map={.functions=functions,.function_count=3};
    XrXirInstruction input={.op=XR_XIR_GO,.type=XR_XIR_I64,.immediate=1},output={0};
    for (uint32_t which=0;which<2;++which) {
        input.op=which ? XR_XIR_INVOKE : XR_XIR_GO;
        if (!source_library_instruction(&ctx,&map,&input,&output)) {status=ctx.diagnostic.status;goto done;}
        CHECK(output.immediate==11 && output.op==input.op);
        XrXirInstruction sentinel=output;functions[1]=UINT32_MAX;
        bool accepted=source_library_instruction(&ctx,&map,&input,&output);
        if (ctx.diagnostic.status==XR_XIR_BUDGET) {status=XR_XIR_BUDGET;goto done;}
        CHECK(!accepted && ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !memcmp(&sentinel,&output,sizeof(output)));
        ctx.diagnostic=(XrXirSourceDiagnostic){0};functions[1]=11;
    }
    input.op=XR_XIR_INVOKE_RESULT;input.immediate=23;
    if (!source_library_instruction(&ctx,&map,&input,&output)) {status=ctx.diagnostic.status;goto done;}
    CHECK(output.immediate==23);
done:
    xr_compile_resources_free(functions);return status;
}
static void library_state_task_i64_callees(const XrXirModule *module) {
    uint32_t direct=0,awaits=0;
    for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[f].instructions[i];
        if (op->op==XR_XIR_GO) {
            CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
            const XrXirFunction *callee=&module->functions[op->immediate];
            CHECK((callee->name_length==8 && !memcmp(callee->name,"identity",8)) ||
                (callee->name_length==4 && !memcmp(callee->name,"fail",4)) ||
                (callee->name_length==4 && !memcmp(callee->name,"spin",4)));++direct;
        }
        awaits+=op->op==XR_XIR_TASK_AWAIT;
    }
    CHECK(direct>=4 && awaits>=2);
}
