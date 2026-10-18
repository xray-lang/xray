/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_write_resources.h - C binding writes share the real ledger
 */
static XrSourceStatus public_repl_write_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[0]=runtime_attempts;
    status=public_repl_input(owners,"var state=0\nconst candidate=42\nvar bytes=\"\"\nconst text=\"owned\\0中\"\n"
        "var items=[1,2]\nconst replacement=[4,5]\nstruct Point{x:i64}\nvar point=Point{x:1}\nconst other=Point{x:9}\n"
        "class Box{x:i64\ninit(x:i64){this.x=x}}\nvar box=Box(1)\nconst otherBox=Box(9)\n"
        "var nothing=print(0)\nconst unit=print(1)\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[1]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"candidate",&owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[2]=runtime_attempts;
    const XrSourceReplBindingInfo *before=NULL,*after=NULL;size_t count=0,next_count=0;
    status=xray_source_repl_bindings(owners->owner,&before,&count);if(status!=XR_SOURCE_OK)return status;
    status=xray_source_repl_write(owners->owner,"state",owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[3]=runtime_attempts;
    status=xray_source_repl_write(owners->owner,"state",owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[4]=runtime_attempts;
    status=xray_source_repl_bindings(owners->owner,&after,&next_count);if(status!=XR_SOURCE_OK)return status;
    CHECK(before==after && count==next_count);
    xray_source_value_free(owners->value);owners->value=NULL;
    status=xray_source_repl_read(owners->owner,"text",&owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[5]=runtime_attempts;
    status=xray_source_repl_write(owners->owner,"bytes",owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[6]=runtime_attempts;
    xray_source_value_free(owners->value);owners->value=NULL;
    static const char *const donors[]={"replacement","other","otherBox","unit"};
    static const char *const targets[]={"items","point","box","nothing"};
    for(size_t i=0;i<sizeof(donors)/sizeof(*donors);++i) {
        status=xray_source_repl_read(owners->owner,donors[i],&owners->value);if(status!=XR_SOURCE_OK)return status;
        status=xray_source_repl_write(owners->owner,targets[i],owners->value);if(status!=XR_SOURCE_OK)return status;
        xray_source_value_free(owners->value);owners->value=NULL;
    }
    status=public_repl_input(owners,"const after=state\nconst first=items[0]\nconst x=point.x\nconst classX=box.x\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;
    static const char *const observed[]={"after","first","x","classX"};
    static const int64_t expected_values[]={42,4,9,9};
    for(size_t i=0;i<sizeof(observed)/sizeof(*observed);++i) {
        status=xray_source_repl_read(owners->owner,observed[i],&owners->value);if(status!=XR_SOURCE_OK)return status;
        XrSourceScalar scalar={0};CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK &&
            scalar.value.signed_value==expected_values[i]);
        xray_source_value_free(owners->value);owners->value=NULL;
    }
    public_repl_boundaries[7]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"bytes",&owners->value);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[8]=runtime_attempts;
    XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);
    if(status!=XR_SOURCE_OK)return status;CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);
    public_repl_boundaries[9]=runtime_attempts;status=xray_source_value_copy(owners->value,&owners->independent);
    if(status!=XR_SOURCE_OK)return status;public_repl_boundaries[10]=runtime_attempts;
    xray_source_value_free(owners->value);owners->value=NULL;
    static const char expected[]="owned\0中";const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(owners->independent,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));return XR_SOURCE_OK;
}
