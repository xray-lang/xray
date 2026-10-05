/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_source_budget_cases.h - Shared Source metadata remaining budget
 *
 * KEY CONCEPT:
 *   Arena owners and imported verification consume the same remaining quota.
 */
static XrXirStatus library_shared_metadata_operation(const XrXirCompileContext *context,void *opaque) {
    const XrXirLibraryInput *input=opaque;XrXirLibraryCatalog *catalog=NULL;XrXirStatus status=xr_xir_compile_library_catalog_new(context,input,1,&catalog);
    if(status!=XR_XIR_OK){CHECK(!catalog);return status;}
    size_t count=0;const XrModuleResourceBinding *resource=xr_xir_compile_library_catalog_resources(catalog,&count);CHECK(resource&&count==1);
    XrModuleSpec spec={0};spec.resource=resource;XrModuleGraph graph={0};graph.specs=&spec;graph.spec_count=1;
    SourceContext ctx={0};ctx.graph=&graph;ctx.compile=*context;
    void *owned=source_alloc(&ctx,1,1);if(owned){const XrXirModule *module=source_library_module(&ctx,0);if(module)CHECK(module->linkage_kind==XR_XIR_LIBRARY);}
    status=ctx.diagnostic.status;while(ctx.memory)source_release_private(ctx.memory+1);xr_xir_compile_library_catalog_free(catalog);return status;
}
static void library_source_metadata_cases(const XrXirLibraryCatalog *catalog) {
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources(catalog,&count);CHECK(resources&&count==1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(resources[0].checked,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={resources[0].authority,resources[0].logical_path,packet.bytes,packet.length,{0}};xr_sha256(packet.bytes,packet.length,input.sha256);
    library_compile_operation_cases("Source same-ledger allocation/library verify",library_shared_metadata_operation,&input);xr_xir_compile_checked_packet_free(&packet);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);SourceContext ctx={0};ctx.compile=owner.context;
    XrCompileResourceStats before=library_compile_stats(&owner.context);void *owned=source_alloc(&ctx,7,1);CHECK(owned);XrCompileResourceStats required=library_compile_stats(&owner.context);source_release_private(owned);
    CHECK(!ctx.memory&&library_compile_stats(&owner.context).allocated_bytes==required.allocated_bytes);library_compile_owner_drop(&owner);
    for(unsigned mode=0;mode<4;++mode){XrCompileResourceLimits limits=library_compile_limits;limits.allocated_bytes=required.allocated_bytes-(mode==1);CHECK(library_compile_owner_new(&owner,&limits)==XR_XIR_OK);
        ctx=(SourceContext){0};ctx.compile=owner.context;if(mode==2)source_program_compile_fail_at=source_program_compile_attempts;
        owned=source_alloc(&ctx,mode==3?SIZE_MAX:7,1);source_program_compile_fail_at=SIZE_MAX;
        if(!mode){CHECK(owned);source_release_private(owned);CHECK(!ctx.memory);CHECK(!source_alloc(&ctx,1,1)&&ctx.diagnostic.status==XR_XIR_BUDGET);}
        else CHECK(!owned&&!ctx.memory&&ctx.diagnostic.status==(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET));
        CHECK(library_compile_stats(&owner.context).live_bytes==before.live_bytes);library_compile_owner_drop(&owner);
    }
    puts("Source shared metadata and private7 exact/minus1/OOM/overflow/free preserves debit PASS");
}
