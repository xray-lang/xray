/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_workspace.h"
#include "api/xsource_compile.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "legacy_inputs.h"
static bool token(const XlspSourceToken *items,size_t count,int row,int col,int len,int kind) {
    for(size_t n=0;n<count;++n)if(items[n].line==(uint32_t)row&&items[n].column==(uint32_t)col&&items[n].length==(uint32_t)len&&items[n].type==(uint32_t)kind)return true;
    return false;
}
static void position(const char *text,const char *needle,unsigned nth,int *row,int *col) {
    const char *p=text;while(nth--){p=strstr(p,needle);CHECK(p);if(nth)++p;}
    *row=*col=0;for(const char *s=text;s<p;++s){if(*s=='\n'){++*row;*col=0;}else ++*col;}
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==3&&argv[2][0]>='0'&&argv[2][0]<='2'&&!argv[2][1]);unsigned mode=(unsigned)(argv[2][0]-'0');
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;XlspSourceTokens *owner=NULL;XlspSourceWorkspaceFailure failure={0};unsigned stage=0;
    XlspSourceDocument doc={argv[1],texts[mode],strlen(argv[1]),strlen(texts[mode]),1};
    XrXirStatus status=xlsp_source_workspace_build(&context,&doc,1,&workspace,&stage,&failure);
    if(status==XR_XIR_OK)status=xlsp_source_workspace_tokens(workspace,argv[1],strlen(argv[1]),NULL,&owner);
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(owner,&count);bool matches=status==XR_XIR_OK;int row=0,col=0;
    if(matches&&mode==0){const char *words[]={"ref","move","ref","move"};for(unsigned n=0;n<4;++n){position(texts[mode],words[n],n<2?1:2,&row,&col);matches=token(items,count,row,col,(int)strlen(words[n]),16)&&matches;}}
    if(matches&&mode==1){position(texts[mode],"after",1,&row,&col);matches=token(items,count,row,col,5,8);for(size_t n=0;n<count;++n)matches=matches&&items[n].line!=1&&items[n].line!=4;}
    if(matches&&mode==2){unsigned properties=0;for(size_t n=0;n<count;++n)properties+=items[n].type==9&&items[n].length==5;matches=properties>=5;for(unsigned nth=1;nth<=4;nth+=(nth==1?2:1)){position(texts[mode],"value",nth,&row,&col);matches=token(items,count,row,col,5,9)&&matches;}}
    printf("Original semantic assertions mode=%u status=%u stage=%u tokens=%zu matched=%u diagnostic=%s\n",mode,(unsigned)status,stage,count,matches?1u:0u,failure.diagnostic.message);
    XrCompileResourceStats stats=sdk_stats(resources);
    printf("Original semantic ledger allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    xlsp_source_tokens_free(owner);xlsp_source_workspace_free(workspace);xlsp_source_workspace_failure_free(&failure);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    printf("Original semantic Source owner physical=0/0; failing Source facts remain a failing test\n");
    return matches?0:1;
}
int wmain(int argc,wchar_t **wide) {XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
