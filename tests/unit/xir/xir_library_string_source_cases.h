/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_string_source_cases.h - Original literal and growth boundaries
 */
typedef struct StringLiteralFixture {bool grow;unsigned mode;} StringLiteralFixture;
static XrXirStatus string_literal_operation(const XrXirCompileContext *context,void *opaque) {
    const StringLiteralFixture *fixture=opaque;SourceContext ctx={0};ctx.compile=*context;
    XrXirLiteral original[16];uint32_t id=99;
    if(fixture->grow){for(unsigned i=0;i<16;++i)original[i]=(XrXirLiteral){"owned",5};ctx.literals=original;ctx.literal_count=ctx.literal_capacity=16;}
    if(fixture->mode==4){if(fixture->grow)ctx.literal_count=ctx.literal_capacity=UINT32_MAX/2+1;else ctx.literal_count=UINT32_MAX;}
    bool ok=source_literal_append(&ctx,NULL,(!fixture->grow&&fixture->mode==5)?NULL:"",
        fixture->grow&&fixture->mode==3?(size_t)UINT32_MAX+1:!fixture->grow&&fixture->mode==5?1:0,
        fixture->grow&&fixture->mode==5?NULL:&id);
    if(ok){CHECK(fixture->mode==0&&id==(fixture->grow?16u:0u));CHECK(ctx.literal_count==(fixture->grow?17u:1u)&&!ctx.functions);
        CHECK(ctx.literals[id].length==0);if(fixture->grow)CHECK(ctx.literal_capacity==32&&ctx.literals!=original&&!memcmp(ctx.literals,original,sizeof(original)));}
    else {CHECK(id==99);CHECK(ctx.literal_count==(fixture->mode==4?(fixture->grow?UINT32_MAX/2+1:UINT32_MAX):fixture->grow?16u:0u));if(fixture->grow)CHECK(ctx.literals==original);}
    while(ctx.memory)source_release_private(ctx.memory+1);
    return ok?XR_XIR_OK:ctx.diagnostic.status;
}
static void string_literal_boundaries(bool grow) {
    StringLiteralFixture fixture={grow,0};LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);CHECK(string_literal_operation(&owner.context,&fixture)==XR_XIR_OK);
    XrCompileResourceStats required=library_compile_stats(&owner.context);library_compile_owner_drop(&owner);
    for(unsigned mode=0;mode<6;++mode){XrCompileResourceLimits caps=library_compile_limits;
        if(!grow){caps.allocated_bytes=required.allocated_bytes;if(mode==1)--caps.allocated_bytes;if(mode==2)caps.work=1;}
        else {if(mode==1)caps.work=required.work-1;if(mode==2)caps.allocated_bytes=required.allocated_bytes-1;}
        CHECK(library_compile_owner_new(&owner,&caps)==XR_XIR_OK);fixture.mode=mode<3?0:mode;
        if(!grow&&mode==3){fixture.mode=0;source_program_compile_fail_at=source_program_compile_attempts;source_program_compile_injected=false;}
        XrXirStatus status=string_literal_operation(&owner.context,&fixture);source_program_compile_fail_at=SIZE_MAX;
        XrXirStatus expected=mode==0?XR_XIR_OK:(!grow&&mode==3)?XR_XIR_OUT_OF_MEMORY:mode==5?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;
        CHECK(status==expected);if(!grow&&mode==3)CHECK(source_program_compile_injected);library_compile_owner_drop(&owner);
    }
    fixture.mode=0;library_compile_operation_cases(grow?"String literal growth17":"String literal empty",string_literal_operation,&fixture);
}
static void library_string_source_boundaries(void) {
    string_literal_boundaries(false);LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    SourceContext ctx={0};ctx.compile=owner.context;SourceLibraryMap map={0};map.literal_begin=100;map.literal_count=1;
    XrXirInstruction op={XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}},output={0};
    CHECK(!source_library_instruction(&ctx,&map,&op,&output));CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE&&!strcmp(ctx.diagnostic.message,"library literal identity is invalid"));
    library_compile_owner_drop(&owner);
}
static void library_string_growth_boundaries(void) {string_literal_boundaries(true);}
static void library_string_source_negatives(const XrXirSourceRequest *request) {
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes,rlive=runtime_live,rbytes=runtime_bytes;
    XrXirSourceRequest negative=*request;negative.entry_path=XR_SOURCE_FIXTURES "/private.xr";XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&negative,&result,&diagnostic,NULL);
    fprintf(stderr,"negative private.xr status%u reason%s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_BAD_STRUCTURE&&!result.checked&&!result.snapshot&&!strcmp(diagnostic.message,"import requires an exported declaration"));
    xr_xir_compile_source_result_free(&result);CHECK(source_program_compile_live==blocks&&source_program_compile_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
}
