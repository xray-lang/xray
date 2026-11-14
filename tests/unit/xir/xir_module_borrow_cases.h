/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_borrow_cases.h - Module loan effects and their owned storage
 */
#ifndef XIR_MODULE_BORROW_CASES_H
#define XIR_MODULE_BORROW_CASES_H

typedef struct ModuleBorrowFixture {
    XrXirTypeNode nodes[2];
    XrXirTypes types;
    XrXirType parameter;
    XrXirType callback_type;
    XrXirCallableParameter callback_parameter;
    XrXirInstruction init[6], read[3], call[3];
    XrXirBlock blocks[3];
    XrXirFunction functions[3];
    XrXirFunctionIdentity identities[3];
    XrXirSourceModule source;
    XrXirSlot slots[2];
    XrXirDeclarations declarations;
    uint32_t actual;
    XrXirModule module;
} ModuleBorrowFixture;

static void module_borrow_fixture(ModuleBorrowFixture *fixture, bool conflict) {
    *fixture = (ModuleBorrowFixture){0};
    fixture->nodes[0] = (XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    fixture->types = (XrXirTypes){.nodes=fixture->nodes,.count=1};
    fixture->parameter = XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirType cell = fixture->parameter;
    XrXirInstruction init[] = {
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_CELL_NEW,.type=cell,.args={0}},
        {.op=XR_XIR_SLOT_INIT,.args={1}},
        {.op=XR_XIR_CELL_NEW,.type=cell,.args={0}},
        {.op=XR_XIR_SLOT_INIT,.args={3},.immediate=1},
        {.op=XR_XIR_RETURN}};
    XrXirInstruction read[] = {
        {.op=XR_XIR_SLOT_LOAD,.type=cell,.immediate=conflict?0:1},
        {.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={1}},
        {.op=XR_XIR_RETURN,.args={2}}};
    XrXirInstruction call[] = {
        {.op=XR_XIR_SLOT_LOAD,.type=cell},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1},
        {.op=XR_XIR_RETURN,.args={1}}};
    memcpy(fixture->init,init,sizeof(init));
    memcpy(fixture->read,read,sizeof(read));
    memcpy(fixture->call,call,sizeof(call));
    fixture->blocks[0].count=6;
    fixture->blocks[1].count=fixture->blocks[2].count=3;
    fixture->functions[0]=(XrXirFunction){.name="init",.name_length=4,.error_type=XR_XIR_NEVER,
        .blocks=fixture->blocks,.block_count=1,.instructions=fixture->init,.instruction_count=6};
    fixture->functions[1]=(XrXirFunction){.name="read",.name_length=4,.result=XR_XIR_I64,
        .parameters=&fixture->parameter,.parameter_count=1,.error_type=XR_XIR_NEVER,
        .blocks=fixture->blocks+1,.block_count=1,.instructions=fixture->read,.instruction_count=3};
    fixture->functions[2]=(XrXirFunction){.name="call",.name_length=4,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .blocks=fixture->blocks+2,.block_count=1,.instructions=fixture->call,.instruction_count=3,
        .operands=&fixture->actual,.operand_count=1};
    fixture->source=(XrXirSourceModule){.name="module",.name_length=6};
    fixture->slots[0]=fixture->slots[1]=(XrXirSlot){.type=cell,.mutable=1};
    fixture->declarations=(XrXirDeclarations){.modules=&fixture->source,.module_count=1,
        .functions=fixture->identities,.slots=fixture->slots,.slot_count=2,.entry_function=2};
    fixture->module=(XrXirModule){.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,
        .types=&fixture->types,.functions=fixture->functions,.function_count=3,.declarations=&fixture->declarations};
}

