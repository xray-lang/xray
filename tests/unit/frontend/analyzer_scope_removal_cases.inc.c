/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * analyzer_scope_removal_cases.inc.c - Owned declaration removal and key lifetime
 */
static XrCompileResourceStatus removal_query(XaScope *scope,const char *name,
    const char **key,void **value) {
    return capture_io(scope->registry,xr_hashmap_owned_get_pair(scope->symbols,name,key,value));
}
static XrCompileResourceStatus detach_checked(XaScope *scope,XaSymbol *symbol) {
    MapSnapshot map=snapshot(scope->symbols);XaSymbol *head=scope->owned_symbols;
    XaSymbol *previous=symbol->scope_owned_previous,*next=symbol->scope_owned_next;
    uint32_t id=symbol->id;XaSymbol *slot=scope->registry->slots[id];
    XrCompileResourceStatus status=xa_scope_remove_owned_symbol(scope,symbol);
    if (status!=XR_COMPILE_RESOURCE_OK) {
        unchanged(scope->symbols,&map);CHECK(scope->owned_symbols==head && symbol->scope==scope);
        CHECK(symbol->scope_owned_previous==previous && symbol->scope_owned_next==next);
        CHECK(!previous || previous->scope_owned_next==symbol);
        CHECK(!next || next->scope_owned_previous==symbol);
        CHECK(scope->registry->slots[id]==slot);
    } else CHECK(!symbol->scope && !symbol->scope_owned_next && !symbol->scope_owned_previous && !scope->registry->slots[id]);
    return status;
}
static XrCompileResourceStatus removal_pipeline(const XrCompileResourceLimits *limits,
    bool producer_dead,XrCompileResourceStats *observed) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XaSymbolRegistry *registry=NULL;
    XaScope *root=NULL,*child=NULL;XaSymbol *symbols[6]={0};bool root_released=false,registry_released=false;
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&resources);
    if (status==XR_COMPILE_RESOURCE_OK) status=open_session(resources,&session);
    XrCompileState *state=session?xr_compile_session_compile_state(session):NULL;
    if (status==XR_COMPILE_RESOURCE_OK) status=open_registry_with_pool(state,&registry);
    if (status==XR_COMPILE_RESOURCE_OK) { root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL);if (!root) status=xr_compile_state_status(state); }
    if (status==XR_COMPILE_RESOURCE_OK) { child=xa_scope_new(registry,XA_SCOPE_BLOCK,root);if (!child) status=xr_compile_state_status(state); }
    const char *names[] = {"alpha","beta","alpha","gamma","gamma","nested"};
    for (unsigned i=0;i<6 && status==XR_COMPILE_RESOURCE_OK;++i) {
        symbols[i]=xa_symbol_new(registry,names[i],XA_SYM_VARIABLE);
        if (!symbols[i]) { status=xr_compile_state_status(state);break; }
        CHECK(symbols[i]->id==i+1);
        status=xa_scope_add_symbol(i==5?child:root,symbols[i]);
        if (status==XR_COMPILE_RESOURCE_OK && (i==0 || i==1 || i==3 || i==5)) {
            symbols[i]->links.class_info=xa_class_info_new(state,names[i]);
            symbols[i]->links.owns_class_info=true;
            if (!symbols[i]->links.class_info) status=xr_compile_state_status(state);
            else if (i) symbols[i]->links.class_info->base=symbols[0]->links.class_info;
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK && !xa_scope_set_alias(child,"historic",symbols[0])) status=xr_compile_state_status(state);
    const char *key=NULL;void *value=NULL;
    if (status==XR_COMPILE_RESOURCE_OK) {
        status=removal_query(root,"alpha",&key,&value);
        if (status==XR_COMPILE_RESOURCE_OK)
            CHECK(value==symbols[2] && key!=symbols[0]->name && key!=symbols[2]->name && !strcmp(key,"alpha"));
    }
    if (status==XR_COMPILE_RESOURCE_OK && producer_dead) {
        xr_compile_session_free(session);session=NULL;
        xr_compile_resources_release(resources);root_released=true;
        xa_symbol_registry_release(registry);registry_released=true;
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        XrClassInfo *target=symbols[0]->links.class_info;
        status=xa_scope_clear_class_base_references(root,target);
        if (status==XR_COMPILE_RESOURCE_OK)
            CHECK(!symbols[1]->links.class_info->base && !symbols[3]->links.class_info->base && !symbols[5]->links.class_info->base);
        else CHECK(symbols[0]->links.class_info==target && symbols[0]->scope==root);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        status=detach_checked(root,symbols[0]);
        if (status==XR_COMPILE_RESOURCE_OK) {
            XaSymbol *retired=symbols[0];
            xa_class_info_free(retired->links.class_info);retired->links.class_info=NULL;retired->links.owns_class_info=false;
            xa_symbol_free(retired);symbols[0]=NULL;
            CHECK(!registry->slots[1]);
            XaSymbol *alias=xa_scope_lookup_local(child,"historic");status=xr_compile_state_status(state);
            if (status==XR_COMPILE_RESOURCE_OK) CHECK(alias==retired && !alias->links.class_info && !strcmp(alias->name,"alpha"));
            if (status==XR_COMPILE_RESOURCE_OK) {
                const char *after=NULL;status=removal_query(root,"alpha",&after,&value);
                if (status==XR_COMPILE_RESOURCE_OK) CHECK(after==key && value==symbols[2] && !strcmp(after,"alpha"));
            }
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        status=detach_checked(root,symbols[3]);
        if (status==XR_COMPILE_RESOURCE_OK) { xa_symbol_free(symbols[3]);symbols[3]=NULL; }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        XaSymbol *active=xa_scope_lookup_local(root,"gamma");status=xr_compile_state_status(state);
        if (status==XR_COMPILE_RESOURCE_OK) CHECK(active==symbols[4]);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        MapSnapshot old=snapshot(root->symbols);
        if (!xa_scope_remove_symbol(root,"gamma")) { status=xr_compile_state_status(state);unchanged(root->symbols,&old);CHECK(symbols[4]->scope==root); }
        else { CHECK(!symbols[4]->scope);xa_symbol_free(symbols[4]);symbols[4]=NULL; }
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        int count=-17;XaSymbol **all=xa_scope_get_all_symbols(root,&count);status=xr_compile_state_status(state);
        if (status==XR_COMPILE_RESOURCE_OK) {
            CHECK(all && count==2 && root->owned_symbols==symbols[2] && symbols[2]->scope_owned_next==symbols[1]);
            CHECK(!symbols[2]->scope_owned_previous && symbols[1]->scope_owned_previous==symbols[2]);
            CHECK((all[0]==symbols[1] && all[1]==symbols[2]) || (all[0]==symbols[2] && all[1]==symbols[1]));
        } else CHECK(!all && count==-17);
        xr_compile_state_free(all);
    }
    if (status==XR_COMPILE_RESOURCE_OK) {
        XaSymbol *gone=xa_symbol_registry_lookup(registry,1);status=xr_compile_state_status(state);
        if (status==XR_COMPILE_RESOURCE_OK) CHECK(!gone);
    }
    if (root && status!=XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats before=stats(resources);size_t attempts=attempt_count;
        CHECK(xa_scope_remove_owned_symbol(root,(XaSymbol *)(uintptr_t)1)==status);
        CHECK(xa_scope_clear_class_base_references(root,(XrClassInfo *)(uintptr_t)1)==status);
        XrCompileResourceStats after=stats(resources);
        CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && attempts==attempt_count);
    }
    if (resources) *observed=stats(resources);
    xr_compile_session_free(session);if (!root_released) xr_compile_resources_release(resources);
    if (!registry_released) xa_symbol_registry_release(registry);
    for (unsigned i=0;i<6;++i) if (symbols[i] && !symbols[i]->scope) xa_symbol_free(symbols[i]);
    xa_scope_free(root);CHECK(!physical_live && !allocation_count);return status;
}
static void removal_resources(void) {
    for (unsigned dead=0;dead<2;++dead) {
        reset();work_count=0;record_work=true;XrCompileResourceStats baseline={0};
        CHECK(removal_pipeline(&ample,dead!=0,&baseline)==XR_COMPILE_RESOURCE_OK);record_work=false;
        size_t sites=attempt_count,cuts=work_count;CHECK(sites && cuts);
        for (size_t i=0;i<sites;++i) { reset();fail_at=i;XrCompileResourceStats observed={0};CHECK(removal_pipeline(&ample,dead!=0,&observed)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY); }
        for (size_t i=0;i<cuts;++i) { reset();XrCompileResourceLimits limits=ample;limits.work=work_bounds[i]-1;XrCompileResourceStats observed={0};CHECK(removal_pipeline(&limits,dead!=0,&observed)==XR_COMPILE_RESOURCE_BUDGET); }
        XrCompileResourceLimits exact={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
        for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits=exact;uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            *bound=(uint64_t)((int64_t)*bound+delta);reset();XrCompileResourceStats observed={0};
            CHECK(removal_pipeline(&limits,dead!=0,&observed)==(delta<0?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
        }
        printf("owned hidden removal dead%u: %zu allocation faults/%zu actual work cuts, independent keys/links/IDs/current binding, real hidden/child base clear, aliases and physical0 PASS\n",dead,sites,cuts);
    }
}
static XrCompileResourceStatus deep_base_case(const XrCompileResourceLimits *limits,
    size_t scratch_failure,bool trace,XrCompileResourceStats *observed,size_t *scratch_sites) {
    reset();XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XaSymbolRegistry *registry=NULL;
    CHECK(xr_compile_resources_new(limits,&resources)==XR_COMPILE_RESOURCE_OK && open_session(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrCompileState *state=xr_compile_session_compile_state(session);CHECK(open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
    XaScope *root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL),*leaf=root;CHECK(root);
    for (unsigned i=0;i<128;++i) { leaf=xa_scope_new(registry,XA_SCOPE_BLOCK,leaf);CHECK(leaf); }
    XaSymbol *base=xa_symbol_new(registry,"Base",XA_SYM_CLASS),*hidden=xa_symbol_new(registry,"Derived",XA_SYM_CLASS),*active=xa_symbol_new(registry,"Derived",XA_SYM_CLASS);
    CHECK(base && hidden && active && xa_scope_add_symbol(root,base)==XR_COMPILE_RESOURCE_OK);
    CHECK(xa_scope_add_symbol(leaf,hidden)==XR_COMPILE_RESOURCE_OK && xa_scope_add_symbol(leaf,active)==XR_COMPILE_RESOURCE_OK);
    base->links.class_info=xa_class_info_new(state,"Base");base->links.owns_class_info=true;
    hidden->links.class_info=xa_class_info_new(state,"Derived");hidden->links.owns_class_info=true;
    CHECK(base->links.class_info && hidden->links.class_info);hidden->links.class_info->base=base->links.class_info;
    xr_compile_session_free(session);xr_compile_resources_release(resources);xa_symbol_registry_release(registry);
    size_t begin=attempt_count;if (scratch_failure!=SIZE_MAX) fail_at=begin+scratch_failure;
    if (trace) { work_count=0;record_work=true; }
    XrCompileResourceStatus status=xa_scope_clear_class_base_references(root,base->links.class_info);
    if (trace) record_work=false;
    *scratch_sites=attempt_count-begin;*observed=stats(resources);
    if (status==XR_COMPILE_RESOURCE_OK) CHECK(!hidden->links.class_info->base && hidden->scope==leaf && active->scope==leaf);
    else CHECK(hidden->links.class_info->base==NULL || hidden->links.class_info->base==base->links.class_info);
    xa_scope_free(root);CHECK(!physical_live && !allocation_count);return status;
}
static void deep_base_resources(void) {
    XrCompileResourceStats baseline={0},observed={0};size_t sites=0,ignored=0;
    CHECK(deep_base_case(&ample,SIZE_MAX,true,&baseline,&sites)==XR_COMPILE_RESOURCE_OK);size_t cuts=work_count;CHECK(sites>1 && cuts);
    for (size_t i=0;i<sites;++i) CHECK(deep_base_case(&ample,i,false,&observed,&ignored)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    for (size_t i=0;i<cuts;++i) { XrCompileResourceLimits limits=ample;limits.work=work_bounds[i]-1;CHECK(deep_base_case(&limits,SIZE_MAX,false,&observed,&ignored)==XR_COMPILE_RESOURCE_BUDGET); }
    XrCompileResourceLimits exact={baseline.allocated_bytes,baseline.peak_bytes,baseline.work};
    for (unsigned axis=0;axis<3;++axis) for (int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits=exact;uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(deep_base_case(&limits,SIZE_MAX,false,&observed,&ignored)==(delta<0?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
    }
    printf("real base clear depth129: %zu post-setup scratch allocation faults/%zu actual clear work cuts, staged growth, producer death, allaxes and physical0 PASS\n",sites,cuts);
}

static void base_owner_guards(void) {
    for (unsigned mode=0;mode<3;++mode) {
        bool foreign=mode==1;
        reset();XrCompileResources *resources=NULL,*peer_resources=NULL;XrCompileState *state=NULL,*peer=NULL;
        XaSymbolRegistry *registry=NULL,*other=NULL;
        CHECK(xr_compile_resources_new(&ample,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_resources_new(&ample,&peer_resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK && open_registry_with_pool(state,&registry)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(peer_resources,&peer)==XR_COMPILE_RESOURCE_OK && open_registry_with_pool(peer,&other)==XR_COMPILE_RESOURCE_OK);
        XaScope *root=xa_scope_new(registry,XA_SCOPE_GLOBAL,NULL),*peer_root=xa_scope_new(other,XA_SCOPE_GLOBAL,NULL);CHECK(root && peer_root);
        XaSymbol *base=xa_symbol_new(registry,"Base",XA_SYM_CLASS),*derived=xa_symbol_new(foreign?other:registry,"Derived",XA_SYM_CLASS),*view=xa_symbol_new(registry,"View",XA_SYM_CLASS);
        CHECK(base && derived && view);
        base->links.class_info=xa_class_info_new(state,"Base");base->links.owns_class_info=true;
        derived->links.class_info=xa_class_info_new(foreign?peer:state,"Derived");derived->links.owns_class_info=true;
        CHECK(base->links.class_info && derived->links.class_info);derived->links.class_info->base=base->links.class_info;
        view->links.class_info=mode==2?base->links.class_info:derived->links.class_info;
        CHECK(xa_scope_add_symbol(root,base)==XR_COMPILE_RESOURCE_OK);
        CHECK(xa_scope_add_symbol(foreign?peer_root:root,derived)==XR_COMPILE_RESOURCE_OK);
        CHECK(xa_scope_add_symbol(root,view)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before=stats(peer_resources);
        XrCompileResourceStatus status=xa_scope_clear_class_base_references(root,base->links.class_info);
        CHECK(status==(mode?XR_COMPILE_RESOURCE_BAD_ARGUMENT:XR_COMPILE_RESOURCE_OK));
        CHECK(derived->links.class_info->base==(foreign?base->links.class_info:NULL));
        CHECK(xr_compile_state_status(peer)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats after=stats(peer_resources);
        CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
        view->links.class_info=NULL;
        derived->links.class_info->base=NULL;
        xa_scope_free(peer_root);xa_scope_free(root);xa_symbol_registry_release(other);xa_symbol_registry_release(registry);
        xr_compile_state_release(peer);xr_compile_state_release(state);xr_compile_resources_release(peer_resources);xr_compile_resources_release(resources);
        CHECK(!physical_live && !allocation_count);
    }
    puts("class base clear: local borrowed view follows actual owner; foreign descriptor/ledger and direct target view preserve ownership and refuse retirement PASS");
}
