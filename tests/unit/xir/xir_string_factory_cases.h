/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_factory_cases.h - Independent complete string Program factory faults
 *
 * KEY CONCEPT:
 *   Factory faults and runtime faults have separate complete physical censuses.
 */
#include "xir/xxir_program_packet_internal.h"
#ifndef XIR_STRING_FACTORY_CASES_H
#define XIR_STRING_FACTORY_CASES_H
typedef struct StringFactoryCensus {
    size_t metadata_sites,runtime_sites;
    XrCompileResourceStats stats;
} StringFactoryCensus;
static const XrCompileResourceLimits string_factory_limits={
    UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static XrXirStatus string_factory_operation(const XrXirCompileContext *context,
    const XrXirProgramPacket *proof) {
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *artifact=NULL;XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_checked_read_lower(context,proof->bytes,proof->length,
        &target,&artifact,NULL);
    if (status!=XR_XIR_OK) CHECK(!artifact);
    else {
        XrXirArtifact *original=artifact;
        status=string_vm_program_take(&artifact,NULL,&program);
        CHECK(status==XR_XIR_OK?program && !artifact:!program && artifact==original);
    }
    if (status!=XR_XIR_OK) CHECK(!program);
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(artifact);
    return status;
}
static XrXirStatus string_factory_trial(const XrXirProgramPacket *proof,
    const XrCompileResourceLimits *limits,size_t metadata_failure,size_t runtime_failure,
    StringFactoryCensus *census) {
    const uint64_t compiler_bytes=metadata_bytes,compiler_blocks=metadata_live;
    const size_t runtime_blocks=runtime_live,runtime_base=runtime_bytes;
    metadata_attempts=runtime_attempts=0;metadata_fail_at=metadata_failure;runtime_fail_at=runtime_failure;
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus created=xr_compile_resources_new(limits,&context.resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:
        created==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrCompileResourceStats initial={0};
    if (status==XR_XIR_OK) {
        CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
        status=string_factory_operation(&context,proof);
    }
    const size_t metadata_sites=metadata_attempts,runtime_sites=runtime_attempts;
    metadata_fail_at=runtime_fail_at=SIZE_MAX;
    XrCompileResourceStats stats={0};
    if (context.resources) {
        CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.live_bytes==initial.live_bytes);
        xr_compile_resources_release(context.resources);
    }
    CHECK(metadata_bytes==compiler_bytes && metadata_live==compiler_blocks &&
        runtime_live==runtime_blocks && runtime_bytes==runtime_base);
    if (metadata_failure!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && metadata_sites>metadata_failure);
    if (runtime_failure!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_sites>runtime_failure);
    if (census) *census=(StringFactoryCensus){metadata_sites,runtime_sites,stats};
    return status;
}
static void string_factory_failures(void) {
    for (uint32_t variant=0;variant<2;++variant) {
        reset_metadata();runtime_fail_at=SIZE_MAX;
        SourceFixtureOwner producer={0};source_fixture_owner_new(&producer);
        XrXirArtifact *artifact=string_fixture(&producer.context,variant);
        XrXirProgramPacket proof=xir_program_packet_borrow(artifact);
        CHECK(proof.bytes && proof.length && proof.identity && xr_xir_compile_artifact_layout(artifact, 0));
        StringFactoryCensus census={0};
        CHECK(string_factory_trial(&proof,&string_factory_limits,SIZE_MAX,SIZE_MAX,&census)==XR_XIR_OK);
        CHECK(census.metadata_sites && census.stats.allocated_bytes && census.stats.peak_bytes && census.stats.work);
        for (size_t site=0;site<census.metadata_sites;++site)
            string_factory_trial(&proof,&string_factory_limits,site,SIZE_MAX,NULL);
        for (size_t site=0;site<census.runtime_sites;++site)
            string_factory_trial(&proof,&string_factory_limits,SIZE_MAX,site,NULL);
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=string_factory_limits;
            uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            *bound=(axis==0?census.stats.allocated_bytes:axis==1?census.stats.peak_bytes:census.stats.work)-minus;
            CHECK(string_factory_trial(&proof,&limits,SIZE_MAX,SIZE_MAX,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        }
        xr_xir_compile_artifact_free(artifact);source_fixture_owner_free(&producer);
        CHECK(!metadata_live && !metadata_bytes && !runtime_live && !runtime_bytes);
        printf("String complete Program factory: variant=%u metadata=%zu runtime=%zu physical faults, six axes\n",
            variant,census.metadata_sites,census.runtime_sites);
    }
}
#endif // XIR_STRING_FACTORY_CASES_H
