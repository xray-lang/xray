/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_repl_generation.c - Original production generation publication
 *
 * KEY CONCEPT:
 *   Inspect actual Source records and paired runtime births. Failed serials
 *   remain consumed without keeping a second declaration history or ledger.
 */
#include "../test_framework.h"
#include "api/xsource_api.c"
#include "api/xsource_submission.c"
#include "xir/xxir_source.c"
#include <string.h>

static XrSourceRepl *generation_owner;
static XrSourceResult *generation_result;
static XrSourceDiagnostic *generation_diagnostic;
static XrSourceValue *generation_value, *generation_current;
static XrSourceEvent generation_event;
static void generation_close(void) {
    XrSourceEvent cleanup = {0};
    XrSourceStatus status = xray_source_repl_drop(&generation_owner,&cleanup);
    XrSourceScalar scalar = {0};
    bool held = generation_value != NULL;
    XrSourceStatus read = held ? xray_source_value_scalar(generation_value,&scalar) : XR_SOURCE_OK;
    xray_source_result_free(generation_result);generation_result = NULL;
    xray_source_diagnostic_free(generation_diagnostic);generation_diagnostic = NULL;
    xray_source_value_free(generation_value);generation_value = NULL;
    xray_source_value_free(generation_current);generation_current = NULL;
    ASSERT_EQ_INT(status,XR_SOURCE_OK);
    ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
    ASSERT_EQ_INT(read,XR_SOURCE_OK);
    if (held) ASSERT_EQ_INT(scalar.value.signed_value,42);
}
static XrSourceStatus generation_begin(const char *source) {
    xray_source_result_free(generation_result);generation_result = NULL;
    xray_source_diagnostic_free(generation_diagnostic);generation_diagnostic = NULL;
    generation_event = (XrSourceEvent){0};
    const XrSourceText text = {NULL,"compiler-session-fixture-v1",source,strlen(source)};
    return xray_source_repl_begin(generation_owner,&text,&generation_diagnostic);
}
static XrSourceStatus generation_drive(void) {
    unsigned polls = 0;
    XrSourceStatus status;
    do {
        if (++polls > 100000) return XR_SOURCE_BAD_STATE;
        status = xray_source_repl_poll(generation_owner,16,&generation_event);
    } while (status == XR_SOURCE_OK && generation_event.state == XR_SOURCE_RUN_READY);
    if (status != XR_SOURCE_OK) return status;
    return xray_source_repl_take(generation_owner,&generation_result);
}
TEST(production_repl_eval_publishes_and_abandons_declaration_generations) {
    XrSourceCompilerOptions compiler;
    XrSourceCallOptions runtime;
    ASSERT_EQ_INT(xray_source_compiler_options_init(&compiler,sizeof(compiler)),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_call_options_init(&runtime,sizeof(runtime)),XR_SOURCE_OK);
    compiler.stdlib_path = XR_GENERATION_STDLIB;
    ASSERT_EQ_INT(xray_source_repl_new(&compiler,"compiler-session-fixture-v1",
        &runtime,&generation_owner),XR_SOURCE_OK);
    XrSourceSubmission *submission = generation_owner->submission;
    XrXirSourceGenerations *source = submission->source;
    ASSERT_EQ_INT(generation_begin("var generation_value = 41\n"),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_drive(),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_event.state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(source->published->view.serial,1);
    ASSERT_EQ_INT(source->published->view.parent,0);
    ASSERT_EQ_INT(source->published->view.ast->as.program.count,1);
    ASSERT_TRUE(source->published->view.published);
    ASSERT_EQ_INT(generation_begin("var it = 1\n"),XR_SOURCE_REJECTED);
    ASSERT_NOT_NULL(generation_diagnostic);
    ASSERT_NULL(generation_result);
    ASSERT_EQ_INT(source->serial,2);
    ASSERT_NULL(source->pending);
    ASSERT_EQ_INT(source->published->view.serial,1);
    ASSERT_EQ_INT(generation_begin("var generation_next = generation_value + 1\n"),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_drive(),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_event.state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(source->published->view.serial,3);
    ASSERT_EQ_INT(source->published->view.parent,1);
    ASSERT_EQ_INT(source->published->view.ast->as.program.count,1);
    ASSERT_TRUE(source->published->view.published);
    ASSERT_EQ_INT(generation_begin("enum ReplGenerationError { Bad }\n"
        "throw ReplGenerationError.Bad\n"),XR_SOURCE_OK);
    ASSERT_NOT_NULL(source->pending);
    ASSERT_EQ_INT(source->pending->view.serial,4);
    ASSERT_EQ_INT(source->pending->view.parent,3);
    ASSERT_FALSE(source->pending->view.published);
    ASSERT_EQ_INT(generation_drive(),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_event.state,XR_SOURCE_RUN_THROWN);
    ASSERT_NOT_NULL(generation_result);
    ASSERT_EQ_INT(source->serial,4);
    ASSERT_NULL(source->pending);
    ASSERT_EQ_INT(source->published->view.serial,3);
    ASSERT_EQ_INT(source->published->previous->view.serial,1);
    ASSERT_NULL(source->published->previous->previous);
    ASSERT_EQ_INT(submission->published_births->source,3);
    ASSERT_EQ_INT(submission->published_births->previous->source,1);
    ASSERT_NULL(submission->published_births->previous->previous);
    ASSERT_NULL(submission->pending_birth);
    ASSERT_EQ_INT(submission->source_serial,0);
    ASSERT_EQ_INT(submission->runtime_serial,0);
    ASSERT_EQ_INT(xray_source_repl_read(generation_owner,"generation_next",&generation_value),XR_SOURCE_OK);
    XrSourceScalar scalar = {0};
    ASSERT_EQ_INT(xray_source_value_scalar(generation_value,&scalar),XR_SOURCE_OK);
    ASSERT_EQ_INT(scalar.value.signed_value,42);
    ASSERT_EQ_INT(generation_begin("generation_next += 1\n"),XR_SOURCE_OK);
    ASSERT_EQ_INT(source->pending->view.serial,5);
    ASSERT_EQ_INT(source->pending->view.parent,3);
    ASSERT_EQ_INT(generation_drive(),XR_SOURCE_OK);
    ASSERT_EQ_INT(generation_event.state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(source->published->view.serial,5);
    ASSERT_EQ_INT(source->published->previous->view.serial,3);
    ASSERT_EQ_INT(source->published->previous->previous->view.serial,1);
    ASSERT_NULL(source->published->previous->previous->previous);
    ASSERT_NULL(source->pending);
    ASSERT_EQ_INT(xray_source_repl_read(generation_owner,"generation_next",&generation_current),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_value_scalar(generation_current,&scalar),XR_SOURCE_OK);
    ASSERT_EQ_INT(scalar.value.signed_value,43);
    puts("actual Source serials1/3 published,2/4 abandoned,next5; parents5/3/1; current43/owned42");
}
#undef RUN_TEST
#define RUN_TEST(name) do { \
    unsigned failed = (unsigned)xr_tests_failed; ++xr_tests_run; \
    printf("  %-55s ",#name); fflush(stdout); test_##name(); generation_close(); \
    if ((unsigned)xr_tests_failed == failed) { ++xr_tests_passed; puts("PASS"); } \
} while (0)
TEST_MAIN_BEGIN()
RUN_TEST(production_repl_eval_publishes_and_abandons_declaration_generations);
TEST_MAIN_END()
