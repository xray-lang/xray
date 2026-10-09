/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_construction_owner_cases.h - Construction ownership and failure checks
 *
 * KEY CONCEPT:
 *   Receiving owners preserve complete facts across producer destruction.
 *   Failure paths retain output ownership and charged resource limits.
 */
#ifndef XIR_CONSTRUCTION_OWNER_CASES_H
#define XIR_CONSTRUCTION_OWNER_CASES_H
static void construction_dense_owner(void) {
    /* One struct row and two absent bindings: owner56 + row24 + kind4 + fields8.
     * clone shape3, constructor shape2, callocs57+25+5, row2, copy1+8 = 103.
     * This observation fixture creates no executable Artifact or authority. */
    _Static_assert(sizeof(XrXirConstruction)==56 && sizeof(XrXirConstructionRow)==24,
        "Independent x64 construction allocation formula");
    for (unsigned mode=0;mode<11;++mode) {
        reset_observer();XrXirCompileContext producer=context_new(UINT64_MAX);
        XrXirNominalField fields[]={{{"a",1},XR_XIR_I64,0},{{"b",1},XR_XIR_I64,0}};
        XrXirNominalDeclaration declaration={0};declaration.module=(XrXirLiteral){"m",1};
        declaration.name=(XrXirLiteral){"Pair",4};declaration.kind=XR_XIR_NOMINAL_STRUCT;
        declaration.fields=fields;declaration.field_count=2;
        XrXirNominalTable table={&declaration,1,NULL};XrXirTypes types={0};types.nominals=&table;
        uint32_t helpers[2]={0,0};XrXirConstructionRow row={0,helpers,2};
        XrXirConstruction *source=NULL,*copy=NULL;
        CHECK(xr_xir_compile_construction_new(&producer,&types,&row,1,&source)==XR_XIR_OK);
        helpers[0]=UINT32_MAX;
        CHECK(xr_xir_compile_construction_row(source,0)->field_initializers[0]==0);
        uint64_t exact_bytes=sizeof(XrCompileResources)+4*sizeof(CompileAllocation)+92;
        XrCompileResourceLimits caps={UINT64_MAX,UINT64_MAX,UINT64_MAX};
        if (mode>=5) {
            unsigned axis=(mode-5)/2,minus=(mode-5)%2;
            if (!axis) caps.allocated_bytes=exact_bytes-minus;
            else if (axis==1) caps.live_bytes=exact_bytes-minus;
            else caps.work=104-minus;
        }
        XrXirCompileContext receiver={0};receiver.limits=xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_new(&caps,&receiver.resources)==XR_COMPILE_RESOURCE_OK);
        size_t start=attempts,baseline_live=live,baseline_physical=physical;
        if (mode>=1 && mode<=4) fail_at=start+mode-1;
        XrXirStatus expected=mode>=1 && mode<=4?XR_XIR_OUT_OF_MEMORY:
            mode>=5 && (mode-5)%2?XR_XIR_BUDGET:XR_XIR_OK;
        CHECK(xir_construction_clone(&receiver,&types,source,&copy)==expected);
        XrCompileResourceStats used={0};CHECK(xr_compile_resources_stats(receiver.resources,&used)==XR_COMPILE_RESOURCE_OK);
        if (expected==XR_XIR_OK) {
            CHECK(copy && copy!=source && copy->context.resources==receiver.resources && attempts==start+4);
            CHECK(used.work==104 && used.allocated_bytes==exact_bytes && used.peak_bytes==exact_bytes);
            const XrXirConstructionRow *got=xr_xir_compile_construction_row(copy,0);
            CHECK(got && got->field_count==2 && !got->default_initializer &&
                !got->field_initializers[0] && !got->field_initializers[1]);
            CHECK(got!=xr_xir_compile_construction_row(source,0) &&
                got->field_initializers!=xr_xir_compile_construction_row(source,0)->field_initializers);
            if (!mode) {
                XrXirConstruction *untouched=copy;size_t before_attempts=attempts;
                CHECK(xir_construction_clone(&receiver,&types,source,&untouched)==XR_XIR_BAD_STRUCTURE && untouched==copy);
                CHECK(attempts==before_attempts);
                XrXirConstruction *bad=NULL;
                CHECK(xir_construction_clone(&receiver,&types,NULL,&bad)==XR_XIR_BAD_STRUCTURE && !bad);
                declaration.field_count=1;
                CHECK(xir_construction_clone(&receiver,&types,source,&bad)==XR_XIR_BAD_STRUCTURE && !bad);
                declaration.field_count=2;declaration.kind=XR_XIR_NOMINAL_CLASS;
                CHECK(xir_construction_clone(&receiver,&types,source,&bad)==XR_XIR_BAD_STRUCTURE && !bad);
                declaration.kind=XR_XIR_NOMINAL_STRUCT;table.count=0;
                CHECK(xir_construction_clone(&receiver,&types,source,&bad)==XR_XIR_BAD_STRUCTURE && !bad);
                table.count=1;CHECK(attempts==before_attempts);
            }
        } else {
            CHECK(!copy && live==baseline_live && physical==baseline_physical);
            if (expected==XR_XIR_OUT_OF_MEMORY) CHECK(attempts==start+mode);
            else {
                /* Original receiving limits and consumed work/storage remain. */
                CHECK(xir_construction_clone(&receiver,&types,source,&copy)==XR_XIR_BUDGET && !copy);
                CHECK(live==baseline_live && physical==baseline_physical);
            }
        }
        fail_at=SIZE_MAX;
        xr_xir_compile_construction_free(source);xr_compile_resources_release(producer.resources);
        if (copy) {
            const XrXirConstructionRow *got=xr_xir_compile_construction_row(copy,0);
            CHECK(got && got->field_count==2 && !got->field_initializers[0] && !got->field_initializers[1]);
        }
        xr_xir_compile_construction_free(copy);xr_compile_resources_release(receiver.resources);
        CHECK(!live && !physical);
    }
}
static void construction_cross_owner(void) {
    construction_dense_owner();
    size_t sites=0;
    for (size_t failure=SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext producer=context_new(UINT64_MAX);
        XrXirArtifact *lowered=owner_lowered(&producer),*checked=NULL;
        XrXirProgramProof proof=xr_xir_compile_program_proof(lowered);
        CHECK(xr_xir_compile_checked_read(&producer,proof.bytes,proof.length,&checked,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);lowered=NULL;
        const XrXirConstruction *facts=xr_xir_compile_artifact_construction(checked);
        CHECK(facts && xr_xir_compile_construction_count(facts)==0);
        XrXirCompileContext receiver=context_new(UINT64_MAX);
        CHECK(receiver.resources!=producer.resources);
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(receiver.resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirArtifact *copy=NULL;
        size_t start=attempts,physical_before=physical,live_before=live;
        if (failure!=SIZE_MAX) fail_at=start+failure;
        XrXirStatus status=xr_xir_compile_recheck_v2(&receiver,
            xr_xir_compile_artifact_module(checked),facts,&copy,NULL);
        CHECK(xr_compile_resources_stats(receiver.resources,&after)==XR_COMPILE_RESOURCE_OK);
        if (failure==SIZE_MAX) {
            CHECK(status==XR_XIR_OK && copy && copy!=checked);
            sites=attempts-start;CHECK(sites);
            CHECK(xr_xir_compile_artifact_context(copy)->resources==receiver.resources);
            CHECK(xr_xir_compile_artifact_construction(copy)!=facts);
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !copy);
            CHECK(after.live_bytes==before.live_bytes && physical==physical_before && live==live_before);
            CHECK(attempts==start+failure+1);
            copy=NULL;fail_at=SIZE_MAX;
            CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
        }
        xr_xir_compile_artifact_free(checked);checked=NULL;
        xr_compile_resources_release(producer.resources);producer.resources=NULL;
        if (copy) {
            CHECK(xr_xir_compile_artifact_verify(copy,NULL)==XR_XIR_OK);
            CHECK(xr_xir_compile_construction_count(xr_xir_compile_artifact_construction(copy))==0);
            const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
            CHECK(xr_xir_compile_lower(copy,&target,&lowered,NULL)==XR_XIR_OK);
            xr_xir_compile_artifact_free(copy);copy=NULL;
            XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
            XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            xr_xir_compile_program_drop(program);
            CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
            CHECK(result.type==XR_XIR_I64 && result.payload==42);xr_xir_value_drop(&result);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        }
        xr_compile_resources_release(receiver.resources);
        CHECK(!live && !physical);
        if (failure==SIZE_MAX) failure=0;
        else if (++failure==sites) break;
    }
    printf("Construction receiving-owner recheck OOM points: %zu\n",sites);
}
#endif // XIR_CONSTRUCTION_OWNER_CASES_H
