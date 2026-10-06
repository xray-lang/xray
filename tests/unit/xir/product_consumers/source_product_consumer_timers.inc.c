/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_timers.inc.c - Original source duration and host readiness
 *
 * KEY CONCEPT:
 *   The true standard module produces the timer; only elapsed host time resumes it.
 */
#include "execution/xr_xir_host_time.h"
#include "os/os_time.h"

static bool consumer_timer_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "time_sleep");
}

static const char *consumer_stdlib_path(void) {
    return consumer_timer_case() ? XR_CONSUMER_STDLIB : NULL;
}

static void consumer_timer_source(const XrXirSourceProduct *product) {
    if (!consumer_timer_case()) return;
    const XrXirSourceView *view = xr_xir_compile_source_product_view(product);
    const char identity[] = "stdlib-module-v1:module=4:time:path=12:time/time.xr";
    const char suffix[] = "/time/time.xr";
    CHECK(view && view->complete && view->module_count == 2);
    uint32_t matches = 0;
    for (uint32_t m = 0; m < view->module_count; ++m) {
        const XrXirSourceQueryModule *module = &view->modules[m];
        CHECK(module->identity);
        if (strcmp(module->identity, identity)) continue;
        CHECK(!matches++ && module->path);
        size_t root = strlen(XR_CONSUMER_STDLIB), length = strlen(module->path);
        CHECK(length == root + sizeof(suffix) - 1);
        for (size_t p = 0; p < length; ++p) {
            char expected = p < root ? XR_CONSUMER_STDLIB[p] : suffix[p - root];
            char actual = module->path[p];
            CHECK((actual == '\\' ? '/' : actual) == (expected == '\\' ? '/' : expected));
        }
    }
    CHECK(matches == 1);
}

static void consumer_timer_shape(const XrXirModule *module, uint32_t answer) {
    if (!consumer_timer_case()) return;
    const XrXirDeclarations *d = module->declarations;
    const char identity[] = "stdlib-module-v1:module=4:time:path=12:time/time.xr";
    CHECK(d && d->module_count == 2 && answer < module->function_count);
    uint32_t timer_module = UINT32_MAX;
    for (uint32_t m = 0; m < d->module_count; ++m)
        if (d->modules[m].name_length == sizeof(identity) - 1 &&
            !memcmp(d->modules[m].name, identity, sizeof(identity) - 1)) {
            CHECK(timer_module == UINT32_MAX); timer_module = m;
        }
    CHECK(timer_module != UINT32_MAX && timer_module != d->root_module);
    const XrXirFunction *function = &module->functions[answer];
    uint32_t sleeps = 0, timers = 0, returns = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_CALL) {
            CHECK(op->immediate >= 0 && (uint64_t)op->immediate < module->function_count);
            uint32_t callee = (uint32_t)op->immediate;
            const XrXirFunction *sleep = &module->functions[callee];
            CHECK(!sleeps++ && d->functions[callee].module == timer_module && d->functions[callee].exported);
            CHECK(sleep->name_length == 5 && !memcmp(sleep->name, "sleep", 5));
            CHECK(sleep->parameter_count == 1 && sleep->parameters[0] == XR_XIR_I64 && sleep->result == XR_XIR_UNIT);
            CHECK(op->type == XR_XIR_UNIT && op->args[1] == 1 && op->args[0] < function->operand_count);
            uint32_t value = function->operands[op->args[0]];
            CHECK(value < function->instruction_count && function->instructions[value].op == XR_XIR_CONST_INT);
            CHECK(function->instructions[value].type == XR_XIR_I64 && function->instructions[value].immediate == 10);
        } else if (op->op == XR_XIR_RETURN) {
            uint32_t value = op->args[0];
            CHECK(!returns++ && value < function->instruction_count);
            CHECK(function->instructions[value].op == XR_XIR_CONST_INT && function->instructions[value].type == XR_XIR_I64);
            CHECK(function->instructions[value].immediate == 239);
        }
    }
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            const XrXirInstruction *op = &module->functions[f].instructions[i];
            if (op->op != XR_XIR_TIMER_AFTER_MS) continue;
            CHECK(!timers++ && d->functions[f].module == timer_module && op->type == XR_XIR_UNIT);
            CHECK(xr_xir_operand_type(&module->functions[f], op->args[0]) == XR_XIR_I64);
            CHECK(!op->args[1] && !op->targets[0] && !op->targets[1] && !op->immediate);
        }
    CHECK(sleeps == 1 && timers == 1 && returns == 1);
}

static void consumer_timer_arm(XrXirHostWait *host, const XrXirWaitRequest *request, size_t resumes) {
    CHECK(!resumes && request->kind == XR_XIR_WAIT_TIMER_MS && !request->reserved && request->after_ms == 10);
    CHECK(!request->subject && !request->generation && !request->ticket && !host->active);
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_host_wait_begin(host, request) == XR_XIR_HOST_WAIT_PENDING);
    CHECK(host->active && host->start_ns && host->duration_ns == UINT64_C(10000000));
    CHECK(runtime_attempts == attempts);
}

static void consumer_timer_due(XrXirHostWait *host) {
    CHECK(host->active && host->duration_ns == UINT64_C(10000000));
    XrXirHostWaitStatus status;
    unsigned polls = 0;
    size_t attempts = runtime_attempts;
    do {
        CHECK(++polls < 1000);
        status = xr_xir_host_wait_poll(host, UINT64_C(1000000));
        CHECK(status == XR_XIR_HOST_WAIT_PENDING || status == XR_XIR_HOST_WAIT_DUE);
    } while (status == XR_XIR_HOST_WAIT_PENDING);
    uint64_t now = xr_time_monotonic_ns();
    CHECK(now >= host->start_ns && now - host->start_ns >= UINT64_C(10000000));
    CHECK(runtime_attempts == attempts);
    printf("timer-host-due requested-ms=10 elapsed-ns=%llu polls=%u\n",
        (unsigned long long)(now - host->start_ns), polls);
    *host = (XrXirHostWait){0};
}

static void consumer_timer_cancel_wait(XrXirInstance *instance, uint64_t epoch,
                                      uint64_t wake, XrXirHostWait *host) {
    CHECK(consumer_timer_case() && host->active && host->duration_ns == UINT64_C(10000000));
    XrXirWaitRequest sentinel;
    unsigned char before[sizeof(sentinel)];
    memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(before, &sentinel, sizeof(before));
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_wait_request(instance, epoch, wake, &sentinel) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&sentinel, before, sizeof(sentinel)) && runtime_attempts == attempts);
    *host = (XrXirHostWait){0};
    CHECK(xr_xir_host_wait_poll(host, 0) == XR_XIR_HOST_WAIT_BAD_ARGUMENT);
}

static void consumer_timer_cancel_result(XrXirInstance *instance) {
    if (!consumer_timer_case()) return;
    XrXirValue empty = {0};
    unsigned char empty_before[sizeof(empty)];
    memcpy(empty_before, &empty, sizeof(empty_before));
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_instance_take_result(instance, &empty) == XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&empty, empty_before, sizeof(empty)) && runtime_attempts == attempts);
    XrXirValue sentinel;
    unsigned char before[sizeof(sentinel)];
    memset(&sentinel, 0xa5, sizeof(sentinel)); memcpy(before, &sentinel, sizeof(before));
    CHECK(xr_xir_instance_take_result(instance, &sentinel) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&sentinel, before, sizeof(sentinel)) && runtime_attempts == attempts);
}
