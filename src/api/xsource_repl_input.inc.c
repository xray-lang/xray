/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_repl_input.inc.c - Bounded real-lexer observation of interactive input
 */
#include "../toolchain/xcompiler_arena_backing.h"
XRAY_API XrSourceStatus xray_source_repl_check_input(XrSourceRepl *owner,const char *bytes,size_t length,
    XrSourceInputState *output) {
    if(!output || (!bytes && length) || length==SIZE_MAX)return XR_SOURCE_BAD_ARGUMENT;
    XrSourceStatus status=api_repl_admit(owner);if(status!=XR_SOURCE_OK)return status;
    if(owner->active)return XR_SOURCE_BUSY;
    if(!length){*output=XR_SOURCE_INPUT_COMPLETE;return XR_SOURCE_OK;}
    owner->busy=true;char *source=NULL;XrCompileState *state=xr_source_compiler_state(owner->compiler);
    status=api_resource(xr_compile_state_strndup(state,bytes,length,&source));
    for(size_t i=0;status==XR_SOURCE_OK && i<length;++i) {
        status=api_resource(xr_compile_state_work(state,1));
        if(status==XR_SOURCE_OK && !source[i])status=XR_SOURCE_INVALID;
    }
    XrArena arena={0};XrArenaBacking backing;
    Scanner scanner;int64_t depth[3]={0};XrSourceInputState complete=XR_SOURCE_INPUT_COMPLETE;
    if(status==XR_SOURCE_OK) {
        (void)xr_compiler_arena_state_backing(state,&backing);
        (void)xr_arena_open(&arena,XR_ARENA_SEGMENT_SIZE,&backing);
        status=api_resource(xr_compiler_arena_capture_status(&arena,state));
    }
    if(status==XR_SOURCE_OK)status=api_resource(xr_compile_scanner_open(&scanner,state,&arena,source));
    while(status==XR_SOURCE_OK) {
        Token token=xr_scanner_scan(&scanner);
        status=api_resource(xr_compile_state_status(state));if(status!=XR_SOURCE_OK)break;
        if(token.type==TK_EOF){if(depth[0]>0 || depth[1]>0 || depth[2]>0)complete=XR_SOURCE_INPUT_INCOMPLETE;break;}
        if(token.type==TK_ERROR) {
            if(token.error_message && strstr(token.error_message,"nterminated"))complete=XR_SOURCE_INPUT_INCOMPLETE;
            break;
        }
        unsigned axis=0;int delta=0;
        switch(token.type) {
        case TK_LPAREN:delta=1;break;
        case TK_RPAREN:delta=-1;break;
        case TK_LBRACKET:case TK_SET_START:axis=1;delta=1;break;
        case TK_RBRACKET:axis=1;delta=-1;break;
        case TK_LBRACE:case TK_EMPTY_MAP_START:axis=2;delta=1;break;
        case TK_RBRACE:axis=2;delta=-1;break;
        default:break;
        }
        if((delta>0 && depth[axis]==INT64_MAX) || (delta<0 && depth[axis]==INT64_MIN))status=XR_SOURCE_BUDGET;
        else depth[axis]+=delta;
    }
    xr_arena_destroy(&arena);xr_compile_state_free(source);
    if(status==XR_SOURCE_OK)*output=complete;
    status=api_repl_capture(owner,status);owner->busy=false;return status;
}
