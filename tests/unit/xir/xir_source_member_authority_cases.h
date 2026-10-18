/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_member_authority_cases.h - Genuine private binding creation and sealed input integrity
 */
#ifndef XIR_SOURCE_MEMBER_AUTHORITY_CASES_H
#define XIR_SOURCE_MEMBER_AUTHORITY_CASES_H
#include "xir/xxir_vm.h"
static void source_member_vm(XrXirArtifact *checked) {
    const XrXirCompileContext context=*xr_xir_compile_artifact_context(checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length==6 && !memcmp(function->name,"answer",6)) { CHECK(entry==UINT32_MAX);entry=f; }
    }
    CHECK(entry!=UINT32_MAX);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    for (uint32_t run=0;run<2;++run) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirCallResult outcome=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
        CHECK(outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==7);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
static void source_member_authorized_reference(XrXirSourceRequest *request) {
    write_generic_source(request->entry_path,
        "struct S{private secret()->i64{return 7};get()->fn()->i64{return this.secret}}\n"
        "export fn answer()->i64{const callback=S().get();return callback()}\n");
    XrXirSourceResult result={0};CHECK(source_generic_check(request,&result,NULL)==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);uint32_t authorized=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        for (uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *op=&function->instructions[i];if (op->op!=XR_XIR_FUNCTION_REF) continue;
            CHECK(op->immediate>=0 && (uint64_t)op->immediate<module->function_count);
            const XrXirFunctionIdentity *target=&module->declarations->functions[op->immediate];
            if (target->member_access!=XR_XIR_MEMBER_PRIVATE) continue;
            CHECK(target->nominal_owner && target->nominal_owner==module->declarations->functions[f].nominal_owner);
            ++authorized;
        }
    }
    CHECK(authorized==1 && xr_xir_compile_artifact_verify(result.checked,NULL)==XR_XIR_OK);
    XrXirArtifact *checked=result.checked;result.checked=NULL;xr_xir_compile_source_result_free(&result);source_member_vm(checked);
}
#endif // XIR_SOURCE_MEMBER_AUTHORITY_CASES_H
