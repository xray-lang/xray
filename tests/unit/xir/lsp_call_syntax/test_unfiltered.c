/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define xlsp_source_syntax_build call_syntax_filtered_build
#include "../../../../src/app/lsp/xlsp_source_syntax.c"
#undef xlsp_source_syntax_build
/* Retain every real parser observation to test the reachability requirement. */
XR_FUNC XrParseStatus xlsp_source_syntax_build(XrCompilerSession *session,
    const XlspSourceDocument *document,XlspSyntaxSnapshot **out) {
    XrParseStatus status=call_syntax_filtered_build(session,document,out);
    if(status==XR_PARSE_OK||status==XR_PARSE_RECOVERED)
        for(size_t i=0;i<(*out)->call_count;++i)(*out)->calls[i].live=true;
    return status;
}
#include "test_call_syntax.c"
