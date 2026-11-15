/* Standalone signature facts cannot use witness candidates as authentication. */
static void closed_reader_failures(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    uint32_t function=module->declarations->hashable_witnesses[0].equals_function;
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XirMetadataFunctionSignature output={0};
    XrXirCompileContext tiny=*compile;tiny.limits.instructions=0;
    CHECK(xir_metadata_module_signature(module,&tiny,function,NULL,NULL,&output)==XR_XIR_BUDGET);
    CHECK(!output.name && !output.parameters);
    XirReadScratch scratch={compile->resources,NULL,0};
    for(unsigned tries=0;;++tries) {
        CHECK(tries<8);CHECK(xir_read_scratch_begin(compile,&scratch)==XR_XIR_OK);
        size_t attempts=source_fixture_compile_attempts;
        XrXirStatus status=xir_metadata_module_signature(module,compile,function,&scratch,NULL,&output);
        CHECK(source_fixture_compile_attempts==attempts);
        if(status==XR_XIR_OK)break;
        CHECK(status==XR_XIR_BUDGET && scratch.required && !output.name && !output.parameters);
        CHECK(xir_read_scratch_grow(compile,&scratch)==XR_XIR_OK);
    }
    CHECK(output.name_length==6 && !memcmp(output.name,"equals",6));
    CHECK(output.parameters==module->functions[function].parameters);
    xir_read_scratch_free(&scratch);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    source_fixture_compile_attempts=0;output=(XirMetadataFunctionSignature){0};
    CHECK(xir_metadata_module_signature(module,compile,function,NULL,NULL,&output)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;CHECK(sites);
    for(size_t point=0;point<sites;++point) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=point;source_fixture_compile_injected=false;
        output=(XirMetadataFunctionSignature){0};
        CHECK(xir_metadata_module_signature(module,compile,function,NULL,NULL,&output)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.name && !output.parameters);
        CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    Cancel cancellation={0,SIZE_MAX};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&cancellation};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_metadata_module_signature(module,compile,function,NULL,&control,&output)==XR_XIR_OK);
    size_t polls=cancellation.polls;CHECK(polls);
    for(size_t point=1;point<=polls;++point) {
        cancellation=(Cancel){0,point};control=(XrCompileRequestControl){0};output=(XirMetadataFunctionSignature){0};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        CHECK(xir_metadata_module_signature(module,compile,function,NULL,&control,&output)==XR_XIR_CANCELLED);
        CHECK(!output.name && !output.parameters && source_fixture_compile_live==live && source_fixture_compile_bytes==bytes);
    }
    printf("Hashable closed signature FI=%zu cancel-prefixes=%zu\n",sites,polls);
}
static void closed_module_refusal(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    uint32_t function=m->declarations->hashable_witnesses[0].equals_function;
    uint32_t owner=m->declarations->functions[function].module;
    XrXirSourceModule *modules=malloc((size_t)m->declarations->module_count*sizeof(*modules));CHECK(modules);
    memcpy(modules,m->declarations->modules,(size_t)m->declarations->module_count*sizeof(*modules));
    CHECK(modules[owner].name_length);
    char *different=malloc(modules[owner].name_length);CHECK(different);
    memcpy(different,modules[owner].name,modules[owner].name_length);
    different[0]=different[0]=='x'?'y':'x';modules[owner].name=different;
    XrXirDeclarations declarations=*m->declarations;declarations.modules=modules;
    XrXirModule altered=*m;altered.declarations=&declarations;
    XirMetadataFunctionSignature output={0};
    CHECK(xir_metadata_module_signature(&altered,compile,function,NULL,NULL,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.name && !output.parameters);
    free(different);free(modules);
}
static void closed_nominal_refusal(const XrXirCompileContext *compile,const XrXirArtifact *artifact) {
    const XrXirModule *m=xr_xir_compile_artifact_module(artifact);
    XrXirHashableWitness row=m->declarations->hashable_witnesses[0];
    uint32_t owner=xr_xir_type_node(m->types,row.nominal_type)->nominal.declaration;
    XrXirNominalDeclaration *declarations=malloc((size_t)m->types->nominals->count*sizeof(*declarations));CHECK(declarations);
    memcpy(declarations,m->types->nominals->declarations,(size_t)m->types->nominals->count*sizeof(*declarations));
    XrXirLiteral name=declarations[owner].name;CHECK(name.length);
    char *different=malloc(name.length);CHECK(different);memcpy(different,name.bytes,name.length);
    different[0]=different[0]=='x'?'y':'x';declarations[owner].name.bytes=different;
    XrXirNominalTable table=*m->types->nominals;table.declarations=declarations;
    XrXirTypes types=*m->types;types.nominals=&table;
    XrXirModule altered=*m;altered.types=&types;
    XirMetadataFunctionSignature output={0};
    CHECK(xir_metadata_module_signature(&altered,compile,row.equals_function,NULL,NULL,&output)==XR_XIR_BAD_TYPE);
    CHECK(!output.name && !output.parameters);
    free(different);free(declarations);
}
static void unused_origin_argument_refusal(const XrXirCompileContext *compile) {
    const char text[]="fn unused<T>(value:i64)->i64{return value}\nconst value=unused<i64>(7)\n";
    XrXirSourceResult result={0};
    CHECK(source(compile,"unused-origin.xr",text,XR_XIR_PROGRAM,NULL,NULL,&result)==XR_XIR_OK);
    XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    const XrXirModule *m=xr_xir_compile_artifact_module(closed);
    const XrXirModule *original=xr_xir_compile_artifact_module(m->provenance->source);
    XrXirOrigin *origins=malloc((size_t)m->function_count*sizeof(*origins));CHECK(origins);
    memcpy(origins,m->provenance->origins,(size_t)m->function_count*sizeof(*origins));
    uint32_t target=UINT32_MAX;
    for(uint32_t f=0;f<m->function_count;++f) {
        const XrXirFunction *function=&original->functions[origins[f].function];
        if(function->name_length==6 && !memcmp(function->name,"unused",6)){CHECK(target==UINT32_MAX);target=f;}
    }
    CHECK(target!=UINT32_MAX && origins[target].argument_count==1);
    XrXirType invalid=(XrXirType)4;origins[target].arguments=&invalid;
    XrXirProvenance provenance=*m->provenance;provenance.origins=origins;
    XrXirModule altered=*m;altered.provenance=&provenance;
    XirMetadataFunctionSignature output={0};
    CHECK(xir_metadata_module_signature(&altered,compile,target,NULL,NULL,&output)==XR_XIR_BAD_TYPE);
    CHECK(!output.name && !output.parameters);
    free(origins);xr_xir_compile_artifact_free(closed);
}
