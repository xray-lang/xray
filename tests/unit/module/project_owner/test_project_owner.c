/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "../../xir/xir_construction_fixture.h"
#include "module/xproject.h"
#include "app/cli/xcli_graph_authority.h"
#include "app/cli/xcli_fs.h"
#include "xir/xxir_library_catalog.h"
#include "runtime/value/xstruct_layout.h"
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[8192];
static size_t attempts, fail_at = SIZE_MAX, physical_live, physical_total, physical_peak, block_count;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_malloc(bytes); CHECK(pointer);
    size_t i = 0; while (i < 8192 && allocations[i].pointer) ++i; CHECK(i < 8192);
    allocations[i] = (Allocation){pointer,bytes}; ++block_count; physical_live += bytes; physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live; return pointer;
}
static void observed_free(void *pointer) {
    if (!pointer) return;
    size_t i = 0; while (i < 8192 && allocations[i].pointer != pointer) ++i; CHECK(i < 8192);
    physical_live -= allocations[i].bytes; --block_count; allocations[i] = (Allocation){0}; xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_alloc(bytes)
#define xr_free(pointer) observed_free(pointer)
#define xr_compile_resources_work real_resources_work
#define xr_compile_resources_alloc real_resources_alloc
#define xr_compile_resources_calloc real_resources_calloc
#define xr_compile_resources_resize real_resources_resize
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
static bool recording;

static uint64_t boundaries[200000]; static size_t boundary_count;
static XrCompileResourceStatus record(XrCompileResources *resources, XrCompileResourceStatus status) {
    if (recording && status == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats measured; CHECK(xr_compile_resources_stats(resources,&measured) == XR_COMPILE_RESOURCE_OK);
        if (!boundary_count || boundaries[boundary_count-1] != measured.work) {
            CHECK(boundary_count < 200000); boundaries[boundary_count++] = measured.work;
        }
    }
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r,uint64_t n) { return record(r,real_resources_work(r,n)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r,size_t n,void **out) { return record(r,real_resources_alloc(r,n,out)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r,size_t n,size_t s,void **out) { return record(r,real_resources_calloc(r,n,s,out)); }
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(XrCompileResources *r,void **p,size_t n) {
    return record(r,real_resources_resize(r,p,n));
}

static const XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
static const XrTomlParseLimits parse_limits={1024*1024,64};
static char root_path[4096], entry_path[4096];
static XrCompileResourceStats measured;
static void reset(size_t failure) {
    CHECK(!physical_live&&!block_count);attempts=physical_total=physical_peak=0;fail_at=failure;
}
static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats value;CHECK(xr_compile_resources_stats(owner,&value)==XR_COMPILE_RESOURCE_OK);
    CHECK(value.live_bytes==physical_live&&value.allocated_bytes==physical_total&&value.peak_bytes==physical_peak);return value;
}
static XrManifestStatus pipeline(XrCompileResources *owner, bool validate) {
    XrOsIoPolicy policy=xr_compile_io_policy(owner);XrProject *project=NULL;XrProjectAuthority *authority=NULL;
    XrManifestDiagnostic diagnostic={0};XrManifestStatus status=xr_project_load_owned(&policy,root_path,&parse_limits,&project,&diagnostic);
    if (status!=XR_MANIFEST_OK) { if(validate)fprintf(stderr,"load: %s\n",diagnostic.message);CHECK(!project);return status; }
    CHECK(xr_project_uses_policy(project,&policy)&&xr_native_package_plan_uses_policy(project->native_plan,&policy));
    status=xr_project_authority_build_owned(project,&authority,&diagnostic);
    const XrTargetConfig *target=NULL;
    if (status==XR_MANIFEST_OK)status=xr_project_find_target_config_owned(project,"x86_64-windows-msvc",&target);
    if (status==XR_MANIFEST_OK&&validate)CHECK(target&&strcmp(target->cc,"cl.exe")==0);
    char *dependency=NULL;
    if (status==XR_MANIFEST_OK)status=xr_resolve_local_dependency_owned(project,"local",&dependency);
    if (status==XR_MANIFEST_OK&&validate)CHECK(dependency&&strstr(dependency,"sub"));
    xr_project_path_free_owned(dependency);
    if (status==XR_MANIFEST_OK)status=xr_native_package_configure_c_exports_owned(project->native_plan,"prefix_","drop",&diagnostic);
    const XrCExportPlan *entry=NULL;
    if (status==XR_MANIFEST_OK)status=xr_native_package_find_export_owned(project->native_plan,"answer",&entry);
    if (status==XR_MANIFEST_OK&&validate)CHECK(entry&&strcmp(entry->symbol,"prefix_answer")==0&&project->native_plan->export_count==1);
    if (status==XR_MANIFEST_OK)status=xr_native_package_note_layout_subject_owned(project->native_plan,"Point");
    XrAggregateLayout layout={0};layout.field_count=1;layout.total_size=8;layout.alignment=8;
    const char *field_names[]={"value"};layout.field_names=field_names;layout.fields[0].offset=0;bool matched=false;
    if(status==XR_MANIFEST_OK)status=xr_native_package_resolve_layout_owned(project->native_plan,"Point",&layout,&matched);
    if(status==XR_MANIFEST_OK&&validate)CHECK(matched&&project->native_plan->layouts[0].resolved&&project->native_plan->layouts[0].field_count==1);
    if(status==XR_MANIFEST_OK) {
        const XrNativeSymbol *symbol=NULL;status=xr_native_package_find_symbol_owned(project->native_plan,"answer",&symbol);
        if(status==XR_MANIFEST_OK&&validate)CHECK(symbol&&strcmp(symbol->native_name,"owner_answer")==0);
    }
    if(status==XR_MANIFEST_OK) {
        XrXirCompileContext context={owner,xr_xir_compile_default_limits()};XrCliGraphAuthority *graph=NULL;
        status=xr_cli_compile_graph_authority_open(&context,entry_path,NULL,&parse_limits,&graph,&diagnostic);
        if(status==XR_MANIFEST_OK&&validate)CHECK(xr_cli_compile_graph_authority_project(graph)&&
            xr_cli_compile_graph_authority_entry(graph)->kind==XR_MODULE_IDENTITY_PROJECT&&
            xr_lockfile_uses_policy(xr_cli_compile_graph_authority_lockfile(graph),&policy));
        xr_cli_compile_graph_authority_close(graph);
    }
    xr_project_free_owned(project);
    if(status==XR_MANIFEST_OK&&validate) {
        const XrModuleIdentityAuthority *view=xr_project_authority_view(authority);
        CHECK(view&&view->kind==XR_MODULE_IDENTITY_PROJECT&&strcmp(view->namespace_id,"owner-test")==0);
    }
    if(validate&&status!=XR_MANIFEST_OK)fprintf(stderr,"pipeline: %s\n",diagnostic.message);
    xr_project_authority_free_owned(authority);return status;
}
static size_t run(const XrCompileResourceLimits *limits, size_t failure, XrManifestStatus expected) {
    reset(failure);XrCompileResources *owner=NULL;
    XrCompileResourceStatus init=xr_compile_resources_new(limits,&owner);
    if(init!=XR_COMPILE_RESOURCE_OK) { CHECK(!owner&&!physical_live);CHECK(expected==(init==XR_COMPILE_RESOURCE_BUDGET?XR_MANIFEST_BUDGET:XR_MANIFEST_OUT_OF_MEMORY));return attempts; }
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    XrManifestStatus status=pipeline(owner,expected==XR_MANIFEST_OK);
    if(status!=expected||block_count!=1)fprintf(stderr,"failure=%zu work=%llu expected=%d actual=%d blocks=%zu\n",failure,(unsigned long long)limits->work,expected,status,block_count);
    CHECK(status==expected&&block_count==1);CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    measured=stats(owner);xr_compile_resources_release(owner);CHECK(!physical_live&&!block_count);return attempts;
}
static void qualify(void) {
    recording=true;boundary_count=0;size_t count=run(&unlimited,SIZE_MAX,XR_MANIFEST_OK);recording=false;
    XrCompileResourceStats baseline=measured;size_t cuts=boundary_count;
    printf("baseline allocations=%zu workcuts=%zu work=%llu\n",count,cuts,(unsigned long long)baseline.work);fflush(stdout);
    for(size_t i=0;i<count;++i) { run(&unlimited,i,XR_MANIFEST_OUT_OF_MEMORY);CHECK(attempts==i+1); }
    puts("OOM PASS");fflush(stdout);
    for(size_t i=0;i<cuts;++i) { XrCompileResourceLimits limits=unlimited;limits.work=boundaries[i]-1;run(&limits,SIZE_MAX,XR_MANIFEST_BUDGET); }
    for(unsigned axis=0;axis<3;++axis)for(unsigned below=0;below<2;++below) {
        XrCompileResourceLimits limits=unlimited;
        if(!axis)limits.allocated_bytes=baseline.allocated_bytes-below;
        else if(axis==1)limits.live_bytes=baseline.peak_bytes-below;
        else limits.work=baseline.work-below;
        run(&limits,SIZE_MAX,below?XR_MANIFEST_BUDGET:XR_MANIFEST_OK);
    }
    printf("project native: %zu real OOM, %zu work cutoffs, three exact-minus1 axes, physical/handles zero PASS\n",count,cuts);
}

static size_t mutation_sites;
static XrManifestStatus mutate(XrProject *project, unsigned kind, bool *matched) {
    if(kind==0)return xr_native_package_configure_c_exports_owned(project->native_plan,"prefix_","drop",NULL);
    if(kind==2)return xr_native_package_note_layout_subject_owned(project->native_plan,"Point");
    XrAggregateLayout layout={0};layout.field_count=2;layout.total_size=16;layout.alignment=8;
    const char *names[]={"left","right"};layout.field_names=names;layout.fields[0].offset=0;layout.fields[1].offset=8;
    return xr_native_package_resolve_layout_owned(project->native_plan,"Point",&layout,matched);
}
static void mutation_case(unsigned kind, size_t failure, uint64_t limit, XrManifestStatus expected, bool record_work) {
    reset(SIZE_MAX);XrCompileResources *owner=NULL;XrCompileResourceLimits limits=unlimited;limits.work=limit;
    CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy=xr_compile_io_policy(owner);XrProject *project=NULL;
    CHECK(xr_project_load_owned(&policy,root_path,&parse_limits,&project,NULL)==XR_MANIFEST_OK);
    bool matched=false;if(kind==1)CHECK(mutate(project,kind,&matched)==XR_MANIFEST_OK&&matched);
    XrNativePackagePlan before=*project->native_plan;
    XrCExportPlan exports[2];memcpy(exports,before.exports,sizeof(exports));
    XrNativeLayoutAssertion layout=before.layouts[0];
    XrCompileResourceStats held=stats(owner);size_t start=attempts;
    fail_at=failure==SIZE_MAX?SIZE_MAX:start+failure;recording=record_work;matched=false;
    XrManifestStatus status=mutate(project,kind,&matched);recording=false;
    if(status!=expected)fprintf(stderr,"mutation kind=%u fail=%zu limit=%llu got=%d expected=%d\n",kind,failure,(unsigned long long)limit,status,expected);
    CHECK(status==expected);mutation_sites=attempts-start;
    if(expected!=XR_MANIFEST_OK) {
        CHECK(memcmp(project->native_plan,&before,sizeof(before))==0);
        CHECK(memcmp(before.exports,exports,sizeof(exports))==0&&memcmp(&before.layouts[0],&layout,sizeof(layout))==0);
        CHECK(stats(owner).live_bytes==held.live_bytes&&!matched);
        if(expected==XR_MANIFEST_OUT_OF_MEMORY) {
            CHECK(attempts==fail_at+1);fail_at=SIZE_MAX;
            CHECK(mutate(project,kind,&matched)==XR_MANIFEST_OK);
        }
    }
    xr_project_free_owned(project);xr_compile_resources_release(owner);CHECK(!block_count&&!physical_live);
}
static void mutation_gates(void) {
    for(unsigned kind=0;kind<3;++kind) {
        boundary_count=0;mutation_case(kind,SIZE_MAX,UINT64_MAX,XR_MANIFEST_OK,true);
        size_t sites=mutation_sites,cuts=boundary_count;
        for(size_t i=0;i<sites;++i)mutation_case(kind,i,UINT64_MAX,XR_MANIFEST_OUT_OF_MEMORY,false);
        for(size_t i=0;i<cuts;++i)mutation_case(kind,SIZE_MAX,boundaries[i]-1,XR_MANIFEST_BUDGET,false);
        printf("mutation %u: %zu OOM, %zu work cutoffs preserve plan/live; OOM retry PASS\n",kind,sites,cuts);fflush(stdout);
    }
}
static XrOsIoStatus forwarded_work(void *context, uint64_t work) {
    XrOsIoPolicy real=xr_compile_io_policy(context);return real.work(context,work);
}
static XrXirLibraryCatalog *real_catalog(const XrXirCompileContext *context) {
    const XrXirInstruction ops[]={{XR_XIR_RETURN,XR_XIR_UNIT,{0,0},{0,0},0,{0,0}}};
    const XrXirBlock block={0,1,0,0};
    const XrXirFunction function={"init",4,NULL,0,XR_XIR_UNIT,&block,1,ops,1,NULL,0};
    const char canonical[]="module-id-v1:kind=6:script:namespace=0::path=6:lib.xr";
    XrXirSourceModule source={canonical,sizeof(canonical)-1,NULL,0,0};
    const XrXirFunctionIdentity name={0,0,0,0,0,0,0, 0, 0};
    XrXirDeclarations declarations={&source,1,&name,NULL,0,NULL,0,UINT32_MAX,UINT32_MAX,NULL};
    XrXirModule module={XR_XIR_BUILT,&function,1,&declarations,NULL,NULL,NULL,XR_XIR_LIBRARY,NULL};
    XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    CHECK(xir_fixture_check(context, &module, &artifact, NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={packet.bytes,packet.length,{0}, (XrXirLibraryModuleInput[]){{{XR_MODULE_IDENTITY_SCRIPT,NULL,root_path},"lib.xr"}},1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);xr_xir_compile_checked_packet_free(&packet);return catalog;
}
static void ownership_rejections(void) {
    reset(SIZE_MAX);XrCompileResources *one=NULL,*two=NULL;
    CHECK(xr_compile_resources_new(&unlimited,&one)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&unlimited,&two)==XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy first=xr_compile_io_policy(one),other=xr_compile_io_policy(two),forged=first;forged.work=forwarded_work;
    const char text[]="[[export.c]]\nxray='answer'\nsymbol='answer'\n";
    XrTomlValue *document=NULL;CHECK(xtoml_parse_owned(&first,text,sizeof(text)-1,&parse_limits,&document)==XR_TOML_PARSE_OK);
    CHECK(xtoml_owned_uses_policy(document,&first)&&!xtoml_owned_uses_policy(document,&other)&&!xtoml_owned_uses_policy(document,&forged));
    XrNativePackagePlan *plan=NULL;size_t before=attempts;
    CHECK(xr_native_package_plan_parse_owned(&other,document,root_path,&plan,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!plan&&attempts==before);
    CHECK(xr_native_package_plan_parse_owned(&forged,document,root_path,&plan,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!plan&&attempts==before);
    CHECK(xr_native_package_plan_parse_owned(&first,document,root_path,&plan,NULL)==XR_MANIFEST_OK);
    CHECK(xr_native_package_plan_uses_policy(plan,&first)&&!xr_native_package_plan_uses_policy(plan,&forged));
    xtoml_owned_free(document);xr_compile_resources_release(one);xr_compile_resources_release(two);
    const XrCExportPlan *entry=NULL;CHECK(xr_native_package_find_export_owned(plan,"answer",&entry)==XR_MANIFEST_OK&&entry&&strcmp(entry->symbol,"answer")==0);
    xr_native_package_plan_free_owned(plan);CHECK(!physical_live&&!block_count);

    reset(SIZE_MAX);one=NULL;two=NULL;CHECK(xr_compile_resources_new(&unlimited,&one)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&unlimited,&two)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext a={one,xr_xir_compile_default_limits()},b={two,xr_xir_compile_default_limits()};XrXirLibraryCatalog *catalog=NULL;
    catalog=real_catalog(&a);
    XrCliGraphAuthority *authority=NULL;before=attempts;
    CHECK(xr_cli_compile_graph_authority_open(&b,entry_path,catalog,&parse_limits,&authority,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!authority&&before==attempts);
    CHECK(xr_cli_compile_graph_authority_open(&a,entry_path,catalog,&parse_limits,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_catalog(authority)==catalog&&xr_cli_compile_graph_authority_context(authority)->resources==one);
    xr_compile_resources_release(one);xr_compile_resources_release(two);
    CHECK(xr_cli_compile_graph_authority_entry(authority)->kind==XR_MODULE_IDENTITY_PROJECT);
    xr_cli_compile_graph_authority_close(authority);xr_xir_compile_library_catalog_free(catalog);CHECK(!physical_live&&!block_count);
    puts("foreign DOM/callback table/Catalog and producer release lifetime PASS");
}
static const char *const rejected[] = {
    "[native]\nunknown=true\n",
    "[native]\nname='p'\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='unknown'\nvm='unsupported'\n",
    "[native]\nname='p'\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='shipping'\nvm='unsupported'\n",
    "[native]\nname=42\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='exploratory'\nvm='unsupported'\n",
    "[[export.c]]\nxray='f'\nsymbol='bad-name'\n",
    "[[export.c]]\nxray='f'\nsymbol='f'\n[[export.c]]\nxray='f'\nsymbol='g'\n",
    "[[export.c]]\nxray='f'\nsymbol='f'\nvisibility='internal'\n",
    "[[link.symbol]]\nxray='f'\n",
    "[[freestanding.entry]]\nxray='f'\nsymbol='f'\nkind='interrupt'\nsection='irq'\n",
    "[native]\nname='p'\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='exploratory'\nvm='unsupported'\n[[native.unit]]\nname='c'\nkind='c'\npurpose='test'\nsources=['owner.c','owner.c']\nsource_hashes=['abc']\n",
    "[native]\nname='p'\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='exploratory'\nvm='unsupported'\n[native.target.'a..b']\nprofile='release'\n",
    "[native]\nname='p'\nversion='1'\nlicense='MIT'\nsource='local'\naudit_mode='exploratory'\nvm='unsupported'\n[[native.layout]]\nxray_type='T'\nc_type='T'\nheader='../escape.h'\nassert={size=true,align=true,fields=true}\n"
};
static size_t reject_case(const char *text, size_t failure, XrManifestStatus expected) {
    reset(failure);XrCompileResources *owner=NULL;
    if(xr_compile_resources_new(&unlimited,&owner)!=XR_COMPILE_RESOURCE_OK) { CHECK(expected==XR_MANIFEST_OUT_OF_MEMORY&&!physical_live);return attempts; }
    XrOsIoPolicy policy=xr_compile_io_policy(owner);XrTomlValue *document=NULL;XrNativePackagePlan *plan=NULL;
    XrTomlParseStatus parsed=xtoml_parse_owned(&policy,text,strlen(text),&parse_limits,&document);
    XrManifestStatus status=parsed==XR_TOML_PARSE_OUT_OF_MEMORY?XR_MANIFEST_OUT_OF_MEMORY:parsed==XR_TOML_PARSE_OK?
        xr_native_package_plan_parse_owned(&policy,document,root_path,&plan,NULL):XR_MANIFEST_INVALID;
    CHECK(status==expected&&!plan);xtoml_owned_free(document);xr_compile_resources_release(owner);CHECK(!physical_live&&!block_count);return attempts;
}
static void reject_gates(void) {
    size_t total=0;
    for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        size_t sites=reject_case(rejected[i],SIZE_MAX,XR_MANIFEST_INVALID);total+=sites;
        for(size_t j=0;j<sites;++j) { reject_case(rejected[i],j,XR_MANIFEST_OUT_OF_MEMORY);CHECK(attempts==j+1); }
    }
    CHECK(reject_case("[project]\nname='p'\n",SIZE_MAX,XR_MANIFEST_NOT_FOUND)>0);
    printf("12 independent manifest rejections, %zu actual OOM cleanup points PASS\n",total);
}

static const char complete_contract[] =
    "[native]\n"
    "name='full'\n"
    "version='1.0.0'\n"
    "license='MIT'\n"
    "source='local'\n"
    "audit_mode='exploratory'\n"
    "vm='unsupported'\n"
    "[[native.unit]]\n"
    "name='platform'\n"
    "kind='platform'\n"
    "purpose='host function'\n"
    "optimization='none'\nvisibility='default'\nwarnings='strict'\n"
    "[[native.symbol]]\n"
    "xray='invoke'\n"
    "native='host_invoke'\n"
    "kind='function'\n"
    "calling_convention='c'\n"
    "unit='platform'\n"
    "[native.symbol.contract]\n"
    "params=[{index=0,access='none',escape='noescape',ownership='value',output='none',nullable=false}]\n"
    "return={ownership='value',nullable=false,validity='always'}\n"
    "effects=['foreign']\n"
    "callbacks=[{index=0,context_index=-1,escape='noescape',thread='caller',lifetime='call',runtime_attach='not-required',reentrant=true}]\n"
    "failure='none'\n"
    "allocation='none'\n"
    "blocking='never'\n"
    "suspend='never'\n"
    "io='none'\n"
    "sync='none'\n"
    "panic='never'\n"
    "error='none'\n"
    "[[native.capability]]\n"
    "type='Host'\n"
    "request='share'\n"
    "attestation='test'\n"
    "scope='local'\n"
    "[[link.symbol]]\n"
    "xray='invoke'\n"
    "section='host'\n"
    "used=true\n"
    "weak=false\n"
    "[[freestanding.entry]]\n"
    "xray='start'\n"
    "symbol='start'\n"
    "kind='start'\n"
    "section='start'\n";

static size_t contract_case(size_t failure,uint64_t work,XrManifestStatus expected,bool trace) {
    reset(failure);XrCompileResources *owner=NULL;XrCompileResourceLimits limits=unlimited;limits.work=work;
    if(xr_compile_resources_new(&limits,&owner)!=XR_COMPILE_RESOURCE_OK) { CHECK(expected==XR_MANIFEST_OUT_OF_MEMORY||expected==XR_MANIFEST_BUDGET);return attempts; }
    XrOsIoPolicy policy=xr_compile_io_policy(owner);XrTomlValue *document=NULL;XrNativePackagePlan *plan=NULL;
    recording=trace;XrTomlParseStatus parsed=xtoml_parse_owned(&policy,complete_contract,sizeof(complete_contract)-1,&parse_limits,&document);
    XrManifestStatus status=parsed==XR_TOML_PARSE_OUT_OF_MEMORY?XR_MANIFEST_OUT_OF_MEMORY:parsed==XR_TOML_PARSE_BUDGET?XR_MANIFEST_BUDGET:
        parsed==XR_TOML_PARSE_OK?xr_native_package_plan_parse_owned(&policy,document,root_path,&plan,NULL):XR_MANIFEST_INVALID;
    const XrNativeSymbol *symbol=NULL;const XrFreestandingEntryPlan *entry=NULL;const XrLinkSymbolPlan *link=NULL;const XrNativeUnit *unit=NULL;
    if(status==XR_MANIFEST_OK)status=xr_native_package_find_symbol_owned(plan,"invoke",&symbol);
    if(status==XR_MANIFEST_OK)status=xr_native_package_find_entry_owned(plan,"start",&entry);
    if(status==XR_MANIFEST_OK)status=xr_native_package_find_link_symbol_owned(plan,"invoke",&link);
    if(status==XR_MANIFEST_OK)status=xr_native_package_find_unit_owned(plan,"platform",&unit);
    if(status==XR_MANIFEST_OK)status=xr_native_package_validate_symbol_arity_owned(plan,"invoke",1,NULL);
    recording=false;CHECK(status==expected);
    if(status==XR_MANIFEST_OK)CHECK(symbol&&symbol->contract.complete&&symbol->contract.callback_count==1&&entry&&link&&unit&&plan->capability_count==1&&!plan->capabilities[0].verified);
    xr_native_package_plan_free_owned(plan);xtoml_owned_free(document);xr_compile_resources_release(owner);CHECK(!physical_live&&!block_count);return attempts;
}
static void contract_overflow_rejections(void) {
    const char *values[]={"4294967296","-1"};
    for(size_t i=0;i<2;++i) {
        reset(SIZE_MAX);XrCompileResources *owner=NULL;CHECK(xr_compile_resources_new(&unlimited,&owner)==XR_COMPILE_RESOURCE_OK);
        XrOsIoPolicy policy=xr_compile_io_policy(owner);char input[4096];const char *index=strstr(complete_contract,"index=0");CHECK(index);
        size_t prefix=(size_t)(index-complete_contract);memcpy(input,complete_contract,prefix);
        CHECK(snprintf(input+prefix,sizeof(input)-prefix,"index=%s%s",values[i],index+7)>0);
        XrTomlValue *document=NULL;XrNativePackagePlan *plan=NULL;
        CHECK(xtoml_parse_owned(&policy,input,strlen(input),&parse_limits,&document)==XR_TOML_PARSE_OK);
        CHECK(xr_native_package_plan_parse_owned(&policy,document,root_path,&plan,NULL)==XR_MANIFEST_INVALID&&!plan);
        xtoml_owned_free(document);xr_compile_resources_release(owner);CHECK(!physical_live&&!block_count);
    }
}
static void contract_gates(void) {
    boundary_count=0;size_t sites=contract_case(SIZE_MAX,UINT64_MAX,XR_MANIFEST_OK,true),cuts=boundary_count;
    for(size_t i=0;i<sites;++i)CHECK(contract_case(i,UINT64_MAX,XR_MANIFEST_OUT_OF_MEMORY,false)==i+1);
    for(size_t i=0;i<cuts;++i)contract_case(SIZE_MAX,boundaries[i]-1,XR_MANIFEST_BUDGET,false);
    printf("typed parameter/return/callback, capability, link and entry: %zu OOM, %zu work cutoffs PASS\n",sites,cuts);fflush(stdout);
}

typedef struct FaultPolicy { XrCompileResources *owner; XrOsIoStatus failure; bool fail_alloc,fail_work; } FaultPolicy;
static XrOsIoStatus fault_alloc(void *opaque,size_t bytes,void **out) {
    FaultPolicy *fault=opaque;if(fault->fail_alloc)return fault->failure;
    XrOsIoPolicy policy=xr_compile_io_policy(fault->owner);return policy.alloc(policy.context,bytes,out);
}
static void fault_free(void *opaque,void *memory) {(void)opaque;xr_compile_resources_free(memory);}
static XrOsIoStatus fault_work(void *opaque,uint64_t units) {
    FaultPolicy *fault=opaque;if(fault->fail_work)return fault->failure;
    XrOsIoPolicy policy=xr_compile_io_policy(fault->owner);return policy.work(policy.context,units);
}
static void typed_failures(void) {
    reset(SIZE_MAX);XrCompileResources *owner=NULL;CHECK(xr_compile_resources_new(&unlimited,&owner)==XR_COMPILE_RESOURCE_OK);
    FaultPolicy fault={owner,XR_OS_IO_OK,false,false};XrOsIoPolicy policy={&fault,fault_alloc,fault_free,fault_work};
    const char text[]="[[export.c]]\nxray='answer'\nsymbol='answer'\n";XrTomlValue *document=NULL;
    CHECK(xtoml_parse_owned(&policy,text,sizeof(text)-1,&parse_limits,&document)==XR_TOML_PARSE_OK);
    XrOsIoStatus failures[]={XR_OS_IO_IO,XR_OS_IO_OUT_OF_MEMORY,XR_OS_IO_BUDGET};
    XrManifestStatus expected[]={XR_MANIFEST_IO,XR_MANIFEST_OUT_OF_MEMORY,XR_MANIFEST_BUDGET};
    for(unsigned i=0;i<3;++i)for(unsigned mode=0;mode<2;++mode) {
        fault.failure=failures[i];fault.fail_alloc=mode==0;fault.fail_work=mode==1;
        XrProject *project=NULL;XrNativePackagePlan *plan=NULL;size_t live=physical_live;
        CHECK(xr_project_load_owned(&policy,root_path,&parse_limits,&project,NULL)==expected[i]&&!project);
        CHECK(xr_native_package_plan_parse_owned(&policy,document,root_path,&plan,NULL)==expected[i]&&!plan);
        CHECK(physical_live==live);
    }
    fault.fail_alloc=fault.fail_work=false;xtoml_owned_free(document);xr_compile_resources_release(owner);CHECK(!physical_live);
    puts("allocation/work policy IO, OOM and BUDGET preserve first cause/output/live PASS");
}
static void unc_gates(const XrOsIoPolicy *policy,const XrXirCompileContext *context) {
    char root[4096],entry[4096],share[64],manifest[80];
    CHECK(snprintf(root,sizeof(root),"\\\\localhost\\%c$%s",root_path[0],root_path+2)>0);
    DWORD attributes=GetFileAttributesA(root);
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        printf("UNC administrative share NOT_RUN: Win32 error %lu\n",(unsigned long)GetLastError());return;
    }
    XrProject *project=NULL;XrCliGraphAuthority *authority=NULL;
    CHECK(xr_project_load_owned(policy,root,&parse_limits,&project,NULL)==XR_MANIFEST_OK&&project->native_plan);
    CHECK(xr_project_uses_policy(project,policy));xr_project_free_owned(project);
    CHECK(snprintf(entry,sizeof(entry),"%s\\sub\\inner.xr",root)>0);
    CHECK(xr_cli_compile_graph_authority_open(context,entry,NULL,&parse_limits,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_entry(authority)->kind==XR_MODULE_IDENTITY_PROJECT);
    xr_cli_compile_graph_authority_close(authority);
    CHECK(snprintf(share,sizeof(share),"\\\\localhost\\%c$\\",root_path[0])>0);
    CHECK(snprintf(manifest,sizeof(manifest),"%sxray.toml",share)>0);
    attributes=GetFileAttributesA(manifest);char *found=NULL;
    XrOsIoStatus status=xr_cli_find_project_root_owned(policy,share,&found);
    CHECK(status==(attributes==INVALID_FILE_ATTRIBUTES?XR_OS_IO_NOT_FOUND:XR_OS_IO_OK));
    if(found)policy->free(policy->context,found);
    puts("actual UNC project/native/GraphAuthority and share-root traversal PASS");
}
static void path_and_schema_gates(const char *script_path) {
    reset(SIZE_MAX);XrCompileResources *owner=NULL;CHECK(xr_compile_resources_new(&unlimited,&owner)==XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy=xr_compile_io_policy(owner);XrXirCompileContext context={owner,xr_xir_compile_default_limits()};
    XrCliGraphAuthority *graph=NULL;XrProject *project=NULL;char *found=NULL;char path[4096];
    CHECK(xr_cli_compile_graph_authority_open(&context,"relative.xr",NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!graph);
    CHECK(xr_cli_compile_graph_authority_open(&context,"/drive-relative.xr",NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!graph);
    CHECK(xr_project_load_owned(&policy,"/drive-relative",&parse_limits,&project,NULL)==XR_MANIFEST_BAD_ARGUMENT&&!project);
    CHECK(xr_cli_find_project_root_owned(&policy,".",&found)==XR_OS_IO_BAD_ARGUMENT&&!found);
    char drive[]={root_path[0],':','\\',0},root_manifest[]={root_path[0],':','\\','x','r','a','y','.','t','o','m','l',0};
    DWORD attr=GetFileAttributesA(root_manifest);XrOsIoStatus drive_status=xr_cli_find_project_root_owned(&policy,drive,&found);
    CHECK(drive_status==(attr==INVALID_FILE_ATTRIBUTES?XR_OS_IO_NOT_FOUND:XR_OS_IO_OK));
    if(found){CHECK(!strcmp(found,drive));xr_compile_resources_free(found);found=NULL;}
    CHECK(xr_cli_find_project_root_owned(&policy,entry_path,&found)==XR_OS_IO_BAD_ARGUMENT&&!found);
    snprintf(path,sizeof(path),"%s\\absent.xr",root_path);
    CHECK(xr_cli_compile_graph_authority_open(&context,path,NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_NOT_FOUND&&!graph);
    CHECK(xr_cli_compile_graph_authority_open(&context,script_path,NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_OK);
    CHECK(!xr_cli_compile_graph_authority_project(graph)&&xr_cli_compile_graph_authority_entry(graph)->kind==XR_MODULE_IDENTITY_SCRIPT);
    xr_cli_compile_graph_authority_close(graph);graph=NULL;
    snprintf(path,sizeof(path),"%s\\bad\\main.xr",root_path);
    CHECK(xr_cli_compile_graph_authority_open(&context,path,NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_INVALID&&!graph);
    snprintf(path,sizeof(path),"%s\\package",root_path);
    CHECK(xr_project_load_owned(&policy,path,&parse_limits,&project,NULL)==XR_MANIFEST_OK);
    char package_entry[4096];CHECK(snprintf(package_entry,sizeof(package_entry),"%s\\main.xr",path)>0);
    CHECK(xr_cli_compile_graph_authority_open(&context,package_entry,NULL,&parse_limits,&graph,NULL)==XR_MANIFEST_OK);
    CHECK(xr_cli_compile_graph_authority_entry(graph)->kind==XR_MODULE_IDENTITY_PACKAGE&&!xr_cli_compile_graph_authority_lockfile(graph));
    xr_cli_compile_graph_authority_close(graph);graph=NULL;
    XrProjectAuthority *authority=NULL;CHECK(xr_project_authority_build_owned(project,&authority,NULL)==XR_MANIFEST_OK);
    CHECK(strcmp(xr_project_authority_view(authority)->namespace_id,"owner/package@1.2.3")==0);
    xr_project_authority_free_owned(authority);xr_project_free_owned(project);project=NULL;authority=NULL;
    snprintf(path,sizeof(path),"%s\\badversion",root_path);
    CHECK(xr_project_load_owned(&policy,path,&parse_limits,&project,NULL)==XR_MANIFEST_OK);
    CHECK(xr_project_authority_build_owned(project,&authority,NULL)==XR_MANIFEST_INVALID&&!authority);xr_project_free_owned(project);project=NULL;
    snprintf(path,sizeof(path),"%s\\sub",root_path);
    CHECK(xr_project_load_owned(&policy,path,&parse_limits,&project,NULL)==XR_MANIFEST_NOT_FOUND&&!project);
    XrTomlParseLimits tiny={4,64};
    CHECK(xr_project_load_owned(&policy,root_path,&tiny,&project,NULL)==XR_MANIFEST_LIMIT&&!project);
    CHECK(xr_project_load_owned(&policy,root_path,&parse_limits,&project,NULL)==XR_MANIFEST_OK);
    FILE *file=tmpfile();CHECK(file&&xr_native_package_explain_owned(project->native_plan,file)==XR_MANIFEST_OK);
    rewind(file);char prefix[64]={0};CHECK(fread(prefix,1,12,file)==12&&!memcmp(prefix,"native-plan ",12));fclose(file);
    snprintf(path,sizeof(path),"%s\\owner.c",root_path);file=fopen(path,"rb");CHECK(file);
    CHECK(xr_native_package_explain_owned(project->native_plan,file)==XR_MANIFEST_IO);fclose(file);
    CHECK(xr_native_package_explain_owned(project->native_plan,NULL)==XR_MANIFEST_BAD_ARGUMENT);
    XrProject *canary=project;
    CHECK(xr_project_load_owned(&policy,root_path,&parse_limits,&canary,NULL)==XR_MANIFEST_BAD_ARGUMENT&&canary==project);
    xr_project_free_owned(project);unc_gates(&policy,&context);xr_compile_resources_release(owner);CHECK(!physical_live&&!block_count);
    puts("absolute input, script/project authority, invalid nearer manifest, exact package version, limits and FILE IO PASS");
}
#include "entry_text_cases.h"
int main(int argc, char **argv) {
    CHECK(argc>=3&&strlen(argv[1])<sizeof(root_path));strcpy(root_path,argv[1]);
    CHECK(snprintf(entry_path,sizeof(entry_path),"%s\\sub\\inner.xr",root_path)>0);
    DWORD handles_before=0,handles_after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    if(argc==3){qualify();mutation_gates();}
    entry_text_failure_matrix();entry_text_boundaries(argv[2]);
    ownership_rejections();reject_gates();contract_overflow_rejections();contract_gates();typed_failures();path_and_schema_gates(argv[2]);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after)&&handles_before==handles_after);
    puts("all fixture Windows handle delta zero PASS");return 0;
}
