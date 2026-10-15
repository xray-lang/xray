/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_constructed_sdk_consumer.c - Generated Task values through admitted archives
 */
#include "xir/xxir_program.h"
#include "xir/xxir_call.h"
#include "xir/xxir_task.h"
#include "xir/xxir_array.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_atomic.h"
#include "xir/xxir_value_internal.h"
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Task SDK FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "task_constructed_generated.h"
static void sdk_code_release(void *owner) { unsigned *count = owner; CHECK(!(*count)++); }
static bool constructed_i64_array(const XrXirValue *value, XrXirValueAdmission *admission) {
    int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, admission, &length);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
    CHECK(length == 2);
    for (int64_t i = 0; i < 2; ++i) {
        XrXirValue field = {0}; XrXirFaultDetail fault = {0};
        status = xr_xir_array_get(value, i, admission, &field, &fault);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(field.type == XR_XIR_I64 && field.payload == 10 * (i + 1)); xr_xir_value_drop(&field);
    }
    return true;
}
static bool constructed_value(const XrXirValue *value, unsigned family) {
    CHECK(xr_xir_value_valid(value));
    const XirObject *object = (const XirObject *)(uintptr_t)value->payload;
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, 1000000, 16777216};
    if (!family) return constructed_i64_array(value, &admission);
    if (family == 1) {
        int64_t length = 0; XrXirValueStatus status = xr_xir_array_len(value, &admission, &length);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        CHECK(length == 2); const char *words[] = {"owned", "array", "typed"}; unsigned word = 0;
        for (unsigned i = 0; i < 2; ++i) {
            XrXirValue inner = {0}; XrXirFaultDetail fault = {0};
            status = xr_xir_array_get(value, i, &admission, &inner, &fault);
            if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
            status = xr_xir_array_len(&inner, &admission, &length);
            if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT); return false; }
            CHECK(length == (i ? 1 : 2));
            for (int64_t j = 0; j < length; ++j) {
                XrXirValue text = {0}; status = xr_xir_array_get(&inner, j, &admission, &text, &fault);
                if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&inner); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
                const char *bytes = NULL; size_t size = 0;
                CHECK(xr_xir_string_view(&text, &bytes, &size) && size == 5 && !memcmp(bytes, words[word++], size));
                xr_xir_value_drop(&text);
            }
            xr_xir_value_drop(&inner);
        }
        CHECK(word == 3); return true;
    }
    if (family == 2 || family == 3) {
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(value, &some, &payload) && some && payload);
        if (family == 3) return constructed_i64_array(payload, &admission);
        CHECK(payload->type == XR_XIR_I64 && payload->payload == 42); return true;
    }
    if (family == 4) {
        XrXirValue number = {0}, text = {0};
        XrXirValueStatus status = xr_xir_tuple_get(value, 0, &number);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        status = xr_xir_tuple_get(value, 1, &text);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&number); CHECK(status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_OOM); return false; }
        const char *bytes = NULL; size_t length = 0;
        CHECK(number.type == XR_XIR_I64 && number.payload == 42 && xr_xir_string_view(&text, &bytes, &length) && length == 5 && !memcmp(bytes, "owned", length));
        xr_xir_value_drop(&number); xr_xir_value_drop(&text); return true;
    }
    if (family == 5) {
        XrXirEnumBorrow record = {0}; CHECK(xr_xir_enum_borrow(value, &record) == XR_XIR_VALUE_OK);
        CHECK(record.name.length == 6 && !memcmp(record.name.bytes, "Choice", 6) && record.member.length == 8 && !memcmp(record.member.bytes, "Selected", 8) && record.field_count == 1);
        return constructed_i64_array(&record.fields[0], &admission);
    }
    if (family == 6) {
        XrXirAtomicRequest request = {value, NULL, NULL, 0, XR_XIR_ATOMIC_OPERATION_LOAD, XR_XIR_I64};
        XrXirAtomicProgress progress = {0}; XrXirValue number = {0};
        XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &admission, NULL, &progress, &number);
        xr_xir_atomic_progress_clear(&progress);
        if (outcome.status != XR_XIR_RUN_OK || outcome.permission != XR_XIR_CALL_READY) {
            CHECK(outcome.status == XR_XIR_RUN_STEP_LIMIT || outcome.status == XR_XIR_RUN_FRAME_LIMIT || outcome.permission == XR_XIR_CALL_LIMIT || outcome.status == XR_XIR_RUN_OUT_OF_MEMORY); return false;
        }
        CHECK(!outcome.continuing && number.type == XR_XIR_I64 && number.payload == 42); xr_xir_value_drop(&number); return true;
    }
    CHECK(family == 7); XrXirCallResult inner = {0}; XrXirCallStatus status = xr_xir_task_copy_outcome(value, &inner);
    if (status != XR_XIR_CALL_RETURNED) { CHECK(status == XR_XIR_CALL_LIMIT); return false; }
    CHECK(inner.value.type == XR_XIR_I64 && inner.value.payload == 42); xr_xir_call_result_drop(&inner); return true;
}
int main(void) {
    XrXirDomain *probe = NULL; CHECK(xr_xir_domain_new(67108864,&probe) == XR_XIR_VALUE_OK);
    uint64_t domain_baseline = xr_xir_domain_stats(probe).live_bytes; xr_xir_domain_drop(probe);
    for (unsigned family = 0; family < 8; ++family) {
        const XrCompileResourceLimits limits = {67108864,67108864,128000000};
        XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_new(&limits,&context.resources) == XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats baseline = {0}; CHECK(xr_compile_resources_stats(context.resources,&baseline) == XR_COMPILE_RESOURCE_OK);
        unsigned code_releases = 0; XrXirProgramSpec spec = task_constructed_native_program;
        spec.code = (XrXirCodeLease){&code_releases,sdk_code_release}; XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&context,&spec,&program) == XR_XIR_OK);
        XrXirCallResult hosted[2] = {{0}}; XrXirDomain *host_domains[2] = {0};
        XrXirInstanceConfig host_config; CHECK(xr_xir_instance_config_init(&host_config,sizeof(host_config)) == XR_XIR_CALL_READY);
        for (unsigned mode = 0; mode < 2; ++mode) {
            XrXirHostExecutionRequest request = {program,&host_config,constructed_entries[family][mode],NULL,0};
            CHECK(xr_xir_host_execute(&request,&hosted[mode]) == XR_XIR_CALL_RETURNED);
            host_domains[mode] = ((const XirObject *)(uintptr_t)hosted[mode].value.payload)->domain;
            CHECK(xr_xir_domain_retain(host_domains[mode]));
        }
        XrXirInstance *instances[2] = {0}; XrXirValue tasks[2] = {{0}}; XrXirDomain *domains[2] = {0};
        for (unsigned i = 0; i < 2; ++i) {
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
        }
        xr_xir_compile_program_drop(program);
        for (unsigned i = 0; i < 2; ++i) {
            for (unsigned mode = 0; mode < 2; ++mode) {
                CHECK(xr_xir_instance_start(instances[i],constructed_entries[family][mode],NULL,0) == XR_XIR_CALL_READY);
                XrXirInstanceResult result = {0}; unsigned polls = 0;
                do { result = xr_xir_instance_poll_bounded(instances[i],1); CHECK(++polls < 4096); }
                while (result.outcome.status == XR_XIR_CALL_READY);
                CHECK(result.outcome.status == XR_XIR_CALL_RETURNED); XrXirValue value = {0};
                CHECK(xr_xir_instance_take_result(instances[i],&value) == XR_XIR_CALL_RETURNED);
                if (mode) {
                    tasks[i] = value; domains[i] = ((const XirObject *)(uintptr_t)value.payload)->domain;
                    CHECK(xr_xir_domain_retain(domains[i]));
                } else { CHECK(constructed_value(&value,family)); xr_xir_value_drop(&value); }
            }
            CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        }
        CHECK(code_releases == 1 && domains[0] != domains[1]);
        CHECK(constructed_value(&hosted[0].value,family));
        XrXirCallResult host_first = {0}, host_second = {0};
        CHECK(xr_xir_task_copy_outcome(&hosted[1].value,&host_first) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_task_copy_outcome(&hosted[1].value,&host_second) == XR_XIR_CALL_RETURNED);
        CHECK(constructed_value(&host_first.value,family) && constructed_value(&host_second.value,family));
        xr_xir_call_result_drop(&host_first); xr_xir_call_result_drop(&host_second);
        for (unsigned mode = 0; mode < 2; ++mode) {
            xr_xir_call_result_drop(&hosted[mode]);
            CHECK(xr_xir_domain_stats(host_domains[mode]).live_bytes == domain_baseline);
            XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(host_domains[mode]);
            CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
                spent.metadata_allocations == spent.metadata_frees); xr_xir_domain_drop(host_domains[mode]);
        }
        for (unsigned i = 0; i < 2; ++i) {
            XrXirCallResult first = {0}, second = {0};
            CHECK(xr_xir_task_copy_outcome(&tasks[i],&first) == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_task_copy_outcome(&tasks[i],&second) == XR_XIR_CALL_RETURNED);
            CHECK(constructed_value(&first.value,family)); CHECK(constructed_value(&second.value,family));
            if (family == 6 || family == 7) CHECK(first.value.payload == second.value.payload);
            xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second); xr_xir_value_drop(&tasks[i]);
            CHECK(xr_xir_domain_stats(domains[i]).live_bytes == domain_baseline);
            XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(domains[i]);
            CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations == spent.call_frees &&
                spent.metadata_allocations == spent.metadata_frees); xr_xir_domain_drop(domains[i]);
        }
        XrCompileResourceStats final = {0}; CHECK(xr_compile_resources_stats(context.resources,&final) == XR_COMPILE_RESOURCE_OK);
        CHECK(final.live_bytes == baseline.live_bytes); xr_compile_resources_release(context.resources);
    }
    puts("TASK_SDK families=8 twoInstances repeatedAwait escapedStickyCopies2 hostedOutcomesAfterCodeDeath lastCodeLease1 ownerBaselines=restored");
    return 0;
}
