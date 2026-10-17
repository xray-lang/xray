/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_compact_receiving_cases.h - Full-byte receiving on one owner
 */
#ifndef XIR_SOURCE_COMPACT_RECEIVING_CASES_H
#define XIR_SOURCE_COMPACT_RECEIVING_CASES_H
static XrCompileResourceStats source_compact_stats(const SourceContext *ctx) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(ctx->compile.resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void source_compact_links(SourceContext *ctx) {
    SourceMemory **link=&ctx->memory;size_t count=0;
    for(SourceMemory *p=ctx->memory;p;p=p->next){CHECK(p->previous==link && ++count<64);link=&p->next;}
    link=&ctx->retired_vectors;count=0;
    for(SourceMemory *p=ctx->retired_vectors;p;p=p->next){CHECK(p->previous==link && ++count<64);link=&p->next;}
}
static unsigned char source_compact_byte(size_t i) { return (unsigned char)((i*37+19)^(i>>3)); }
static void source_compact_oracle(const unsigned char *data,size_t bytes) {
    for(size_t i=0;i<bytes;++i)CHECK(data[i]==source_compact_byte(i));
}
/* These byte patterns exercise complete record widths, not query authority. */
static void source_compact_widths(void) {
    const size_t widths[]={sizeof(XrXirSourceDeclaration),sizeof(XrXirSourceReference),sizeof(XrXirSourceExpression),
        sizeof(XrXirSourceDeclarationSyntax),sizeof(XrXirSourceRange),sizeof(XrXirFunction),sizeof(SourceFunction),
        sizeof(XrXirFunctionIdentity),sizeof(XrXirGeneric)};
    for(size_t w=0;w<sizeof(widths)/sizeof(*widths);++w)for(uint32_t count=1;count<=3;++count){
        SourceContext ctx={0};CHECK(allocation_private_context(&ctx,(XrCompileResourceLimits){1048576,524288,100000}));
        uint32_t capacity=count+5;size_t bytes=(size_t)count*widths[w];unsigned char *original=source_alloc(&ctx,capacity,widths[w]);CHECK(original);
        for(size_t i=0;i<(size_t)capacity*widths[w];++i)CHECK(!original[i]);
        for(size_t i=0;i<bytes;++i)original[i]=source_compact_byte(i);
        memset(original+bytes,0xCE,(size_t)(capacity-count)*widths[w]);
        size_t attempts=source_fixture_compile_attempts;XrCompileResourceStats before=source_compact_stats(&ctx);
        const unsigned char *copy=source_storage_compact(&ctx,original,count,&capacity,widths[w]);
        XrCompileResourceStats after=source_compact_stats(&ctx);CHECK(copy && copy!=original && capacity==count &&
            ctx.diagnostic.status==XR_XIR_OK && source_fixture_compile_attempts==attempts+1 &&
            after.work-before.work==1+sizeof(SourceMemory)+sizeof(void *)+bytes &&
            after.allocated_bytes-before.allocated_bytes==sizeof(CompileAllocation)+sizeof(SourceMemory)+bytes);
        source_compact_oracle(copy,bytes);source_compact_oracle(original,bytes);source_compact_links(&ctx);
        uint64_t work=after.work;attempts=source_fixture_compile_attempts;
        CHECK(source_storage_compact(&ctx,copy,count,&capacity,widths[w])==copy &&
            source_fixture_compile_attempts==attempts && source_compact_stats(&ctx).work==work);
        source_vector_retired_free(&ctx);source_compact_links(&ctx);source_compact_oracle(copy,bytes);
        source_release_private((void *)copy);CHECK(!ctx.memory && !ctx.retired_vectors);
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}
static void source_compact_work_cuts(void) {
    const uint32_t count=3;const size_t width=sizeof(XrXirSourceExpression),bytes=(size_t)count*width;
    uint64_t exact=1+sizeof(SourceMemory)+sizeof(void *)+bytes;
    for(uint64_t cut=0;cut<=exact;++cut){SourceContext ctx={0};CHECK(allocation_private_context(&ctx,(XrCompileResourceLimits){1048576,524288,100000}));
        uint32_t capacity=8;unsigned char *original=source_alloc(&ctx,capacity,width);CHECK(original);
        for(size_t i=0;i<bytes;++i)original[i]=source_compact_byte(i);
        uint64_t entry=source_compact_stats(&ctx).work;ctx.compile.resources->limits.work=entry+cut;
        const void *copy=source_storage_compact(&ctx,original,count,&capacity,width);
        CHECK((copy!=NULL)==(cut==exact) && ctx.diagnostic.status==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
        source_compact_oracle(original,bytes);source_compact_links(&ctx);
        if(cut<exact){CHECK(capacity==8 && !ctx.retired_vectors);size_t attempts=source_fixture_compile_attempts;
            uint64_t work=source_compact_stats(&ctx).work;
            CHECK(!source_storage_compact(&ctx,original,count,&capacity,width) && capacity==8 &&
                source_fixture_compile_attempts==attempts && source_compact_stats(&ctx).work==work);}
        else {CHECK(capacity==count);source_compact_oracle(copy,bytes);}
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}
static void source_compact_faults(void) {
    for(unsigned fault=0;fault<6;++fault){SourceContext ctx={0};CHECK(allocation_private_context(&ctx,(XrCompileResourceLimits){1048576,524288,100000}));
        uint32_t capacity=8;unsigned char *original=source_alloc(&ctx,capacity,16);CHECK(original);memset(original,0xCE,128);
        size_t attempts=source_fixture_compile_attempts;
        if(!fault){source_fixture_compile_fail_at=attempts;source_fixture_compile_injected=false;}
        const void *copy=source_storage_compact(&ctx,fault==3?NULL:original,fault==1?9:fault==2?0:3,
            &capacity,fault==4?0:fault==5?SIZE_MAX:16);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(!copy && capacity==8 && ctx.diagnostic.status==(!fault?XR_XIR_OUT_OF_MEMORY:
            fault<4?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET));
        for(size_t i=0;i<128;++i)CHECK(original[i]==0xCE);
        if(!fault)CHECK(source_fixture_compile_injected && source_fixture_compile_attempts==attempts+1);
        else CHECK(source_fixture_compile_attempts==attempts);
        attempts=source_fixture_compile_attempts;CHECK(!source_storage_compact(&ctx,original,3,&capacity,16) &&
            source_fixture_compile_attempts==attempts && capacity==8);source_compact_links(&ctx);
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}
static XrXirStatus source_compact_whole(XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    SourceContext ctx={0};if(!allocation_private_context(&ctx,limits))return ctx.diagnostic.status;
    uint32_t capacity=8;size_t width=sizeof(XrXirSourceExpression),bytes=3*width;
    unsigned char *original=source_alloc(&ctx,capacity,width);const void *copy=NULL;
    if(original){for(size_t i=0;i<bytes;++i)original[i]=source_compact_byte(i);
        copy=source_storage_compact(&ctx,original,3,&capacity,width);}
    if(copy){CHECK(capacity==3);source_compact_oracle(copy,bytes);source_compact_oracle(original,bytes);source_compact_links(&ctx);}
    XrXirStatus status=ctx.diagnostic.status;if(stats)*stats=source_compact_stats(&ctx);
    allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return status;
}
static XrXirStatus source_compact_genuine(XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrXirCompileContext context=allocation_context(limits);XrCompilerSession *session=NULL;
    XrCompilerSessionStatus made=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char source[]="fn add(x:i64)->i64{return x+1}\nexport fn answer()->i64{return add(41)}\n";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-compact-receiving",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_LIBRARY};
    XrXirSourceText text={NULL,source,sizeof(source)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if(status==XR_XIR_OK)status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    xr_compile_session_free(session);memset(source,0xCE,sizeof(source));
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL;
    if(status==XR_XIR_OK){const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result.snapshot);bool found=false;
        CHECK(view && view->complete && syntax && syntax->declaration_count==view->declaration_count);
        for(uint32_t i=0;i<view->declaration_count;++i)if(!strcmp(view->declarations[i].name,"answer")){
            const XrXirSourceDeclaration *d=&view->declarations[i];const XrXirSourceDeclarationSyntax *s=&syntax->declarations[i];
            CHECK(!found && d->kind==XR_XIR_SOURCE_FUNCTION && d->exported && !d->parameter_count &&
                s->role==XR_XIR_SOURCE_SYNTAX_NAME && s->name.line==2 && s->name.column==11 && s->name.end_line==2 && s->name.end_column==17);found=true;}
        CHECK(found);status=xr_xir_compile_checked_write(result.checked,&packet,NULL);}
    xr_xir_compile_source_result_free(&result);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL);
    if(packet.bytes)memset(packet.bytes,0xCE,packet.length);xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(received,NULL);
    xr_xir_compile_artifact_free(received);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    allocation_context_close(&context);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return status;
}
static void source_compact_receiving_cases(void) {
    source_compact_widths();source_compact_work_cuts();source_compact_faults();
    XrCompileResourceStats physical={0},genuine={0};
    CHECK(source_compact_whole((XrCompileResourceLimits){1048576,524288,100000},&physical)==XR_XIR_OK);
    CHECK(source_compact_genuine(allocation_limits(),&genuine)==XR_XIR_OK);
    for(unsigned fixture=0;fixture<2;++fixture)for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){
        XrCompileResourceStats measured=fixture?genuine:physical;XrCompileResourceLimits caps=fixture?allocation_limits():(XrCompileResourceLimits){1048576,524288,100000};
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        XrXirStatus status=fixture?source_compact_genuine(caps,NULL):source_compact_whole(caps,NULL);
        CHECK(status==(minus?XR_XIR_BUDGET:XR_XIR_OK));}
    fprintf(stderr,"Source complete-copy receiving: nine actual query/syntax/function record widths across three counts, independent complete byte oracle and original zeroed allocator, exact link/allocation/copy fees, alias no-op and ownership active/retired chains, every work unit and same finite-owner sticky refusal, sole OOM and five bad extent/overflow refusals, physical six axes%llu/%llu/%llu; genuine Source query/name syntax ranges and independent Checked receiving after producer/session/packet death, whole genuine six axes%llu/%llu/%llu exact/minusone, physical0 PASS\n",
        (unsigned long long)physical.allocated_bytes,(unsigned long long)physical.peak_bytes,(unsigned long long)physical.work,
        (unsigned long long)genuine.allocated_bytes,(unsigned long long)genuine.peak_bytes,(unsigned long long)genuine.work);
}
#endif // XIR_SOURCE_COMPACT_RECEIVING_CASES_H
