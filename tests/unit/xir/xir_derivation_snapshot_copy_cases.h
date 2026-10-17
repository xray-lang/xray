/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derivation_snapshot_copy_cases.h - Complete packed snapshot storage ownership
 *
 * KEY CONCEPT:
 *   Literal storage ownership grants no module permission. The independent
 *   real Source lifecycle gate checks admission after its producer dies.
 */
#ifndef XIR_DERIVATION_SNAPSHOT_COPY_CASES_H
#define XIR_DERIVATION_SNAPSHOT_COPY_CASES_H
typedef struct SnapshotCopyFixture {
    SnapshotDerivationFixture values;
    XrXirSourceModule module;
    XrXirFunctionIdentity functions[6];
    XrXirDeclarations declarations;
} SnapshotCopyFixture;
static void snapshot_copy_fixture(SnapshotCopyFixture *f) {
    memset(f,0,sizeof(*f));snapshot_fixture(&f->values);
    f->module=(XrXirSourceModule){.name="snapshot",.name_length=8};
    f->functions[3].derivation=1;f->functions[5].derivation=2;
    f->declarations=(XrXirDeclarations){.modules=&f->module,.module_count=1,
        .functions=f->functions,.derivations=&f->values.table};
}
static XrXirStatus snapshot_copy_operation(const XrXirCompileContext *context,
    SnapshotCopyFixture *fixture,bool poison) {
    XrXirDeclarations *copy=NULL;
    XrXirStatus status=effect_invocation_declarations_clone(context,&fixture->declarations,6,&copy);
    if(status!=XR_XIR_OK){CHECK(!copy);return status;}
    CHECK(copy && copy->derivations && copy->derivations!=fixture->declarations.derivations &&
        copy->derivations->policies!=&fixture->values.policy &&
        copy->derivations->records!=fixture->values.grants &&
        copy->derivations->policies[0].members!=fixture->values.members);
    if(poison)memset(&fixture->values,0xa5,sizeof(fixture->values));
    status=xir_derivations_structure_verify(context,copy->derivations);
    if(status==XR_XIR_OK)CHECK(copy->derivations->policies[0].member_count==2 &&
        copy->derivations->policies[0].members[1].field==5 &&
        copy->derivations->policies[0].members[1].operations==XR_XIR_DERIVE_WRITE &&
        copy->derivations->records[1].function==5 &&
        copy->derivations->records[1].origins.instructions[1].module.length==8 &&
        !memcmp(copy->derivations->records[1].origins.instructions[1].module.bytes,"snapshot",8));
    effect_invocation_declarations_free(copy);return status;
}
static XrXirCompileContext snapshot_copy_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrCompileResourceStats snapshot_copy_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void snapshot_copy_controls(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};
    size_t outer_blocks=snapshot_live,outer_bytes=snapshot_bytes;
    XrXirCompileContext context=snapshot_copy_context(caps);
    SnapshotCopyFixture fixture;snapshot_copy_fixture(&fixture);
    XrCompileResourceStats baseline=snapshot_copy_stats(&context);
    CHECK(snapshot_copy_operation(&context,&fixture,true)==XR_XIR_OK);
    CHECK(snapshot_copy_stats(&context).live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);CHECK(snapshot_live==outer_blocks && snapshot_bytes==outer_bytes);
    context=snapshot_copy_context(caps);baseline=snapshot_copy_stats(&context);
    size_t blocks=snapshot_live,bytes=snapshot_bytes,start=snapshot_attempts;
    snapshot_copy_fixture(&fixture);CHECK(snapshot_copy_operation(&context,&fixture,false)==XR_XIR_OK);
    size_t sites=snapshot_attempts-start;CHECK(sites);
    XrCompileResourceStats required=snapshot_copy_stats(&context);
    CHECK(required.live_bytes==baseline.live_bytes && snapshot_live==blocks && snapshot_bytes==bytes);
    for(size_t n=0;n<sites;++n) {
        snapshot_copy_fixture(&fixture);snapshot_fail_at=snapshot_attempts+n;snapshot_injected=false;
        XrXirStatus status=snapshot_copy_operation(&context,&fixture,false);
        snapshot_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && snapshot_injected);
        XrCompileResourceStats paid=snapshot_copy_stats(&context);
        CHECK(paid.live_bytes==baseline.live_bytes && snapshot_live==blocks && snapshot_bytes==bytes);
        CHECK(snapshot_copy_operation(&context,&fixture,false)==XR_XIR_OK);
        XrCompileResourceStats after=snapshot_copy_stats(&context);
        CHECK(after.live_bytes==baseline.live_bytes && after.allocated_bytes>paid.allocated_bytes && after.work>paid.work);
        CHECK(snapshot_live==blocks && snapshot_bytes==bytes);
    }
    xr_compile_resources_release(context.resources);CHECK(snapshot_live==outer_blocks && snapshot_bytes==outer_bytes);
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=caps;
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;CHECK(exact);*cap=exact-less;
        context=snapshot_copy_context(limits);baseline=snapshot_copy_stats(&context);snapshot_copy_fixture(&fixture);
        XrXirStatus status=snapshot_copy_operation(&context,&fixture,false);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK) && snapshot_copy_stats(&context).live_bytes==baseline.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(snapshot_live==outer_blocks && snapshot_bytes==outer_bytes);
    }
    printf("Packed declaration snapshot: poisoned producer, all%zu actual allocations entered OOM, same ledger retries, six axes and physical baseline PASS\n",sites);
}
#endif // XIR_DERIVATION_SNAPSHOT_COPY_CASES_H
