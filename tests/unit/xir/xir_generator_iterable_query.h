/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_iterable_query.h - Implicit calls never select the receiver's spelling
 */
#ifndef XIR_GENERATOR_ITERABLE_QUERY_H
#define XIR_GENERATOR_ITERABLE_QUERY_H
static void iterable_query(const XrXirSourceSnapshot *snapshot) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(snapshot);
    CHECK(view && view->complete && syntax && syntax->reference_count==view->reference_count);
    unsigned implicit=0,explicit=0;
    for(uint32_t n=0;n<view->reference_count;++n) {
        const XrXirSourceReference *reference=&view->references[n];
        if(reference->access!=XR_XIR_SOURCE_CALL || !reference->target) continue;
        CHECK(reference->target<=view->declaration_count);
        const XrXirSourceDeclaration *target=&view->declarations[reference->target-1];
        if(strcmp(target->name,"iterator")) continue;
        const XrXirSourceRange *range=&syntax->references[n];
        if(range->module!=target->range.module || range->line<=0 || range->column<=0)
            fprintf(stderr,"Iterator range module%u line%d col%d target_module%u\n",range->module,range->line,range->column,target->range.module);
        CHECK(range->module==target->range.module && range->line>0 && range->column>0);
        const char *text=iterable_source;
        for(int line=1;line<range->line;++line) {text=strchr(text,'\n');CHECK(text);++text;}
        text+=(unsigned)range->column-1;
        if(!strncmp(text,"iterator",8)) ++explicit;
        else {
            if(strncmp(text,"for",3)) fprintf(stderr,"Implicit iterator selected receiver text: line%d column%d %.12s\n",range->line,range->column,text);
            CHECK(!strncmp(text,"for",3));++implicit;
        }
    }
    CHECK(implicit==8 && explicit==1);
    puts("Custom iterator query:8 implicit for selections +1 explicit method selection PASS");
}
#endif
