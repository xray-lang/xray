/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_write.h - Typed C writes use real birth execution authority
 */
static void binding_writes(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    execute(owner,"var value=1\nconst replacement=42\nconst fixed=7\nfn old()->i64{return value}\n");
    XrSourceValue *number=read_value(owner,"replacement");trace.write_value=number;
    const XrSourceReplBindingInfo *before=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&before,&count)==XR_SOURCE_OK);size_t original_count=count;
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_OK);integer(owner,"value",42);
    const XrSourceReplBindingInfo *after=NULL;count=0;
    CHECK(xray_source_repl_bindings(owner,&after,&count)==XR_SOURCE_OK && count==original_count && before==after);
    CHECK(xray_source_repl_write(owner,"fixed",number)==XR_SOURCE_REJECTED);
    CHECK(xray_source_repl_write(owner,"old",number)==XR_SOURCE_REJECTED);
    CHECK(xray_source_repl_write(owner,"missing",number)!=XR_SOURCE_OK);
    CHECK(xray_source_repl_write(owner,NULL,number)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,"value",NULL)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(begin(owner,"print(7)\nconst seen=old()\n",NULL)==XR_SOURCE_OK);
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_BUSY);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
    CHECK(trace.callbacks && trace.count==2);integer(owner,"seen",42);
    execute(owner,"var value=\"new\"\n");
    CHECK(xray_source_repl_write(owner,"value",number)==XR_SOURCE_BAD_ARGUMENT);
    execute(owner,"const pinned=old()\n");integer(owner,"pinned",42);
    execute(owner,"const text=\"owned\\0中\"\n");XrSourceValue *text=read_value(owner,"text");
    CHECK(xray_source_repl_write(owner,"value",text)==XR_SOURCE_OK);
    XrSourceValue *copied=read_value(owner,"value");
    execute(owner,"42\n");CHECK(xray_source_repl_write(owner,"it",number)==XR_SOURCE_REJECTED);integer(owner,"it",42);
    execute(owner,"var nothing=print(0)\nconst unit=print(1)\n");XrSourceValue *unit=read_value(owner,"unit");
    CHECK(xray_source_repl_write(owner,"nothing",number)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,"nothing",unit)==XR_SOURCE_OK);
    XrSourceEvent cleanup={0};CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;trace.write_value=NULL;const char *bytes=NULL;size_t length=0;static const char expected[]="owned\0中";
    CHECK(xray_source_value_bytes(copied,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xray_source_value_free(copied);xray_source_value_free(text);xray_source_value_free(unit);xray_source_value_free(number);
}
static void composite_binding_writes(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    execute(owner,"var items=[1,2]\nconst replacement=[4,5]\nstruct Point{x:i64}\nvar point=Point{x:1}\nconst other=Point{x:9}\n");
    XrSourceValue *items=read_value(owner,"replacement"),*point=read_value(owner,"other");
    CHECK(xray_source_repl_write(owner,"items",items)==XR_SOURCE_OK);
    execute(owner,"const first=items[0]\n");integer(owner,"first",4);
    execute(owner,"const before=point.x\n");integer(owner,"before",1);
    CHECK(xray_source_repl_write(owner,"point",point)==XR_SOURCE_OK);
    execute(owner,"const x=point.x\n");integer(owner,"x",9);
    Trace foreign_trace={0};XrSourceRepl *foreign=open_repl(&foreign_trace,128);
    execute(foreign,"const replacement=[7,8]\nstruct Point{x:i64}\nconst other=Point{x:19}\n");
    XrSourceValue *foreign_items=read_value(foreign,"replacement"),*foreign_point=read_value(foreign,"other");
    CHECK(xray_source_repl_write(owner,"items",foreign_items)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_repl_write(owner,"point",foreign_point)==XR_SOURCE_BAD_ARGUMENT);
    XrSourceEvent cleanup={0};CHECK(xray_source_repl_drop(&foreign,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    CHECK(xray_source_repl_write(owner,"items",items)==XR_SOURCE_OK);
    execute(owner,"const unchanged=items[0]\nconst unchangedPoint=point.x\n");integer(owner,"unchanged",4);integer(owner,"unchangedPoint",9);
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    xray_source_value_free(foreign_point);xray_source_value_free(foreign_items);xray_source_value_free(point);xray_source_value_free(items);
    puts("C binding writes: ordinary birth writer, exact composite arena, foreign refusal, scalar/String/Unit, old closure, shadow, reentry and owned survivor PASS");
}

static void class_binding_write_history(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    execute(owner,"class Point{x:i64\ninit(x:i64){this.x=x}}\nvar point=Point(1)\nconst other=Point(9)\n");
    execute(owner,"const before=point.x\n");integer(owner,"before",1);
    XrSourceValue *point=read_value(owner,"other");
    CHECK(xray_source_repl_write(owner,"point",point)==XR_SOURCE_OK);
    Trace foreign_trace={0};XrSourceRepl *foreign=open_repl(&foreign_trace,128);
    execute(foreign,"class Point{x:i64\ninit(x:i64){this.x=x}}\nconst other=Point(19)\n");
    XrSourceValue *foreign_point=read_value(foreign,"other");
    CHECK(xray_source_repl_write(owner,"point",foreign_point)==XR_SOURCE_BAD_ARGUMENT);
    XrSourceEvent cleanup={0};CHECK(xray_source_repl_drop(&foreign,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    execute(owner,"const after=point.x\n");integer(owner,"after",9);
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
    xray_source_value_free(point);xray_source_value_free(foreign_point);
}
