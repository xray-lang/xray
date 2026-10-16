/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_array_resources.h - Checked birth mutator allocation cuts
 */
static XrSourceStatus public_repl_array_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[0]=runtime_attempts;
    status=public_repl_input(owners,"var items:Array<i64>=[]\nconst fixed=[4,5]\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[1]=runtime_attempts;
    status=public_repl_input(owners,"fn append(){items.push(1)}\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[2]=runtime_attempts;
    status=public_repl_input(owners,"append();append()\nconst n=len(items)\nconst first=fixed[0]\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[3]=runtime_attempts;
    status=public_repl_input(owners,"append()\nassert(false)\n",XR_SOURCE_RUN_PANICKED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[4]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"n",&owners->value);if(status!=XR_SOURCE_OK)return status;
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==2);
    xray_source_value_free(owners->value);owners->value=NULL;public_repl_boundaries[5]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"items",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[6]=runtime_attempts;XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);
    if(status!=XR_SOURCE_OK)return status;CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);
    public_repl_boundaries[7]=runtime_attempts;status=xray_source_value_copy(owners->value,&owners->independent);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[8]=runtime_attempts;
    xray_source_value_free(owners->value);owners->value=NULL;
    status=xray_source_value_format(owners->independent,discard,NULL);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[9]=public_repl_boundaries[10]=runtime_attempts;return XR_SOURCE_OK;
}
