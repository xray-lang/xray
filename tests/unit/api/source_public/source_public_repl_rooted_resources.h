/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_rooted_resources.h - Authority and AST lifetime fault cuts
 */
static XrSourceStatus public_repl_rooted_input(PublicReplOwners *owners,const char *source) {
    const XrSourceText text={XR_PUBLIC_REPL_FIXTURE "/script/resources.xr",NULL,source,strlen(source)};
    return public_repl_text(owners,&text,XR_SOURCE_RUN_RETURNED);
}
static XrSourceStatus public_repl_rooted_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[0]=runtime_attempts;
    status=public_repl_rooted_input(owners,"import \"./sibling\" as sibling\nvar state=40\n"
        "fn next()->i64{state+=1;return state+sibling.answer()-42}\nconst bytes=\"owned\\0中\"\n11\n");
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[1]=runtime_attempts;
    status=public_repl_rooted_input(owners,"var state=\"shadow\"\nconst observed=next()\nobserved\n");
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[2]=runtime_attempts;
    static const char rejected[]="const forbiddenFallback=1\n";
    XrSourceText invalid={XR_PUBLIC_REPL_FIXTURE "/project/nearer/invalid.xr",NULL,rejected,sizeof(rejected)-1};
    status=xray_source_repl_begin(owners->owner,&invalid,&owners->diagnostic);
    if(status!=XR_SOURCE_INVALID)return status;
    CHECK(owners->diagnostic);xray_source_diagnostic_free(owners->diagnostic);owners->diagnostic=NULL;
    public_repl_boundaries[3]=runtime_attempts;
    status=public_repl_query(owners);if(status!=XR_SOURCE_OK)return status;
    CHECK(owners->result && owners->result->value.status==XR_XIR_CALL_RETURNED);
    public_repl_boundaries[4]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"it",&owners->value);if(status!=XR_SOURCE_OK)return status;
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==41);
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[5]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"bytes",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[6]=runtime_attempts;
    XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);
    if(status!=XR_SOURCE_OK)return status;
    CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);public_repl_boundaries[7]=runtime_attempts;
    status=xray_source_value_copy(owners->value,&owners->independent);if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[8]=runtime_attempts;
    status=xray_source_result_format(owners->result,discard,NULL);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[9]=runtime_attempts;
    const char *bytes=NULL;size_t length=0;static const char expected[]="owned\0中";
    CHECK(xray_source_value_bytes(owners->independent,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(xr_xir_string_view(&owners->result->value.value,&bytes,&length) && length==3 && !memcmp(bytes,"i64",3));
    public_repl_boundaries[10]=runtime_attempts;return XR_SOURCE_OK;
}
