/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * execution.h - Complete math calls after declaration ownership transfer
 */
#ifndef XIR_MATH_EXECUTION_H
#define XIR_MATH_EXECUTION_H
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
static void math_program(const XrXirCompileContext *context,const XrXirProgramSpec *spec) {
    XrXirProgram *program=NULL;
    XrXirStatus sealed=xr_xir_compile_program_seal(context,spec,&program);
    if (sealed!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Math Program seal status%u work%llu allocated%llu live%llu\n",sealed,
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(sealed==XR_XIR_OK);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    const uint32_t types[]={XR_XIR_I64,XR_XIR_F64};
    const int64_t results[]={51,INT64_C(0x4022000000000000)};
    for (uint32_t repeat=0;repeat<2;++repeat) for (uint32_t n=0;n<2;++n) {
        uint32_t entry=UINT32_MAX,ordinal=0;
        for (uint32_t f=0;f<spec->entry_count;++f) {
            const XrXirFunctionIdentity *identity=&spec->declarations->functions[f];
            if (!identity->exported || identity->module!=spec->declarations->root_module) continue;
            if (ordinal++==n) {entry=f;break;}
        }
        CHECK(entry!=UINT32_MAX && xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==types[n] && !value.reserved && value.payload==results[n]);
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
}
static void math_vm(const XrXirCompileContext *context,XrXirArtifact *checked,const char *emit) {
    XrXirArtifact *special=NULL,*lowered=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_specialize(checked,&special,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(special,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(special);
    if (emit) {
        XrXirCSource source={0};
        CHECK(xr_xir_compile_emit_c(lowered,"math_library",1048576,&source)==XR_XIR_OK);
        FILE *file=fopen(emit,"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    XrXirVmBinding *bindings=NULL;XrXirCallEntry *entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(lowered,&bindings,&entries)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
        module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    math_program(context,&spec);
    xr_compile_resources_free(entries);xr_compile_resources_free(bindings);
    xr_xir_compile_artifact_free(lowered);
}
#endif // XIR_MATH_EXECUTION_H
