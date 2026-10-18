/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_repl_publication_allocations.c - Actual Source coordinator allocation cuts
 *
 * KEY CONCEPT:
 *   Substitute only allocations requested by the real public coordinator and
 *   paired submission. Each cut fails the actual ledger allocator; parser and
 *   compiler internals keep their separate complete allocation obligations.
 */
#include "../test_framework.h"
#include "base/xcompile_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"

static size_t coordinator_attempts, coordinator_fail_at;
static bool coordinator_armed;
static void publication_cut(void) {
    if (coordinator_armed && ++coordinator_attempts == coordinator_fail_at)
        runtime_fail_at = runtime_attempts;
}
static XrCompileResourceStatus publication_calloc(XrCompileState *state,
    size_t count, size_t size, void **output) {
    publication_cut();
    XrCompileResourceStatus status = xr_compile_state_calloc(state,count,size,output);
    runtime_fail_at = SIZE_MAX;
    return status;
}
static XrCompileResourceStatus publication_strdup(XrCompileState *state,
    const char *text, char **output) {
    publication_cut();
    XrCompileResourceStatus status = xr_compile_state_strdup(state,text,output);
    runtime_fail_at = SIZE_MAX;
    return status;
}
static XrCompileResourceStatus publication_strndup(XrCompileState *state,
    const char *text, size_t length, char **output) {
    publication_cut();
    XrCompileResourceStatus status = xr_compile_state_strndup(state,text,length,output);
    runtime_fail_at = SIZE_MAX;
    return status;
}
#define xr_compile_state_calloc publication_calloc
#define xr_compile_state_strdup publication_strdup
#define xr_compile_state_strndup publication_strndup
#include "api/xsource_api.c"
#include "api/xsource_submission.c"
#undef xr_compile_state_calloc
#undef xr_compile_state_strdup
#undef xr_compile_state_strndup

