/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_generated_c.c - Strict C fixtures through the current Source owner
 *
 * KEY CONCEPT:
 *   Unmodified source fixtures use ordinary admission and portable C emission.
 */
#include "api/xsource_compile.h"
#include "aot/program/xr_xir_native_projection.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir/xir_instance_compile_observer.h"

static void generated_fingerprint(FILE *file,const char *name,const uint8_t bytes[32]) {
    fprintf(file,"\"%s\":\"",name);
    for(unsigned i=0;i<32;++i)fprintf(file,"%02x",bytes[i]);
    fputc('"',file);
}
static void generated_facts(const char *path,const XrXirNativeProjection *projection) {
    const XrXirNativeProjectionFacts *facts=xr_compile_native_projection_facts(projection);
    const XrXirNativeProjectionSource *source=xr_compile_native_projection_source(projection);
    CHECK(facts&&source);FILE *file=fopen(path,"wb");CHECK(file);
    fprintf(file,"{\"architecture\":%u,\"abi\":%u,\"entry\":%u,\"functions\":%u,\"modules\":%u,\"prefix\":\"%s\",\"CBytes\":%zu,",
        (unsigned)facts->source.target.architecture,(unsigned)facts->source.target.abi_version,
        facts->source.entry,facts->source.function_count,facts->source.module_count,facts->prefix,source->length);
    generated_fingerprint(file,"SOURCE",facts->source.source_digest);fputc(',',file);
    generated_fingerprint(file,"CLOSED",facts->source.closed_digest);fputc(',',file);
    generated_fingerprint(file,"LAYOUT",facts->source.lowered_layout_digest);fputc(',',file);
    generated_fingerprint(file,"POLICY",facts->codegen_policy_id.bytes);fputc(',',file);
    generated_fingerprint(file,"GENERATED",facts->generated_digest.bytes);fputs("}\n",file);
    CHECK(!ferror(file)&&fclose(file)==0);
}
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: source-generated-c ABSOLUTE_SOURCE OUTPUT_C ABSOLUTE_STDLIB [OUTPUT_FACTS]\n");
        return 2;
    }
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    const XrSourceCompileRequest request = {&context, argv[1], argv[3], NULL,
        xr_source_compile_default_manifest_limits(),
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, NULL};
    XrXirSourceProduct *product = NULL;
    XrSourceCompileDiagnostic diagnostic = {0};
    XrXirNativeProjection *projection = NULL;
    int result = 1;
    XrSourceCompileStatus source_status = xr_source_compile_build(&request, &product, &diagnostic);
    if (source_status != XR_SOURCE_COMPILE_OK) {
        char *message=NULL;(void)xr_frontend_report_render(diagnostic.source.failure.report,&message);
        fprintf(stderr,"%s\n",message?message:"diagnostic unavailable");xr_compile_resources_free(message);
        goto cleanup;
    }
    const XrXirNativeProjectionRequest prepare = {product, "source_fixture", (size_t)UINT64_C(64) << 20};
    XrXirStatus status = xr_compile_native_projection_prepare(&prepare, &projection);
    if (status != XR_XIR_OK) {
        fprintf(stderr, "Source C projection failed: status=%u\n", (unsigned)status);
        goto cleanup;
    }
    if(argc==5)generated_facts(argv[4],projection);
    /* The C projection owns its bytes after the producing Source owner dies. */
    xr_xir_compile_source_product_free(product);
    product = NULL;
    xr_source_compile_diagnostic_free(&diagnostic);
    const XrXirNativeProjectionSource *generated = xr_compile_native_projection_source(projection);
    CHECK(generated && generated->text && generated->length);
    FILE *file = fopen(argv[2], "wb");
    if (!file) {
        perror("generated C output");
        goto cleanup;
    }
    size_t written = fwrite(generated->text, 1, generated->length, file);
    int closed = fclose(file);
    if (written != generated->length || closed) {
        fprintf(stderr, "generated C output was not completely written\n");
        goto cleanup;
    }
    result = 0;
cleanup:
    xr_compile_native_projection_owner_free(projection);
    xr_xir_compile_source_product_free(product);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK &&
        stats.held_bytes == baseline.held_bytes);
    printf("Source C owner: allocated=%" PRIu64 " peak=%" PRIu64 " held=baseline\n",
        stats.allocated_bytes, stats.peak_bytes);
    xr_compile_resources_release(resources);
    instance_compile_report();
    return result;
}
