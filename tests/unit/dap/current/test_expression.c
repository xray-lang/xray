/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_expression.c - Complete expression input on the original parser owner
 */
#include "frontend/parser/xparse.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"expression FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
static size_t expression_fault(size_t fail) {
    XrCompileResources *resources=NULL;XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;AstNode *program=NULL;
    XrParseStatus status=xr_compile_parse_expression_string(session,"1; print(2)","<debug-expression>",&program);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(!program && !xr_compile_session_current_arena(session));
    if (fail==SIZE_MAX) CHECK(status==XR_PARSE_SYNTAX);else CHECK(status==XR_PARSE_OUT_OF_MEMORY && sites>fail);
    xr_compile_session_free(session);XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);return sites;
}
int main(int argc,char **argv) {
    bool full=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || full);
    xr_test_suppress_dialogs();XrCompileResources *resources=NULL;XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    const char *accepted[]={"1+2","value == 2","(value+1)*2\n", "true && !false /* trailing comment */"};
    for (uint32_t i=0;i<sizeof(accepted)/sizeof(*accepted);++i) {
        AstNode *program=NULL;CHECK(xr_compile_parse_expression_string(session,accepted[i],"<debug-expression>",&program)==XR_PARSE_OK);
        CHECK(program && program->type==AST_PROGRAM && program->as.program.count==1);xr_program_destroy(program);
        CHECK(!xr_compile_session_current_arena(session));
    }
    const char *rejected[]={"1; print(2)","1 2","value) }\nimport \"other\"","1\n2"};
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        AstNode *program=NULL;XrParseStatus status=xr_compile_parse_expression_string(session,rejected[i],"<debug-expression>",&program);
        if (status!=XR_PARSE_SYNTAX) fprintf(stderr,"Trailing expression accepted: %s status=%u\n",rejected[i],status);
        CHECK(status==XR_PARSE_SYNTAX && !program);CHECK(!xr_compile_session_current_arena(session));
    }
    xr_compile_session_free(session);XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
    size_t sites=expression_fault(SIZE_MAX);CHECK(sites);
    if (full) for (size_t i=0;i<sites;++i) expression_fault(i);
    else { expression_fault(0);if (sites>1) expression_fault(sites-1); }
    printf("Complete expression/EOF and trailing injection refusal; parser sites=%zu injection=%s original ledger and physical=0/0 PASS\n",sites,full ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
