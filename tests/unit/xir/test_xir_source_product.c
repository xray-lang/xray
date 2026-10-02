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
/* Test-local symbols instrument the actual verifier without duplicating the
 * verifier definitions already supplied by the frontend object aggregate. */
#define xi_cgen_verify_output source_test_cgen_verify_output
#define xi_cgen_verify_output_or_ice source_test_cgen_verify_output_or_ice
#define xi_cgen_verify_category_name source_test_cgen_verify_category_name
#define xi_cgen_verify_c90_output source_test_cgen_verify_c90_output
#include "aot/xi_cgen_verify_output.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_emit_c.c"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_library_catalog.c"
#include "xir/xxir_source.c"
#include "program/xr_xir_source_product.c"
#include "xir_source_product_execution.h"
#include "xir_source_product_identity.h"
static void product_guards(const XrXirSourceProductRequest *request) {
    XrXirSourceProduct *held=(XrXirSourceProduct *)(uintptr_t)17,*before=held;
    CHECK(xr_xir_source_product_build(request,&held,NULL)==XR_XIR_BAD_STRUCTURE && held==before);
    XrXirSourceProduct *zero=NULL;XrXirSourceProductRequest malformed=*request;
    malformed.target.abi_version=15;
    CHECK(xr_xir_source_product_build(&malformed,&zero,NULL)==XR_XIR_BAD_STRUCTURE && !zero);
    malformed=*request;malformed.source.linkage_kind=XR_XIR_LIBRARY;
    CHECK(xr_xir_source_product_build(&malformed,&zero,NULL)==XR_XIR_BAD_STRUCTURE && !zero);
    CHECK(xr_xir_source_product_build(request,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!xr_xir_source_product_facts(NULL) && !xr_xir_source_product_view(NULL));
    XrXirSourceProductPacketView view={(const uint8_t *)(uintptr_t)19,23},saved=view;
    CHECK(xr_xir_source_product_packet(NULL,XR_XIR_SOURCE_PRODUCT_SOURCE,&view)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&view,&saved,sizeof(view)));
}
static void product_build_oom(const XrXirSourceProductRequest *request) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_source_product_build(request,&product,&diagnostic);
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Source allocation %zu status=%u stage=%u\n",point-1,status,diagnostic.stage);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !product);
        }
        runtime_fail_at=SIZE_MAX;xr_xir_source_product_free(product);
        xr_xir_source_product_diagnostic_free(&diagnostic);CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("opaque Source actual inserted allocations=%zu; failure publication/physical baseline PASS\n",sites);
}
static void product_verify_oom(const XrXirSourceProduct *product) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_source_product_verify(product,NULL,16777216,NULL);
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
        XrXirStatus status=xr_xir_source_product_emit(product,"original_source",16777216,&code);
        if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;length=code.length;}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !code.text && !code.length);
        runtime_fail_at=SIZE_MAX;xr_xir_c_source_free(&code);CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    XrXirCSource code={0};CHECK(xr_xir_source_product_emit(product,"original_source",length+1,&code)==XR_XIR_OK);
    CHECK(code.length==length);xr_xir_c_source_free(&code);
    CHECK(xr_xir_source_product_emit(product,"original_source",length,&code)==XR_XIR_BUDGET && !code.text && !code.length);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("native projection allocations=%zu bytes_exact=%zu/minus1; precise OOM/zero output/physical PASS\n",sites,length+1);
}
static void product_take_oom(const XrXirSourceProductRequest *request) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirSourceProduct *product=NULL;CHECK(xr_xir_source_product_build(request,&product,NULL)==XR_XIR_OK);
        XrXirSourceProduct before=*product;XrXirProgram *program=NULL;
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_source_product_vm_take(product,(XrXirProgramBudget){33554432,64000000},&program);
        if (!point) {CHECK(status==XR_XIR_OK && program);sites=runtime_attempts;}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at && !program && !memcmp(product,&before,sizeof(before)));
        runtime_fail_at=SIZE_MAX;xr_xir_program_drop(program);xr_xir_source_product_free(product);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("VM ownership transfer actual allocations=%zu; failure code/packets/query intact and physical PASS\n",sites);
}
static void product_packet_gates(const XrXirSourceProduct *product,bool has_lowered) {
    XrXirSourceProductPacketView source={0},closed={0};
    CHECK(xr_xir_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&source)==XR_XIR_OK);
    CHECK(xr_xir_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&closed)==XR_XIR_OK);
    const XrXirSourceProductFacts *facts=xr_xir_source_product_facts(product);CHECK(facts);
    uint8_t digest[32];xr_sha256(source.bytes,source.length,digest);CHECK(!memcmp(digest,facts->source_digest,32));
    xr_sha256(closed.bytes,closed.length,digest);CHECK(!memcmp(digest,facts->closed_digest,32));
    size_t live=runtime_live,bytes=runtime_bytes;XrXirArtifact *read=NULL;
    CHECK(xr_xir_checked_read(source.bytes,source.length,NULL,&read,NULL)==XR_XIR_OK);xr_xir_artifact_free(read);read=NULL;
    CHECK(xr_xir_checked_read(closed.bytes,closed.length,NULL,&read,NULL)==XR_XIR_OK);xr_xir_artifact_free(read);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirSourceProductPacketView saved=source;
    CHECK(xr_xir_source_product_packet(product,(XrXirSourceProductPacketKind)9,&source)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&source,&saved,sizeof(source)));
    CHECK(xr_xir_source_product_verify(product,NULL,16777216,NULL)==(has_lowered ? XR_XIR_OK : XR_XIR_BAD_STAGE));
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("owned original Checked=%zu and closed=%zu; ordinary reader and %s PASS\n",source.length,closed.length,
        has_lowered ? "specialize/lower correspondence" : "post-take packet lifetime");
}
int main(int argc,char **argv) {
    CHECK(argc==8);product_layout_vector();XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,NULL,argv[3],NULL,XR_XIR_PROGRAM,NULL},
                                     {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    bool scan=!strcmp(argv[7],"scan");product_guards(&request);
    if (scan && !strcmp(argv[4],"accept")) {product_build_oom(&request);product_take_oom(&request);}
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_product_build(&request,&product,&diagnostic);
    xr_compiler_session_delete(session);memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    bool accepted=!strcmp(argv[4],"accept");
    printf("opaque owner status=%u stage=%u line=%d column=%d message=%s\n",status,diagnostic.stage,
           diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK((status==XR_XIR_OK)==accepted);
    if (!accepted) {
        CHECK(!product);const XrXirSourceView *view=xr_xir_source_snapshot_view(diagnostic.snapshot);
        CHECK(view && !view->complete && view->module_count && view->modules[0].path);
        xr_xir_source_product_diagnostic_free(&diagnostic);CHECK(!runtime_live && !runtime_bytes);return 0;
    }
    const XrXirSourceView *view=xr_xir_source_product_view(product);CHECK(view && view->complete);
    product_packet_gates(product,true);product_corruptions(product);
    if (!strcmp(argv[6],"tiny")) product_crossed_owners(product,argv[1],argv[3]);
    if (scan) {product_verify_oom(product);product_emit_oom(product);}
    const XrXirSourceProductFacts facts=*xr_xir_source_product_facts(product);
    CHECK(facts.function_count && facts.module_count && facts.entry<facts.function_count);
    for (uint32_t f=0;f<facts.function_count;++f) {
        XrXirSourceProductLayoutView layout={0};CHECK(xr_xir_source_product_layout(product,f,&layout)==XR_XIR_OK && layout.layout);
    }
    XrXirCSource code={0};CHECK(xr_xir_source_product_emit(product,"original_source",16777216,&code)==XR_XIR_OK);
    FILE *file=fopen(argv[5],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length && !fclose(file));
    xr_xir_c_source_free(&code);XrXirProgram *program=NULL;
    CHECK(xr_xir_source_product_vm_take(product,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    CHECK(!memcmp(&facts,xr_xir_source_product_facts(product),sizeof(facts)));
    XrXirSourceProductLayoutView layout={17,(const XrXirFunctionLayout *)(uintptr_t)23},saved=layout;
    CHECK(xr_xir_source_product_layout(product,0,&layout)==XR_XIR_BAD_STAGE && !memcmp(&layout,&saved,sizeof(layout)));
    CHECK(xr_xir_source_product_view(product)->complete);product_packet_gates(product,false);
    XrXirCSource unavailable={0};XrXirProgram *transferred=NULL;
    CHECK(xr_xir_source_product_emit(product,"original_source",16777216,&unavailable)==XR_XIR_BAD_STAGE);
    CHECK(!unavailable.text && !unavailable.length);
    CHECK(xr_xir_source_product_vm_take(product,(XrXirProgramBudget){33554432,64000000},&transferred)==XR_XIR_BAD_STAGE);
    CHECK(!transferred);
    xr_xir_source_product_free(product);xr_xir_source_product_diagnostic_free(&diagnostic);
    const char *expected=!strcmp(argv[6],"array") ? "array-growth-accounting-ok\n" : !strcmp(argv[6],"tiny") ? "source-product-ok\n" : "";
    if (scan) probe_program(program,facts.entry,expected);
    else CHECK(probe_once(program,facts.entry,expected,64000000,SIZE_MAX,0).status==XR_XIR_CALL_RETURNED);
    xr_xir_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    puts("opaque source owner: producers destroyed before first query/packet/verify/emit; owner destroyed before execution PASS");return 0;
}
