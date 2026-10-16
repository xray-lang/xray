/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_array_history.h - Mutation executes in the actual birth module
 */
static void array_history(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);
    execute(owner,"var items:Array<i64>=[]\nconst fixed=[4,5]\nvar calls=0\n");
    execute(owner,"fn append(x:i64){items.push(x)}\nfn value()->i64{calls+=1;return calls}\n");
    execute(owner,"append(value())\nappend(value())\nconst size=len(items)\nconst first=fixed[0]\n");
    integer(owner,"calls",2);integer(owner,"size",2);integer(owner,"first",4);
    CHECK(begin(owner,"append(3)\nassert(false)\n",NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    execute(owner,"const retained=len(items)\n");integer(owner,"retained",3);
    XrSourceValue *old=read_value(owner,"items");
    XrSourceDiagnostic *diagnostic=NULL;CHECK(begin(owner,"fixed.push(9)\n",&diagnostic)==XR_SOURCE_REJECTED && diagnostic);
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.line==1);
    xray_source_diagnostic_free(diagnostic);execute(owner,"const unchanged=fixed[0]\n");integer(owner,"unchanged",4);
    execute(owner,"var items:Array<i64>=[99]\nappend(4)\nconst fresh=len(items)\n");integer(owner,"fresh",1);
    execute(owner,"append(5)\n");XrSourceEvent cleanup;
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);trace.owner=NULL;
    CHECK(xray_source_value_format(old,collect,&trace)==XR_SOURCE_OK);
    static const char expected[]="[1, 2, 3]";
    CHECK(trace.count==sizeof(expected)-1 && !memcmp(trace.bytes,expected,trace.count));xray_source_value_free(old);
    puts("Array history: real birth mutation, argument once, const read/refusal, failed-input old effect, later shadow and owned snapshot PASS");
}
