/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_class_storage.c - Real class factory and member failure boundaries
 */
#include "frontend/analyzer/xanalyzer_symbol.h"
#include "toolchain/xcompiler_session.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool record_work;
static uint64_t work_bounds[4096];
static size_t work_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (record_work && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current = {0};
        CHECK(xr_compile_resources_stats(resources, &current) == XR_COMPILE_RESOURCE_OK);
        CHECK(work_count < sizeof(work_bounds) / sizeof(work_bounds[0]));
        work_bounds[work_count++] = current.work;
    }
    return status;
}
static const XrCompileResourceLimits ample = {1048576,1048576,1048576};
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
static XrCompileResourceStatus session_status(XrCompilerSessionStatus status) {
    switch (status) {
        case XR_COMPILER_SESSION_OK: return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT: return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET: return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false);return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}
static XaSymbol **members(const XrClassInfo *info, unsigned category) {
    return category==0 ? info->fields : category==1 ? info->methods :
        category==2 ? info->static_fields : info->static_methods;
}
static int member_count(const XrClassInfo *info, unsigned category) {
    return category==0 ? info->field_count : category==1 ? info->method_count :
        category==2 ? info->static_field_count : info->static_method_count;
}
static XrCompileResourceStatus add(XrCompileState *state, XrClassInfo *info, XaSymbol *symbol, unsigned category) {
    return category&1 ? xa_class_info_add_method(state,info,symbol) : xa_class_info_add_field(state,info,symbol);
}
static void sticky_retry(XrCompileResources *resources, XrCompileState *state,
    XrClassInfo *info, XrCompileResourceStatus status) {
    XrCompileResourceStats before=stats(resources);size_t attempts=attempt_count;
    if (info) {
        CHECK(xa_class_info_add_field(state,info,(XaSymbol *)(uintptr_t)1)==status);
        CHECK(xa_class_info_add_method(state,info,(XaSymbol *)(uintptr_t)1)==status);
    }
    CHECK(xa_class_info_add_method(state,(XrClassInfo *)(uintptr_t)1,(XaSymbol *)(uintptr_t)1)==status);
    CHECK(!xa_class_info_new(state,(const char *)(uintptr_t)1));
    XrCompileResourceStats after=stats(resources);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes &&
        before.live_bytes==after.live_bytes && attempts==attempt_count);
}
static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits,
    XrCompileResourceStats *observed, bool use_peer) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XrCompileState *state=NULL;
    XrCompileState *request=NULL,*peer=NULL;
    XrClassInfo *info=NULL;XaSymbol symbols[4][13]={0};char names[4][13][24];
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&resources);
    if (status==XR_COMPILE_RESOURCE_OK) status=session_status(xr_compile_session_new(resources,&session));
    if (status==XR_COMPILE_RESOURCE_OK) {
        state=xr_compile_session_compile_state(session);
        request=state;
        char name[]="Widget";info=xa_class_info_new(state,name);
        if (!info) status=xr_compile_state_status(state);
        else { memset(name,'!',sizeof(name)-1);CHECK(!strcmp(info->name,"Widget")); }
    }
    if (status==XR_COMPILE_RESOURCE_OK && use_peer) {
        status=xr_compile_state_new(resources,&peer);request=peer;
    }
    for (unsigned c=0;c<4 && status==XR_COMPILE_RESOURCE_OK;++c) {
        for (int i=0;i<13 && status==XR_COMPILE_RESOURCE_OK;++i) {
            snprintf(names[c][i],sizeof(names[c][i]),"member_%u_%d",c,i);
            symbols[c][i].name=names[c][i];symbols[c][i].is_static=c>=2;
            XaSymbol **old[4];int counts[4];
            for (unsigned j=0;j<4;++j) { old[j]=members(info,j);counts[j]=member_count(info,j); }
            status=add(request,info,&symbols[c][i],c);
            for (unsigned j=0;j<4;++j) {
                int expected=counts[j]+(status==XR_COMPILE_RESOURCE_OK && c==j ? 1 : 0);
                CHECK(member_count(info,j)==expected);
                if (status!=XR_COMPILE_RESOURCE_OK || c!=j) CHECK(members(info,j)==old[j]);
                for (int k=0;k<expected;++k) CHECK(members(info,j)[k]==&symbols[j][k]);
            }
        }
    }
    if (state && status!=XR_COMPILE_RESOURCE_OK && request) {
        CHECK(xr_compile_state_status(state)==status);
        CHECK(xr_compile_state_status(request)==status);sticky_retry(resources,request,info,status);
    } else if (state && status!=XR_COMPILE_RESOURCE_OK) {
        CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OK && info);
        for (unsigned c=0;c<4;++c) CHECK(!members(info,c) && !member_count(info,c));
    }
    if (resources) *observed=stats(resources);
    xr_compile_state_release(peer);
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    if (info) CHECK(!strcmp(info->name,"Widget"));
    xa_class_info_free(info);
    CHECK(!physical_live && !allocation_count);
    return status;
}
static void allocation_and_budget_boundaries(void) {
    for (unsigned peer=0;peer<2;++peer) {
    reset();XrCompileResourceStats baseline={0};record_work=true;work_count=0;
    CHECK(pipeline(&ample,&baseline,peer!=0)==XR_COMPILE_RESOURCE_OK);
    record_work=false;size_t sites=attempt_count,cuts=work_count;
    CHECK(sites && cuts && baseline.allocated_bytes && baseline.peak_bytes && baseline.work);
    for (size_t point=0;point<sites;++point) {
        reset();fail_at=point;XrCompileResourceStats measured={0};
        CHECK(pipeline(&ample,&measured,peer!=0)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    XrCompileResourceLimits exact={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
    for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits=exact;
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(uint64_t)((int64_t)*bound+delta);reset();XrCompileResourceStats measured={0};
        CHECK(pipeline(&limits,&measured,peer!=0)==(delta<0?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
    }
    for (size_t i=0;i<cuts;++i) {
        XrCompileResourceLimits limits=ample;limits.work=work_bounds[i]-1;
        reset();XrCompileResourceStats measured={0};
        CHECK(pipeline(&limits,&measured,peer!=0)==XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("actual class identity/four member arrays peer%u: %zu allocation faults, %zu actual work cutoffs; "
        "all three exact/minus1/plus1 axes, atomic failed growth, no-charge sticky retry, physical0 PASS\n",peer,sites,cuts);
    }
}

static void requesting_state_guards(void) {
    reset();XrCompileResources *resources=NULL,*foreign_resources=NULL;
    XrCompileState *owner=NULL,*peer=NULL,*foreign=NULL;
    CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&ample,&foreign_resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(resources,&owner)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(resources,&peer)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(foreign_resources,&foreign)==XR_COMPILE_RESOURCE_OK);
    XrClassInfo *info=xa_class_info_new(owner,"Guarded");CHECK(info);
    XaSymbol field={0};field.name="value";
    XrCompileResourceStats before=stats(resources),other=stats(foreign_resources);
    CHECK(xa_class_info_add_field(foreign,info,&field)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileResourceStats after=stats(resources),other_after=stats(foreign_resources);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
    CHECK(other.work==other_after.work && other.allocated_bytes==other_after.allocated_bytes && other.live_bytes==other_after.live_bytes);
    CHECK(xr_compile_state_status(owner)==XR_COMPILE_RESOURCE_OK && !info->fields && !info->field_count);
    CHECK(xr_compile_state_work(peer,ample.work+1)==XR_COMPILE_RESOURCE_BUDGET);
    sticky_retry(resources,peer,info,XR_COMPILE_RESOURCE_BUDGET);
    CHECK(xr_compile_state_status(owner)==XR_COMPILE_RESOURCE_OK && !info->fields && !info->field_count);
    CHECK(xa_class_info_add_field(owner,info,&field)==XR_COMPILE_RESOURCE_OK);
    CHECK(info->field_count==1 && info->fields[0]==&field);
    xa_class_info_free(info);
    xr_compile_state_release(owner);xr_compile_state_release(peer);xr_compile_state_release(foreign);
    xr_compile_resources_release(resources);xr_compile_resources_release(foreign_resources);
    CHECK(!physical_live && !allocation_count);
    puts("foreign-ledger request rejected without charge; failed same-ledger requester preserves healthy owner PASS");
}
static void producer_death_and_rejections(void) {
    for (unsigned rejection=0;rejection<4;++rejection) {
        reset();XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
        CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        XrCompileState *state=xr_compile_session_compile_state(session);
        if (rejection<2) {
            CHECK(!xa_class_info_new(state,rejection?"":NULL));
            CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        } else {
            XrClassInfo *info=xa_class_info_new(state,"Detached");CHECK(info);
            XaSymbol symbols[5]={0};char names[5][8];
            for (int i=0;i<4;++i) {
                snprintf(names[i],sizeof(names[i]),"field%d",i);symbols[i].name=names[i];
                CHECK(xa_class_info_add_field(state,info,&symbols[i])==XR_COMPILE_RESOURCE_OK);
            }
            XaSymbol **original=info->fields;
            if (rejection==2) {
                info->field_count=INT_MAX;
                CHECK(xa_class_info_add_field(state,info,&symbols[0])==XR_COMPILE_RESOURCE_BUDGET);
                CHECK(info->fields==original && info->field_count==INT_MAX);
                info->field_count=4;
                sticky_retry(resources,state,info,XR_COMPILE_RESOURCE_BUDGET);
            } else {
                xr_compile_session_free(session);session=NULL;
                xr_compile_resources_release(resources);resources=NULL;
                strcpy(names[4],"last");symbols[4].name=names[4];
                CHECK(xa_class_info_add_field(state,info,&symbols[4])==XR_COMPILE_RESOURCE_OK);
                CHECK(info->field_count==5 && !strcmp(info->name,"Detached"));
                for (int i=0;i<5;++i) CHECK(info->fields[i]==&symbols[i]);
                CHECK(xa_class_info_add_method(state,info,NULL)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
                CHECK(info->method_count==0 && !info->methods);
            }
            xa_class_info_free(info);
        }
        xr_compile_session_free(session);xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    CHECK(xa_class_info_add_field(NULL,NULL,NULL)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    xa_class_info_free(NULL);
    puts("class identity deep copy, all four arrays, overflow/invalid rejection and genuine post-Session/ledger growth PASS");
}
int main(void) {
    allocation_and_budget_boundaries();producer_death_and_rejections();requesting_state_guards();return 0;
}
