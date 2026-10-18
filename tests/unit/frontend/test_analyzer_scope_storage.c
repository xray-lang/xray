/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_scope_storage.c - Actual scope/index publication and lifetime witnesses
 */
#include "frontend/analyzer/xanalyzer_symbol.h"
#include "toolchain/xcompiler_session.h"
#include "runtime/value/xtype_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
#include "analyzer_registry_fixture.h"
static bool record_work;
static uint64_t work_bounds[32768];static size_t work_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,uint64_t units) {
    XrCompileResourceStatus status=xr_test_actual_resources_work(resources,units);
    if (record_work && status==XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats current={0};CHECK(xr_compile_resources_stats(resources,&current)==XR_COMPILE_RESOURCE_OK);
        CHECK(work_count<sizeof(work_bounds)/sizeof(work_bounds[0]));work_bounds[work_count++]=current.work;
    }
    return status;
}
#include "frontend/analyzer/xanalyzer_scope_storage.c"
static const XrCompileResourceLimits ample={1048576,1048576,1048576};
typedef struct MapSnapshot {
    XrHashMapEntry *original;uint32_t capacity,count;XrHashMapEntry entries[256];
} MapSnapshot;
static MapSnapshot snapshot(XrHashMap *map) {
    CHECK(map && map->capacity<=256);MapSnapshot copy={0};
    copy.original=map->entries;copy.capacity=map->capacity;copy.count=map->count;
    memcpy(copy.entries,map->entries,map->capacity*sizeof(*map->entries));return copy;
}
static void unchanged(XrHashMap *map,const MapSnapshot *before) {
    CHECK(map->entries==before->original && map->capacity==before->capacity && map->count==before->count);
    CHECK(!memcmp(map->entries,before->entries,map->capacity*sizeof(*map->entries)));
}
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static XrCompileResourceStatus open_session(XrCompileResources *resources,XrCompilerSession **session) {
    switch (xr_compile_session_new(resources,session)) {
        case XR_COMPILER_SESSION_OK:return XR_COMPILE_RESOURCE_OK;
        case XR_COMPILER_SESSION_BAD_ARGUMENT:return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        case XR_COMPILER_SESSION_BUDGET:return XR_COMPILE_RESOURCE_BUDGET;
        case XR_COMPILER_SESSION_OUT_OF_MEMORY:return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    CHECK(false);return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}
static XrCompileResourceStatus pipeline(const XrCompileResourceLimits *limits,XrCompileResourceStats *observed) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XaSymbolRegistry *registry=NULL;
    XaScope *root=NULL,*children[5]={0};XaSymbol *symbols[66]={0};size_t created=0;
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&resources);
    if (status==XR_COMPILE_RESOURCE_OK) status=open_session(resources,&session);
    XrCompileState *state=session ? xr_compile_session_compile_state(session) : NULL;
    if (status==XR_COMPILE_RESOURCE_OK) status=open_registry_with_pool(state,&registry);
    if (status==XR_COMPILE_RESOURCE_OK) {
        root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL);if (!root) status=xr_compile_state_status(state);
    }
    for (unsigned i=0;i<5 && status==XR_COMPILE_RESOURCE_OK;++i) {
        XaScope **old=root->children;int count=root->child_count,capacity=root->child_capacity;
        children[i]=xa_scope_new(registry,XA_SCOPE_BLOCK,root);
        if (!children[i]) {
            status=xr_compile_state_status(state);
            CHECK(root->children==old && root->child_count==count && root->child_capacity==capacity);
        } else CHECK(root->child_count==(int)i+1);
    }
    for (unsigned i=0;i<66 && status==XR_COMPILE_RESOURCE_OK;++i) {
        uint64_t next=registry->next_id;XaSymbol **slots=registry->slots;char name[16];
        snprintf(name,sizeof(name),"symbol_%u",i);symbols[i]=xa_symbol_new(registry,name,XA_SYM_VARIABLE);
        if (!symbols[i]) {
            status=xr_compile_state_status(state);CHECK(registry->next_id==next && registry->slots==slots);break;
        }
        ++created;CHECK(symbols[i]->id==i+1 && registry->next_id==i+2 && !registry->slots[i+1]);
        memset(name,'!',sizeof(name));
        if (!i) {
            symbols[i]->links.class_info=xa_class_info_new(state,"OwnedClass");
            symbols[i]->links.owns_class_info=true;
            if (!symbols[i]->links.class_info) { status=xr_compile_state_status(state);break; }
        }
        MapSnapshot old=snapshot(root->symbols);XaSymbol *chain=root->owned_symbols;
        status=xa_scope_add_symbol(root,symbols[i]);
        if (status!=XR_COMPILE_RESOURCE_OK) {
            unchanged(root->symbols,&old);CHECK(root->owned_symbols==chain && !symbols[i]->scope && !registry->slots[i+1]);
        } else CHECK(symbols[i]->scope==root && registry->slots[i+1]==symbols[i]);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        char alias[]="previous";MapSnapshot old=snapshot(children[0]->aliases);
        if (!xa_scope_set_alias(children[0],alias,symbols[0])) {
            status=xr_compile_state_status(state);unchanged(children[0]->aliases,&old);CHECK(!children[0]->alias_owners);
        }
        memset(alias,'?',sizeof(alias)-1);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        CHECK(xa_scope_lookup_local(children[0],"previous")==symbols[0] || xr_compile_state_status(state)!=XR_COMPILE_RESOURCE_OK);
        status=xr_compile_state_status(state);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        ScopeAlias *old=children[0]->alias_owners;uint64_t refs=atomic_load(&symbol_owner(symbols[1])->references);
        if (!xa_scope_set_alias(children[0],"previous",symbols[1])) {
            status=xr_compile_state_status(state);CHECK(old->symbol==symbols[0] && atomic_load(&symbol_owner(symbols[1])->references)==refs);
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        MapSnapshot old=snapshot(children[0]->aliases);ScopeAlias *alias=children[0]->alias_owners;
        if (!xa_scope_remove_alias(children[0],"previous")) {
            status=xr_compile_state_status(state);unchanged(children[0]->aliases,&old);CHECK(children[0]->alias_owners==alias);
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        MapSnapshot old=snapshot(root->symbols);XaSymbol *chain=root->owned_symbols;
        if (!xa_scope_remove_symbol(root,"symbol_65")) {
            status=xr_compile_state_status(state);unchanged(root->symbols,&old);CHECK(root->owned_symbols==chain && registry->slots[66]==symbols[65]);
        } else {
            CHECK(!symbols[65]->scope && !registry->slots[66]);status=xa_scope_add_symbol(root,symbols[65]);
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        int count=-7;XaSymbol **all=xa_scope_get_all_symbols(root,&count);
        status=xr_compile_state_status(state);
        if (status==XR_COMPILE_RESOURCE_OK) {
            CHECK(all && count==66);bool seen[66]={0};
            for (int i=0;i<count;++i) { CHECK(all[i]->id && all[i]->id<=66);unsigned id=all[i]->id-1;CHECK(!seen[id] && all[i]==symbols[id]);seen[id]=true; }
            CHECK(xa_scope_lookup_by_id(children[0],66)==symbols[65] || xr_compile_state_status(state)!=XR_COMPILE_RESOURCE_OK);
            status=xr_compile_state_status(state);
        } else CHECK(!all && count==-7);
        xr_compile_state_free(all);
    }
    if (registry && status!=XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats before=stats(resources);size_t attempts=attempt_count;
        CHECK(!xa_symbol_new(registry,(const char *)(uintptr_t)1,XA_SYM_VARIABLE));
        if (root) {
            CHECK(xa_scope_add_symbol(root,(XaSymbol *)(uintptr_t)1)==status);
            CHECK(!xa_scope_lookup(root,(const char *)(uintptr_t)1));
            CHECK(!xa_scope_get_all_symbols(root,(int *)(uintptr_t)1));
        }
        XrCompileResourceStats after=stats(resources);
        CHECK(before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.work==after.work && attempts==attempt_count);
    }
    if (resources) *observed=stats(resources);
    xr_compile_session_free(session);xr_compile_resources_release(resources);xa_symbol_registry_release(registry);
    for (size_t i=0;i<created;++i) if (!symbols[i]->scope) xa_symbol_free(symbols[i]);
    xa_scope_free(root);CHECK(!physical_live && !allocation_count);return status;
}
static void producer_death(void) {
    reset();XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XaSymbolRegistry *registry=NULL;
    CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(open_session(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrCompileState *state=xr_compile_session_compile_state(session);
    CHECK(open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
    XaScope *scope=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL);CHECK(scope);
    XaSymbol *symbol=xa_symbol_new(registry,"retained",XA_SYM_VARIABLE);CHECK(symbol);
    char alias[]="it";CHECK(xa_scope_set_alias(scope,alias,symbol));memset(alias,'!',sizeof(alias)-1);
    xa_symbol_free(symbol);xr_compile_session_free(session);xr_compile_resources_release(resources);xa_symbol_registry_release(registry);
    XaSymbol *live=xa_scope_lookup(scope,"it");CHECK(live && !strcmp(live->name,"retained"));
    CHECK(xa_scope_remove_alias(scope,"it"));CHECK(!xa_scope_lookup(scope,"it"));
    xa_scope_free(scope);CHECK(!physical_live && !allocation_count);
    puts("actual registry/scope/alias references survive creator Symbol, Session and initial ledger/registry handles PASS");
}
static void rejection_guards(void) {
    for (unsigned mode=0;mode<5;++mode) {
        reset();XrCompileResources *resources=NULL,*other_resources=NULL;XrCompileState *state=NULL,*other=NULL;
        XaSymbolRegistry *registry=NULL,*other_registry=NULL;XaScope *root=NULL;XaSymbol *symbol=NULL;
        CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK);
        CHECK(open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
        root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL);CHECK(root);
        if (mode<2) {
            if (!mode) CHECK(xr_compile_resources_new(&ample,&other_resources)==XR_COMPILE_RESOURCE_OK);
            CHECK(xr_compile_state_new(mode?resources:other_resources,&other)==XR_COMPILE_RESOURCE_OK);
            CHECK(open_registry_with_pool(other,&other_registry)==XR_COMPILE_RESOURCE_OK);
            symbol=xa_symbol_new(other_registry,"foreign",XA_SYM_VARIABLE);CHECK(symbol);
        } else if (mode<4) { symbol=xa_symbol_new(registry,"local",XA_SYM_VARIABLE);CHECK(symbol); }
        MapSnapshot old=snapshot(root->symbols);XaSymbol *chain=root->owned_symbols;
        if (mode==2) ++symbol->id;
        if (mode==3) CHECK(xr_compile_state_work(state,ample.work+1)==XR_COMPILE_RESOURCE_BUDGET);
        if (mode==4) registry->next_id=(uint64_t)UINT32_MAX+1;
        XrCompileResourceStats before=stats(resources);size_t attempts=attempt_count;
        XrCompileResourceStatus expected=mode>=3?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        if (mode==4) CHECK(!xa_symbol_new(registry,"overflow",XA_SYM_VARIABLE));
        else CHECK(xa_scope_add_symbol(root,mode==3?(XaSymbol *)(uintptr_t)1:symbol)==expected);
        CHECK(xr_compile_state_status(state)==expected);unchanged(root->symbols,&old);CHECK(root->owned_symbols==chain);
        XrCompileResourceStats after=stats(resources);
        CHECK(before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && before.work==after.work && attempts==attempt_count);
        if (mode<2) CHECK(xr_compile_state_status(other)==XR_COMPILE_RESOURCE_OK && !symbol->scope);
        if (mode==2) --symbol->id;
        xa_symbol_free(symbol);xa_scope_free(root);xa_symbol_registry_release(registry);xa_symbol_registry_release(other_registry);
        xr_compile_state_release(state);xr_compile_state_release(other);xr_compile_resources_release(resources);xr_compile_resources_release(other_resources);
        CHECK(!physical_live && !allocation_count);
    }
    puts("actual foreign/same-ledger different registry, forged birth ID, sticky unreadable input and counter overflow reject without mutation/fees PASS");
}
static void replacement_and_deep_cleanup(void) {
    for (unsigned owned_free=0;owned_free<2;++owned_free) {
        reset();XrCompileResources *resources=NULL;XrCompileState *state=NULL;XaSymbolRegistry *registry=NULL;
        CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK);
        CHECK(open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
        XaScope *root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL);CHECK(root);
        XaSymbol *first=xa_symbol_new(registry,"shadow",XA_SYM_VARIABLE),*last=xa_symbol_new(registry,"shadow",XA_SYM_VARIABLE);
        CHECK(first && last && first->id==1 && last->id==2);
        CHECK(xa_scope_add_symbol(root,first)==XR_COMPILE_RESOURCE_OK && xa_scope_add_symbol(root,last)==XR_COMPILE_RESOURCE_OK);
        CHECK(xa_scope_lookup(root,"shadow")==last && xa_scope_lookup_by_id(root,1)==first);
        int count=0;XaSymbol **all=xa_scope_get_all_symbols(root,&count);CHECK(all && count==1 && all[0]==last);xr_compile_state_free(all);
        if (owned_free) {
            MapSnapshot old=snapshot(root->symbols);xa_symbol_free(first);
            CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);unchanged(root->symbols,&old);
            CHECK(first->scope==root && last->scope==root && !strcmp(first->name,"shadow"));
        } else {
            CHECK(xa_scope_remove_symbol(root,"shadow") && !last->scope && !registry->slots[2]);
            CHECK(!xa_scope_lookup(root,"shadow") && registry->slots[1]==first);xa_symbol_free(last);
        }
        xa_scope_free(root);xa_symbol_registry_release(registry);xr_compile_state_release(state);xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    reset();XrCompileResources *resources=NULL;XrCompileState *state=NULL;XaSymbolRegistry *registry=NULL;
    CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK && open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
    XaScope *root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL),*cursor=root;CHECK(root);
    for (unsigned i=0;i<128;++i) { cursor=xa_scope_new(registry,XA_SCOPE_BLOCK,cursor);CHECK(cursor); }
    xa_symbol_registry_release(registry);xr_compile_state_release(state);xr_compile_resources_release(resources);xa_scope_free(root);
    CHECK(!physical_live && !allocation_count);
    puts("same-name active binding/old owner preserved, owned creator free refused and iterative deep129 cleanup PASS");
}
#include "analyzer_scope_removal_cases.inc.c"

int main(int argc,char **argv) {
    bool faults=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || faults);
    reset();XrCompileResourceStats baseline={0};record_work=true;work_count=0;
    CHECK(pipeline(&ample,&baseline)==XR_COMPILE_RESOURCE_OK);record_work=false;
    size_t sites=attempt_count,cuts=work_count;CHECK(sites && cuts);
    XrCompileResourceLimits exact={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
    for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits=exact;uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(uint64_t)((int64_t)*bound+delta);reset();XrCompileResourceStats observed={0};
        CHECK(pipeline(&limits,&observed)==(delta<0?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
    }
    if (faults) {
        for (size_t i=0;i<sites;++i) { reset();fail_at=i;XrCompileResourceStats observed={0};CHECK(pipeline(&ample,&observed)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY); }
        for (size_t i=0;i<cuts;++i) { reset();XrCompileResourceLimits limits=ample;limits.work=work_bounds[i]-1;XrCompileResourceStats observed={0};CHECK(pipeline(&limits,&observed)==XR_COMPILE_RESOURCE_BUDGET); }
    }
    producer_death();rejection_guards();replacement_and_deep_cleanup();
    removal_resources();deep_base_resources();base_owner_guards();
    printf("actual scope66: observed%zu allocator sites/%zu work boundaries, swept%zu/%zu; all axes and physical0 PASS\n",sites,cuts,faults?sites:0,faults?cuts:0);return 0;
}