static void module_borrow_captured_origin(void) {
    for (uint32_t conflict=0;conflict<2;++conflict) {
        ModuleBorrowFixture fixture;
        module_borrow_fixture(&fixture,true);
        XrXirType fn=XR_XIR_CONSTRUCTED_TYPE_BASE+1;
        fixture.nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
            .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_REQUIRED};
        fixture.types.count=2;
        uint32_t capture_actual=0;
        XrXirInstruction captured[]={
            {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1},
            {.op=XR_XIR_RETURN,.args={1}}};
        XrXirInstruction invoke[]={
            {.op=XR_XIR_SLOT_LOAD,.type=fixture.parameter,.immediate=conflict?0:1},
            {.op=XR_XIR_FUNCTION_REF,.type=fn,.args={0,1},.immediate=3},
            {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1},
            {.op=XR_XIR_RETURN,.args={2}}};
        XrXirBlock blocks[]={{.count=2},{.count=4}};
        XrXirFunction functions[4]={fixture.functions[0],fixture.functions[1],
            {.name="invoke",.name_length=6,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
                .instructions=invoke,.instruction_count=4,.blocks=blocks+1,.block_count=1,
                .operands=&capture_actual,.operand_count=1},
            {.name="captured",.name_length=8,.parameters=&fixture.parameter,.parameter_count=1,
                .result=XR_XIR_I64,.error_type=XR_XIR_NEVER,.instructions=captured,.instruction_count=2,
                .blocks=blocks,.block_count=1,.operands=&capture_actual,.operand_count=1}};
        XrXirFunctionIdentity identities[4]={{0},{0},{0},{.capture_owner=3,.capture_count=1}};
        fixture.declarations.functions=identities;
        fixture.declarations.entry_function=2;
        fixture.module.functions=functions;
        fixture.module.function_count=4;
        XrXirConstruction *construction=NULL;
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_module_construction(&effect_context,&fixture.module,&construction)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_check_v2(&effect_context,&fixture.module,construction,&artifact,&diagnostic);
        CHECK(status==XR_XIR_BAD_VALUE && !artifact);
        CHECK(diagnostic.function==2 && diagnostic.instruction==1);
        xr_xir_compile_artifact_free(artifact);
        xr_xir_compile_construction_free(construction);
    }
}

