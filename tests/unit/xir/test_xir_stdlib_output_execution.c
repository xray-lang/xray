/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stdlib_output_execution.c - Owned Checked stdlib output consumers
 *
 * KEY CONCEPT: Source-free VM and native executions use independent byte oracles.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#if !CONSUMER_KIND
#include "xir/xxir_emit_c.c"
#include "xir/xxir_library_catalog.c"
#endif
#include "xir_stdlib_output_runtime.h"
typedef struct PublicationProbe { unsigned vector, calls; XrXirOutputStatus status; } PublicationProbe;
static const char *const publication_bytes[] = {"out\0\xe4\xb8\xad", "err!", ""};
static const size_t publication_lengths[] = {7, 4, 0};
static XrXirOutputStatus publication_write(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    PublicationProbe *probe = context;
    CHECK(stream == (probe->vector == 1 ? XR_XIR_STDERR : XR_XIR_STDOUT));
    CHECK(length == publication_lengths[probe->vector]);
    CHECK(!length || !memcmp(bytes, publication_bytes[probe->vector], length));
    ++probe->calls;
    return probe->status;
}
static bool publication_run(XrXirProgram *program, const uint32_t ids[2]) {
    const XrXirOutputStatus statuses[] = {XR_XIR_OUTPUT_OK, XR_XIR_OUTPUT_ERROR,
        XR_XIR_OUTPUT_OOM, XR_XIR_OUTPUT_LIMIT};
    for (unsigned instance_number = 0; instance_number < 2; ++instance_number)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned vector = 0; vector < 3; ++vector) {
                PublicationProbe probe = {vector, 0, statuses[mode]};
                XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION,0,publication_write,&probe,1024};
                XrXirInstanceConfig config;
                CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
                config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
                XrXirInstance *instance = NULL; XrXirDomain *domain = NULL;
                XrXirValue argument = {0}, result = {0};
                XrXirCallStatus status = xr_xir_instance_new(program,&config,&instance);
                bool oom = status == XR_XIR_CALL_OOM;
                if (!oom) {
                    CHECK(status == XR_XIR_CALL_READY);
                    XrXirValueStatus value_status = xr_xir_domain_new(65536,&domain);
                    oom = value_status == XR_XIR_VALUE_OOM;
                    if (!oom) {
                        CHECK(value_status == XR_XIR_VALUE_OK);
                        value_status = xr_xir_string_new(domain,publication_bytes[vector],publication_lengths[vector],&argument);
                        oom = value_status == XR_XIR_VALUE_OOM;
                        CHECK(oom || value_status == XR_XIR_VALUE_OK);
                    }
                }
                if (!oom) {
                    status = xr_xir_instance_start(instance,ids[vector == 1],&argument,1);
                    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
                    oom = status == XR_XIR_CALL_OOM && (mode != 2 || !probe.calls);
                    if (!oom) {
                        CHECK(probe.calls == 1);
                        CHECK(status == (mode < 2 ? XR_XIR_CALL_RETURNED :
                            mode == 2 ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT));
                        if (mode < 2) {
                            CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_RETURNED);
                            CHECK(result.type == XR_XIR_BOOL && result.payload == (mode == 0));
                        }
                    }
                }
                xr_xir_value_drop(&argument); xr_xir_value_drop(&result);
                if (domain) xr_xir_domain_drop(domain);
                if (instance) CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
                if (oom) return false;
            }
    return true;
}
static void publication_finish(XrXirProgram *program, const uint32_t ids[2]) {
    size_t live = runtime_live, bytes = runtime_bytes;
    runtime_attempts = 0; CHECK(publication_run(program,ids)); size_t sites = runtime_attempts;
    CHECK(runtime_live == live && runtime_bytes == bytes);
    for (size_t failure = 0; failure < sites; ++failure) {
        runtime_attempts = 0; runtime_fail_at = failure;
        bool ok = publication_run(program,ids); runtime_fail_at = SIZE_MAX;
        CHECK(!ok && runtime_live == live && runtime_bytes == bytes);
    }
    xr_xir_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
    printf("stdlib output consumer=%u twoInstances streams/BOOL/NUL/empty OOM=%zu physicalzero\n",CONSUMER_KIND,sites);
}
#if !CONSUMER_KIND
XR_FUNC size_t *xr_test_stdlib_output_counter(unsigned index) {
    CHECK(index < 4); size_t *counters[] = {&runtime_attempts,&runtime_fail_at,&runtime_live,&runtime_bytes};
    return counters[index];
}
XR_FUNC void xr_test_stdlib_output_execute(XrXirArtifact *checked, const char *generated) {
    XrXirArtifact *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_specialize(checked,NULL,&closed,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL) == XR_XIR_OK);
    xr_xir_artifact_free(closed);
    const XrXirModule *module = xr_xir_artifact_module(lowered);
    const char *names[] = {"stdoutText","stderrText"}; uint32_t ids[2] = {UINT32_MAX,UINT32_MAX};
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (unsigned i = 0; i < 2; ++i)
            if (module->functions[f].name_length == strlen(names[i]) &&
                !memcmp(module->functions[f].name,names[i],strlen(names[i]))) {
                CHECK(ids[i] == UINT32_MAX); ids[i] = f;
            }
    CHECK(ids[0] != UINT32_MAX && ids[1] != UINT32_MAX);
    XrXirCSource source = {0};
    CHECK(xr_xir_emit_c(lowered,"stdlib_output",1048576,&source) == XR_XIR_OK);
    FILE *file = fopen(generated,"wb"); CHECK(file);
    CHECK(fwrite(source.text,1,source.length,file) == source.length);
    CHECK(fprintf(file,"\nconst uint32_t stdlib_output_exports[2]={%u,%u};\n",ids[0],ids[1]) > 0);
    CHECK(!fclose(file)); xr_xir_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){1048576,1048576},&program) == XR_XIR_OK);
    CHECK(!lowered); publication_finish(program,ids);
}
#else
extern const XrXirProgramSpec stdlib_output_program;
extern const uint32_t stdlib_output_exports[2];
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&stdlib_output_program,(XrXirProgramBudget){1048576,1048576},&program) == XR_XIR_OK);
    publication_finish(program,stdlib_output_exports); return 0;
}
#endif
