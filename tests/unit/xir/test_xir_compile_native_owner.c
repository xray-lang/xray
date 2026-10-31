/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_native_owner.c - Native entries checked against fixed expected values
 */
#include "xir/xxir_program.h"
#include "xir/xxir_array.h"
#include "xir/xxir_buffer_storage.h"
#include "xir/xxir_type_arena.h"
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
static void storage_container_domains(XrXirCompileContext *context) {
    XrCompileResourceStats before=ledger_stats(context->resources);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_BUFFER_STORAGE};
    const XrXirTypes types={&node,1,NULL,NULL};XrXirTypeArena *arena=NULL;
    const XrXirType array_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    for(unsigned path=0;path<2;++path)for(unsigned cross=0;cross<2;++cross)for(unsigned populated=0;populated<2;++populated) {
        XrXirDomain *home=NULL,*other=NULL;
        CHECK(xr_xir_domain_new(1048576,&home)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_domain_new(1048576,&other)==XR_XIR_VALUE_OK);
        uint64_t home_frame=xr_xir_domain_stats(home).live_bytes,other_frame=xr_xir_domain_stats(other).live_bytes;
        XrXirValueAdmission owner={.arena=arena,.domain=home,.work=UINT64_MAX,.scratch_bytes=1048576};
        XrXirValueAdmission caller={.arena=arena,.domain=cross?other:home,.work=UINT64_MAX,.scratch_bytes=1048576};
        XrXirValue array={0},old={0},incoming={0},read={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_with_capacity(array_type,4,&owner,&array)==XR_XIR_VALUE_OK);
        int64_t capacity=0;CHECK(xr_xir_array_capacity(&array,&owner,&capacity)==XR_XIR_VALUE_OK && capacity==4);
        XrXirValuePlace root={array_type,&array.payload};
        if(populated) {
            CHECK(xr_xir_buffer_storage_new(home,21,0,true,&old)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_array_push(&root,&old,&owner)==XR_XIR_VALUE_OK);
        }
        CHECK(xr_xir_buffer_storage_new(caller.domain,13,0,true,&incoming)==XR_XIR_VALUE_OK);
        int64_t original=array.payload;
        uint64_t held_home=xr_xir_domain_stats(home).live_bytes,held_other=xr_xir_domain_stats(other).live_bytes;
        XrXirValuePath no_steps={0};
        XrXirValueStatus status=path ? xr_xir_value_path_push(&root,&no_steps,&incoming,&caller,&fault) :
            xr_xir_array_push(&root,&incoming,&caller);
        if(cross && populated) {
            CHECK(status==XR_XIR_VALUE_BAD_ARGUMENT && array.payload==original);
            CHECK(xr_xir_domain_stats(home).live_bytes==held_home && xr_xir_domain_stats(other).live_bytes==held_other);
            CHECK(xr_xir_value_admit(&array,array_type,&caller)==XR_XIR_VALUE_BAD_ARGUMENT);
            int64_t length=0;CHECK(xr_xir_array_len(&array,&owner,&length)==XR_XIR_VALUE_OK && length==1);
            CHECK(xr_xir_array_get(&array,0,&owner,&read,&fault)==XR_XIR_VALUE_OK && read.payload==old.payload);
            CHECK(xr_xir_value_admit(&read,XR_XIR_BUFFER_STORAGE,&owner)==XR_XIR_VALUE_OK);
        } else {
            CHECK(status==XR_XIR_VALUE_OK && (cross ? array.payload!=original : array.payload==original));
            CHECK(xr_xir_value_admit(&array,array_type,&caller)==XR_XIR_VALUE_OK);
            int64_t length=0;CHECK(xr_xir_array_len(&array,&caller,&length)==XR_XIR_VALUE_OK && length==1+(int64_t)populated);
            CHECK(xr_xir_array_get(&array,populated,&caller,&read,&fault)==XR_XIR_VALUE_OK && read.payload==incoming.payload);
            CHECK(xr_xir_value_admit(&read,XR_XIR_BUFFER_STORAGE,&caller)==XR_XIR_VALUE_OK);
            if(cross)CHECK(xr_xir_value_admit(&array,array_type,&owner)==XR_XIR_VALUE_BAD_ARGUMENT);
        }
        xr_xir_value_drop(&read);xr_xir_value_drop(&array);xr_xir_value_drop(&old);xr_xir_value_drop(&incoming);
        CHECK(xr_xir_domain_stats(home).live_bytes==home_frame && xr_xir_domain_stats(other).live_bytes==other_frame);
        xr_xir_domain_drop(home);xr_xir_domain_drop(other);
    }
    xr_xir_compile_type_arena_drop(arena);
    CHECK(ledger_stats(context->resources).held_bytes==before.held_bytes);
}
static void packet_rejections(const XrXirCompileContext *context) {
    CHECK(compile_owner_program.packet.bytes && compile_owner_program.packet.length>=64 &&
        compile_owner_program.packet.identity);
    XrXirProgramSpec missing=compile_owner_program;
    missing.packet=(XrXirProgramPacket){0};
    XrXirProgram *output=NULL;XrCompileResourceStats before=ledger_stats(context->resources);
    CHECK(xr_xir_compile_program_seal(context,&missing,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(ledger_stats(context->resources).held_bytes==before.held_bytes);
    uint8_t identity[32];memcpy(identity,compile_owner_program.packet.identity,sizeof(identity));
    identity[0]^=1;
    XrXirProgramSpec changed=compile_owner_program;changed.packet.identity=identity;
    CHECK(xr_xir_compile_program_seal(context,&changed,&output)==XR_XIR_BAD_STRUCTURE && !output);
    CHECK(ledger_stats(context->resources).held_bytes==before.held_bytes);
}
extern const XrXirProgramSpec zero_scratch_cleanup_program;
int main(void) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    const XrCompileResourceStats baseline=ledger_stats(context.resources);
    storage_container_domains(&context);
    packet_rejections(&context);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&compile_owner_program,&program)==XR_XIR_OK);
    XrCompileResources *ledger=context.resources;
    XrXirProgram *cleanup=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&zero_scratch_cleanup_program,&cleanup)==XR_XIR_OK);
    XrCompileResourceStats sealed=ledger_stats(ledger);CHECK(sealed.held_bytes>baseline.held_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(ledger_stats(ledger).held_bytes==sealed.held_bytes);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(ledger_stats(ledger).held_bytes>baseline.held_bytes && ledger_stats(ledger).allocated_bytes>=sealed.allocated_bytes);
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
