/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_inspection_resources.h - Real type-query lifetime boundaries
 */
static bool public_repl_inspection_case;
static XrSourceStatus public_repl_query(PublicReplOwners *owners) {
    static const char expression[]="next()\n";
    XrSourceText text={public_repl_rooted_case?XR_PUBLIC_REPL_FIXTURE "/script/resources.xr":NULL,
        public_repl_rooted_case?NULL:"public-repl-resources",expression,sizeof(expression)-1};
    XrSourceStatus status=xray_source_repl_begin_type(owners->owner,&text,&owners->diagnostic);
    if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event={0};size_t polls=0;
    do {status=xray_source_repl_poll(owners->owner,8,&event);CHECK(++polls<10000);}
    while(status==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    if(status!=XR_SOURCE_OK)return status;
    if(event.state==XR_SOURCE_RUN_OUT_OF_MEMORY || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
        event.engine_status==XR_XIR_CALL_OOM))return XR_SOURCE_OUT_OF_MEMORY;
    if(event.state==XR_SOURCE_RUN_LIMIT || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
        event.engine_status==XR_XIR_CALL_LIMIT))return XR_SOURCE_LIMIT;
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    return xray_source_repl_take(owners->owner,&owners->result);
}
static XrSourceStatus public_repl_inspection_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[0]=runtime_attempts;
    status=public_repl_input(owners,"var state=40\nfn next()->i64{state+=1;return state}\n11\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[1]=runtime_attempts;
    const XrSourceReplBindingInfo *before=NULL,*after=NULL;size_t count=0,after_count=0;
    CHECK(xray_source_repl_bindings(owners->owner,&before,&count)==XR_SOURCE_OK && count==3);
    public_repl_boundaries[2]=runtime_attempts;
    status=public_repl_query(owners);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[3]=runtime_attempts;
    CHECK(owners->result && owners->result->value.status==XR_XIR_CALL_RETURNED);
    CHECK(xray_source_repl_bindings(owners->owner,&after,&after_count)==XR_SOURCE_OK && after==before && after_count==count);
    public_repl_boundaries[4]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"it",&owners->value);if(status!=XR_SOURCE_OK)return status;
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==11);
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[5]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"state",&owners->value);if(status!=XR_SOURCE_OK)return status;
    CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==41);
    public_repl_boundaries[6]=runtime_attempts;
    XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);
    if(status!=XR_SOURCE_OK)return status;
    CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);public_repl_boundaries[7]=runtime_attempts;
    status=xray_source_value_copy(owners->value,&owners->independent);if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[8]=runtime_attempts;
    CHECK(xray_source_result_format(owners->result,discard,NULL)==XR_SOURCE_OK);public_repl_boundaries[9]=runtime_attempts;
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&owners->result->value.value,&bytes,&length) && length==3 && !memcmp(bytes,"i64",3));
    CHECK(xray_source_value_scalar(owners->independent,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==41);
    public_repl_boundaries[10]=runtime_attempts;return XR_SOURCE_OK;
}
