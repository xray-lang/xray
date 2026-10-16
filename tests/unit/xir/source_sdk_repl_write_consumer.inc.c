/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_repl_write_consumer.inc.c - Installed writes retain authentic types
 */
static void sdk_class_history_control(const char *stdlib_path) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    sdk_repl_array_execute(owner,"class Point{x:i64\ninit(x:i64){this.x=x}}\nvar point=Point(1)\nconst other=Point(9)\n");
    sdk_repl_array_execute(owner,"const before=point.x\n");sdk_repl_integer(owner,"before",1);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner);
    puts("Installed Class history control: independent later field read PASS");
}
static XrSourceValue *sdk_repl_write_read(XrSourceRepl *owner,const char *name) {
    XrSourceValue *value=NULL;CHECK(xray_source_repl_read(owner,name,&value)==XR_SOURCE_OK && value);return value;
}
static void sdk_repl_write_scalars(const char *stdlib_path) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    sdk_repl_array_execute(owner,"var value=1\nconst replacement=42\nconst fixed=7\nfn old()->i64{return value}\n");
    XrSourceValue *number=sdk_repl_write_read(owner,"replacement");trace.write_value=number;
    const XrSourceReplBindingInfo *before=NULL,*after=NULL;size_t count=0,next_count=0;
    CHECK(xray_source_repl_bindings(owner,&before,&count)==XR_SOURCE_OK && count==4);
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_OK);sdk_repl_integer(owner,"value",42);
    CHECK(xray_source_repl_bindings(owner,&after,&next_count)==XR_SOURCE_OK && before==after && count==next_count);
    CHECK(xray_source_repl_write(owner,"fixed",number)==XR_SOURCE_REJECTED);
    CHECK(xray_source_repl_write(owner,"old",number)==XR_SOURCE_REJECTED);
    CHECK(xray_source_repl_write(owner,"missing",number)!=XR_SOURCE_OK);
    CHECK(xray_source_repl_write(NULL,"value",number)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,NULL,number)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,"value",NULL)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(sdk_repl_begin(owner,"print(7)\nconst seen=old()\n",NULL)==XR_SOURCE_OK);
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_BUSY);sdk_repl_finish(owner);
    CHECK(trace.write_callbacks && trace.count==2 && !memcmp(trace.bytes,"7\n",2));sdk_repl_integer(owner,"seen",42);
    sdk_repl_array_execute(owner,"42\n");CHECK(xray_source_repl_write(owner,"it",number)==XR_SOURCE_REJECTED);
    sdk_repl_integer(owner,"it",42);sdk_repl_array_execute(owner,"var value=\"new\"\n");
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_BAD_ARGUMENT);
    sdk_repl_array_execute(owner,"const pinned=old()\n");sdk_repl_integer(owner,"pinned",42);
    sdk_repl_array_execute(owner,"const text=\"owned\\0中\"\n");XrSourceValue *text=sdk_repl_write_read(owner,"text");
    CHECK(xray_source_repl_write(owner,"value",text)==XR_SOURCE_OK);XrSourceValue *copied=sdk_repl_write_read(owner,"value");
    sdk_repl_array_execute(owner,"var nothing=print(0)\nconst unit=print(1)\n");
    XrSourceValue *unit=sdk_repl_write_read(owner,"unit");
    CHECK(xray_source_repl_write(owner,"nothing",number)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,"nothing",unit)==XR_SOURCE_OK);sdk_repl_integer(owner,"it",42);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;trace.write_value=NULL;const char *bytes=NULL;size_t length=0;static const char expected[]="owned\0中";
    CHECK(xray_source_value_bytes(copied,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xray_source_value_free(copied);xray_source_value_free(text);xray_source_value_free(unit);xray_source_value_free(number);
}
static void sdk_repl_write_composites(const char *stdlib_path) {
    SdkReplTrace trace;memset(&trace,0,sizeof(trace));XrSourceRepl *owner=sdk_repl_open(stdlib_path,&trace);
    sdk_repl_array_execute(owner,"var items=[1,2]\nconst replacement=[4,5]\nstruct Point{x:i64}\nvar point=Point{x:1}\nconst other=Point{x:9}\n"
        "class Box{x:i64\ninit(x:i64){this.x=x}}\nvar box=Box(1)\nconst otherBox=Box(9)\n");
    sdk_repl_array_execute(owner,"const before=box.x\n");sdk_repl_integer(owner,"before",1);
    const char *const donors[]={"replacement","other","otherBox"},*const targets[]={"items","point","box"};
    XrSourceValue *values[3];
    for(size_t i=0;i<3;++i) {values[i]=sdk_repl_write_read(owner,donors[i]);CHECK(xray_source_repl_write(owner,targets[i],values[i])==XR_SOURCE_OK);}
    sdk_repl_array_execute(owner,"const first=items[0]\nconst x=point.x\nconst classX=box.x\n");
    sdk_repl_integer(owner,"first",4);sdk_repl_integer(owner,"x",9);sdk_repl_integer(owner,"classX",9);
    SdkReplTrace foreign_trace;memset(&foreign_trace,0,sizeof(foreign_trace));XrSourceRepl *foreign=sdk_repl_open(stdlib_path,&foreign_trace);
    sdk_repl_array_execute(foreign,"const replacement=[7,8]\nstruct Point{x:i64}\nconst other=Point{x:19}\n"
        "class Box{x:i64\ninit(x:i64){this.x=x}}\nconst otherBox=Box(19)\n");
    XrSourceValue *foreign_values[3];
    for(size_t i=0;i<3;++i) {foreign_values[i]=sdk_repl_write_read(foreign,donors[i]);CHECK(xray_source_repl_write(owner,targets[i],foreign_values[i])==XR_SOURCE_BAD_ARGUMENT);}
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&foreign,&cleanup)==XR_SOURCE_OK && !foreign && cleanup.state==XR_SOURCE_RUN_READY);
    for(size_t i=0;i<3;++i) {
        CHECK(xray_source_repl_write(owner,targets[i],foreign_values[i])==XR_SOURCE_BAD_ARGUMENT);
        CHECK(xray_source_repl_write(owner,targets[i],values[i])==XR_SOURCE_OK);
    }
    sdk_repl_array_execute(owner,"const retained=items[0]\nconst retainedPoint=point.x\nconst retainedBox=box.x\n");
    sdk_repl_integer(owner,"retained",4);sdk_repl_integer(owner,"retainedPoint",9);sdk_repl_integer(owner,"retainedBox",9);
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);
    for(size_t i=0;i<3;++i) {xray_source_value_free(foreign_values[i]);xray_source_value_free(values[i]);}
}
static void sdk_repl_write_cases(const char *stdlib_path) {
    sdk_class_history_control(stdlib_path);sdk_repl_write_scalars(stdlib_path);sdk_repl_write_composites(stdlib_path);
    puts("Installed typed binding writes: birth authority, scalar/String/Unit/Array/struct/Class, foreign refusal, busy and owned survivor PASS");
}
