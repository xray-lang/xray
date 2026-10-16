/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_repl_hosted_consumer.inc.c - Explicit installed owners retain calls
 */
static void sdk_repl_hosted_options(void) {
    XrSourceCallOptions options;memset(&options,0xa5,sizeof(options));
    CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    CHECK(!options.write && !options.context && options.work_limit && options.poll_limit);
    options.write=sdk_repl_output;options.context=&options;
    CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK && !options.write && !options.context);
    unsigned char before[sizeof(options)];memcpy(before,&options,sizeof(options));
    CHECK(xray_source_call_options_init(&options,sizeof(options)-1)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(!memcmp(before,&options,sizeof(options)));
    CHECK(xray_source_call_options_init(NULL,sizeof(options))==XR_SOURCE_BAD_ARGUMENT);
}
static void sdk_repl_hosted_cases(const char *stdlib_path) {
    sdk_repl_hosted_options();SdkReplTrace traces[2];memset(traces,0,sizeof(traces));
    XrSourceRepl *first=sdk_repl_open(stdlib_path,&traces[0]),*second=sdk_repl_open(stdlib_path,&traces[1]);
    CHECK(sdk_repl_begin(first,"fn worker()->i64{print(7);Coro.yield();return 41}\n"
        "fn run()->i64{const n=await go worker();Coro.yield();return n}\nvar value=run()\nprint(value)\n",NULL)==XR_SOURCE_OK);
    XrSourceEvent progress;unsigned polls=0;
    while(!traces[0].count) {
        CHECK(xray_source_repl_poll(first,1,&progress)==XR_SOURCE_OK && progress.state==XR_SOURCE_RUN_READY);
        CHECK(++polls<100000);
    }
    CHECK(traces[0].count==2 && !memcmp(traces[0].bytes,"7\n",2));
    CHECK(sdk_repl_begin(second,"var value=42\nprint(value)\n",NULL)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(second).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *results[2];XrSourceValue *values[2];results[1]=sdk_repl_take(second,XR_SOURCE_RUN_RETURNED);
    values[1]=sdk_repl_write_read(second,"value");sdk_repl_integer(second,"value",42);
    CHECK(traces[1].count==3 && !memcmp(traces[1].bytes,"42\n",3));
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&second,&cleanup)==XR_SOURCE_OK && !second && cleanup.state==XR_SOURCE_RUN_READY);
    traces[1].owner=NULL;XrSourceEvent paused=sdk_repl_pump(first);
    CHECK(paused.state==XR_SOURCE_RUN_SUSPENDED && traces[0].count==2);
    XrSourceWait wait;CHECK(xray_source_repl_wait_request(first,paused.epoch,paused.wake,&wait,sizeof(wait))==XR_SOURCE_OK && wait.kind==XR_SOURCE_WAIT_YIELD);
    CHECK(xray_source_repl_resume(first,paused.epoch,paused.wake+1)!=XR_SOURCE_OK);
    CHECK(xray_source_repl_resume(first,paused.epoch,paused.wake)==XR_SOURCE_OK);
    CHECK(sdk_repl_pump(first).state==XR_SOURCE_RUN_RETURNED);
    results[0]=sdk_repl_take(first,XR_SOURCE_RUN_RETURNED);values[0]=sdk_repl_write_read(first,"value");
    CHECK(traces[0].count==5 && !memcmp(traces[0].bytes,"7\n41\n",5));
    CHECK(xray_source_repl_drop(&first,&cleanup)==XR_SOURCE_OK && !first && cleanup.state==XR_SOURCE_RUN_READY);
    traces[0].owner=NULL;
    for(unsigned i=0;i<2;++i) {
        integer(values[i],i?42:41);XrSourceEvent event;
        CHECK(xray_source_result_event(results[i],&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        xray_source_value_free(values[i]);xray_source_result_free(results[i]);
    }
    puts("Installed hosted lifecycle: explicit owners, child and root yields, busy drop, defaults reset and owned survivors PASS");
}
