/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_imports.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"

static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char *const sources[]={
    "import \"./util\" as u\n",
    "// import \"./fake\"\nfn text()->string{return \"import \\\"./fake\\\"\"}\nimport { value as x } from \"./u\xf0\x9f\x98\x80til\"\n",
    "import \"\"\"\n    ./util\n    \"\"\"\n",
    "fn value()->i64{return 1}",
    "import time as t\nimport { sleep } from time\n",
    "// \xf0\x9f\x98\x80\r\nimport \"./util\";\rimport time\n",
    "fn pattern(){var pattern=/import\\{fake\\}/}\nimport \"./util\"\n",
    "/* \xf0\x9f\x98\x80 */ import \"./util\"\n"
};
static void span(XrLspRange r,uint32_t line,uint32_t column,uint32_t end_line,uint32_t end_column) {
    CHECK(r.start.line==line&&r.start.character==column&&r.end.line==end_line&&r.end.character==end_column);
}
static void facts(const XlspSourceImports *owner,unsigned mode) {
    size_t count=SIZE_MAX;const XlspSourceImport *items=xlsp_source_imports_items(owner,&count);
    CHECK(count==(mode==3?0:mode==4||mode==5?2:1));if(!count)return;CHECK(items);
    if(mode==0){CHECK(!strcmp(items[0].specifier,"./util")&&items[0].quoted);span(items[0].range,0,7,0,15);}
    else if(mode==1){CHECK(!strcmp(items[0].specifier,"./u\xf0\x9f\x98\x80til")&&items[0].quoted);span(items[0].range,2,27,2,37);}
    else if(mode==2){CHECK(!strcmp(items[0].specifier,"./util")&&items[0].quoted);span(items[0].range,0,7,2,7);}
    else if(mode==4){CHECK(!strcmp(items[0].specifier,"time")&&!items[0].quoted);span(items[0].range,0,7,0,11);
        CHECK(!strcmp(items[1].specifier,"time")&&!items[1].quoted);span(items[1].range,1,22,1,26);}
    else if(mode==5){CHECK(!strcmp(items[0].specifier,"./util")&&items[0].quoted);span(items[0].range,1,7,1,15);
        CHECK(!strcmp(items[1].specifier,"time")&&!items[1].quoted);span(items[1].range,2,7,2,11);}
    else if(mode==6){CHECK(!strcmp(items[0].specifier,"./util")&&items[0].quoted);span(items[0].range,1,7,1,15);}
    else {CHECK(!strcmp(items[0].specifier,"./util")&&items[0].quoted);span(items[0].range,0,16,0,24);}
}
static XrXirStatus once(unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    XrCompileResources *r=NULL;XlspSourceImports *owner=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&r);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char text[256],uri[]="file:///imports.xr";size_t length=strlen(sources[mode]);CHECK(length<sizeof(text));memcpy(text,sources[mode],length+1);
    XlspSourceDocument document={uri,text,sizeof(uri)-1,length,1};XrXirStatus status=xlsp_source_imports_build(r,&document,&owner);
    if(stats&&status!=XR_XIR_OK)fprintf(stderr,"Import observation mode=%u status=%u\n",mode,(unsigned)status);
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));
    if(status==XR_XIR_OK){facts(owner,mode);XlspSourceImports *occupied=owner;size_t before=runtime_attempts;
        CHECK(xlsp_source_imports_build(r,&document,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==owner&&runtime_attempts==before);}
    else CHECK(!owner);
    if(stats)*stats=sdk_stats(r);xr_compile_resources_release(r);
    if(status==XR_XIR_OK)facts(owner,mode);xlsp_source_imports_free(owner);CHECK(!runtime_live&&!runtime_bytes);return status;
}
static void rejected(void) {
    XrCompileResources *r=sdk_ledger(&limits);XlspSourceImports *owner=NULL;
    XlspSourceDocument document={"file:///imports.xr","import \"./util\"\nexport * from \"./other\"",18,40,0};
    document.uri_length=strlen(document.uri);document.length=strlen(document.text);
    CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_UNSUPPORTED&&!owner);
    document.text="// \xf0\x9f\x98\x80\r\nimport \"./util\"\rimport time\n";document.length=strlen(document.text);
    CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_UNRESOLVED&&!owner);
    document.text="import {";document.length=strlen(document.text);
    CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_UNRESOLVED&&!owner);
    document.text="\x80";document.length=1;CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_BAD_STRUCTURE&&!owner);
    document.text="\0";CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_BAD_STRUCTURE&&!owner);
    document.text="";document.length=0;document.uri="file:\0bad";document.uri_length=9;
    CHECK(xlsp_source_imports_build(r,&document,&owner)==XR_XIR_BAD_STRUCTURE&&!owner);
    xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes);
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;
    for(unsigned mode=0;mode<sizeof(sources)/sizeof(*sources);++mode) {
        XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(once(mode,limits,&stats)==XR_XIR_OK);size_t sites=runtime_attempts;CHECK(sites);
        if(full) {
            for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(mode,limits,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}
            runtime_fail_at=SIZE_MAX;
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
                XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
                uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
                CHECK(once(mode,exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            }
        }
        printf("Source imports mode=%u SourceFI=%zu full=%u exactToken/UTF16, producer death, physical0/0\n",mode,sites,full?1u:0u);
    }
    rejected();return 0;
}
