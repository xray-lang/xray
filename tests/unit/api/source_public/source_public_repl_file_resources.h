/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_file_resources.h - File intake and lexer resource boundaries
 */
static XrSourceStatus public_repl_file_run(PublicReplOwners *owners) {
    XrSourceStatus status=xray_source_repl_begin_file(owners->owner,XR_PUBLIC_REPL_FILE_FIXTURE "/main.xr",&owners->diagnostic);
    if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event={0};unsigned polls=0;
    do{status=xray_source_repl_poll(owners->owner,8,&event);CHECK(++polls<10000);}
    while(status==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    if(status!=XR_SOURCE_OK)return status;
    if(event.state==XR_SOURCE_RUN_OUT_OF_MEMORY || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED && event.engine_status==XR_XIR_CALL_OOM))
        return XR_SOURCE_OUT_OF_MEMORY;
    if(event.state==XR_SOURCE_RUN_LIMIT || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED && event.engine_status==XR_XIR_CALL_LIMIT))return XR_SOURCE_LIMIT;
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);status=xray_source_repl_take(owners->owner,&owners->result);
    if(status==XR_SOURCE_OK){xray_source_result_free(owners->result);owners->result=NULL;}
    return status;
}
static XrSourceStatus public_repl_file_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[0]=runtime_attempts;
    XrSourceInputState complete=XR_SOURCE_INPUT_COMPLETE;
    static const char partial[]="fn incomplete(){ /* real trivia */";
    status=xray_source_repl_check_input(owners->owner,partial,sizeof(partial)-1,&complete);
    if(status!=XR_SOURCE_OK)return status;CHECK(complete==XR_SOURCE_INPUT_INCOMPLETE);public_repl_boundaries[1]=runtime_attempts;
    status=public_repl_file_run(owners);if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[2]=runtime_attempts;
    status=public_repl_input(owners,"const saved=next\nnext()\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[3]=runtime_attempts;
    status=public_repl_file_run(owners);if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[4]=runtime_attempts;
    status=xray_source_repl_begin_file(owners->owner,XR_PUBLIC_REPL_FILE_FIXTURE "/syntax.xr",&owners->diagnostic);
    if(status!=XR_SOURCE_INVALID)return status;CHECK(owners->diagnostic);
    xray_source_diagnostic_free(owners->diagnostic);owners->diagnostic=NULL;public_repl_boundaries[5]=runtime_attempts;
    status=public_repl_input(owners,"saved()\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[6]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"it",&owners->value);if(status!=XR_SOURCE_OK)return status;
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==42);
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[7]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"bytes",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[8]=runtime_attempts;
    XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);if(status!=XR_SOURCE_OK)return status;
    CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);public_repl_boundaries[9]=runtime_attempts;
    status=xray_source_value_copy(owners->value,&owners->independent);if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[10]=runtime_attempts;
    static const char expected[]="owned\0中";const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(owners->independent,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    return XR_SOURCE_OK;
}
