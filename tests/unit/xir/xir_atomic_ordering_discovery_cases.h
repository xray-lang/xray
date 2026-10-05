/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_ordering_discovery_cases.h - Discovery through declaration and aggregate type slots
 */
#ifndef XIR_ATOMIC_ORDERING_DISCOVERY_CASES_H
#define XIR_ATOMIC_ORDERING_DISCOVERY_CASES_H
#include "xir/xxir_declarations.h"
static void atomic_ordering_dependencies(void) {
    char foreign[XR_TEST_PATH_MAX];CHECK(snprintf(foreign,sizeof(foreign),"%s/foreign.xr",directory_path)>0);
    FILE *f=fopen(foreign,"wb");CHECK(f);CHECK(fputs("export fn normal()->i64{return 1}\n",f)>=0 && fclose(f)==0);
    write_source("import \"./foreign\" as lib\nconst order=Ordering.SeqCst\nconst n=lib.normal()\n");
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory_path};
    XrXirSourceRequest request={session,root_path,&authority,&owner.context,XR_ATOMIC_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};CHECK(xr_xir_compile_source_check(&request,&source,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);CHECK(view&&module&&module->declarations);
    CHECK(view->module_count==3 && module->declarations->module_count==3);
    uint32_t fi=UINT32_MAX,oi=UINT32_MAX;
    for(uint32_t i=0;i<view->module_count;++i){
        if(strstr(view->modules[i].path,"foreign.xr"))fi=i;
        if(!strcmp(view->modules[i].identity,"stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"))oi=i;
    }
    CHECK(fi!=UINT32_MAX && oi!=UINT32_MAX && fi!=oi);
    CHECK(module->declarations->modules[fi].dependency_count==0);
    bool root_dep=false;const XrXirSourceModule *root=&module->declarations->modules[0];
    for(uint32_t i=0;i<root->dependency_count;++i)if(root->dependencies[i]==oi)root_dep=true;
    CHECK(root_dep);atomic_source_query(view,AS_ORDERING);
    xr_xir_compile_source_result_free(&source);library_compile_owner_drop(&owner);CHECK(remove(foreign)==0);
}
static void atomic_ordering_discovery_controls(void) {
    AtomicSourceCase cases[]={
        {"struct literal only","struct Box<T> {}\nconst x=Box<Ordering>{}\n",XR_XIR_OK,AS_ORDERING},
        {"unused generic constraint only","interface Witness<A> {}\nfn unused<T:Witness<Ordering>>() {}\n",XR_XIR_OK,AS_ORDERING},
        {"implements only","interface Witness<A> {}\nstruct S implements Witness<Ordering> {}\n",XR_XIR_OK,AS_ORDERING},
        {"extends only","interface Witness<A> {}\ninterface Child extends Witness<Ordering> {}\n",XR_XIR_OK,AS_ORDERING},
        {"nested return","fn unused()->Array<Ordering?> {return []}\n",XR_XIR_OK,AS_ORDERING},
        {"generic constraint not authorization","interface Witness<A> {}\nfn unused<T:Witness<Ordering>>(a:Atomic<T>) {}\n",XR_XIR_BAD_TYPE,0}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){
        write_source(cases[i].source);LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        CHECK(atomic_source_control(&owner.context,&cases[i])==cases[i].expected);library_compile_owner_drop(&owner);
    }
    atomic_ordering_dependencies();fprintf(stderr,"Ordering discovery: five legal pipelines, one definition rejection, unrelated module dependency0/physical0\n");
}
#endif
