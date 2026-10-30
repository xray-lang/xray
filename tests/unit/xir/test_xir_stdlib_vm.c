/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_vm.c - Owned Checked library execution and provider outcomes
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_stdlib_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"stdlib test FAIL %d %s\n",__LINE__,#test); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
typedef struct StdlibProbe { unsigned vector, calls; XrXirOutputStatus status; } StdlibProbe;
static const char *const stdlib_oracle_bytes[] = {"out\0\xe4\xb8\xad","err!",""};
static const size_t stdlib_oracle_lengths[] = {7,4,0};
static XrXirOutputStatus stdlib_write(void *context, XrXirOutputStream stream,
    const char *data, size_t length) {
    StdlibProbe *probe = context;
    CHECK(stream == (probe->vector == 1 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(length == stdlib_oracle_lengths[probe->vector] && (!length || !memcmp(data,stdlib_oracle_bytes[probe->vector],length)));
    ++probe->calls; return probe->status;
}
static XrXirArtifact *stdlib_program(const XrXirCompileContext *context,
    const char *root, const char *entry, uint32_t ids[6]) {
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xir_stdlib_library_catalog_new(context,root,&catalog) == XR_XIR_OK);
    XrCompilerSession *session = NULL; CHECK(xr_compile_session_new(context->resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status != XR_XIR_OK) fprintf(stderr,"stdlib consumer Source status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    for (unsigned i = 0; i < 6; ++i) ids[i] = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *declaration = &module->declarations->functions[f];
        const char *names[] = {"writeStdout","writeStderr","stdoutText","stderrText","writeStdout","writer"};
        for (unsigned i = 0; i < 6; ++i) {
            bool library = declaration->module != module->declarations->root_module;
            if (library != (i < 2)) continue;
            if (function->name_length == strlen(names[i]) && !memcmp(function->name,names[i],function->name_length)) {
                CHECK(ids[i] == UINT32_MAX); ids[i] = f;
            }
        }
    }
    for (unsigned i = 0; i < 6; ++i) CHECK(ids[i] != UINT32_MAX);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    return lowered;
}
static void stdlib_execute(XrXirProgram *program, const uint32_t ids[2]) {
    const XrXirOutputStatus statuses[] = {XR_XIR_OUTPUT_OK,XR_XIR_OUTPUT_ERROR,XR_XIR_OUTPUT_OOM,XR_XIR_OUTPUT_LIMIT};
    for (unsigned instance_number = 0; instance_number < 2; ++instance_number)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned vector = 0; vector < 3; ++vector) {
                StdlibProbe probe = {vector,0,statuses[mode]};
                XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION,0,stdlib_write,&probe,1024};
                XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
                config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
                XrXirInstance *instance = NULL; CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
                XrXirDomain *domain = NULL; XrXirValue argument = {0}, output = {0};
                CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
                CHECK(xr_xir_string_new(domain,stdlib_oracle_bytes[vector],stdlib_oracle_lengths[vector],&argument) == XR_XIR_VALUE_OK);
                CHECK(xr_xir_instance_start(instance,ids[vector == 1],&argument,1) == XR_XIR_CALL_READY);
                xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
                XrXirCallStatus status = xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
                CHECK(probe.calls == 1);
                CHECK(status == (mode < 2 ? XR_XIR_CALL_RETURNED : mode == 2 ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT));
                if (mode < 2) {
                    CHECK(xr_xir_instance_take_result(instance,&output) == XR_XIR_CALL_RETURNED);
                    CHECK(output.type == XR_XIR_BOOL && output.payload == (mode == 0));
                } else {
                    /* Ordinary root resource faults survive result transfer and physical release. */
                    CHECK(xr_xir_instance_take_result(instance,&output) == status);
                    CHECK(!output.type && !output.payload && !output.reserved);
                    output = (XrXirValue){XR_XIR_I64,0,77};
                    XrXirValue sentinel; memcpy(&sentinel,&output,sizeof(output));
                    CHECK(xr_xir_instance_take_result(instance,&output) == status);
                    CHECK(!memcmp(&output,&sentinel,sizeof(output)));
                    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status == status);
                }
                xr_xir_value_drop(&output);
                CHECK(xr_xir_instance_free(instance) == (mode < 2 ? XR_XIR_CALL_READY : status));
                CHECK(!runtime_live && !runtime_bytes);
            }
}
static void stdlib_escape(XrXirProgram *program, uint32_t writer) {
    StdlibProbe probe = {0,0,XR_XIR_OUTPUT_OK};
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION,0,stdlib_write,&probe,1024};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *first = NULL, *second = NULL;
    CHECK(xr_xir_instance_new(program,&config,&first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&second) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(first,writer,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue function = {0}, alias = {0}, argument = {0}, retained = {0}, output = {0};
    CHECK(xr_xir_instance_take_result(first,&function) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_copy(&function,&alias) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,stdlib_oracle_bytes[0],stdlib_oracle_lengths[0],&argument) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&argument,&retained) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_start_function(second,&alias,&argument,1) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_function(first,&alias,&argument,1) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument); xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(first,&output) == XR_XIR_CALL_RETURNED && output.type == XR_XIR_BOOL && output.payload == 1);
    CHECK(probe.calls == 1); CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(second,&alias,NULL,0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    CHECK(xr_xir_value_copy(&alias,&argument) == XR_XIR_VALUE_OK);
    const char *text = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&retained,&text,&length) && length == 7 && !memcmp(text,stdlib_oracle_bytes[0],7));
    xr_xir_value_drop(&argument); xr_xir_value_drop(&retained); xr_xir_value_drop(&output);
    xr_xir_value_drop(&function); xr_xir_value_drop(&alias);
    CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    XrCompileResourceLimits caps = {UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial = {0}; CHECK(xr_compile_resources_stats(context.resources,&initial) == XR_COMPILE_RESOURCE_OK);
    for (unsigned mode = 0; mode < 3; ++mode) {
        uint32_t ids[6]; XrXirArtifact *lowered = stdlib_program(&context,argv[1],argv[2],ids);
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered,&program) == XR_XIR_OK && !lowered);
        const uint32_t selected[] = {ids[mode == 2 ? 2 : 0],ids[mode == 2 ? 3 : 1]};
        stdlib_execute(program,selected);
        if (mode == 1) stdlib_escape(program,ids[5]); else xr_xir_compile_program_drop(program);
        printf("stdlib mode=%u twoInstances provider4 vector3 owns producer-death baseline\n",mode);
    }
    XrCompileResourceStats final = {0}; CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == initial.live_bytes); xr_compile_resources_release(context.resources); context.resources = NULL;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("stdlib normal allocated=%llu peak=%llu work=%llu ledger baseline\n",(unsigned long long)final.allocated_bytes,
        (unsigned long long)final.peak_bytes,(unsigned long long)final.work);
    return 0;
}
