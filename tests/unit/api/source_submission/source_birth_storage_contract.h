/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_birth_storage_contract.h - Complete wire and real Instance admission
 */
#ifndef SOURCE_BIRTH_STORAGE_CONTRACT_H
#define SOURCE_BIRTH_STORAGE_CONTRACT_H
#include "xir/xxir_checked.h"
#include "xir/compile_owner/birth_storage76_golden.h"
#include "xir/storage76_previous_vectors.h"
static void birth_storage_contract(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    const XrXirCompileContext context={owned.resources,xr_xir_compile_default_limits()};
    const uint8_t *valid[]={birth76_program,birth76_fresh,birth76_library_fresh};
    const size_t sizes[]={sizeof(birth76_program),sizeof(birth76_fresh),sizeof(birth76_library_fresh)};
    for (unsigned i=0;i<3;++i) {
        XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
        XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_checked_read(&context,valid[i],sizes[i],&checked,&diagnostic);
        if (status!=XR_XIR_OK) fprintf(stderr,"storage positive%u status=%u function=%u instruction=%u\n",
            i,status,diagnostic.function,diagnostic.instruction);
        CHECK(status==XR_XIR_OK);
        const XrXirModule *module=xr_xir_compile_artifact_module(checked);
        CHECK(module->declarations->module_count==2 &&
            module->declarations->modules[0].storage==(uint32_t)(i==0 ? XR_XIR_MODULE_BIRTH_STORAGE : XR_XIR_MODULE_FRESH_STORAGE) &&
            module->declarations->modules[1].storage==XR_XIR_MODULE_FRESH_STORAGE);
        XrXirCheckedPacket encoded={0};
        CHECK(xr_xir_compile_checked_write(checked,&encoded,NULL)==XR_XIR_OK);
        CHECK(encoded.length==sizes[i] && !memcmp(encoded.bytes,valid[i],encoded.length));
        xr_xir_compile_checked_packet_free(&encoded);
        if (i!=2) {
            CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
            CHECK(xr_xir_compile_lower(closed,&owned.owner->target, NULL,&lowered,NULL)==XR_XIR_OK);
            XrXirProgram *program=NULL;
            CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
            CHECK(program->requires_birth_storage==(i==0));
            XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
            XrXirInstance *instance=NULL;
            CHECK(xr_xir_instance_new(program,&config,&instance)==(i==0 ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_READY));
            if (i==0) {
                CHECK(!instance && xr_xir_instance_generation_new(&config,&instance)==XR_XIR_CALL_READY);
                uint64_t serial=0;
                CHECK(xr_xir_instance_generation_prepare(instance,program,&serial)==XR_XIR_CALL_BAD_STATE && !serial);
            } else {
                CHECK(xr_xir_instance_start(instance,2,NULL,0)==XR_XIR_CALL_READY);
                XrXirCallStatus terminal;
                do { terminal=xr_xir_instance_poll_bounded(instance,8).outcome.status; } while (terminal==XR_XIR_CALL_READY);
                XrXirValue value={0};CHECK(terminal==XR_XIR_CALL_RETURNED &&
                    xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
                    value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
            }
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
        }
        xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    }
    const uint8_t *invalid[]={birth76_root_rejected,birth76_unknown_rejected,birth76_library_rejected,birth75_retired};
    const size_t lengths[]={sizeof(birth76_root_rejected),sizeof(birth76_unknown_rejected),sizeof(birth76_library_rejected),sizeof(birth75_retired)};
    for (unsigned i=0;i<4;++i) for (unsigned occupied=0;occupied<2;++occupied) {
        XrXirArtifact *sentinel=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*output=sentinel;
        CHECK(xr_xir_compile_checked_read(&context,invalid[i],lengths[i],&output,NULL)==XR_XIR_BAD_STRUCTURE && output==sentinel);
    }
    for (size_t i=0;i<sizeof(storage76_previous)/sizeof(*storage76_previous);++i)
        for (unsigned occupied=0;occupied<2;++occupied) {
            XrXirArtifact *sentinel=occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL,*output=sentinel;
            CHECK(xr_xir_compile_checked_read(&context,storage76_previous[i].bytes,storage76_previous[i].length,
                &output,NULL)==XR_XIR_BAD_STRUCTURE && output==sentinel);
        }
    submission_close(&owned);
    puts("Independent complete storage packets: Program/fresh/Library positive, root/unknown/Library/old refusal, exact encoding, real fresh Instance and unbound generation denial, physical0");
}
#endif // SOURCE_BIRTH_STORAGE_CONTRACT_H
