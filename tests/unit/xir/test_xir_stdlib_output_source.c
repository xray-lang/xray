/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_output_source.c - Real standard library Checked publication
 *
 * KEY CONCEPT:
 *   Published library bytes survive deletion of their original module source.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static size_t source_attempts,source_fail_at=SIZE_MAX,source_live,source_bytes,source_peak;
typedef struct LibrarySourceAllocation {void *pointer;size_t bytes;} LibrarySourceAllocation;
static LibrarySourceAllocation source_owned[4096];
XR_FUNC void *xr_test_stdlib_output_source_calloc(size_t count,size_t size){
 CHECK(!size||count<=SIZE_MAX/size);
 if(source_attempts++==source_fail_at)return NULL;void *p=xr_calloc(count,size);
 if(p){CHECK(source_live<4096);CHECK(count*size<=SIZE_MAX-source_bytes);source_owned[source_live++]=(LibrarySourceAllocation){p,count*size};source_bytes+=count*size;if(source_bytes>source_peak)source_peak=source_bytes;}return p;
}
XR_FUNC void xr_test_stdlib_output_source_free(void *p){
 if(p)for(size_t i=0;i<source_live;++i)if(source_owned[i].pointer==p){source_bytes-=source_owned[i].bytes;source_owned[i]=source_owned[--source_live];break;}
 xr_free(p);
}
XR_FUNC void *xr_test_stdlib_output_source_malloc(size_t size){return xr_test_stdlib_output_source_calloc(1,size);}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(s) xr_test_stdlib_output_source_malloc(s)
#define xr_calloc(c,s) xr_test_stdlib_output_source_calloc(c,s)
#define xr_free(p) xr_test_stdlib_output_source_free(p)
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#define XR_STDLIB_OUTPUT_SOURCE
#include "xir_stdlib_output_runtime.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xfileio.h"