typedef struct PublicationFixture {
    XrSourceRepl *owner;
    XrSourceResult *result;
    XrSourceDiagnostic *diagnostic;
    XrSourceValue *last, *function;
    XrXirValue call_result;
} PublicationFixture;
static PublicationFixture publication;
static void publication_close(void) {
    coordinator_armed = false;
    runtime_fail_at = SIZE_MAX;
    xr_xir_value_drop(&publication.call_result);
    XrSourceEvent cleanup = {0};
    XrSourceStatus status = xray_source_repl_drop(&publication.owner,&cleanup);
    XrSourceScalar held = {0};
    XrSourceStatus scalar_status = publication.last ?
        xray_source_value_scalar(publication.last,&held) : XR_SOURCE_OK;
    int64_t result = 15;
    XrSourceStatus result_status = publication.result ?
        xray_source_result_i64(publication.result,&result) : XR_SOURCE_OK;
    bool had_last = publication.last != NULL;
    xray_source_result_free(publication.result);
    xray_source_diagnostic_free(publication.diagnostic);
    xray_source_value_free(publication.last);
    xray_source_value_free(publication.function);
    memset(&publication,0,sizeof(publication));
    ASSERT_EQ_INT(scalar_status,XR_SOURCE_OK);
    if (had_last) ASSERT_EQ_INT(held.value.signed_value,11);
    ASSERT_EQ_INT(result_status,XR_SOURCE_OK);
    ASSERT_EQ_INT(result,15);
    ASSERT_EQ_INT(status,XR_SOURCE_OK);
    ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
    ASSERT_EQ_INT(runtime_live,0);
    ASSERT_EQ_INT(runtime_bytes,0);
}
static XrSourceWriteStatus publication_output(void *context,
    XrSourceOutputStream stream, const char *bytes, size_t length) {
    (void)context;
    CHECK(stream == XR_SOURCE_STDOUT && (bytes || !length));
    return XR_SOURCE_WRITE_OK;
}
static XrSourceStatus publication_submit(const char *source) {
    const XrSourceText input = {NULL,"repl-publication-allocations",source,strlen(source)};
    XrSourceStatus status = xray_source_repl_begin(publication.owner,&input,&publication.diagnostic);
    XrSourceEvent event = {0};
    unsigned polls = 0;
    while (status == XR_SOURCE_OK) {
        if (++polls > 100000) return XR_SOURCE_BAD_STATE;
        status = xray_source_repl_poll(publication.owner,16,&event);
        if (status != XR_SOURCE_OK || event.state != XR_SOURCE_RUN_READY) break;
    }
    if (status != XR_SOURCE_OK) return status;
    if (event.state != XR_SOURCE_RUN_RETURNED) return XR_SOURCE_EXECUTION_FAILED;
    return xray_source_repl_take(publication.owner,&publication.result);
}
static void publication_old_function(void) {
    XrXirInstance *runtime = publication.owner->submission->runtime;
    ASSERT_EQ_INT(xr_xir_instance_start_function(runtime,&publication.function->value,NULL,0),XR_XIR_CALL_READY);
    XrXirInstanceResult event = {0};
    unsigned polls = 0;
    do {
        ASSERT_LT(++polls,100000);
        event = xr_xir_instance_poll_bounded(runtime,16);
    } while (event.outcome.status == XR_XIR_CALL_READY);
    ASSERT_EQ_INT(event.outcome.status,XR_XIR_CALL_RETURNED);
    ASSERT_EQ_INT(xr_xir_instance_take_result(runtime,&publication.call_result),XR_XIR_CALL_RETURNED);
    ASSERT_EQ_INT(publication.call_result.type,XR_XIR_I64);
    ASSERT_EQ_INT(publication.call_result.payload,5);
    xr_xir_value_drop(&publication.call_result);
}
static void exercise_publication(size_t fail, size_t *allocation_attempts) {
    XrSourceCompilerOptions compiler;
    XrSourceCallOptions runtime;
    ASSERT_EQ_INT(xray_source_compiler_options_init(&compiler,sizeof(compiler)),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_call_options_init(&runtime,sizeof(runtime)),XR_SOURCE_OK);
    compiler.stdlib_path = XR_PUBLICATION_STDLIB;
    runtime.write = publication_output;
    ASSERT_EQ_INT(xray_source_repl_new(&compiler,"repl-publication-allocations",
        &runtime,&publication.owner),XR_SOURCE_OK);
    ASSERT_EQ_INT(publication_submit("fn value() -> i64 { return 5 }\n11 + 0\n"),XR_SOURCE_OK);
    xray_source_result_free(publication.result);
    publication.result = NULL;
    XrSourceRepl *owner = publication.owner;
    const XrSourceReplBindingInfo *before = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(xray_source_repl_bindings(owner,&before,&count),XR_SOURCE_OK);
    ASSERT_LE(count,16);
    XrSourceReplBindingInfo names[16];
    memcpy(names,before,count*sizeof(*names));
    SubmissionBirth *birth = owner->submission->published_births;
    ASSERT_NOT_NULL(birth);
    ASSERT_NULL(birth->previous);
    ASSERT_EQ_INT(birth->source,1);
    ASSERT_EQ_INT(xray_source_repl_read(owner,"it",&publication.last),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_repl_read(owner,"value",&publication.function),XR_SOURCE_OK);
    ASSERT_NOT_NULL(publication.function);
    coordinator_attempts = 0;
    coordinator_fail_at = fail;
    coordinator_armed = true;
    XrSourceStatus status = publication_submit(
        "fn value() -> string { return \"later\" }\nvar added = 9\n7 + 8\n");
    coordinator_armed = false;
    *allocation_attempts = coordinator_attempts;
    if (fail) {
        ASSERT_EQ_INT(status,XR_SOURCE_OUT_OF_MEMORY);
        ASSERT_EQ_INT(coordinator_attempts,fail);
        ASSERT_NULL(publication.result);
        const XrSourceReplBindingInfo *after = NULL;
        size_t after_count = 0;
        ASSERT_EQ_INT(xray_source_repl_bindings(owner,&after,&after_count),XR_SOURCE_OK);
        ASSERT_TRUE(after == before);
        ASSERT_EQ_INT(after_count,count);
        ASSERT_EQ_INT(memcmp(after,names,count*sizeof(*names)),0);
        ASSERT_NULL(owner->prepared_names);
        ASSERT_EQ_INT(owner->prepared_count,0);
        ASSERT_TRUE(owner->submission->published_births == birth);
        ASSERT_NULL(owner->submission->pending_birth);
        ASSERT_EQ_INT(owner->submission->source_serial,0);
        ASSERT_EQ_INT(owner->submission->runtime_serial,0);
        XrCompileState *state = xr_source_compiler_state(owner->compiler);
        ASSERT_EQ_INT(xr_compile_state_status(state),XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        XrCompileResources *resources = xr_source_compiler_resources(owner->compiler);
        XrCompileResourceStats spent = {0}, retry = {0};
        ASSERT_EQ_INT(xr_compile_resources_stats(resources,&spent),XR_COMPILE_RESOURCE_OK);
        size_t physical_attempts = runtime_attempts;
        const XrSourceText recovered = {NULL,"repl-publication-allocations",
            "var recovered = value()\n",strlen("var recovered = value()\n")};
        ASSERT_EQ_INT(xray_source_repl_begin(owner,&recovered,NULL),
            owner->stopped ? XR_SOURCE_BAD_STATE : XR_SOURCE_OUT_OF_MEMORY);
        ASSERT_EQ_INT(xr_compile_resources_stats(resources,&retry),XR_COMPILE_RESOURCE_OK);
        ASSERT_EQ_INT(memcmp(&spent,&retry,sizeof(spent)),0);
        ASSERT_EQ_INT(runtime_attempts,physical_attempts);
    } else {
        ASSERT_EQ_INT(status,XR_SOURCE_OK);
        ASSERT_NOT_NULL(publication.result);
        ASSERT_TRUE(owner->submission->published_births != birth);
        ASSERT_TRUE(owner->submission->published_births->previous == birth);
        XrSourceValue *last = NULL;
        ASSERT_EQ_INT(xray_source_repl_read(owner,"it",&last),XR_SOURCE_OK);
        XrSourceScalar scalar = {0};
        ASSERT_EQ_INT(xray_source_value_scalar(last,&scalar),XR_SOURCE_OK);
        xray_source_value_free(last);
        ASSERT_EQ_INT(scalar.value.signed_value,15);
    }
    publication_old_function();
    XrSourceScalar scalar = {0};
    ASSERT_EQ_INT(xray_source_value_scalar(publication.last,&scalar),XR_SOURCE_OK);
    ASSERT_EQ_INT(scalar.value.signed_value,11);
}
TEST(repl_actual_coordinator_allocations_do_not_publish_partial_metadata) {
    size_t successful_allocations = 0;
    exercise_publication(0,&successful_allocations);
    publication_close();
    ASSERT_GT(successful_allocations,0);
    for (size_t i = 1; i <= successful_allocations; ++i) {
        unsigned failed = (unsigned)xr_tests_failed;
        size_t actual = 0;
        exercise_publication(i,&actual);
        publication_close();
        ASSERT_EQ_INT(xr_tests_failed,failed);
        ASSERT_EQ_INT(actual,i);
    }
    printf("actual Source coordinator allocations: %zu; swept=%zu; original values11/15/5; "
        "sticky retry unchanged; physical=0/0\n",successful_allocations,successful_allocations);
}
#undef RUN_TEST
#define RUN_TEST(name) do { \
    unsigned failed = (unsigned)xr_tests_failed; ++xr_tests_run; \
    printf("  %-55s ",#name); fflush(stdout); test_##name(); publication_close(); \
    if ((unsigned)xr_tests_failed == failed) { ++xr_tests_passed; puts("PASS"); } \
} while (0)
TEST_MAIN_BEGIN()
RUN_TEST_SUITE("REPL publication allocation safety");
RUN_TEST(repl_actual_coordinator_allocations_do_not_publish_partial_metadata);
TEST_MAIN_END()
