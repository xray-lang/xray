/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product.c - Opaque source ownership and ordinary packet re-admission
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
static size_t source_physical_total,source_physical_peak,source_base_bytes;
static void *source_observed_allocate(size_t bytes) {
    void *memory=runtime_malloc(bytes);
    if (memory) {
        CHECK(bytes<=SIZE_MAX-source_physical_total);source_physical_total+=bytes;
        CHECK(runtime_bytes>=source_base_bytes);
        size_t held=runtime_bytes-source_base_bytes;
        if (held>source_physical_peak) source_physical_peak=held;
    }
    return memory;
}
#undef xr_malloc
#define xr_malloc(bytes) source_observed_allocate(bytes)
#include "base/xcompile_resources.c"
#undef xr_malloc
#define xr_malloc(bytes) runtime_malloc(bytes)
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_emit_c.c"
#include "program/xr_xir_source_product.c"
#include "xir_source_product_execution.h"
#include "xir_source_product_identity.h"
#include "xir_native_projection_fixture.h"
#include "xir_native_projection_faults.h"
static void product_guards(const XrXirSourceProductRequest *request) {
    XrXirSourceProduct *held=(XrXirSourceProduct *)(uintptr_t)17,*before=held;
    CHECK(xr_xir_compile_source_product_build(request,&held,NULL)==XR_XIR_BAD_STRUCTURE && held==before);
    XrXirSourceProduct *zero=NULL;XrXirSourceProductRequest malformed=*request;
    malformed.target.abi_version=15;
    CHECK(xr_xir_compile_source_product_build(&malformed,&zero,NULL)==XR_XIR_BAD_STRUCTURE && !zero);
    malformed=*request;malformed.source.linkage_kind=XR_XIR_LIBRARY;
    CHECK(xr_xir_compile_source_product_build(&malformed,&zero,NULL)==XR_XIR_BAD_STRUCTURE && !zero);
    CHECK(xr_xir_compile_source_product_build(request,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!xr_xir_compile_source_product_facts(NULL) && !xr_xir_compile_source_product_view(NULL));
    XrXirSourceProductPacketView view={(const uint8_t *)(uintptr_t)19,23},saved=view;
    CHECK(xr_xir_compile_source_product_packet(NULL,XR_XIR_SOURCE_PRODUCT_SOURCE,&view)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&view,&saved,sizeof(view)));
}
static void product_build_oom(const XrXirSourceProductRequest *request) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
        XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
        XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrCompileResourceStatus allocation=xr_compile_resources_new(&limits,&resources);
        XrXirStatus status=xir_compile_resource_status(allocation);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        if (status==XR_XIR_OK) {
            XrCompilerSessionStatus created=xr_compile_session_new(resources,&session);
            status=created==XR_COMPILER_SESSION_OK ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY;
        }
        if (status==XR_XIR_OK) {
            XrXirSourceProductRequest owned=*request;
            owned.source.context=&context;owned.source.session=session;
            status=xr_xir_compile_source_product_build(&owned,&product,&diagnostic);
        }
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Source allocation %zu status=%u stage=%u\n",point-1,status,diagnostic.stage);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !product);
        }
        runtime_fail_at=SIZE_MAX;xr_compile_session_free(session);
        xr_compile_resources_release(resources);
        xr_xir_compile_source_product_free(product);
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("complete Source/Session/ledger actual inserted allocations=%zu; failure publication/physical baseline PASS\n",sites);
}
static XrXirStatus product_limited_pipeline(const XrXirSourceProductRequest *request,
    const XrCompileResourceLimits *limits,unsigned stop,XrCompileResourceStats *stats) {
    size_t live=runtime_live,bytes=runtime_bytes;
    source_base_bytes=bytes;source_physical_total=source_physical_peak=0;
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    XrXirSourceProduct *product=NULL;XrXirProgram *program=NULL;
    XrXirSourceProductDiagnostic diagnostic={0};XrXirCSource code={0};
    XrXirNativeProjection projection={0};
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_new(limits,&resources));
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    if (status==XR_XIR_OK) {
        XrCompilerSessionStatus created=xr_compile_session_new(resources,&session);
        status=created==XR_COMPILER_SESSION_OK ? XR_XIR_OK :
            created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    }
    if (status==XR_XIR_OK) {
        XrXirSourceProductRequest owned=*request;owned.source.context=&context;owned.source.session=session;
        status=xr_xir_compile_source_product_build(&owned,&product,&diagnostic);
    }
    xr_compile_session_free(session);
    if (stop==4 && status==XR_XIR_OK) {
        XrToolchainInput input={XR_TOOLCHAIN_BINDING_SCHEMA_VERSION,XR_TOOLCHAIN_BINDING_PROVIDER_MSVC,{0},
            "fixture-cl","fixture-windows-x86_64","fixture-opt=2",{{1}},{{2}},{{3}}};
        XrToolchainBinding binding;
        XrToolchainBindingStatus built=xr_compile_toolchain_binding_build(resources,&input,&binding);
        status=built==XR_TOOLCHAIN_BINDING_OK ? XR_XIR_OK : XR_XIR_BUDGET;
        XrXirNativeProjectionRequest project={product,&binding,"original_source",16777216};
        if (status==XR_XIR_OK) status=xr_compile_native_projection_build(&project,&projection,NULL);
    } else {
        if (status==XR_XIR_OK && stop>=1) status=xr_xir_compile_source_product_verify(product,16777216,NULL);
        if (status==XR_XIR_OK && stop>=2) status=xr_xir_compile_source_product_emit(product,"original_source",16777216,&code);
        if (status==XR_XIR_OK && stop>=3) status=xr_xir_compile_source_product_vm_take(product,&program);
    }
    if (resources) {
        CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats->allocated_bytes==source_physical_total && stats->peak_bytes==source_physical_peak);
        CHECK(stats->live_bytes==runtime_bytes-bytes);
        CHECK(stats->allocated_bytes<=limits->allocated_bytes && stats->peak_bytes<=limits->live_bytes && stats->work<=limits->work);
    }
    xr_compile_resources_release(resources);
    xr_xir_compile_c_source_free(&code);xr_xir_compile_program_drop(program);
    xr_compile_native_projection_free(&projection);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(runtime_live==live && runtime_bytes==bytes);source_base_bytes=0;
    return status;
}
static void product_cumulative_limits(const XrXirSourceProductRequest *request) {
    for (unsigned stage=0;stage<5;++stage) {
        const XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
        XrCompileResourceStats normal={0},actual={0};
        CHECK(product_limited_pipeline(request,&unlimited,stage,&normal)==XR_XIR_OK);
        for (unsigned axis=0;axis<3;++axis) {
            XrCompileResourceLimits limits=unlimited;
            if (axis==0) limits.allocated_bytes=normal.allocated_bytes;
            if (axis==1) limits.live_bytes=normal.peak_bytes;
            if (axis==2) limits.work=normal.work;
            CHECK(product_limited_pipeline(request,&limits,stage,&actual)==XR_XIR_OK);
            CHECK(normal.allocated_bytes==actual.allocated_bytes && normal.peak_bytes==actual.peak_bytes && normal.work==actual.work);
            if (axis==0) --limits.allocated_bytes;
            if (axis==1) --limits.live_bytes;
            if (axis==2) --limits.work;
            CHECK(product_limited_pipeline(request,&limits,stage,&actual)==XR_XIR_BUDGET);
        }
        printf("cumulative stage=%u physical_total=%llu peak=%llu work=%llu exact/minus1 PASS\n",stage,
            (unsigned long long)normal.allocated_bytes,(unsigned long long)normal.peak_bytes,(unsigned long long)normal.work);
    }
}
static void product_verify_oom(const XrXirSourceProduct *product) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_product_verify(product,16777216,NULL);
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"verify allocation %zu status=%u\n",point-1,status);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
        }
        runtime_fail_at=SIZE_MAX;CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("ordinary Source/closed/lower re-admission actual inserted allocations=%zu; physical baseline PASS\n",sites);
}
static void product_emit_oom(const XrXirSourceProduct *product) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0,length=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirCSource code={0};runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_product_emit(product,"original_source",16777216,&code);
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;length=code.length;}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !code.text && !code.length);
        runtime_fail_at=SIZE_MAX;xr_xir_compile_c_source_free(&code);CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    XrXirCSource code={0};CHECK(xr_xir_compile_source_product_emit(product,"original_source",length+1,&code)==XR_XIR_OK);
    CHECK(code.length==length);xr_xir_compile_c_source_free(&code);
    CHECK(xr_xir_compile_source_product_emit(product,"original_source",length,&code)==XR_XIR_BUDGET && !code.text && !code.length);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("native projection allocations=%zu bytes_exact=%zu/minus1; precise OOM/zero output/physical PASS\n",sites,length+1);
}
static void product_take_oom(const XrXirSourceProductRequest *request) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirSourceProduct *product=NULL;CHECK(xr_xir_compile_source_product_build(request,&product,NULL)==XR_XIR_OK);
        XrXirSourceProduct before=*product;XrXirProgram *program=NULL;
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_product_vm_take(product,&program);
        if (!point) {CHECK(status==XR_XIR_OK && program);sites=runtime_attempts;}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !program && !memcmp(product,&before,sizeof(before)));
        runtime_fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);xr_xir_compile_source_product_free(product);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("VM ownership transfer actual allocations=%zu; failure code/packets/query intact and physical PASS\n",sites);
}
static void product_packet_gates(const XrXirSourceProduct *product,bool has_lowered) {
    XrXirSourceProductPacketView source={0},closed={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&source)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&closed)==XR_XIR_OK);
    const XrXirSourceProductFacts *facts=xr_xir_compile_source_product_facts(product);CHECK(facts);
    uint8_t digest[32];xr_sha256(source.bytes,source.length,digest);CHECK(!memcmp(digest,facts->source_digest,32));
    xr_sha256(closed.bytes,closed.length,digest);CHECK(!memcmp(digest,facts->closed_digest,32));
    size_t live=runtime_live,bytes=runtime_bytes;XrXirArtifact *read=NULL;
    CHECK(xr_xir_compile_checked_read(&product->context,source.bytes,source.length,&read,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);read=NULL;
    CHECK(xr_xir_compile_checked_read(&product->context,closed.bytes,closed.length,&read,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirSourceProductPacketView saved=source;
    CHECK(xr_xir_compile_source_product_packet(product,(XrXirSourceProductPacketKind)9,&source)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&source,&saved,sizeof(source)));
    CHECK(xr_xir_compile_source_product_verify(product,16777216,NULL)==(has_lowered ? XR_XIR_OK : XR_XIR_BAD_STAGE));
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("owned original Checked=%zu and closed=%zu; ordinary reader and %s PASS\n",source.length,closed.length,
        has_lowered ? "specialize/lower correspondence" : "post-take packet lifetime");
}
int main(int argc,char **argv) {
    CHECK(argc==8);product_layout_vector();
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,&context,argv[3],NULL,XR_XIR_PROGRAM,NULL},
                                     {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    bool scan=!strcmp(argv[7],"scan");product_guards(&request);
    if (scan && !strcmp(argv[4],"accept")) {product_build_oom(&request);product_take_oom(&request);product_cumulative_limits(&request);}
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    xr_compile_session_free(session);xr_compile_resources_release(resources);memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    bool accepted=!strcmp(argv[4],"accept");
    printf("opaque owner status=%u stage=%u line=%d column=%d message=%s\n",status,diagnostic.stage,
           diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK((status==XR_XIR_OK)==accepted);
    if (!accepted) {
        CHECK(!product);const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(diagnostic.snapshot);
        CHECK(view && !view->complete && view->module_count && view->modules[0].path);
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);CHECK(!runtime_live && !runtime_bytes);return 0;
    }
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);CHECK(view && view->complete);
    product_packet_gates(product,true);product_corruptions(product);
    if (!strcmp(argv[6],"tiny")) product_crossed_owners(product,argv[1],argv[3]);
    product_projection_guards(product);
    if (scan) {product_verify_oom(product);product_emit_oom(product);product_projection_oom(product);}
    const XrXirSourceProductFacts facts=*xr_xir_compile_source_product_facts(product);
    CHECK(facts.function_count && facts.module_count && facts.entry<facts.function_count);
    for (uint32_t f=0;f<facts.function_count;++f) {
        XrXirSourceProductLayoutView layout={0};CHECK(xr_xir_compile_source_product_layout(product,f,&layout)==XR_XIR_OK && layout.layout);
    }
    XrToolchainBinding binding=projection_fixture_binding(product);
    XrXirNativeProjectionRequest project={product,&binding,"original_source",16777216};
    XrXirNativeProjection projection={0};
    CHECK(xr_compile_native_projection_build(&project,&projection,NULL)==XR_XIR_OK);
    projection_fixture_check(product,&projection,false);
    FILE *file=fopen(argv[5],"wb");CHECK(file && fwrite(projection.source.text,1,projection.source.length,file)==projection.source.length && !fclose(file));
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    CHECK(!memcmp(&facts,xr_xir_compile_source_product_facts(product),sizeof(facts)));
    XrXirSourceProductLayoutView layout={17,(const XrXirFunctionLayout *)(uintptr_t)23},saved=layout;
    CHECK(xr_xir_compile_source_product_layout(product,0,&layout)==XR_XIR_BAD_STAGE && !memcmp(&layout,&saved,sizeof(layout)));
    CHECK(xr_xir_compile_source_product_view(product)->complete);product_packet_gates(product,false);
    XrXirCSource unavailable={0};XrXirProgram *transferred=NULL;
    CHECK(xr_xir_compile_source_product_emit(product,"original_source",16777216,&unavailable)==XR_XIR_BAD_STAGE);
    CHECK(!unavailable.text && !unavailable.length);
    XrXirNativeProjection refused={0},saved_projection=refused;
    CHECK(xr_compile_native_projection_build(&project,&refused,NULL)==XR_XIR_BAD_STAGE);
    CHECK(!memcmp(&refused,&saved_projection,sizeof(refused)));
    CHECK(xr_xir_compile_source_product_vm_take(product,&transferred)==XR_XIR_BAD_STAGE);
    CHECK(!transferred);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(projection.source.text && projection.source.length);
    xr_compile_native_projection_free(&projection);xr_compile_native_projection_free(&projection);
    const char *expected=!strcmp(argv[6],"array") ? "array-growth-accounting-ok\n" : !strcmp(argv[6],"tiny") ? "source-product-ok\n" : "";
    if (scan) probe_program(program,facts.entry,expected);
    else CHECK(probe_once(program,facts.entry,expected,64000000,SIZE_MAX,0).status==XR_XIR_CALL_RETURNED);
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    puts("opaque source owner: producers destroyed before first query/packet/verify/emit; owner destroyed before execution PASS");return 0;
}
