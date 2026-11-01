/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * crypto_suite_runtime.h - Execute authentic discovered crypto test entries
 *
 * KEY CONCEPT:
 *   Each original test starts through test-role admission and has an actual
 *   terminal outcome. A fresh Instance isolates failures without skipping work.
 */
#ifndef CRYPTO_SUITE_RUNTIME_H
#define CRYPTO_SUITE_RUNTIME_H
#include "xir/xxir_program.h"
#include "crypto_suite_inventory.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static bool crypto_suite_run(XrXirProgram *program,uint32_t function,const char *backend,const char *name) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance,function)==XR_XIR_CALL_READY);
    XrXirInstanceResult result;unsigned slices=0;
    do {
        CHECK(++slices<=100000);
        result=xr_xir_instance_poll_bounded(instance,1024);
    } while(result.outcome.status==XR_XIR_CALL_READY);
    bool passed=result.outcome.status==XR_XIR_CALL_RETURNED;
    if(passed) {
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_UNIT);xr_xir_value_drop(&value);
    } else fprintf(stderr,"%s %s status=%u panic=%u\n",backend,name,result.outcome.status,result.outcome.panic.detail.code);
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    CHECK(closed==XR_XIR_CALL_READY || (!passed && closed==result.outcome.status));
    printf("%s %s %s\n",backend,name,passed?"PASS":"FAIL");fflush(stdout);
    return passed;
}
static void crypto_suite_report(FILE *report,const CryptoSuite *suite,const char *digest,
    const uint32_t *functions,const bool *passed,bool executed,unsigned ordinal) {
    CHECK(fprintf(report,"%s{\"source\":\"%s\",\"sourceSha256\":\"%s\",\"entries\":[",
        ordinal?",":"",suite->path,digest)>0);
    for(uint32_t i=0;i<suite->count;++i)
        CHECK(fprintf(report,"%s{\"name\":\"%s\",\"function\":%u,\"role\":1,\"timeout\":0,\"executed\":%s,\"passed\":%s}",
            i?",":"",suite->names[i],functions[i],executed?"true":"false",passed[i]?"true":"false")>0);
    CHECK(fputs("]}",report)>=0);
}
#endif // CRYPTO_SUITE_RUNTIME_H
