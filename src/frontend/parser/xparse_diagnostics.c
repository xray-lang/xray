/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_diagnostics.c - Explicit compiler policy for the shared formatter
 */
#include "xparse_internal.h"
#include "../xdiag_fmt.h"

bool xr_parser_format(Parser *parser,char *output,size_t capacity,const char *format,...) {
    if(!xr_parser_healthy(parser))return false;
    if(!output || !capacity || !format) {
        (void)xr_compile_state_fail(parser->state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);return false;
    }
    va_list arguments;va_start(arguments,format);
    int written=vsnprintf(output,capacity,format,arguments);va_end(arguments);
    if(written<0) {
        (void)xr_compile_state_fail(parser->state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);return false;
    }
    return true;
}
void xr_parser_diagnostic_print(Parser *parser,int level,int code,
    const char *message,int line,int column,int token_length,const char *token_start) {
    if(!xr_parser_healthy(parser))return;
    xr_diag_print((XrDiagLevel)level,code,message,parser->source_file,line,column,token_length,
        parser->scanner.source,token_start);
    if(ferror(stderr))xr_parser_diagnostic_fail(parser,XR_PARSE_IO);
}
void xr_parser_diagnostic_summary(Parser *parser) {
    if(!xr_parser_healthy(parser))return;
    xr_diag_print_summary(parser->source_file,parser->error_count,0,parser->error_count>=parser->max_errors);
    if(ferror(stderr))xr_parser_diagnostic_fail(parser,XR_PARSE_IO);
}
