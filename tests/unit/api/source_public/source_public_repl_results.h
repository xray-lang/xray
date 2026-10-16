/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_results.h - Actual typed tails and published result identity
 */
static void typed_results(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    execute(owner,"print(9)\n");
    XrSourceValue *empty=NULL;
    CHECK(xray_source_repl_read(owner,"it",&empty)!=XR_SOURCE_OK && !empty);
    XrSourceDiagnostic *missing=NULL;XrSourceStatus absent_status=begin(owner,"it\n",&missing);
    CHECK(absent_status==XR_SOURCE_REJECTED);xray_source_diagnostic_free(missing);
    CHECK(begin(owner,"3\n",NULL)==XR_SOURCE_OK && pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *actual=take(owner,XR_SOURCE_RUN_RETURNED);int64_t result=0;
    CHECK(xray_source_result_i64(actual,&result)==XR_SOURCE_OK && result==3);integer(owner,"it",3);
    XrSourceValue *old=read_value(owner,"it");
    execute(owner,"it*10\n");integer(owner,"it",30);
    execute(owner,"fn prior()->i64{return it}\n");integer(owner,"it",30);
    execute(owner,"\"new\"\n");
    XrSourceValue *text=read_value(owner,"it");const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(text,&bytes,&length)==XR_SOURCE_OK && length==3 && !memcmp(bytes,"new",3));
    execute(owner,"const historical=prior()\n");integer(owner,"historical",30);
    execute(owner,"var calls=0\nfn next()->i64{calls+=1;return calls}\nnext()\n");
    integer(owner,"it",1);integer(owner,"calls",1);
    execute(owner,"fn work(){calls+=1}\nwork\n");
    XrSourceValue *callable=read_value(owner,"it");XrSourceValueKind kind;
    CHECK(xray_source_value_kind(callable,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);
    execute(owner,"it()\n");execute(owner,"it()\n");integer(owner,"calls",3);
    CHECK(begin(owner,"const it=4\n",NULL)==XR_SOURCE_REJECTED);
    CHECK(begin(owner,"fn it(){}\n",NULL)==XR_SOURCE_REJECTED);
    execute(owner,"fn echo(it:i64)->i64{return it}\nconst echoed=echo(8)\n");integer(owner,"echoed",8);
    CHECK(begin(owner,"assert(false)\n77\n",NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    execute(owner,"it()\n");integer(owner,"calls",4);
    execute(owner,"[[1,2],[3]]\n");
    execute(owner,"fn optional()->i64?{return 41}\noptional()\n");
    int some=0;XrSourceValue *nullable=read_value(owner,"it"),*payload=NULL;
    CHECK(xray_source_value_nullable(nullable,&some,&payload)==XR_SOURCE_OK && some && payload);
    xray_source_value_free(payload);xray_source_value_free(nullable);
    execute(owner,"fn absent()->i64?{return null}\nabsent()\n");
    nullable=read_value(owner,"it");some=1;payload=NULL;
    CHECK(xray_source_value_nullable(nullable,&some,&payload)==XR_SOURCE_OK && !some && !payload);
    xray_source_value_free(nullable);
    static const char expected[]="9\n3\n30\nnew\n1\n<fn>\n[[1, 2], [3]]\n41\n";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,sizeof(expected)-1));
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;XrSourceScalar scalar;
    CHECK(xray_source_result_i64(actual,&result)==XR_SOURCE_OK && result==3);xray_source_result_free(actual);
    CHECK(xray_source_value_scalar(old,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==3);
    CHECK(xray_source_value_bytes(text,&bytes,&length)==XR_SOURCE_OK && length==3 && !memcmp(bytes,"new",3));
    CHECK(xray_source_value_kind(callable,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);
    xray_source_value_free(callable);xray_source_value_free(text);xray_source_value_free(old);
}
static void display_limit(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,2);
    execute(owner,"1\n");integer(owner,"it",1);
    CHECK(begin(owner,"2\n",NULL)==XR_SOURCE_OK);
    XrSourceEvent event={.engine_status=777};XrSourceStatus status;
    do {status=xray_source_repl_poll(owner,8,&event);}while(status==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    CHECK(status==XR_SOURCE_LIMIT && trace.count==2 && !memcmp(trace.bytes,"1\n",2));
    XrSourceResult *result=NULL;CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_BAD_STATE && !result);
    CHECK(begin(owner,"3\n",NULL)==XR_SOURCE_BAD_STATE);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
}