static void module_borrow_projected_capture(void) {
    for (uint32_t conflict=0;conflict<2;++conflict) {
        XrXirType array=XR_XIR_CONSTRUCTED_TYPE_BASE,cell_array=array+1,cell=array+2,fn=array+3;
        XrXirTypeNode nodes[]={
            {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
            {.kind=XR_XIR_TYPE_CELL,.element=array},
            {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
            {.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_REQUIRED}};
        XrXirTypes types={.nodes=nodes,.count=4};
        uint32_t initial=0,capture=4,forward=0;
        XrXirInstruction init[]={
            {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
            {.op=XR_XIR_ARRAY_NEW,.type=array,.args={0,1}},
            {.op=XR_XIR_CELL_NEW,.type=cell_array,.args={1}},
            {.op=XR_XIR_SLOT_INIT,.args={2}},
            {.op=XR_XIR_CELL_NEW,.type=cell_array,.args={1}},
            {.op=XR_XIR_SLOT_INIT,.args={4},.immediate=1},
            {.op=XR_XIR_RETURN}};
        XrXirInstruction read[]={
            {.op=XR_XIR_SLOT_LOAD,.type=cell_array},
            {.op=XR_XIR_CELL_READ,.type=array,.args={1}},
            {.op=XR_XIR_ARRAY_LEN,.type=XR_XIR_I64,.args={2}},
            {.op=XR_XIR_RETURN,.args={3}}};
        XrXirInstruction invoke[]={
            {.op=XR_XIR_SLOT_LOAD,.type=cell_array,.immediate=conflict?0:1},
            {.op=XR_XIR_CELL_PLACE,.type=array,.args={0}},
            {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
            {.op=XR_XIR_INDEX_PLACE,.type=XR_XIR_I64,.args={1,2}},
            {.op=XR_XIR_CELL_PROJECT,.type=cell,.args={3}},
            {.op=XR_XIR_FUNCTION_REF,.type=fn,.args={0,1},.immediate=3},
            {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=5},
            {.op=XR_XIR_RETURN,.args={6}}};
        XrXirInstruction captured[]={
            {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1},
            {.op=XR_XIR_RETURN,.args={1}}};
        XrXirBlock blocks[]={{.count=7},{.count=4},{.count=8},{.count=2}};
        XrXirFunction functions[]={
            {.name="init",.name_length=4,.error_type=XR_XIR_NEVER,.instructions=init,.instruction_count=7,
                .blocks=blocks,.block_count=1,.operands=&initial,.operand_count=1},
            {.name="read",.name_length=4,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,.parameters=&cell,.parameter_count=1,
                .instructions=read,.instruction_count=4,.blocks=blocks+1,.block_count=1},
            {.name="invoke",.name_length=6,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,.instructions=invoke,.instruction_count=8,
                .blocks=blocks+2,.block_count=1,.operands=&capture,.operand_count=1},
            {.name="captured",.name_length=8,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,.parameters=&cell,.parameter_count=1,
                .instructions=captured,.instruction_count=2,.blocks=blocks+3,.block_count=1,.operands=&forward,.operand_count=1}};
        XrXirFunctionIdentity identities[4]={{0},{0},{0},{.capture_owner=3,.capture_count=1}};
        XrXirSourceModule source={.name="module",.name_length=6};
        XrXirSlot slots[]={{.type=cell_array,.mutable=1},{.type=cell_array,.mutable=1}};
        XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.slots=slots,
            .slot_count=2,.entry_function=2};
        XrXirModule module={.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.types=&types,
            .functions=functions,.function_count=4,.declarations=&declarations};
        XrXirConstruction *construction=NULL;
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_module_construction(&effect_context,&module,&construction)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_check_v2(&effect_context,&module,construction,&artifact,&diagnostic);
        CHECK(status==XR_XIR_BAD_VALUE && !artifact);
        CHECK(diagnostic.function==2 && diagnostic.instruction==5);
        xr_xir_compile_artifact_free(artifact);
        xr_xir_compile_construction_free(construction);
    }
}

static void module_borrow_cleanup_capture(void) {
    for (uint32_t conflict=0;conflict<2;++conflict) {
        ModuleBorrowFixture fixture;
        module_borrow_fixture(&fixture,true);
        uint32_t actual=0;
        XrXirInstruction cleanup[]={
            {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1},
            {.op=XR_XIR_RETURN}};
        XrXirInstruction invoke[]={
            {.op=XR_XIR_SLOT_LOAD,.type=fixture.parameter,.immediate=conflict?0:1},
            {.op=XR_XIR_CLEANUP_REGISTER,.args={0,1},.targets={1},.immediate=3},
            {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
            {.op=XR_XIR_RETURN,.args={2}}};
        XrXirBlock blocks[]={{.count=2},{.first=2,.count=2,.frontier=2},{.count=2}};
        XrXirFunction functions[]={fixture.functions[0],fixture.functions[1],
            {.name="invoke",.name_length=6,.error_type=XR_XIR_NEVER,.result=XR_XIR_I64,
                .instructions=invoke,.instruction_count=4,.blocks=blocks,.block_count=2,.operands=&actual,.operand_count=1},
            {.name="cleanup",.name_length=7,.error_type=XR_XIR_NEVER,.parameters=&fixture.parameter,.parameter_count=1,
                .instructions=cleanup,.instruction_count=2,.blocks=blocks+2,.block_count=1,.operands=&actual,.operand_count=1}};
        XrXirFunctionIdentity identities[4]={{0},{0},{0},{.cleanup_owner=3,.capture_owner=3,.capture_count=1}};
        fixture.declarations.functions=identities;
        fixture.module.functions=functions;
        fixture.module.function_count=4;
        XrXirConstruction *construction=NULL;
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_module_construction(&effect_context,&fixture.module,&construction)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_check_v2(&effect_context,&fixture.module,construction,&artifact,&diagnostic);
        if (status!=(conflict?XR_XIR_BAD_TYPE:XR_XIR_OK))
            fprintf(stderr,"cleanup module capture conflict%u status%u reason%u f%u i%u\n",conflict,status,
                diagnostic.reason,diagnostic.function,diagnostic.instruction);
        CHECK(status==(conflict?XR_XIR_BAD_TYPE:XR_XIR_OK));
        if (conflict) CHECK(!artifact && diagnostic.reason==XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS);
        else CHECK(artifact);
        xr_xir_compile_artifact_free(artifact);
        xr_xir_compile_construction_free(construction);
    }
}

static void module_borrow_sparse_growth(void) {
    const uint32_t count=96;
    XrXirSlot slots[96];
    XrXirInstruction instructions[97];
    for (uint32_t s=0;s<count;++s) {
        slots[s]=(XrXirSlot){.type=XR_XIR_I64,.mutable=1};
        instructions[s]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64,.immediate=s};
    }
    instructions[count]=(XrXirInstruction){.op=XR_XIR_RETURN};
    XrXirBlock block={.count=count+1};
    XrXirFunction function={.name="slots",.name_length=5,.error_type=XR_XIR_NEVER,
        .instructions=instructions,.instruction_count=count+1,.blocks=&block,.block_count=1};
    XrXirFunctionIdentity identity={0};
    XrXirSourceModule source={.name="module",.name_length=6};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=&identity,.slots=slots,.slot_count=count};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.declarations=&declarations};
    EffectMark baseline=effect_mark();
    EffectCheckedRoots *roots=NULL;
    attempts=0;
    CHECK(effect_checked_roots_build(&effect_context,&module,NULL,NULL,NULL,&roots,true)==XR_XIR_OK);
    size_t sites=attempts;
    CHECK(roots->module_index_capacity>=512 && roots->module_members==count*2);
    for (uint32_t s=0;s<count;++s) {
        XirModuleBorrowConflict conflict={0};
        CHECK(effect_module_conflict_set(roots,roots->function[0].root,EFFECT_MODULE_ACCESS,0,s,false,&conflict));
        CHECK(conflict.slot==s && conflict.access_instruction==s);
    }
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);
    for (size_t point=0;point<sites;++point) {
        attempts=0;injected=false;fail_at=point;roots=NULL;
        CHECK(effect_checked_roots_build(&effect_context,&module,NULL,NULL,NULL,&roots,true)==XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !roots);fail_at=SIZE_MAX;
        effect_mark_check(baseline);
    }
    printf("module sparse access: 96 binding identities, all rehashes and %zu failures PASS\n",sites);
}

