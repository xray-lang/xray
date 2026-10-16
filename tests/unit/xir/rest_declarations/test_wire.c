/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_wire.c - Independent rest packets, exact ownership and authentic values
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_types.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_library_compile_owner.h"
#include "wire_golden.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA==32 && XR_XIR_CHECKED_CONTRACT==79 && XR_XIR_PROGRAM_ABI_VERSION==34,
    "Independent physical and semantic identities");
_Static_assert(XR_XIR_ARRAY_NEW==73 && XR_XIR_FUNCTION_REF==57 && XR_XIR_CALL_INDIRECT==58 &&
    XR_XIR_CALLABLE_VARIADIC==16 && sizeof(XrXirFunctionIdentity)==40 &&
    offsetof(XrXirFunctionIdentity,variadic)==36,"Independent rest fields");

static XrXirStatus rest_wire_operation(const XrXirCompileContext *context,void *unused) {
    (void)unused;XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,rest_wire,sizeof(rest_wire),&artifact,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"independent rest packet status%u function%u instruction%u reason%u\n",
            status,diagnostic.function,diagnostic.instruction,diagnostic.reason);
    if (status==XR_XIR_OK) {
        status=xr_xir_compile_checked_write(artifact,&packet,NULL);
        if (status==XR_XIR_OK) CHECK(packet.length==sizeof(rest_wire) && !memcmp(packet.bytes,rest_wire,packet.length));
    }
    xr_xir_compile_artifact_free(artifact);xr_xir_compile_checked_packet_free(&packet);return status;
}
static void rest_word(uint8_t *bytes,uint32_t value) {
    for (uint32_t b=0;b<4;++b) bytes[b]=(uint8_t)(value>>(8*b));
}
static void rest_attacks(const XrXirCompileContext *context) {
    const uint32_t fields[][2]={
        {REST_IDENTITY+40+36,2},{REST_IDENTITY+40+36,0},{REST_IDENTITY+36,1},
        {REST_IDENTITY+80+36,1},{REST_ARRAY,3},{REST_ARRAY+8,0},
        {REST_CALLABLE+24,2},{REST_CALLABLE+24,50},{REST_CALLABLE+16,1},
        {REST_ENTRY+4*40+4,258}};
    for (unsigned a=0;a<sizeof(fields)/sizeof(*fields);++a) {
        uint8_t bytes[sizeof(rest_wire)];memcpy(bytes,rest_wire,sizeof(bytes));
        rest_word(bytes+fields[a][0],fields[a][1]);
        XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,bytes,32);
        xr_sha256_update(&hash,bytes+64,sizeof(bytes)-64);xr_sha256_final(&hash,bytes+32);
        XrXirArtifact *output=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(context,bytes,sizeof(bytes),&output,NULL);
        if (status==XR_XIR_OK) fprintf(stderr,"unexpected accepted rest attack %u\n",a);
        CHECK(status!=XR_XIR_OK && !output);
    }
}
static XrXirResumeEntry original_read;
static void rest_raw_shape(const XrXirCompileContext *context) {
    XrXirCallableParameter parameter={(XrXirType)256,XR_PARAM_READ};
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_I64,.flags=18}};
    const XrXirTypes types={nodes,2,NULL,NULL};
    for (unsigned attack=0;attack<5;++attack) {
        parameter=(XrXirCallableParameter){(XrXirType)256,XR_PARAM_READ};
        nodes[1].parameter_count=1;
        if (attack==1) parameter.type=XR_XIR_I64;
        if (attack==2) parameter.mode=XR_PARAM_REF;
        if (attack==3) nodes[1].parameter_count=0;
        if (attack==4) nodes[1].parameter_count=UINT32_MAX;
        CHECK(xr_xir_compile_callable_weakening(context,&types,(XrXirType)257,(XrXirType)257)==
            (attack ? XR_XIR_BAD_TYPE : XR_XIR_OK));
    }
}
static unsigned authentic_calls;
static XrXirAction rest_authentic_read(XrXirCallView *view) {
    XrXirValue sentinel={XR_XIR_I64,0,900},forged=sentinel,owned={0};
    CHECK(xr_xir_instance_function(view,(XrXirType)258,1,NULL,0,&forged)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&forged,&sentinel,sizeof(forged)));
    CHECK(xr_xir_instance_function(view,(XrXirType)257,1,NULL,0,&owned)==XR_XIR_CALL_READY);
    uint32_t target=UINT32_MAX;
    CHECK(xr_xir_instance_resolve_function(view,&owned,&target)==XR_XIR_CALL_READY && target==1);
    xr_xir_value_drop(&owned);++authentic_calls;
    return original_read(view);
}
static void rest_execution(const XrXirCompileContext *context) {
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,rest_wire,sizeof(rest_wire),&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(xr_xir_compile_callable_weakening(context,module->types,(XrXirType)257,(XrXirType)258)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_callable_weakening(context,module->types,(XrXirType)258,(XrXirType)257)==XR_XIR_BAD_TYPE);
    XrXirFunctionIdentity identities[3];memcpy(identities,module->declarations->functions,sizeof(identities));
    XrXirDeclarations declarations=*module->declarations;declarations.functions=identities;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
        &declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    const XrXirFunctionLayout *layouts=xr_xir_compile_artifact_layout(lowered,0);
    CHECK(xr_xir_compile_program_match(context,&spec,layouts,lowered)==XR_XIR_OK);
    identities[1].variadic=0;
    CHECK(xr_xir_compile_program_match(context,&spec,layouts,lowered)==XR_XIR_BAD_STRUCTURE);
    identities[1].variadic=1;
    XrXirProgram *program=NULL;spec.abi_version=32;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)!=XR_XIR_OK && !program);
    spec.abi_version=XR_XIR_PROGRAM_ABI_VERSION;
    original_read=entries[1].resume;entries[1].resume=rest_authentic_read;
    CHECK(xr_xir_compile_program_seal(context,&spec,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==84 && authentic_calls>=2);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);xr_compile_resources_free(bindings);xr_compile_resources_free(entries);
    xr_xir_compile_artifact_free(lowered);
}
int main(void) {
    const uint32_t roots[]={2,4,8,12};
    for (unsigned a=0;a<4;++a) for (unsigned b=0;b<4;++b) for (uint32_t no_suspend=0;no_suspend<2;++no_suspend) {
        uint32_t left=roots[a]|no_suspend,right=roots[b],joined=UINT32_MAX,rest=UINT32_MAX;
        CHECK(xr_xir_callable_flags_compatible(left|16,right|16)==xr_xir_callable_flags_compatible(left,right));
        CHECK(!xr_xir_callable_flags_compatible(left|16,right) && !xr_xir_callable_flags_compatible(left,right|16));
        CHECK(xr_xir_callable_flags_join(left,right,&joined) && xr_xir_callable_flags_join(left|16,right|16,&rest));
        CHECK(rest==(joined|16));rest=900;
        CHECK(!xr_xir_callable_flags_join(left|16,right,&rest) && rest==900);
        CHECK(xr_xir_callable_root_accepts(a==1 || a==3,a==2 || a==3,right|16)==
            xr_xir_callable_root_accepts(a==1 || a==3,a==2 || a==3,right));
    }
    library_compile_operation_cases("Owned rest packet read/write",rest_wire_operation,NULL);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    rest_raw_shape(&owner.context);
    rest_attacks(&owner.context);rest_execution(&owner.context);
    library_compile_owner_drop(&owner);library_compile_observer_free();
    puts("Independent rest wire, authentic runtime values, exact budgets and physical FI PASS");return 0;
}
