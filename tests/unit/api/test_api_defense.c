/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_api_defense.c - Original boundary and lifecycle checks on explicit owners
 */
#include "../test_framework.h"
#include "api/xsource_api.c"
#include "api/xsource_submission.c"
#include "xir/xxir_task.c"
#include "xir/xxir_instance.c"

typedef struct DefenseTrace { unsigned owner;char bytes[32];size_t count; } DefenseTrace;
static struct {
    XrSourceRepl *owners[2];
    XrSourceResult *results[2];
    XrSourceValue *values[2];
    XrSourceDiagnostic *diagnostic;
    DefenseTrace traces[2];
    bool bad_callback;
} defense;
static const char *const defense_namespaces[]={"api-defense-a","api-defense-b"};
static XrSourceWriteStatus defense_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    DefenseTrace *trace=context;
    if(trace->owner>1 || stream!=XR_SOURCE_STDOUT || count>sizeof(trace->bytes)-trace->count)return XR_SOURCE_WRITE_LIMIT;
    XrSourceRepl *owner=defense.owners[trace->owner],*borrow=owner;XrSourceEvent unchanged={.engine_status=777};
    if(owner && (xray_source_repl_drop(&borrow,&unchanged)!=XR_SOURCE_BUSY || borrow!=owner || unchanged.engine_status!=777)) {
        defense.bad_callback=true;return XR_SOURCE_WRITE_IO;
    }
    memcpy(trace->bytes+trace->count,bytes,count);trace->count+=count;return XR_SOURCE_WRITE_OK;
}
static void defense_open(unsigned index) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    ASSERT_EQ_INT(xray_source_compiler_options_init(&compiler,sizeof(compiler)),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_call_options_init(&runtime,sizeof(runtime)),XR_SOURCE_OK);
    compiler.stdlib_path=XR_API_DEFENSE_STDLIB;defense.traces[index].owner=index;
    runtime.write=defense_output;runtime.context=&defense.traces[index];
    ASSERT_EQ_INT(xray_source_repl_new(&compiler,defense_namespaces[index],&runtime,&defense.owners[index]),XR_SOURCE_OK);
}
static XrSourceStatus defense_begin(unsigned index,const char *source) {
    const XrSourceText input={NULL,defense_namespaces[index],source,source?strlen(source):0};
    return xray_source_repl_begin(defense.owners[index],&input,&defense.diagnostic);
}
static XrSourceEvent defense_drive(unsigned index) {
    XrSourceEvent event={0};unsigned polls=0;
    do {
        if(xray_source_repl_poll(defense.owners[index],1,&event)!=XR_SOURCE_OK || ++polls>10000)
            return (XrSourceEvent){.state=XR_SOURCE_RUN_API_ERROR};
    } while(event.state==XR_SOURCE_RUN_READY);
    return event;
}
static void defense_close(void) {
    XrSourceEvent cleanup[2]={{0}};XrSourceStatus status[2];
    for(unsigned i=0;i<2;++i) {
        status[i]=xray_source_repl_drop(&defense.owners[i],&cleanup[i]);
        xray_source_result_free(defense.results[i]);xray_source_value_free(defense.values[i]);
    }
    bool bad_callback=defense.bad_callback;xray_source_diagnostic_free(defense.diagnostic);
    memset(&defense,0,sizeof(defense));ASSERT_FALSE(bad_callback);
    for(unsigned i=0;i<2;++i){ASSERT_EQ_INT(status[i],XR_SOURCE_OK);ASSERT_EQ_INT(cleanup[i].state,XR_SOURCE_RUN_READY);}
}
TEST(api_isolate_delete_null) {
    XrSourceRepl *owner=NULL;XrSourceEvent cleanup={.engine_status=777};
    ASSERT_EQ_INT(xray_source_repl_drop(&owner,&cleanup),XR_SOURCE_OK);ASSERT_NULL(owner);
    ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
}
TEST(api_isolate_dostring_null_isolate) {
    ASSERT_EQ_INT(defense_begin(0,"print(1)\n"),XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(defense.diagnostic);
}
TEST(api_isolate_dostring_null_source) {
    defense_open(0);ASSERT_NOT_NULL(defense.owners[0]);
    ASSERT_EQ_INT(defense_begin(0,NULL),XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(defense.diagnostic);
    ASSERT_FALSE(defense.owners[0]->active);
}
TEST(api_isolate_dostring_missing_or_invalid_authority) {
    defense_open(0);ASSERT_NOT_NULL(defense.owners[0]);
    XrSourceText input={NULL,NULL,"print(1)\n",9};
    ASSERT_EQ_INT(xray_source_repl_begin(defense.owners[0],&input,&defense.diagnostic),XR_SOURCE_BAD_ARGUMENT);
    input.namespace_id="<eval>";
    ASSERT_EQ_INT(xray_source_repl_begin(defense.owners[0],&input,&defense.diagnostic),XR_SOURCE_BAD_ARGUMENT);
    input.namespace_id=defense_namespaces[1];
    ASSERT_EQ_INT(xray_source_repl_begin(defense.owners[0],&input,&defense.diagnostic),XR_SOURCE_BAD_ARGUMENT);
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    ASSERT_EQ_INT(xray_source_repl_bindings(defense.owners[0],&rows,&count),XR_SOURCE_OK);ASSERT_EQ_INT(count,0);
    ASSERT_NULL(defense.owners[0]->submission->published_births);ASSERT_EQ_INT(defense.traces[0].count,0);
    ASSERT_NULL(defense.diagnostic);
}
TEST(api_isolate_dofile_null_isolate) {
    ASSERT_EQ_INT(xray_source_repl_begin_file(NULL,XR_API_DEFENSE_FILE,&defense.diagnostic),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_NULL(defense.diagnostic);
}
TEST(api_isolate_dofile_null_filename) {
    defense_open(0);ASSERT_NOT_NULL(defense.owners[0]);
    ASSERT_EQ_INT(xray_source_repl_begin_file(defense.owners[0],NULL,&defense.diagnostic),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_NULL(defense.diagnostic);
    ASSERT_EQ_INT(xray_source_repl_begin_file(defense.owners[0],XR_API_DEFENSE_FILE,&defense.diagnostic),XR_SOURCE_OK);
    ASSERT_EQ_INT(defense_drive(0).state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(xray_source_repl_take(defense.owners[0],&defense.results[0]),XR_SOURCE_OK);
    ASSERT_EQ_INT(defense.traces[0].count,3);ASSERT_TRUE(!memcmp(defense.traces[0].bytes,"42\n",3));
}
TEST(api_isolate_create_destroy) {
    defense_open(0);defense_open(1);ASSERT_NOT_NULL(defense.owners[0]);ASSERT_NOT_NULL(defense.owners[1]);
    XrXirInstance *a=defense.owners[0]->submission->runtime,*b=defense.owners[1]->submission->runtime;
    ASSERT_TRUE(a!=b && a->domain!=b->domain);ASSERT_NULL(a->executor);ASSERT_NULL(b->executor);
    ASSERT_TRUE(xr_source_compiler_resources(defense.owners[0]->compiler)!=xr_source_compiler_resources(defense.owners[1]->compiler));
    ASSERT_EQ_INT(defense_begin(0,"fn worker()->i64{Coro.yield();return 41}\n"
        "fn run()->i64{const n=await go worker();Coro.yield();return n}\nvar value=run()\nprint(value)\n"),XR_SOURCE_OK);
    XrSourceEvent progress={0};unsigned polls=0;
    while(!instance_current(a)->executor->active) {
        ASSERT_EQ_INT(xray_source_repl_poll(defense.owners[0],1,&progress),XR_SOURCE_OK);
        ASSERT_EQ_INT(progress.state,XR_SOURCE_RUN_READY);ASSERT_TRUE(++polls<10000);
    }
    XrXirInstance *active_a=instance_current(a);XrXirTaskExecutor *scheduler=active_a->executor;
    ASSERT_NOT_NULL(scheduler);ASSERT_NOT_NULL(scheduler->active);ASSERT_TRUE(scheduler->config.instance==a);
    ASSERT_TRUE(scheduler->config.domain==a->domain);ASSERT_EQ_INT(defense.traces[0].count,0);
    ASSERT_EQ_INT(defense_begin(1,"var value=42\nprint(value)\n"),XR_SOURCE_OK);
    ASSERT_EQ_INT(defense_drive(1).state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(xray_source_repl_take(defense.owners[1],&defense.results[1]),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_repl_read(defense.owners[1],"value",&defense.values[1]),XR_SOURCE_OK);
    ASSERT_TRUE(instance_current(b)->executor!=scheduler);ASSERT_NULL(instance_current(b)->executor->active);
    ASSERT_EQ_INT(defense.traces[1].count,3);ASSERT_TRUE(!memcmp(defense.traces[1].bytes,"42\n",3));
    XrSourceEvent cleanup={0};ASSERT_EQ_INT(xray_source_repl_drop(&defense.owners[1],&cleanup),XR_SOURCE_OK);
    ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);ASSERT_TRUE(active_a->executor==scheduler && scheduler->active);
    XrSourceEvent paused=defense_drive(0);ASSERT_EQ_INT(paused.state,XR_SOURCE_RUN_SUSPENDED);
    ASSERT_NULL(scheduler->active);XrSourceWait wait={0};
    ASSERT_EQ_INT(xray_source_repl_wait_request(defense.owners[0],paused.epoch,paused.wake,&wait,sizeof(wait)),XR_SOURCE_OK);
    ASSERT_EQ_INT(wait.kind,XR_SOURCE_WAIT_YIELD);
    ASSERT_NE(xray_source_repl_resume(defense.owners[0],paused.epoch,paused.wake+1),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_repl_resume(defense.owners[0],paused.epoch,paused.wake),XR_SOURCE_OK);
    ASSERT_EQ_INT(defense_drive(0).state,XR_SOURCE_RUN_RETURNED);
    ASSERT_EQ_INT(xray_source_repl_take(defense.owners[0],&defense.results[0]),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_repl_read(defense.owners[0],"value",&defense.values[0]),XR_SOURCE_OK);
    ASSERT_NULL(instance_current(a)->executor->active);
    ASSERT_EQ_INT(defense.traces[0].count,3);ASSERT_TRUE(!memcmp(defense.traces[0].bytes,"41\n",3));
    ASSERT_EQ_INT(xray_source_repl_drop(&defense.owners[0],&cleanup),XR_SOURCE_OK);ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
    for(unsigned i=0;i<2;++i) {
        XrSourceScalar value={0};ASSERT_EQ_INT(xray_source_value_scalar(defense.values[i],&value),XR_SOURCE_OK);
        ASSERT_EQ_INT(value.kind,XR_SOURCE_VALUE_I64);ASSERT_EQ_INT(value.value.signed_value,i?42:41);
        XrSourceEvent owned={0};ASSERT_EQ_INT(xray_source_result_event(defense.results[i],&owned),XR_SOURCE_OK);
        ASSERT_EQ_INT(owned.state,XR_SOURCE_RUN_RETURNED);
    }
}
#undef RUN_TEST
#define RUN_TEST(name) do { \
    unsigned previous=(unsigned)xr_tests_failed; ++xr_tests_run; \
    printf("  %-55s ",#name);fflush(stdout);test_##name();defense_close(); \
    if((unsigned)xr_tests_failed==previous){++xr_tests_passed;puts("PASS");} \
} while(0)
TEST_MAIN_BEGIN()
RUN_TEST(api_isolate_delete_null);
RUN_TEST(api_isolate_dostring_null_isolate);
RUN_TEST(api_isolate_dostring_null_source);
RUN_TEST(api_isolate_dostring_missing_or_invalid_authority);
RUN_TEST(api_isolate_dofile_null_isolate);
RUN_TEST(api_isolate_dofile_null_filename);
RUN_TEST(api_isolate_create_destroy);
TEST_MAIN_END()
