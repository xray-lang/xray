/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_corpus.c - Captured checking graphs preserve root and storage constraints
 */
#include "api/xsource_authority.h"
#include "api/xsource_paths.h"
#include "xir/xxir_source_corpus.h"
#include "xir/xxir_source_dependencies.h"
#include "xir/xxir_stdlib_internal.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char callback[]="fn target(v:i64)->i64{return v}\nfn outer()->fn(i64)->i64{return fn(v:i64)->i64{return target(v)}}\nconst cb=outer()\n";
static uint32_t corpus_module(const XrXirSourceView *view,const char *path) {
    for(uint32_t i=0;i<view->module_count;++i)if(!strcmp(view->modules[i].path,path))return i;
    CHECK(false);return UINT32_MAX;
}
static const XrXirSourceDependency *corpus_dependency(const XrXirSourceDependencies *deps,uint32_t module) {
    for(uint32_t i=0;i<deps->count;++i)if(deps->entries[i].module==module)return &deps->entries[i];
    CHECK(false);return NULL;
}
static void corpus_once(char **argv,unsigned mode) {
    const char *texts[]={callback,"export fn visible(v:i64)->i64{return v}\n","import \"./root\"\nfn closed()->i64{return 1}\n",
        "import {visible} from \"./lib\"\nfn extra(v:i64)->i64{return visible(v)}\n"};
    if(mode==1){texts[0]="fn main(){}\n";texts[1]="export fn visible(v:i64)->i64{return v}\nfn outer()->fn(i64)->i64{return fn(v:i64)->i64{return v}}\nconst cb=outer()\n";}
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XrStdlibPath stdlib={0};CHECK(xr_source_stdlib_path(r,&stdlib,NULL)==XR_SOURCE_COMPILE_OK);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xir_stdlib_library_catalog_new(&context,stdlib.path,&catalog)==XR_XIR_OK);
    XrSourceAuthority *authorities[4]={0};XrModuleOverlayInput overlays[4];XrTomlParseLimits toml=xr_source_compile_default_manifest_limits();
    for(unsigned i=0;i<4;++i){XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,argv[i+1],texts[i],strlen(texts[i])};
        CHECK(xr_source_authority_open_input(&context,&input,catalog,&toml,&authorities[i],NULL)==XR_MANIFEST_OK);
        overlays[i]=*xr_source_authority_overlay(authorities[i]);}
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(r,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest entry={session,xr_source_authority_source_path(authorities[0]),xr_source_authority_entry(authorities[0]),
        &context,stdlib.path,xr_source_authority_lockfile(authorities[0]),XR_XIR_PROGRAM,catalog};
    size_t included[]={0,1,2,3};XrXirSourceCorpusRequest corpus={&entry,overlays,4,included,4};
    if(mode==2)included[3]=4;if(mode==3)corpus.included=NULL;
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_corpus(&corpus,&result,&diagnostic,NULL);
    XrXirStatus expected=mode==0?XR_XIR_OK:mode==1?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE;
    if(status!=expected)fprintf(stderr,"corpus mode%u expected%d got%d: %s\n",mode,expected,status,diagnostic.message);
    CHECK(status==expected&&diagnostic.status==expected);
    if(mode==0){const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        const XrXirSourceDependencies *deps=xr_xir_compile_source_snapshot_dependencies(result.snapshot);CHECK(view&&view->complete&&deps&&deps->entry<deps->count);
        CHECK(!strcmp(deps->entry_path,entry.entry_path));uint32_t ids[4];for(unsigned i=0;i<4;++i)ids[i]=corpus_module(view,xr_source_authority_source_path(authorities[i]));
        CHECK(deps->entries[deps->entry].module==ids[0]);
        CHECK(!corpus_dependency(deps,ids[0])->import_count&&!corpus_dependency(deps,ids[1])->import_count);
        const XrXirSourceDependency *closed=corpus_dependency(deps,ids[2]),*extra=corpus_dependency(deps,ids[3]);
        CHECK(closed->import_count==1&&closed->imports[0]==ids[0]&&extra->import_count==1&&extra->imports[0]==ids[1]);
    }else CHECK(!result.checked&&!result.snapshot);
    for(unsigned i=0;i<4;++i)xr_source_authority_close(authorities[i]);xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);xr_source_stdlib_path_free(&stdlib);xr_compile_resources_release(r);
    if(mode==0){const XrXirSourceDependencies *deps=xr_xir_compile_source_snapshot_dependencies(result.snapshot);CHECK(deps&&deps->entry_path&&deps->count>=4);}
    xr_xir_compile_source_result_free(&result);CHECK(!runtime_live&&!runtime_bytes);
    printf("Source corpus mode=%u status=%d physical=0/0\n",mode,status);
}
static int run(int argc,char **argv){CHECK(argc==5);(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;(void)sdk_stats;
    for(unsigned mode=0;mode<4;++mode)corpus_once(argv,mode);return 0;}
int wmain(int argc,wchar_t **wide){XrWinPathStatus s;char **argv=xr_win_utf16_arguments(argc,wide,&s);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
