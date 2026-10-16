/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_repl_original_hosted.h - Original hosted authority and root obligations
 */
static bool original_target(XrXirTarget target) {
    return target.architecture==XR_XIR_ARCH_X86_64 && target.abi_version==XR_XIR_VALUE_ABI_VERSION;
}
static bool original_inline_root(void) {
    XrXirInstance *runtime=instance_current(fixture.owner->submission->runtime);
    XrXirTaskExecutor *executor=runtime?runtime->executor:NULL;
    return executor && !executor->active && !executor->root.task && executor->root.call &&
        executor->root.call==runtime->call && executor->root.ticket &&
        executor->next_identity==executor->root.ticket &&
        (!executor->ready_head || executor->ready_head==&executor->root) && !executor->root.ready_next;
}
static void original_trace(void *context,XrXirLifecycleEvent event,uint32_t index) {
    (void)event;(void)index;++*(unsigned *)context;
}
static void original_host_options(XrSourceCallOptions *options) {
    ASSERT_EQ_INT(xray_source_call_options_init(options,sizeof(*options)),XR_SOURCE_OK);
    options->write=original_output;options->context=&fixture;
}
static bool original_drive(uint32_t entry,bool test) {
    XrXirCallStatus started=test?xr_xir_instance_start_test(fixture.runtime,entry):
        xr_xir_instance_start(fixture.runtime,entry,NULL,0);
    if(started!=XR_XIR_CALL_READY)return false;
    XrXirInstanceResult terminal={0};unsigned polls=0;
    do{terminal=xr_xir_instance_poll_bounded(fixture.runtime,1);}while(terminal.outcome.status==XR_XIR_CALL_READY && ++polls<100000);
    if(terminal.outcome.status!=XR_XIR_CALL_RETURNED)return false;
    XrXirValue result={0};XrXirCallStatus taken=xr_xir_instance_take_result(fixture.runtime,&result);
    bool valid=taken==XR_XIR_CALL_RETURNED && (test?result.type==XR_XIR_UNIT:result.type==XR_XIR_I64 && result.payload==0);
    xr_xir_value_drop(&result);return valid;
}
TEST(product_profiles_install_exact_authority_and_reject_missing_profile) {
    ASSERT_TRUE(original_target(xr_xir_host_target()));
    for(unsigned mode=0;mode<4;++mode) {
        original_open();ASSERT_TRUE(original_target(xr_source_compiler_target(fixture.owner->compiler)));
        ASSERT_EQ_INT(original_names(),0);ASSERT_NULL(fixture.owner->submission->published_births);
        XrXirSourceGenerationConfig generation={0};
        ASSERT_EQ_INT(xr_source_compiler_generation_config(fixture.owner->compiler,"repl-fixture-v1",&generation),XR_SOURCE_COMPILE_OK);
        ASSERT_NOT_NULL(generation.libraries);
        if(mode==0) {
            XrSourceCompilerConfig missing={*xr_source_compiler_context(fixture.owner->compiler),{0},
                XR_ORIGINAL_REPL_STDLIB,xr_source_compile_default_manifest_limits()};
            XrSourceCompiler *rejected=NULL;XrCompileResourceStats before={0},after={0};
            ASSERT_EQ_INT(xr_compile_resources_stats(missing.context.resources,&before),XR_COMPILE_RESOURCE_OK);
            ASSERT_EQ_INT(xr_source_compiler_new(&missing,&rejected,NULL),XR_SOURCE_COMPILE_UNSUPPORTED);ASSERT_NULL(rejected);
            missing.target=xr_xir_host_target();missing.target.abi_version=0;
            ASSERT_EQ_INT(xr_source_compiler_new(&missing,&rejected,NULL),XR_SOURCE_COMPILE_UNSUPPORTED);ASSERT_NULL(rejected);
            ASSERT_EQ_INT(xr_compile_resources_stats(missing.context.resources,&after),XR_COMPILE_RESOURCE_OK);
            ASSERT_TRUE(!memcmp(&before,&after,sizeof(before)));
        }
        if(mode==3) {
            ASSERT_TRUE(original_eval("var xs=[1,2,3]\nvar n=len(xs)\nfn count()->i64{return n}\nprint(42)\n"));
            ASSERT_TRUE(original_eval("const answer=count()\n"));ASSERT_TRUE(original_integer("answer",3));ASSERT_TRUE(original_integer("n",3));
            XrXirInstance *actual=instance_current(fixture.owner->submission->runtime);
            ASSERT_TRUE(original_target(actual->program->target));ASSERT_NOT_NULL(actual->program->permissions);
            ASSERT_NOT_NULL(actual->function_gate);ASSERT_EQ_INT(actual->config.abi_version,XR_XIR_CALL_ABI_VERSION);
            ASSERT_NOT_NULL(actual->config.time.clock);ASSERT_NULL(actual->config.trace);
        } else {
            XrSourceStatus built;
            if(mode==1) {
                const char source[]="const xs=[1,2,3]\nassert(len(xs)==3)\nprint(42)\n";
                const XrSourceText input={NULL,"hosted-eval",source,sizeof(source)-1};
                built=xray_source_compile_text(fixture.owner->compiler,&input,&fixture.program,&fixture.diagnostic);
            } else built=xray_source_compile_file(fixture.owner->compiler,
                mode==0?XR_ORIGINAL_REPL_HOSTED "/program.xr":XR_ORIGINAL_REPL_HOSTED "/test.xr",&fixture.program,&fixture.diagnostic);
            ASSERT_EQ_INT(built,XR_SOURCE_OK);ASSERT_EQ_INT(api_program_code(fixture.program),XR_SOURCE_OK);
            ASSERT_TRUE(original_target(fixture.program->code->target));ASSERT_NOT_NULL(fixture.program->code->permissions);
            ASSERT_NOT_NULL(fixture.program->code->permissions->effects);
            XrSourceCallOptions options;original_host_options(&options);
            if(mode==2) {
                const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(fixture.program->product);
                ASSERT_NOT_NULL(tests);ASSERT_EQ_INT(tests->count,1);ASSERT_EQ_INT(tests->entries[0].role,XR_XIR_TEST_ROLE_TEST);
                XrXirInstanceConfig config;
                ASSERT_EQ_INT(api_config(&options,&fixture.sink,&config),XR_SOURCE_OK);
                ASSERT_EQ_INT(xr_xir_instance_new(fixture.program->code,&config,&fixture.runtime),XR_XIR_CALL_READY);
                ASSERT_TRUE(original_drive(tests->entries[0].function,true));
            } else {
                ASSERT_EQ_INT(xray_source_execute(fixture.program,&options,&fixture.results[fixture.result_count],NULL),XR_SOURCE_OK);
                XrSourceEvent event={0};ASSERT_EQ_INT(xray_source_result_event(fixture.results[fixture.result_count++],&event),XR_SOURCE_OK);
                ASSERT_EQ_INT(event.state,XR_SOURCE_RUN_RETURNED);
            }
        }
        ASSERT_STR_EQ(fixture.output,"42\n");original_close();
    }
}
TEST(sync_root_elides_coroutine_with_ordinary_allocation) {
    original_open();ASSERT_NULL(fixture.owner->submission->runtime->executor);
    const char source[]="var xs = [1, 2, 3]\nvar n = len(xs)\n";
    const XrSourceText input={NULL,"repl-fixture-v1",source,sizeof(source)-1};
    ASSERT_EQ_INT(xray_source_repl_begin(fixture.owner,&input,&fixture.diagnostic),XR_SOURCE_OK);
    ASSERT_TRUE(original_inline_root());unsigned polls=0;
    do {
        ASSERT_EQ_INT(xray_source_repl_poll(fixture.owner,1,&fixture.event),XR_SOURCE_OK);++polls;
        if(fixture.event.state==XR_SOURCE_RUN_READY)ASSERT_TRUE(original_inline_root());
        ASSERT_TRUE(polls<100000);
    } while(fixture.event.state==XR_SOURCE_RUN_READY);
    ASSERT_EQ_INT(fixture.event.state,XR_SOURCE_RUN_RETURNED);ASSERT_TRUE(polls>1);
    ASSERT_EQ_INT(xray_source_repl_take(fixture.owner,&fixture.results[fixture.result_count++]),XR_SOURCE_OK);
    ASSERT_TRUE(original_integer("n",3));ASSERT_TRUE(original_inline_root());
    XrSourceValue *array=NULL;ASSERT_EQ_INT(xray_source_repl_read(fixture.owner,"xs",&array),XR_SOURCE_OK);
    fixture.values[fixture.value_count++]=array;XrSourceValueKind kind;
    ASSERT_EQ_INT(xray_source_value_kind(array,&kind),XR_SOURCE_OK);ASSERT_EQ_INT(kind,XR_SOURCE_VALUE_ARRAY);
    ASSERT_TRUE(original_inline_root());
}
TEST(repl_profile_clears_each_call) {
    original_open();const char source[]="var xs=[1,2,3]\nassert(len(xs)==3)\nprint(42)\n";
    const XrSourceText input={NULL,"hosted-trace",source,sizeof(source)-1};
    ASSERT_EQ_INT(xray_source_compile_text(fixture.owner->compiler,&input,&fixture.program,&fixture.diagnostic),XR_SOURCE_OK);
    ASSERT_EQ_INT(api_program_code(fixture.program),XR_SOURCE_OK);
    XrSourceCallOptions options;original_host_options(&options);XrXirInstanceConfig config;
    ASSERT_EQ_INT(api_config(&options,&fixture.sink,&config),XR_SOURCE_OK);
    config.trace=original_trace;config.trace_context=&fixture.trace_events;
    ASSERT_EQ_INT(xr_xir_instance_new(fixture.program->code,&config,&fixture.runtime),XR_XIR_CALL_READY);
    ASSERT_TRUE(original_drive(fixture.program->info.entry,false));ASSERT_TRUE(fixture.trace_events>0);
    ASSERT_EQ_INT(xr_xir_instance_free(fixture.runtime),XR_XIR_CALL_READY);fixture.runtime=NULL;fixture.trace_events=0;
    XrXirOutputProvider output=config.output;
    memset(&config,0xa5,sizeof(config));
    ASSERT_EQ_INT(xr_xir_host_config_init(&config,sizeof(config)),XR_XIR_CALL_READY);
    ASSERT_NULL(config.trace);ASSERT_NULL(config.trace_context);ASSERT_NULL(config.time.context);
    ASSERT_EQ_INT(config.time.abi_version,XR_XIR_CALL_ABI_VERSION);ASSERT_NOT_NULL(config.time.clock);ASSERT_NOT_NULL(config.time.utc_offset);
    int64_t now=0;ASSERT_EQ_INT(config.time.clock(config.time.context,XR_XIR_CLOCK_MONOTONIC,&now),XR_XIR_TIME_OK);ASSERT_TRUE(now>0);
    XrXirInstanceConfig unchanged=config;
    ASSERT_EQ_INT(xr_xir_host_config_init(&config,sizeof(config)-1),XR_XIR_CALL_BAD_ABI);
    ASSERT_TRUE(!memcmp(&config,&unchanged,sizeof(config)));
    config.output=output;
    ASSERT_EQ_INT(xr_xir_instance_new(fixture.program->code,&config,&fixture.runtime),XR_XIR_CALL_READY);
    ASSERT_TRUE(original_drive(fixture.program->info.entry,false));ASSERT_EQ_INT(fixture.trace_events,0);
    ASSERT_STR_EQ(fixture.output,"42\n42\n");
}
