/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_product_vm.c - Owned SourceProduct VM publication
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_stdlib_internal.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
typedef struct ProductVmOutput { unsigned calls, stream; XrXirOutputStatus status; } ProductVmOutput;
static XrXirOutputStatus product_vm_bytes(void *pointer, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    ProductVmOutput *output = pointer;
    CHECK((unsigned)stream == output->stream && length == 3 && !memcmp(bytes,"A\0B",3));
    ++output->calls; return output->status;
}
static XrXirSourceProduct *product_vm_build(const XrXirCompileContext *context,
    const char *root, const char *entry, const char *stdlib, bool library, uint32_t ids[3]) {
    XrXirLibraryCatalog *catalog = NULL;
    if (library) {
        XrXirLibraryInput input = {xir_stdlib_output_checked,xir_stdlib_output_checked_length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_STDLIB,"io",root},"io/output.xr"}},1};
        memcpy(input.sha256,xir_stdlib_output_checked_sha256,32);
        CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog) == XR_XIR_OK);
    }
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceProductRequest request = {{session,entry,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,catalog},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProductDiagnostic diagnostic = {0}; XrXirSourceProduct *product = NULL;
    XrXirStatus status = xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr,"SourceProduct %u stage%u %s\n",status,diagnostic.stage,diagnostic.source.message);
    CHECK(status == XR_XIR_OK);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    {
        XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL;
        CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL) == XR_XIR_OK);
        const XrXirModule *module = xr_xir_compile_artifact_module(checked);
        const char *names[] = {"stdoutText","stderrText","main"};
        ids[0] = ids[1] = ids[2] = UINT32_MAX;
        for (uint32_t f = 0; f < module->function_count; ++f)
            for (unsigned i = 0; i < 3; ++i)
                if (module->functions[f].name_length == strlen(names[i]) &&
                    !memcmp(module->functions[f].name,names[i],strlen(names[i]))) ids[i] = f;
        CHECK(ids[2] != UINT32_MAX);
        if (stdlib) CHECK(ids[0] != UINT32_MAX && ids[1] != UINT32_MAX);
        xr_xir_compile_artifact_free(checked);
    }
    return product;
}
static void product_vm_drive(XrXirProgram *program, uint32_t entry, const uint32_t ids[3], bool with_library) {
    XrXirInstance *instances[2] = {0}; ProductVmOutput outputs[2] = {{0},{0}};
    XrXirOutputSink sinks[2];
    for (unsigned n = 0; n < 2; ++n) {
        outputs[n] = (ProductVmOutput){0,1,XR_XIR_OUTPUT_OK};
        sinks[n] = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,product_vm_bytes,&outputs[n],1024};
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[n]};
        CHECK(xr_xir_instance_new(program,&config,&instances[n]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(xr_xir_instance_start(instances[n],entry,NULL,0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[n],UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instances[n],&value) == XR_XIR_CALL_RETURNED);
        /* The synthetic script entry returns success. The exported function
         * independently advances each Instance's root state from 41 to 42. */
        CHECK(value.type == XR_XIR_I64 && !value.payload && outputs[n].calls == (with_library ? 1u : 0u));
        xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_start(instances[n],ids[2],NULL,0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[n],UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instances[n],&value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 42); xr_xir_value_drop(&value);
        if (with_library) for (unsigned stream = 0; stream < 2; ++stream) for (unsigned fault = 0; fault < 4; ++fault) {
            const XrXirOutputStatus statuses[] = {XR_XIR_OUTPUT_OK,XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT};
            outputs[n] = (ProductVmOutput){0,stream+1,statuses[fault]};
            XrXirDomain *domain = NULL; XrXirValue argument = {0};
            CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
            CHECK(xr_xir_string_new(domain,"A\0B",3,&argument) == XR_XIR_VALUE_OK);
            CHECK(xr_xir_instance_start(instances[n],ids[stream],&argument,1) == XR_XIR_CALL_READY);
            xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
            XrXirCallStatus result = xr_xir_instance_poll_bounded(instances[n],UINT64_MAX).outcome.status;
            CHECK(outputs[n].calls == 1 && result == (fault < 2 ? XR_XIR_CALL_RETURNED : fault == 2 ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT));
            if (fault < 2) {
                CHECK(xr_xir_instance_take_result(instances[n],&value) == XR_XIR_CALL_RETURNED);
                CHECK(value.type == XR_XIR_BOOL && value.payload == (fault == 0)); xr_xir_value_drop(&value);
            }
        }
        CHECK(xr_xir_instance_free(instances[n]) == (with_library ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 5);
    XrCompileResourceLimits limits = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    for (unsigned mode = 0; mode < 3; ++mode) {
        bool with_library = mode != 0;
        const char *stdlib = mode == 0 ? NULL : mode == 1 ? argv[1] : argv[4];
        uint32_t ids[3] = {0};
        XrXirSourceProduct *product = product_vm_build(&context,argv[1],argv[with_library?3:2],stdlib,mode == 1,ids);
        uint32_t entry = xr_xir_compile_source_product_facts(product)->entry;
        XrXirProgram *occupied = (XrXirProgram *)(uintptr_t)17;
        CHECK(xr_compile_resources_stats(context.resources,&after) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats occupied_before = after;
        size_t attempts_before = effects_compile_attempts;
        CHECK(xr_xir_compile_source_product_vm_take(product,&occupied) == XR_XIR_BAD_STRUCTURE && occupied == (XrXirProgram *)(uintptr_t)17);
        CHECK(xr_compile_resources_stats(context.resources,&after) == XR_COMPILE_RESOURCE_OK);
        CHECK(!memcmp(&after,&occupied_before,sizeof(after)) && effects_compile_attempts == attempts_before);
        if (with_library) {
            XrXirSourceProductLayoutView before = {0}, kept = {0};
            CHECK(xr_xir_compile_source_product_layout(product,entry,&before) == XR_XIR_OK);
            effects_compile_fail_at = effects_compile_attempts; effects_compile_injected = false;
            XrXirProgram *rejected = NULL;
            CHECK(xr_xir_compile_source_product_vm_take(product,&rejected) == XR_XIR_OUT_OF_MEMORY && !rejected && effects_compile_injected);
            effects_compile_fail_at = SIZE_MAX;
            CHECK(xr_xir_compile_source_product_layout(product,entry,&kept) == XR_XIR_OK && kept.layout == before.layout);
        }
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_source_product_vm_take(product,&program) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_product_view(product)->complete);
        XrXirProgram *repeated = NULL; CHECK(xr_xir_compile_source_product_vm_take(product,&repeated) == XR_XIR_BAD_STAGE && !repeated);
        xr_xir_compile_source_product_free(product);
        product_vm_drive(program,entry,ids,with_library);
        CHECK(xr_compile_resources_stats(context.resources,&after) == XR_COMPILE_RESOURCE_OK && after.live_bytes == baseline.live_bytes);
    }
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    puts("default SourceProduct Checked/Lowered execution, dead producer, failure preservation and dual physical0 PASS");
    return 0;
}
