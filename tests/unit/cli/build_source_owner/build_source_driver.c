/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * build_source_driver.c - Actual command parser and build consumer
 */
#include "app/cli/xcli.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if BUILD_SOURCE_INJECTED
#define CHECK(c) do { if(!(c)){fprintf(stderr,"injection invariant line=%d\n",__LINE__);exit(90);} } while(0)
#include "publication_faults.inc.h"
#endif
XR_FUNC int cmd_build(const XrCliInvocation *inv);
int wmain(int argc, wchar_t **wide) {
#if BUILD_SOURCE_INJECTED
    const char *mode=getenv("XR_PUBLICATION_TEST_FAILURE");
    if(mode && !strcmp(mode,"published-close"))permanent_close=true;
    else if(mode && !strcmp(mode,"unpublished-close")){force_flush_failure=true;fail_disposition=true;}
    else if(mode && !strcmp(mode,"transient-close"))fail_close=0;
#endif
    XrWinPathStatus status;
    char **argv=xr_win_utf16_arguments(argc,wide,&status);
    if(!argv)return 4;
    XrCliContext context={.program="xray"};
    XrCliInvocation invocation;
    int result=(int)xr_cli_parse_command(xr_cli_find_command("build"),argc-1,argv+1,&context,&invocation);
    if(!result){result=cmd_build(&invocation);xr_cli_invocation_free(&invocation);}
    xr_win_utf8_arguments_free(argc,argv);return result;
}
