/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_factory_cases.h - Source-created Iterator outlives the final code owner
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
#ifndef XIR_GENERATOR_FACTORY_CASES_H
#define XIR_GENERATOR_FACTORY_CASES_H
#include "xir/xxir_generator_internal.h"
#include "xir/xxir_instance_generator_internal.h"
static XrXirOutputStatus factory_unexpected_output(void *opaque,const XrXirOutputGroup *group) {
    (void)opaque;(void)group;CHECK(false);return XR_XIR_OUTPUT_OK;
}
static void generator_factory_cases(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,factory_unexpected_output,NULL};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    XrXirValue seed={XR_XIR_I64,0,25};
    CHECK(xr_xir_instance_start(instance,2,&seed,1)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue iterator={0};CHECK(xr_xir_instance_take_result(instance,&iterator)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&iterator));
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(generator->object.kind==XR_XIR_TYPE_ITERATOR && generator->call && generator->executor &&
        atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_PARKED &&
        !generator->accounting.polls && !generator->child);
    XrXirDomain *foreign=NULL;CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission other={generator->object.arena,foreign,NULL,NULL,10000,65536};
    CHECK(xr_xir_value_admit(&iterator,(XrXirType)iterator.type,&other)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_domain_drop(foreign);
    XrXirValue kept={0};CHECK(xr_xir_value_copy(&iterator,&kept)==XR_XIR_VALUE_OK);xr_xir_value_drop(&iterator);
    CHECK(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue unit={0};CHECK(xr_xir_instance_take_result(instance,&unit)==XR_XIR_CALL_RETURNED && !unit.type);
    CHECK(xr_xir_value_valid(&kept) && generator->call && !generator->accounting.polls);
    XrXirValue refused={0};
    CHECK(xr_xir_instance_generator_at(NULL,0,NULL,0,&refused)==XR_XIR_CALL_BAD_ARGUMENT && !refused.type);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_value_valid(&kept) && !generator->call && !generator->executor &&
        atomic_load_explicit(&generator->state,memory_order_acquire)==XIR_GENERATOR_TERMINAL &&
        generator->outcome.status==XR_XIR_CALL_CANCELLED && !generator->accounting.live_bytes &&
        generator->accounting.allocations==generator->accounting.frees);
    xr_xir_value_drop(&kept);
}
#endif
