/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_cell_escape_cases.h - Escaped yields survive captured Cell cleanup and Program retirement
 */
#ifndef XIR_GENERATOR_CELL_ESCAPE_CASES_H
#define XIR_GENERATOR_CELL_ESCAPE_CASES_H
static void closure_cell_escape_case(XrXirProgram *program,const uint32_t ids[8]) {
    GeneratorWaitLog log={0};XrXirInstance *instance=wait_instance(program,&log);
    xr_xir_compile_program_drop(program);
    XrXirValue iterator=wait_make(instance,ids[0]);
    CHECK(xr_xir_instance_start(instance,ids[2],&iterator,1)==XR_XIR_CALL_READY);
    XrXirInstanceResult pending=wait_timer(instance,5);wait_resume(instance,pending);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue held={0};CHECK(xr_xir_instance_take_result(instance,&held)==XR_XIR_CALL_RETURNED);
    wait_string(&held,"one");CHECK(!log.count);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==1 && !log.relay && !log.cycle);
    CHECK(xr_xir_value_valid(&iterator));xr_xir_value_drop(&iterator);
    wait_string(&held,"one");xr_xir_value_drop(&held);
}
#endif // XIR_GENERATOR_CELL_ESCAPE_CASES_H
