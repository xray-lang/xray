/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_call_instance_owner.h - Actual Instance retirement accounting witnesses
 *
 * KEY CONCEPT:
 *   Execution uses the sealed Program owner; observations grant no authority.
 */
#ifndef XIR_CALL_INSTANCE_OWNER_H
#define XIR_CALL_INSTANCE_OWNER_H

/* The runtime implementation is included by the test's allocation observer.
 * Observe only its actual Instance object, after every activation retires. */
static XrXirInstance *call_retiring_instance;
static XrXirCallAccounting call_retired_accounting[2];
static uint32_t call_retirements;

static void call_runtime_retiring(void *pointer) {
    if (!call_retiring_instance || pointer!=call_retiring_instance) return;
    CHECK(!call_retiring_instance->call && !call_retiring_instance->executor);
    memcpy(call_retired_accounting,call_retiring_instance->accounting,sizeof(call_retired_accounting));
    ++call_retirements;
}

static void call_instance_config(XrXirInstanceConfig *config) {
    CHECK(xr_xir_instance_config_init(config,sizeof(*config))==XR_XIR_CALL_READY);
    config->metadata_limit=65536;config->value_limit=65536;config->call_limit=65536;
    config->poll_limit=100;config->depth_limit=10;
}
static XrXirInstance *call_instance_new(XrXirProgram *program) {
    XrXirInstanceConfig config;call_instance_config(&config);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    return instance;
}

static const XrXirCallAccounting *call_instance_accounting(const XrXirInstance *instance) {
    CHECK(instance && instance->call);
    return &instance->accounting[instance->epoch%2];
}

/* The Instance dispatcher is a real extra root frame. Prove its actual
 * position and keep both the original language depth and full physical cap. */
static uint32_t call_instance_language_depth(const XrXirInstance *instance) {
    const XrXirCallAccounting *accounting=call_instance_accounting(instance);
    const XrXirCall *call=instance->call;
    CHECK(call->config.instance==instance && accounting->depth && accounting->depth<=10);
    const CallFrame *root=call->top;
    uint32_t depth=0;
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        CHECK(++depth<=10);root=frame;
    }
    CHECK(depth==accounting->depth && root && !root->parent);
    CHECK(root->entry==&call->config.entries[instance->program->entry_count] &&
        root->entry->resume==initialization_resume && *(const uint32_t *)root->state==2);
    return depth-1;
}

static XrXirCallStatus call_instance_retire(XrXirInstance *instance) {
    CHECK(instance && !call_retiring_instance);
    call_retiring_instance=instance;call_retirements=0;
    memset(call_retired_accounting,0,sizeof(call_retired_accounting));
    XrXirCallStatus status=xr_xir_instance_free(instance);
    call_retiring_instance=NULL;
    CHECK(call_retirements==1);
    for (uint32_t i=0;i<2;++i) {
        const XrXirCallAccounting *accounting=&call_retired_accounting[i];
        CHECK(!accounting->live_bytes && !accounting->depth && accounting->allocations==accounting->frees);
        CHECK(accounting->peak_bytes<=65536 && accounting->polls<=100 && accounting->peak_depth<=10);
    }
    return status;
}
static void call_instance_free(XrXirInstance *instance,XrXirCallStatus expected) {
    CHECK(call_instance_retire(instance)==expected);
}
#endif // XIR_CALL_INSTANCE_OWNER_H
