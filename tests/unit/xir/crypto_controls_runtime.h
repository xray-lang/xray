/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto_controls_runtime.h - Real ref paths, function values and retired split results
 *
 * KEY CONCEPT:
 *   Both dispatchers execute the same complete control module. An owned String
 *   copied from its split aggregate survives every execution and Program owner.
 */
#ifndef CRYPTO_CONTROLS_RUNTIME_H
#define CRYPTO_CONTROLS_RUNTIME_H
#include "crypto_native_runtime.h"
static XrXirInstanceResult crypto_controls_finish(XrXirInstance *instance) {
    XrXirInstanceResult result;unsigned polls=0;
    do {CHECK(++polls<100000);result=xr_xir_instance_poll_bounded(instance,1024);}while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static void crypto_controls_run(XrXirProgram *program,uint32_t entry,uint32_t retained) {
    CryptoOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,crypto_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=crypto_controls_finish(instance);
    fprintf(stderr,"OWNER status%u panic%u\n",result.outcome.status,result.outcome.panic.detail.code);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(output.count==3 && !memcmp(output.bytes,"42\n",3));
    CHECK(xr_xir_instance_start(instance,retained,NULL,0)==XR_XIR_CALL_READY);
    CHECK(crypto_controls_finish(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    const char *bytes=NULL;size_t count=0;
    CHECK(xr_xir_string_view(&value,&bytes,&count) && count==3 && !memcmp(bytes,"A\0B",3));
    xr_xir_value_drop(&value);
}
#endif // CRYPTO_CONTROLS_RUNTIME_H
