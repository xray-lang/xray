/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_atomic_i64_source_cases.h - Slot closure and malformed state rejection
 */
static XrXirStatus library_state_atomic_i64_map_operation(const XrXirCompileContext *context,void *opaque) {
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
static void library_state_atomic_i64_negative(const XrXirSourceRequest *request,const char *name,XrXirLinkageKind kind) {
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
static void library_state_atomic_i64_source_negatives(const XrXirSourceRequest *request) {
    const char *programs[]={"worker_read.xr","worker_write.xr","worker_snapshot.xr","worker_handle.xr","private_read.xr"};
    for (uint32_t i=0;i<5;++i) library_state_atomic_i64_negative(request,programs[i],XR_XIR_PROGRAM);
}
static void library_state_atomic_i64_bad_slots(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->declarations->slot_count==3 && module->declarations->slots[0].mutable);
    for (uint32_t mode=0;mode<5;++mode) {
        XrXirModule malformed=*module;malformed.stage=XR_XIR_BUILT;
        XrXirDeclarations declarations=*module->declarations;XrXirSlot slots[3];
        memcpy(slots,declarations.slots,sizeof(slots));declarations.slots=slots;malformed.declarations=&declarations;
        if (mode==0) slots[0].mutable=2;
        else if (mode==1) slots[0].module=declarations.module_count;
        else if (mode==2) slots[0].mutable=0;
        else {
            XrXirTypes types=*module->types;XrXirNominalTable table=*types.nominals;
            CHECK(table.count && table.count<=8);XrXirNominalDeclaration records[8];
            memcpy(records,table.declarations,table.count*sizeof(*records));uint32_t owner=UINT32_MAX;
            for (uint32_t n=0;n<table.count;++n) if (records[n].native.native_id==XR_NATIVE_DECLARATION_ORDERING) {
                CHECK(owner==UINT32_MAX);owner=n;
            }
            CHECK(owner<table.count);
            if (mode==3) records[owner].native.source_fingerprint[0]^=1;else records[owner].native.native_id=XR_NATIVE_DECLARATION_ATOMIC;
            table.declarations=records;types.nominals=&table;malformed.types=&types;
            XrXirArtifact *output=NULL;size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
            CHECK(xr_xir_compile_check_v2(context,&malformed,xr_xir_compile_artifact_construction(artifact),&output,NULL)==XR_XIR_BAD_STRUCTURE);
            CHECK(!output && source_program_compile_live==live && source_program_compile_bytes==bytes);continue;
        }
        XrXirArtifact *output=NULL;
        size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
        XrXirStatus status=xr_xir_compile_check_v2(context,&malformed,xr_xir_compile_artifact_construction(artifact),&output,NULL);
        CHECK(status==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(source_program_compile_live==live && source_program_compile_bytes==bytes);
    }
}
/* Exact trusted owner paths do not widen other stdlib resources. */
static void library_state_atomic_i64_catalog_negatives(const XrXirCompileContext *context,
    const XrXirLibraryInput *input) {
    CHECK(input->module_count==2);uint32_t native=UINT32_MAX;
    for (uint32_t i=0;i<2;++i) if (input->modules[i].authority.kind==XR_MODULE_IDENTITY_STDLIB) native=i;
    CHECK(native<2);
    for (uint32_t mode=0;mode<3;++mode) {
        XrXirLibraryModuleInput bindings[2];memcpy(bindings,input->modules,sizeof(bindings));
        XrXirLibraryInput malformed=*input;malformed.modules=bindings;
        if (mode==0) bindings[native].logical_path="prelude/forged.def";
        else if (mode==1) {
            bindings[native].authority.namespace_id="other";
            bindings[native].logical_path="other/builtin_symbols.def";
        } else {
            bindings[1-native].authority=(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_STDLIB,"prelude",XR_SOURCE_STDLIB};
            bindings[1-native].logical_path="prelude/builtin_symbols.def";
        }
        size_t live=source_program_compile_live,bytes=source_program_compile_bytes;
        XrXirLibraryCatalog *output=NULL;
        CHECK(xr_xir_compile_library_catalog_new_v2(context,&malformed,1,&output)==XR_XIR_BAD_STRUCTURE);
        CHECK(!output && source_program_compile_live==live && source_program_compile_bytes==bytes);
    }
}
