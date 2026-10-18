/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_native_owner.c - Native entries checked against fixed expected values
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec compile_owner_program;
static XrCompileResourceStats ledger_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void packet_rejections(const XrXirCompileContext *context) {
    CHECK(compile_owner_program.packet.bytes && compile_owner_program.packet.length>=64 &&
        compile_owner_program.packet.identity);
    XrXirProgramSpec missing=compile_owner_program;
    missing.packet=(XrXirProgramPacket){0};
    XrXirProgram *output=NULL;XrCompileResourceStats before=ledger_stats(context->resources);
    CHECK(xr_xir_compile_program_seal(context,&missing,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(ledger_stats(context->resources).live_bytes==before.live_bytes);
    uint8_t identity[32];memcpy(identity,compile_owner_program.packet.identity,sizeof(identity));
    identity[0]^=1;
    XrXirProgramSpec changed=compile_owner_program;changed.packet.identity=identity;
    CHECK(xr_xir_compile_program_seal(context,&changed,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(ledger_stats(context->resources).live_bytes==before.live_bytes);
}
extern const XrXirProgramSpec zero_scratch_cleanup_program;
int main(void) {
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    packet_rejections(&context);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&compile_owner_program,&program)==XR_XIR_OK);
    XrCompileResources *ledger=context.resources;
    XrXirProgram *cleanup=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&zero_scratch_cleanup_program,&cleanup)==XR_XIR_OK);
    XrCompileResourceStats sealed=ledger_stats(ledger);CHECK(sealed.live_bytes>0);
    xr_compile_resources_release(context.resources);
    CHECK(ledger_stats(ledger).live_bytes==sealed.live_bytes);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(ledger_stats(ledger).live_bytes>0 && ledger_stats(ledger).allocated_bytes>=sealed.allocated_bytes);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);
    xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL; size_t length=0;
    CHECK(xr_xir_string_view(&value,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"A\0\xe4\xb8\xad",5));
    xr_xir_value_drop(&value);
    instance=NULL;
    CHECK(xr_xir_instance_new(cleanup,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(cleanup);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && !value.payload);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
    puts("Native compiler owner: 42, owned UTF-8 with embedded NUL and zero-slot cleanup");
    return 0;
}