static XrXirCheckedPacket publication_packet(const char *root, const char *path) {
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,"io",root};
    XrXirSourceRequest request = {session,path,&authority,NULL,root,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_source_check(&request,&result,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"producer %u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked);
    const XrXirModule *module = xr_xir_artifact_module(result.checked);
    CHECK(module->linkage_kind == XR_XIR_LIBRARY && module->declarations->module_count == 1);
    CHECK(module->function_count == 3 && !module->types && !module->provenance);
    const char *identity = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    CHECK(module->declarations->modules[0].name_length == strlen(identity) &&
        !memcmp(module->declarations->modules[0].name,identity,strlen(identity)));
    unsigned writes = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            if (module->functions[f].instructions[i].op == XR_XIR_WRITE_STREAM) ++writes;
    CHECK(writes == 2);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    CHECK(!source_live && !source_bytes);
    session = xr_compiler_session_new(NULL); CHECK(session);
    authority.kind = XR_MODULE_IDENTITY_SCRIPT; authority.namespace_id = NULL;
    request.session = session; result = (XrXirSourceResult){0};
    CHECK(xr_xir_source_check(&request,&result,&diagnostic) != XR_XIR_OK && !result.checked);
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    CHECK(!source_live && !source_bytes);
    return packet;
}
static void publication_reject(const XrXirLibraryInput *input, size_t count) {
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_library_catalog_new(input,count,NULL,&catalog) != XR_XIR_OK && !catalog);
    CHECK(runtime_live == live && runtime_bytes == bytes);
}
static void publication_catalog_cases(const XrXirLibraryInput *input) {
    XrXirLibraryInput bad = *input;
    bad.sha256[0] ^= 1; publication_reject(&bad,1);
    bad = *input; bad.authority.namespace_id = "fs"; publication_reject(&bad,1);
    bad = *input; bad.logical_path = "io/../output.xr"; publication_reject(&bad,1);
    bad = *input; bad.logical_path = "io/output/../output.xr"; publication_reject(&bad,1);
    bad = *input; bad.authority.physical_root = "relative"; publication_reject(&bad,1);
    bad = *input; bad.authority.kind = XR_MODULE_IDENTITY_SCRIPT;
    bad.authority.namespace_id = NULL; bad.logical_path = "output.xr"; publication_reject(&bad,1);
    XrXirLibraryInput duplicate[] = {*input,*input}; publication_reject(duplicate,2);
    unsigned char *altered = xr_malloc(input->length); CHECK(altered);
    memcpy(altered,input->packet,input->length);
    const char *identity = "stdlib-module-v1:module=2:io:path=12:io/output.xr";
    size_t changed = 0;
    for (size_t i = 64; i + strlen(identity) <= input->length; ++i)
        if (!memcmp(altered+i,identity,strlen(identity))) {
            altered[i+strlen(identity)-9] = 'p'; ++changed;
        }
    CHECK(changed == 1);
    XrSHA256Context hash; xr_sha256_init(&hash); xr_sha256_update(&hash,altered,32);
    xr_sha256_update(&hash,altered+64,input->length-64); xr_sha256_final(&hash,altered+32);
    bad = *input; bad.packet = altered; xr_sha256(altered,input->length,bad.sha256);
    XrXirArtifact *retagged = NULL;
    CHECK(xr_xir_checked_read(altered,input->length,NULL,&retagged,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(retagged); publication_reject(&bad,1); xr_free(altered);
    altered = xr_malloc(input->length); CHECK(altered); memcpy(altered,input->packet,input->length);
    unsigned char write_prefix[24] = {0}; write_prefix[0] = (unsigned char)XR_XIR_WRITE_STREAM;
    write_prefix[4] = (unsigned char)XR_XIR_BOOL; changed = 0;
    for (size_t i = 64; i + 40 <= input->length; ++i)
        if (!memcmp(altered+i,write_prefix,sizeof(write_prefix))) {
            altered[i+24] = 3; ++changed;
        }
    CHECK(changed == 2);
    xr_sha256_init(&hash); xr_sha256_update(&hash,altered,32);
    xr_sha256_update(&hash,altered+64,input->length-64); xr_sha256_final(&hash,altered+32);
    bad = *input; bad.packet = altered; xr_sha256(altered,input->length,bad.sha256);
    publication_reject(&bad,1); xr_free(altered);
    size_t live = runtime_live, bytes = runtime_bytes;
    runtime_attempts = 0; XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_library_catalog_new(input,1,NULL,&catalog) == XR_XIR_OK);
    size_t sites = runtime_attempts; xr_xir_library_catalog_free(catalog);
    for (size_t failure = 0; failure < sites; ++failure) {
        runtime_attempts = 0; runtime_fail_at = failure; catalog = NULL;
        XrXirStatus status = xr_xir_library_catalog_new(input,1,NULL,&catalog);
        runtime_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && !catalog);
        CHECK(runtime_live == live && runtime_bytes == bytes);
    }
    printf("stdlib output Catalog actual OOM=%zu physicalbaseline\n",sites);
    for (unsigned dimension = 0; dimension < 2; ++dimension) {
        XrXirBudget full = xr_xir_default_budget();
        uint64_t low = 0, high = dimension ? full.metadata_bytes : full.work;
        while (low < high) {
            uint64_t middle = low + (high-low)/2; XrXirBudget budget = full;
            if (dimension) budget.metadata_bytes = middle; else budget.work = middle;
            catalog = NULL; XrXirStatus status = xr_xir_library_catalog_new(input,1,&budget,&catalog);
            CHECK(status == XR_XIR_OK || status == XR_XIR_BUDGET);
            xr_xir_library_catalog_free(catalog);
            if (status == XR_XIR_OK) high = middle; else low = middle + 1;
            CHECK(runtime_live == live && runtime_bytes == bytes);
        }
        CHECK(low);
        for (unsigned below = 0; below < 2; ++below) {
            XrXirBudget budget = full;
            if (dimension) budget.metadata_bytes = low-below; else budget.work = low-below;
            catalog = NULL;
            CHECK(xr_xir_library_catalog_new(input,1,&budget,&catalog) == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            xr_xir_library_catalog_free(catalog);
            CHECK(runtime_live == live && runtime_bytes == bytes);
        }
        printf("stdlib output Catalog dimension=%u exact=%llu/minus1\n",dimension,(unsigned long long)low);
    }
}
static void publication_private_case(const XrXirLibraryInput *input, const char *root, const char *entry) {
    XrXirArtifact *original = NULL, *checked = NULL;
    CHECK(xr_xir_checked_read(input->packet,input->length,NULL,&original,NULL) == XR_XIR_OK);
    XrXirModule built = *xr_xir_artifact_module(original); built.stage = XR_XIR_BUILT;
    XrXirDeclarations declarations = *built.declarations;
    XrXirFunctionIdentity functions[3]; CHECK(built.function_count == 3);
    memcpy(functions,declarations.functions,sizeof(functions));
    for (unsigned f = 0; f < 3; ++f) functions[f].exported = 0;
    declarations.functions = functions; built.declarations = &declarations;
    CHECK(xr_xir_check(&built,NULL,&checked,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_checked_write(checked,NULL,&packet,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked); xr_xir_artifact_free(original);
    XrXirLibraryInput private_input = *input;
    private_input.packet = packet.bytes; private_input.length = packet.length;
    xr_sha256(packet.bytes,packet.length,private_input.sha256);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_library_catalog_new(&private_input,1,NULL,&catalog) == XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,NULL,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_source_check(&request,&result,&diagnostic) != XR_XIR_OK && !result.checked);
    CHECK(!strcmp(diagnostic.message,"import requires an exported declaration"));
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    xr_xir_library_catalog_free(catalog);
    CHECK(!source_live && !source_bytes);
}
static void publication_resolver_cases(XrXirLibraryCatalog *catalog, const char *root, const char *entry) {
    size_t count = 0;
    const XrModuleResourceBinding *resources = xr_xir_library_catalog_resources(catalog,&count);
    CHECK(count == 1 && resources);
    XrModuleIdentityAuthority script = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    for (unsigned attack = 0; attack < 10; ++attack) {
        XrModuleResourceBinding binding = resources[0];
        if (attack == 1) binding.authority.kind = XR_MODULE_IDENTITY_SCRIPT;
        if (attack == 2) binding.authority.namespace_id = "fs";
        if (attack == 3) binding.authority.physical_root = "E:/foreign";
        if (attack == 4) binding.logical_path = "io/elsewhere.xr";
        if (attack == 5) binding.checked = NULL;
        if (attack == 6) binding.source_locator = NULL;
        XrModuleResolverConfig config = {root,NULL,&binding,1};
        XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
        XrModuleId id = {0}; char *error = NULL;
        const char *specifier = attack == 7 ? "std/io/../output" : attack == 8 ?
            "std/io/output.xr" : attack == 9 ? "std//io/output" : "std/io/output";
        int result = xr_module_resolver_resolve(resolver,specifier,entry,&script,&id,&error);
        CHECK(attack ? result != 0 : result == 0);
        if (!attack) CHECK(id.kind == XR_MOD_STDLIB && id.representation == XR_MODULE_CHECKED_LIBRARY &&
            id.resource == &binding && !strcmp(id.canonical,resources[0].canonical));
        xr_module_id_cleanup(&id); xr_free(error); xr_module_resolver_free(resolver);
    }
}
static void publication_source_budgets(const XrXirSourceRequest *request) {
    size_t live = source_live, bytes = source_bytes, core_live = runtime_live, core_bytes = runtime_bytes;
    for (unsigned dimension = 0; dimension < 2; ++dimension) {
        XrXirBudget full = xr_xir_default_budget();
        uint64_t low = 0, high = dimension ? full.metadata_bytes : full.work;
        while (low < high) {
            uint64_t middle = low+(high-low)/2; XrXirBudget budget = full;
            if (dimension) budget.metadata_bytes = middle; else budget.work = middle;
            XrXirSourceRequest limited = *request; limited.budget = &budget;
            XrXirSourceResult result = {0};
            XrXirStatus status = xr_xir_source_check(&limited,&result,NULL);
            CHECK(status == XR_XIR_OK || status == XR_XIR_BUDGET);
            xr_xir_source_result_free(&result);
            if (status == XR_XIR_OK) high = middle; else low = middle+1;
            CHECK(source_live == live && source_bytes == bytes && runtime_live == core_live && runtime_bytes == core_bytes);
        }
        CHECK(low);
        for (unsigned below = 0; below < 2; ++below) {
            XrXirBudget budget = full;
            if (dimension) budget.metadata_bytes = low-below; else budget.work = low-below;
            XrXirSourceRequest limited = *request; limited.budget = &budget;
            XrXirSourceResult result = {0};
            CHECK(xr_xir_source_check(&limited,&result,NULL) == (below ? XR_XIR_BUDGET : XR_XIR_OK));
            xr_xir_source_result_free(&result);
            CHECK(source_live == live && source_bytes == bytes && runtime_live == core_live && runtime_bytes == core_bytes);
        }
        printf("stdlib output Source dimension=%u exact=%llu/minus1\n",dimension,(unsigned long long)low);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 3); char *root = xr_realpath(argv[1]); CHECK(root);
    char source[1024], entry[1024], path[1024];
    CHECK(snprintf(source,sizeof(source),"%s/io/output.xr",root) > 0);
    CHECK(snprintf(entry,sizeof(entry),"%s/root.xr",root) > 0);
    CHECK(snprintf(path,sizeof(path),"%s/output.xrc",root) > 0);
    XrXirCheckedPacket packet = publication_packet(root,source);
    FILE *file = fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(packet.bytes,1,packet.length,file) == packet.length && !fclose(file));
    size_t length = packet.length; xr_xir_checked_packet_free(&packet);
    CHECK(!runtime_live && !runtime_bytes);
    void *bytes = xr_malloc(length); CHECK(bytes); file = fopen(path,"rb"); CHECK(file);
    CHECK(fread(bytes,1,length,file) == length && !fclose(file));
    XrXirLibraryInput input = {{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr",bytes,length,{0}};
    xr_sha256(bytes,length,input.sha256); publication_catalog_cases(&input);
    publication_private_case(&input,root,entry);
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xr_xir_library_catalog_new(&input,1,NULL,&catalog) == XR_XIR_OK);
    memset(bytes,0,length); xr_free(bytes);
    publication_resolver_cases(catalog,root,entry);
    CHECK(!remove(source)); publication_resolver_cases(catalog,root,entry);
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,NULL,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    source_attempts = 0; runtime_attempts = 0;
    XrXirStatus status = xr_xir_source_check(&request,&result,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"consumer %u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot);
    size_t sites = source_attempts, core_sites = runtime_attempts, live = source_live, physical = source_bytes;
    size_t core_live = runtime_live, core_bytes = runtime_bytes;
    for (size_t failure = 0; failure < sites; ++failure) {
        source_attempts = 0; source_fail_at = failure; XrXirSourceResult failed = {0};
        status = xr_xir_source_check(&request,&failed,&diagnostic); source_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && !failed.checked && !failed.snapshot);
        xr_xir_source_result_free(&failed);
        CHECK(source_live == live && source_bytes == physical && runtime_live == core_live && runtime_bytes == core_bytes);
    }
    for (size_t failure = 0; failure < core_sites; ++failure) {
        runtime_attempts = 0; runtime_fail_at = failure; XrXirSourceResult failed = {0};
        status = xr_xir_source_check(&request,&failed,&diagnostic); runtime_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && !failed.checked && !failed.snapshot);
        xr_xir_source_result_free(&failed);
        CHECK(source_live == live && source_bytes == physical && runtime_live == core_live && runtime_bytes == core_bytes);
    }
    printf("stdlib output Source actual OOM=%zu physicalbaseline\n",sites);
    printf("stdlib output Source Core actual OOM=%zu physicalbaseline\n",core_sites);
    publication_source_budgets(&request);
    XrXirArtifact *owned = result.checked; result.checked = NULL;
    xr_xir_source_result_free(&result); xr_compiler_session_delete(session);
    xr_xir_library_catalog_free(catalog); xr_free(root);
    CHECK(!source_live && !source_bytes);
    CHECK(xr_xir_artifact_verify(owned,NULL,NULL) == XR_XIR_OK);
    xr_test_stdlib_output_execute(owned,argv[2]);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("real io/output source removed; first owned Checked execution PASS"); return 0;
}
