/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_mixed_class_faults.h - Class result owners retain cumulative execution costs
 */
#ifndef XIR_MIXED_CLASS_FAULTS_H
#define XIR_MIXED_CLASS_FAULTS_H
static bool class_fault_fields(const XrXirValue *receiver, XrXirValue *text, XrXirCallStatus fault) {
    CHECK(xr_xir_value_valid(receiver));
    const XrXirValueStatus failure = fault == XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OOM;
    XrXirValueAdmission admission = {.arena=xr_xir_value_arena(receiver), .work=10000, .scratch_bytes=65536};
    XrXirValue number = {0};
    XrXirValueStatus copied = xr_xir_class_get(receiver, 0, &admission, &number);
    if (copied != XR_XIR_VALUE_OK) {
        CHECK(copied == failure && !number.type && !number.payload); return false;
    }
    CHECK(number.type == XR_XIR_I64 && number.payload == 40); xr_xir_value_drop(&number);
    copied = xr_xir_class_get(receiver, 1, &admission, text);
    if (copied != XR_XIR_VALUE_OK) {
        CHECK(copied == failure && !text->type && !text->payload && xr_xir_value_valid(receiver)); return false;
    }
    const char *data = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(text, &data, &length) && length == 10 && !memcmp(data, "held-class", 10));
    return true;
}
static bool class_fault_operation(XrXirProgram *program,
    const XrXirInstanceConfig *limits, XrXirDomainBudgetStats *cost) {
    const XrXirCallStatus fault = limits ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OOM;
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirInstance *instance = NULL; XrXirDomain *domain = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    bool complete = status == XR_XIR_CALL_READY, driven = false;
    CHECK(complete || (status == fault && !instance));
    XrXirValue retained[2] = {{0}}, text[2] = {{0}}; XrXirDomainBudgetStats previous = {0};
    if (instance) { domain = instance->domain; CHECK(xr_xir_domain_retain(domain)); }
    for (unsigned round = 0; complete && round < 2; ++round) {
        status = xr_xir_instance_start(instance, class_lifetime_entry, NULL, 0);
        if (status == XR_XIR_CALL_READY) {
            unsigned polls = 0;
            do {
                status = xr_xir_instance_poll_bounded(instance, 1).outcome.status;
                CHECK(++polls < 4096);
            } while (status == XR_XIR_CALL_READY);
            driven = status == fault;
            if (status == XR_XIR_CALL_RETURNED) {
                XrXirValue occupied = {XR_XIR_I64, 0, 73}, before = occupied;
                CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT && !memcmp(&occupied, &before, sizeof(before)));
                CHECK(xr_xir_instance_take_result(instance, &retained[round]) == XR_XIR_CALL_RETURNED);
                CHECK(xr_xir_value_valid(&retained[round]));
            }
        }
        complete = status == XR_XIR_CALL_RETURNED;
        CHECK(complete || status == fault);
        CHECK(instance->domain == domain);
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        if (complete && round) CHECK(spent.work > previous.work && spent.requested_bytes > previous.requested_bytes &&
            spent.requested_call_bytes > previous.requested_call_bytes);
        previous = spent;
    }
    XrXirCallStatus expected = driven || (instance && instance->budget.exhausted) ? fault : XR_XIR_CALL_READY;
    XrXirCallStatus freed = xr_xir_instance_free(instance);
    if (freed != expected) fprintf(stderr, "Class retirement site=%zu driven=%u status=%u expected=%u\n",
        runtime_fail_at, (unsigned)driven, freed, expected);
    CHECK(freed == expected);
    for (unsigned round = 0; round < 2; ++round) if (retained[round].type) {
        if (!class_fault_fields(&retained[round], &text[round], fault)) complete = false;
        xr_xir_value_drop(&retained[round]);
        if (text[round].type) {
            const char *data = NULL; size_t length = 0;
            CHECK(xr_xir_string_view(&text[round], &data, &length) && length == 10 && !memcmp(data, "held-class", 10));
        }
        xr_xir_value_drop(&text[round]);
    }
    if (domain) {
        XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domain);
        CHECK(spent.work >= previous.work && spent.requested_bytes >= previous.requested_bytes &&
            spent.requested_call_bytes >= previous.requested_call_bytes);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
            spent.metadata_allocations == spent.metadata_frees && xr_xir_domain_stats(domain).live_bytes == sizeof(*domain));
        if (cost) *cost = spent;
        xr_xir_domain_drop(domain);
    }
    CHECK(runtime_live == blocks && runtime_bytes == bytes && !class_code_releases);
    return complete;
}
static void class_axes(XrXirProgram *program, unsigned backend) {
    XrXirDomainBudgetStats cost = {0}; CHECK(class_fault_operation(program, NULL, &cost));
    const uint64_t axes[] = {cost.requested_bytes, cost.requested_call_bytes, cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(axes[axis]);
        for (unsigned minus = 0; minus < 2; ++minus) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            if (!axis) config.requested_value_limit = axes[axis] - minus;
            else if (axis == 1) config.requested_call_limit = axes[axis] - minus;
            else config.work_limit = axes[axis] - minus;
            CHECK(class_fault_operation(program, &config, NULL) == !minus);
        }
    }
    printf("Class cumulative axes backend=%s value=%llu call=%llu work=%llu, exact/minus1 and no refund\n",
        class_backends[backend], (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
}
static void class_faults(XrXirProgram *program, unsigned backend) {
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    CHECK(class_fault_operation(program, NULL, NULL));
    const size_t sites = runtime_attempts; CHECK(sites && sites < 20000);
    for (size_t site = 0; site < sites; ++site) {
        runtime_attempts = 0; runtime_fail_at = site;
        CHECK(!class_fault_operation(program, NULL, NULL) && runtime_attempts > site);
    }
    runtime_fail_at = SIZE_MAX;
    printf("Class complete runtime FI backend=%s sites=%zu, every point hit, typed OOM and physical0/0\n", class_backends[backend], sites);
}
#endif
