/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_library_names.inc.c - Owned Source spellings of executable names
 *
 * KEY CONCEPT:
 *   Executable diagnostic labels need not denote a Source declaration.
 */
#include "../frontend/lexer/xlex.h"
static char *source_library_owned_name(SourceContext *ctx,const char *bytes,uint32_t length) {
    if (!bytes || !length || (uint64_t)length+1>SIZE_MAX) {
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"library name has no owned Source spelling");return NULL;
    }
    char *name=source_alloc(ctx,(size_t)length+1,1);
    if (!name || !source_copy_bytes(ctx,NULL,name,bytes,length)) return NULL;
    name[length]=0;return name;
}
static bool source_library_source_name(SourceContext *ctx,const char *name,uint32_t length,bool *visible) {
    *visible=false;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    XrCompileState *state=NULL;XrArena arena={0};XrArenaBacking backing={0};Scanner scanner={0};
    XrCompileResourceStatus status=xr_compile_state_new(ctx->compile.resources,&state);
    XrArenaStatus storage=XR_ARENA_OK;
    if (status==XR_COMPILE_RESOURCE_OK) storage=xr_compiler_arena_state_backing(state,&backing);
    if (status==XR_COMPILE_RESOURCE_OK && storage==XR_ARENA_OK)
        storage=xr_arena_open(&arena,0,&backing);
    if (status==XR_COMPILE_RESOURCE_OK && storage==XR_ARENA_OK)
        status=xr_compile_scanner_open(&scanner,state,&arena,name,ctx->request);
    if (status==XR_COMPILE_RESOURCE_OK && storage==XR_ARENA_OK) {
        Token first=xr_scanner_scan(&scanner);
        status=xr_compile_state_status(state);storage=xr_arena_status(&arena);
        /* A healthy TK_ERROR for punctuation is a spelling mismatch, not a
         * resource failure. EOF is meaningful only while both owners are healthy. */
        if (status==XR_COMPILE_RESOURCE_OK && storage==XR_ARENA_OK && first.type==TK_NAME &&
            first.start==name && first.length>=0 && (uint32_t)first.length==length) {
            Token end=xr_scanner_scan(&scanner);
            status=xr_compile_state_status(state);storage=xr_arena_status(&arena);
            *visible=status==XR_COMPILE_RESOURCE_OK && storage==XR_ARENA_OK && end.type==TK_EOF;
        }
    }
    xr_arena_destroy(&arena);xr_compile_state_release(state);
    if (status!=XR_COMPILE_RESOURCE_OK)
        return source_fail(ctx,NULL,xir_compile_resource_status(status),"library name scanner refused its input");
    if (storage!=XR_ARENA_OK)
        return source_fail(ctx,NULL,storage==XR_ARENA_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            storage==XR_ARENA_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
            storage==XR_ARENA_OVERFLOW ? XR_XIR_OVERFLOW : XR_XIR_BAD_STRUCTURE,"library name scanner arena failed");
    return true;
}
