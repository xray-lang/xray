/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derivation_native.c - Actual native grant metadata and body receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_program.h"
#include "xir/xxir_derivation_internal.h"
XR_DATA const XrXirProgramSpec derive_authority_program;

int main(void) {
    const XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirProgramSpec spec=derive_authority_program;
    XrXirDeclarations declarations=*spec.declarations;
    XrXirDerivationTable table=*declarations.derivations;
    CHECK(!table.policy_count && table.count==1);
    XrXirDerivationGrant binding=table.records[0];
    XrXirGeneratedOrigin origin=binding.origins.entry;
    table.records=&binding;declarations.derivations=&table;spec.declarations=&declarations;
    XrXirProgram *program=NULL;
    binding.root=1;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program);
    binding.root=0;binding.origins.entry.line=999;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program);
    binding.origins.entry=origin;declarations.derivations=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_BAD_STRUCTURE && !program);
    declarations.derivations=&table;
    CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK && program);
    memset(&spec,0xa5,sizeof(spec));memset(&declarations,0xa5,sizeof(declarations));
    memset(&table,0xa5,sizeof(table));memset(&binding,0xa5,sizeof(binding));
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,0,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,128).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Native private READ42; actual binding/origin/missing-table rejection; same-owner successor; physical0");
    return 0;
}
