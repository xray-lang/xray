/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_input_storage.c - Current canonical and projection storage
 *
 * KEY CONCEPT:
 *   Genuine live ranges precede every body read, then exact fields still match.
 */
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.h"
#include "xir/xxir_program.h"
#include "xir/xxir_generic.h"
#include "xir_construction_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"%d: %s\n",__LINE__,#condition); exit(1); } } while (0)

static XrXirArtifact *program_input(const XrXirCompileContext *context) {
    const XrXirInstruction init = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction main[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_CALL,XR_XIR_I64,{0,1},{0},2,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}
    };
    const XrXirInstruction identity = {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirType parameter = XR_XIR_I64;
    const uint32_t argument = 0;
    const XrXirBlock blocks[] = {{0,1,0,0},{0,3,0,0},{0,1,0,0}};
    const XrXirFunction functions[] = {
        {"init",4,NULL,0,XR_XIR_UNIT,blocks,1,&init,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks+1,1,main,3,&argument,1},
        {"identity",8,&parameter,1,XR_XIR_I64,blocks+2,1,&identity,1,NULL,0}
    };
    const XrXirSourceModule source = {"root",4,NULL,0,0,0,XR_XIR_MODULE_FRESH_STORAGE};
    const XrXirFunctionIdentity identities[] = {
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0},
        {0,0,0,0,0,0,XR_XIR_NON_MEMBER,0,0}
    };
    const XrXirDeclarations declarations = {&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirModule module = {XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xir_fixture_check(context,&module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_artifact_free(closed);
    return lowered;
}

static void result42(XrXirProgram *program) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,1,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);
    xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}

int main(int argc,char **argv) {
    CHECK(argc==2);
    unsigned attack=(unsigned)strtoul(argv[1],NULL,10);
    CHECK(attack<=19);
    const XrCompileResourceLimits limits={67108864,8388608,128000000};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=program_input(&context);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(artifact,&effects)==XR_XIR_OK);
    CHECK(xir_effects_program_body_match(&context,&artifact->module,effects)==XR_XIR_OK);
    EffectBodyMaterial *material=effects->invocations->body;
    EffectBodyFunction *owned=material->functions;
    EffectBodyFunction saved_owned=owned[1],saved_parameter=owned[2];
    XrXirModule saved_module=artifact->module;
    XrXirFunction *raw=(XrXirFunction *)artifact->module.functions;
    XrXirFunction saved_raw=raw[1];
    const void *invalid=(const void *)(uintptr_t)1;
    if(attack==1)owned[1].name=invalid;
    if(attack==2)owned[2].parameters=invalid;
    if(attack==3)owned[1].blocks=invalid;
    if(attack==4)owned[1].operands=invalid;
    if(attack==5)owned[1].runs=(EffectBodyRun *)invalid;
    if(attack==6)owned[2].initial_roles=invalid;
    if(attack==7)material->functions=(EffectBodyFunction *)invalid;
    if(attack==8)raw[1].name=invalid;
    if(attack==9)raw[1].blocks=invalid;
    if(attack==10)raw[1].instructions=invalid;
    if(attack==11)raw[1].operands=invalid;
    if(attack==12)artifact->module.functions=invalid;
    if(attack==13)artifact->module.provenance=invalid;
    if(attack==14)owned[1].run_capacity=UINT32_MAX;
    if(attack==15)artifact->module.types=invalid;
    if(attack==16)raw[1].blocks=NULL;
    if(attack==17)owned[1].blocks=(const XrXirBlock *)((const uint8_t *)owned+1);
    if(attack==18)raw[1].instructions=(const XrXirInstruction *)((const uint8_t *)raw+1);
    if(attack==19)owned[1].run_count=owned[1].run_capacity+1;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus status=xir_effects_program_body_match(&context,&artifact->module,effects);
    CHECK(status==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work);
    material->functions=owned;owned[1]=saved_owned;owned[2]=saved_parameter;
    artifact->module=saved_module;raw[1]=saved_raw;
    CHECK(xir_effects_program_body_match(&context,&artifact->module,effects)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && !artifact);
    result42(program);xr_xir_compile_program_drop(program);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    printf("storage attack%u status%u; no allocation; same-ledger restored42; live baseline recovered\n",attack,(unsigned)status);
    return 0;
}