static void module_borrow_owner_cases(void) {
    EffectMark baseline=effect_mark();
    ModuleBorrowFixture fixture;
    module_borrow_fixture(&fixture,true);
    EffectCheckedRoots *roots=NULL;
    attempts=0;
    CHECK(effect_checked_roots_build(&effect_context,&fixture.module,NULL,NULL,NULL,&roots,true)==XR_XIR_OK);
    size_t sites=attempts;
    XrXirEffects effects={.resources=effect_context.resources,.count=3,.checked_roots=roots};
    XirModuleBorrowConflict conflict={0};
    CHECK(xir_effects_module_borrow_site(&effect_context,&effects,&fixture.module,2,1,&conflict)==XR_XIR_OK);
    CHECK(conflict.slot==0 && conflict.callee==1 && conflict.access_function==1 && !conflict.unknown);
    uint32_t members=roots->module_members;
    EffectCheckBuild build={.memory=&effect_context,.module=&fixture.module,.roots=roots};
    CHECK(effect_check_close(&build)==XR_XIR_OK && roots->module_members==members);
    memset(&fixture,0xCE,sizeof(fixture));
    CHECK(effect_module_conflict_set(roots,roots->function[1].root,EFFECT_MODULE_ACCESS,1,0,false,&conflict));
    effect_checked_roots_free(roots);
    effect_mark_check(baseline);

    module_borrow_fixture(&fixture,true);
    for (size_t point=0;point<sites;++point) {
        attempts=0;injected=false;fail_at=point;roots=NULL;
        CHECK(effect_checked_roots_build(&effect_context,&fixture.module,NULL,NULL,NULL,&roots,true)==XR_XIR_OUT_OF_MEMORY);
        CHECK(injected && !roots);fail_at=SIZE_MAX;
        effect_mark_check(baseline);
    }
    XrXirCompileContext finite=effect_owner_new((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,128,true});
    uint64_t held=effect_stats(&finite).held_bytes;
    roots=NULL;
    CHECK(effect_checked_roots_build(&finite,&fixture.module,NULL,NULL,NULL,&roots,true)==XR_XIR_MEMORY_LIMIT && !roots);
    effect_owner_free(&finite,held);

    for (uint32_t variant=0;variant<2;++variant) {
        module_borrow_fixture(&fixture,variant==0);
        XrXirConstruction *construction=NULL;
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_module_construction(&effect_context,&fixture.module,&construction)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_check_v2(&effect_context,&fixture.module,construction,&artifact,&diagnostic);
        if (variant==0) {
            if (status!=XR_XIR_BAD_TYPE || diagnostic.reason!=XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS)
                fprintf(stderr,"module borrower raw status%u reason%u f%u b%u i%u\n",status,diagnostic.reason,
                    diagnostic.function,diagnostic.block,diagnostic.instruction);
            CHECK(status==XR_XIR_BAD_TYPE && diagnostic.reason==XR_XIR_DIAGNOSTIC_MODULE_BORROW_ACCESS && !artifact);
            CHECK(diagnostic.function==2 && diagnostic.instruction==1);
        } else {
            if (status!=XR_XIR_OK) fprintf(stderr,"module borrower positive status%u reason%u f%u b%u i%u\n",
                status,diagnostic.reason,diagnostic.function,diagnostic.block,diagnostic.instruction);
            CHECK(status==XR_XIR_OK && artifact);
            XrXirCheckedPacket packet={0};
            CHECK(xr_xir_compile_checked_write(artifact,&packet,&diagnostic)==XR_XIR_OK);
            xr_xir_compile_artifact_free(artifact);artifact=NULL;
            memset(&fixture,0xCE,sizeof(fixture));
            CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&artifact,&diagnostic)==XR_XIR_OK);
            CHECK(xr_xir_compile_artifact_verify(artifact,&diagnostic)==XR_XIR_OK);
            xr_xir_compile_checked_packet_free(&packet);
        }
        xr_xir_compile_artifact_free(artifact);
        xr_xir_compile_construction_free(construction);
        effect_mark_check(baseline);
    }
    for (uint32_t unknown=0;unknown<2;++unknown) {
        module_borrow_fixture(&fixture,false);
        fixture.callback_type=XR_XIR_CONSTRUCTED_TYPE_BASE+1;
        fixture.callback_parameter=(XrXirCallableParameter){fixture.parameter,XR_PARAM_REF,false};
        fixture.nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,
            .parameters=&fixture.callback_parameter,.parameter_count=1,
            .flags=unknown?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_NONE};
        fixture.types.count=2;
        fixture.functions[2].parameters=&fixture.callback_type;
        fixture.functions[2].parameter_count=1;
        fixture.functions[1].parameters=NULL;
        fixture.functions[1].parameter_count=0;
        fixture.read[1].args[0]=0;
        fixture.read[2].args[0]=1;
        fixture.declarations.entry_function=1;
        fixture.actual=1;
        fixture.call[1].op=XR_XIR_CALL_INDIRECT;
        fixture.call[1].immediate=0;
        fixture.call[2].args[0]=2;
        XrXirConstruction *construction=NULL;
        XrXirArtifact *artifact=NULL;
        XrXirDiagnostic diagnostic={0};
        CHECK(xir_fixture_module_construction(&effect_context,&fixture.module,&construction)==XR_XIR_OK);
        XrXirStatus status=xr_xir_compile_check_v2(&effect_context,&fixture.module,construction,&artifact,&diagnostic);
        if (status!=(unknown?XR_XIR_BAD_TYPE:XR_XIR_OK))
            fprintf(stderr,"module unknown%u status%u reason%u f%u i%u\n",unknown,status,
                diagnostic.reason,diagnostic.function,diagnostic.instruction);
        CHECK(status==(unknown?XR_XIR_BAD_TYPE:XR_XIR_OK));
        if (unknown) CHECK(!artifact && diagnostic.reason==XR_XIR_DIAGNOSTIC_MODULE_BORROW_UNKNOWN);
        else CHECK(artifact);
        xr_xir_compile_artifact_free(artifact);
        xr_xir_compile_construction_free(construction);
        effect_mark_check(baseline);
    }
    printf("module borrower effects: direct CALL without FUNCTION_REF, distinct binding, cold owner, %zu failures PASS\n",sites);
    module_borrow_sparse_growth();
    module_borrow_captured_origin();
    module_borrow_projected_capture();
    module_borrow_cleanup_capture();
}
#endif
