/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_resolve.c - Completion display ownership without mutable semantic lookup
 */
#include "test_allocator.h"
#include "app/lsp/xlsp_handlers_textdoc.h"
#include "app/lsp/xlsp_server.h"
#ifdef RESOLVE_BEFORE
#include "frontend/analyzer/xanalyzer.h"
#endif
#include "app/lsp/xlsp_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(2);}}while(0)
static size_t semantic_lookups,document_lookups,unsupported_lookups;
#ifdef RESOLVE_BEFORE
static XaSymbol unrelated;
XR_FUNC XaSymbol *xa_analyzer_lookup(XaAnalyzer *analyzer,const char *name) {
    CHECK(analyzer&&name);++semantic_lookups;unrelated.kind=XA_SYM_CLASS;return &unrelated;
}
XR_FUNC XrType *xa_analyzer_get_type(XaAnalyzer *analyzer,XaSymbol *symbol) {
    (void)analyzer;(void)symbol;++unsupported_lookups;return NULL;
}
XR_FUNC const char *xr_type_to_string(XrType *type) {(void)type;++unsupported_lookups;return "unexpected lookup";}
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    (void)server;(void)uri;++document_lookups;return NULL;
}
#endif
#include "producer.inc.c"
#ifdef RESOLVE_BEFORE
#include "before.inc.c"
#else
#include "current.inc.c"
#endif
static char *encoded(XrJsonValue *value) {
    size_t n=0;char *text=xjson_stringify(value,&n);CHECK(text&&strlen(text)==n);return text;
}
static XrJsonValue *input(unsigned mode) {
    if(mode==0){XrJsonValue *value=make_completion_item("visible",3,"fn visible(value: u32): u32");CHECK(value);return value;}
    const char *cases[]={
        "{\"label\":\"visible\",\"detail\":\"fn visible(value: u32): u32\",\"documentation\":{\"kind\":\"markdown\",\"value\":\"Owned function from this module\"},\"data\":{\"uri\":\"file:///closed.xr\",\"version\":7}}",
        "{\"label\":\"same\",\"documentation\":\"left module\",\"data\":{\"uri\":\"file:///left.xr\"},\"textEdit\":{\"range\":{\"start\":{\"line\":1,\"character\":2},\"end\":{\"line\":1,\"character\":4}},\"newText\":\"same()\"}}",
        "{\"label\":\"same\",\"documentation\":\"right module\",\"data\":{\"uri\":\"file:///right.xr\"},\"additionalTextEdits\":[{\"newText\":\"import same\"}],\"command\":{\"title\":\"show\",\"command\":\"inspect\",\"arguments\":[1,true,null]}}",
        "{\"label\":\"x\\u0000y\",\"documentation\":{\"kind\":\"plaintext\",\"value\":\"\\ud83d\\ude00\\u0000source\"},\"data\":{\"uri\":\"file:///a\\u0000suffix\",\"opaque\":[1,1.0,9223372036854775807]}}",
        "{\"label\":\"fallback\",\"data\":null,\"unknown\":{\"duplicate\":1,\"duplicate\":2}}"
    };
    CHECK(mode<=5);XrJsonValue *value=xjson_parse(cases[mode-1],strlen(cases[mode-1]));CHECK(value);return value;
}
static size_t one(unsigned mode,size_t fault) {
    clone_fault(SIZE_MAX);XrJsonValue *params=input(mode);char *expected=encoded(params);CloneStats baseline=clone_stats();
    (void)baseline;
    XrLspServer server={0};
#ifdef RESOLVE_BEFORE
    XaAnalyzer analyzer={0};server.workspace_analyzer=&analyzer;
#else
    unsigned char opaque_analyzer=0;server.workspace_analyzer=(struct XaAnalyzer *)&opaque_analyzer;
#endif
    server.source_revision=UINT64_C(991);XrLspServer saved=server;semantic_lookups=document_lookups=unsupported_lookups=0;
    clone_fault(fault);XrJsonValue *result=xlsp_handle_td_completion_resolve(&server,params);CloneStats observed=clone_stats();
    size_t prefixes=observed.attempts;clone_fault(SIZE_MAX);
#ifndef RESOLVE_BEFORE
    if (fault != SIZE_MAX) {
        CHECK(server.source_failure.stage == 23 && server.source_failure.status == XR_XIR_OUT_OF_MEMORY);
        saved.source_failure = server.source_failure;
    }
#endif
    CHECK(!memcmp(&server,&saved,sizeof(server)));
    CHECK(!unsupported_lookups);
#ifdef RESOLVE_BEFORE
    CHECK(fault==SIZE_MAX&&result);
    if(mode==1){char *actual=encoded(result);CHECK(strcmp(actual,expected)&&semantic_lookups==1&&!document_lookups);
        CHECK(!strcmp(xjson_get_string(xjson_get(result,"documentation"),"value"),"```xray\nclass visible\n```"));clone_test_free(actual);}
    else {char *actual=encoded(result);CHECK(!strcmp(actual,expected)&&!semantic_lookups&&!document_lookups);clone_test_free(actual);}
#else
    CHECK(!semantic_lookups&&!document_lookups);
    if(fault!=SIZE_MAX){CHECK(observed.injected&&!result&&observed.live==baseline.live&&observed.bytes==baseline.bytes);
        char *same=encoded(params);CHECK(!strcmp(same,expected));clone_test_free(same);}
    else {CHECK(result&&result!=params);xjson_free(params);params=NULL;memset(&server,0xdd,sizeof(server));
        char *actual=encoded(result);CHECK(!strcmp(actual,expected));clone_test_free(actual);}
#endif
    xjson_free(result);xjson_free(params);clone_test_free(expected);CloneStats zero=clone_stats();CHECK(!zero.live&&!zero.bytes);return prefixes;
}
int main(int argc,char **argv) {
#ifdef RESOLVE_BEFORE
    (void)argc;(void)argv;one(0,SIZE_MAX);one(1,SIZE_MAX);
    puts("RESOLVE_BEFORE producer_item_unchanged=1 supplied_documentation_overwritten=1 global_lookup=1 JSON0/0");return 0;
#else
    CHECK(argc==2);bool full=!strcmp(argv[1],"--full");CHECK(full||!strcmp(argv[1],"--normal"));size_t total=0,checked=0;
    for(unsigned mode=0;mode<6;++mode){size_t count=one(mode,SIZE_MAX);CHECK(count);total+=count;
        if(full)for(size_t i=0;i<count;++i){one(mode,i);++checked;}
        else {one(mode,0);++checked;}}
    puts("RESOLVE current_producer_preserved=1 independent_documentation=1 caller_dead=1 no_editor_or_analyzer_lookup=1 JSON0/0");
    printf("RESOLVE_METRICS cases=6 full=%u prefixes=%zu checked=%zu\n",full?1u:0u,total,checked);return 0;
#endif
}
