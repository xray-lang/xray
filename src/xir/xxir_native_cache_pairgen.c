/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_cache_pairgen.c - Genuine Checked/native static pair generation
 *
 * KEY CONCEPT: Same-source Checked and native entries share one semantic build.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_native_cache_projection.inc.c"
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"cache producer FAIL %d %s\n",__LINE__,#test); exit(1); } } while (0)
static void write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(bytes,1,length,file) == length); CHECK(!fclose(file));
}
static void print_digest(FILE *file, const uint8_t bytes[32]) {
    for (unsigned i = 0; i < 32; ++i) CHECK(fprintf(file,"%02x",bytes[i]) == 2);
}
int main(int argc, char **argv) {
    CHECK(argc == 7);
    XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}; CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,"io",argv[1]};
    XrXirSourceRequest request = {session,argv[2],&authority,&context,argv[1],NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"cache library status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket library = {0}; CHECK(xr_xir_compile_checked_write(result.checked,&library,NULL) == XR_XIR_OK);
    write_bytes(argv[4],library.bytes,library.length);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session); session = NULL;
    XrXirLibraryInput input = {library.bytes,library.length,{0}, (XrXirLibraryModuleInput[]){{authority,"io/output.xr"}},1};
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,library.bytes,library.length); xr_sha256_final(&hash,input.sha256);
    XrXirLibraryCatalog *catalog = NULL; CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog) == XR_XIR_OK);
    CHECK(!remove(argv[2]));
    CHECK(xr_compile_session_new(context.resources,&session) == XR_COMPILER_SESSION_OK);
    authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
    request = (XrXirSourceRequest){session,argv[3],&authority,&context,argv[1],NULL,XR_XIR_PROGRAM,catalog};
    status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"cache program status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket program = {0}; XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&program,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context,program.bytes,program.length,&decoded,NULL) == XR_XIR_OK);
    XrXirDiagnostic specialize_diagnostic = {0};
    status = xr_xir_compile_specialize(decoded,&closed,&specialize_diagnostic);
    if (status != XR_XIR_OK)
        fprintf(stderr,"cache specialize status=%u function=%u block=%u instruction=%u reason=%u\n",
            status,specialize_diagnostic.function,specialize_diagnostic.block,
            specialize_diagnostic.instruction,specialize_diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t indices[2] = {UINT32_MAX,UINT32_MAX}; uint8_t keys[2][32];
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const char *owner = module->declarations->modules[module->declarations->functions[f].module].name;
        printf("cache function=%u module=%.*s name=%.*s instructions=%u\n",f,(int)module->declarations->modules[module->declarations->functions[f].module].name_length,owner,(int)function->name_length,function->name,function->instruction_count);
        for (uint32_t i = 0; i < function->instruction_count; ++i)
            printf(" op=%u type=%u arg=%u immediate=%lld\n",function->instructions[i].op,function->instructions[i].type,function->instructions[i].args[0],(long long)function->instructions[i].immediate);
        for (uint32_t stream = 1; stream <= 2; ++stream) {
            status = cache_leaf_id(lowered,f,stream,keys[stream-1]);
            CHECK(status == XR_XIR_OK || status == XR_XIR_UNRESOLVED);
            if (status == XR_XIR_OK) { CHECK(indices[stream-1] == UINT32_MAX); indices[stream-1] = f; }
        }
    }
    CHECK(indices[0] != UINT32_MAX && indices[1] != UINT32_MAX);
    XrXirEffects *effects = NULL; CHECK(xr_xir_compile_effects_analyze(lowered,&effects) == XR_XIR_OK);
    for (unsigned i = 0; i < 2; ++i) {
        const XrXirFunctionEffects *fact = xr_xir_effects_function(effects,indices[i]);
        CHECK(fact && fact->suspend == XR_XIR_EFFECT_NONE && fact->throws == XR_XIR_EFFECT_NONE);
    }
    XrXirCSource source = {0}; CHECK(xr_xir_compile_emit_c(lowered,"xir_output_cache",4*1024*1024,&source) == XR_XIR_OK);
    write_bytes(argv[5],source.text,source.length);
    uint8_t semantic[32], abi[32]; CHECK(cache_semantic_id(&context,library.bytes,library.length,semantic) == XR_XIR_OK);
    CHECK(cache_abi_id(&context,abi) == XR_XIR_OK);
    FILE *manifest = fopen(argv[6],"wb"); CHECK(manifest);
    CHECK(fprintf(manifest,"{\"schema\":1,\"semantic_id\":\"") > 0); print_digest(manifest,semantic);
    CHECK(fprintf(manifest,"\",\"runtime_layout\":\"") > 0); print_digest(manifest,abi);
    CHECK(fprintf(manifest,"\",\"entry_count\":%u,\"leaf_indices\":[%u,%u],\"leaf_digests\":[\"",module->function_count,indices[0],indices[1]) > 0);
    print_digest(manifest,keys[0]); CHECK(fprintf(manifest,"\",\"") > 0); print_digest(manifest,keys[1]);
    CHECK(fprintf(manifest,"\"]}\n") > 0); CHECK(!fclose(manifest));
    xr_xir_compile_effects_free(effects); xr_xir_compile_c_source_free(&source);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&program); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog); xr_xir_compile_checked_packet_free(&library);
    XrCompileResourceStats final = {0}; CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == initial.live_bytes); xr_compile_resources_release(context.resources);
    printf("cache pair genuine pipeline physical ledger baseline allocated=%llu work=%llu\n",(unsigned long long)final.allocated_bytes,(unsigned long long)final.work);
    return 0;
}
