/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_server.h"
#include "app/lsp/xlsp_source_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d:%s\n",__LINE__,#c); exit(2); } } while (0)
#include "xir_sdk_resource_test.h"

static void expect_offset(XrLspDocument *doc, uint32_t line, uint32_t column, uint32_t want) {
    uint32_t got = xlsp_position_to_offset(doc, (XrLspPosition){line, column});
    if (got != want) fprintf(stderr,"offset %u:%u got %u wanted %u\n",line,column,got,want);
    CHECK(got == want);
}
static void expect_position(XrLspDocument *doc, uint32_t byte, uint32_t line, uint32_t column) {
    XrLspPosition got = xlsp_offset_to_position(doc, byte);
    if (got.line != line || got.character != column)
        fprintf(stderr,"position %u got %u:%u wanted %u:%u\n",byte,got.line,got.character,line,column);
    CHECK(got.line == line && got.character == column);
}
static XlspSourceBuffer *make_document(XrCompileResources *resources,
    const char *text, XrLspDocument *doc) {
    XlspSourceBuffer *buffer = NULL;
    size_t length = strlen(text);
    char *borrowed = xr_malloc(length + 1u); CHECK(borrowed);
    memcpy(borrowed,text,length + 1u);
    CHECK(xlsp_source_buffer_new(resources,borrowed,length,7,&buffer) == XR_XIR_OK);
    memset(borrowed,'?',length); xr_free(borrowed);
    memset(doc,0,sizeof(*doc));
    doc->content = (char *)xlsp_source_buffer_text(buffer);
    doc->length = xlsp_source_buffer_length(buffer);
    doc->line_offsets = (uint32_t *)xlsp_source_buffer_lines(buffer,&doc->line_count);
    return buffer;
}
static void crlf(XrCompileResources *resources) {
    XrLspDocument doc; XlspSourceBuffer *buffer = make_document(resources,"A😀é\r\n中\rB\n",&doc);
    CHECK(doc.length == 15 && doc.line_count == 4);
    size_t attempts = runtime_attempts; XrCompileResourceStats before = sdk_stats(resources);
    expect_offset(&doc,0,0,0); expect_offset(&doc,0,1,1);
    expect_offset(&doc,0,2,1); expect_offset(&doc,0,3,5); expect_offset(&doc,0,4,7);
    expect_offset(&doc,0,UINT32_MAX,7); expect_offset(&doc,1,UINT32_MAX,12);
    expect_offset(&doc,2,UINT32_MAX,14); expect_offset(&doc,3,UINT32_MAX,15);
    expect_position(&doc,1,0,1); expect_position(&doc,2,0,1); expect_position(&doc,4,0,1);
    expect_position(&doc,5,0,3); expect_position(&doc,6,0,3); expect_position(&doc,7,0,4);
    expect_position(&doc,8,0,4); expect_position(&doc,9,1,0); expect_position(&doc,10,1,0);
    expect_position(&doc,12,1,1); expect_position(&doc,13,2,0); expect_position(&doc,14,2,1);
    expect_position(&doc,15,3,0); expect_position(&doc,UINT32_MAX,3,0);
    XrCompileResourceStats after = sdk_stats(resources);
    CHECK(runtime_attempts == attempts && !memcmp(&before,&after,sizeof(before)));
    xlsp_source_buffer_free(buffer);
}
static void high_lines(XrCompileResources *resources) {
    XrLspDocument doc; XlspSourceBuffer *buffer = make_document(resources,"a\nb",&doc);
    const uint32_t lines[] = {2,INT32_MAX,UINT32_C(0x80000000),UINT32_MAX};
    for (size_t i=0;i<sizeof(lines)/sizeof(*lines);++i) {
        expect_offset(&doc,lines[i],0,3); expect_offset(&doc,lines[i],UINT32_MAX,3);
    }
    xlsp_source_buffer_free(buffer);
}
static void controls(XrCompileResources *resources) {
    const char *texts[] = {"","a","a\n","a\r","a\r\n","\n\r\n"};
    for (size_t i=0;i<sizeof(texts)/sizeof(*texts);++i) {
        XrLspDocument doc; XlspSourceBuffer *buffer = make_document(resources,texts[i],&doc);
        expect_offset(&doc,0,0,0);
        uint32_t line = (uint32_t)doc.line_count - 1u;
        uint32_t column = i == 1 ? 1u : 0u;
        expect_position(&doc,(uint32_t)doc.length,line,column);
        expect_offset(&doc,line,column,(uint32_t)doc.length);
        xlsp_source_buffer_free(buffer);
    }
    XrLspDocument invalid = {0};
    expect_offset(NULL,UINT32_MAX,UINT32_MAX,0); expect_position(NULL,UINT32_MAX,0,0);
    expect_offset(&invalid,UINT32_MAX,UINT32_MAX,0); expect_position(&invalid,UINT32_MAX,0,0);
}
int main(int argc,char **argv) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrCompileResourceStats initial = sdk_stats(resources);
    if (argc == 2 && !strcmp(argv[1],"--high-line")) high_lines(resources);
    else if (argc == 2 && !strcmp(argv[1],"--crlf")) crlf(resources);
    else { CHECK(argc == 1); crlf(resources); high_lines(resources); controls(resources); }
    XrCompileResourceStats final = sdk_stats(resources);
    CHECK(final.live_bytes == initial.live_bytes); xr_compile_resources_release(resources);
    CHECK(runtime_live == 0 && runtime_bytes == 0);
    printf("position display PASS allocated=%llu work=%llu physical=%zu/%zu\n",
        (unsigned long long)final.allocated_bytes,(unsigned long long)final.work,
        runtime_live,runtime_bytes);
    return 0;
}
