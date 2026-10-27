/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_scope_storage.c - Explicit symbol births and atomic lexical bindings
 */
#include "xanalyzer_symbol.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../base/xchecks.h"
#include "../../base/xio_policy.inc.h"
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

struct XaSymbolRegistry {
    XrCompileState *state;
    XrTypePoolPin type_pool;
    _Atomic uint64_t references;
    uint64_t next_id;
    XaSymbol **slots;
    size_t capacity;
};
typedef struct SymbolOwner {
    XaSymbolRegistry *registry;
    _Atomic uint64_t references;
    uint32_t birth_id;
    bool creator_released;
    XaSymbol symbol;
} SymbolOwner;
typedef struct ScopeAlias {
    struct ScopeAlias *next, *previous;
    XaSymbol *symbol;
    char *name;
} ScopeAlias;

static XrCompileState *registry_state(XaSymbolRegistry *registry) {
    return registry ? registry->state : NULL;
}
static SymbolOwner *symbol_owner(XaSymbol *symbol) {
    return (SymbolOwner *) ((char *) symbol - offsetof(SymbolOwner, symbol));
}
static XrCompileResourceStatus invalid_registry(XaSymbolRegistry *registry) {
    return xr_compile_state_fail(registry_state(registry),XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}
static XrCompileResourceStatus capture_io(XaSymbolRegistry *registry,XrOsIoStatus status) {
    XrCompileResourceStatus failure=status==XR_OS_IO_OK ? XR_COMPILE_RESOURCE_OK :
        status==XR_OS_IO_OVERFLOW ? XR_COMPILE_RESOURCE_BUDGET :
        status==XR_OS_IO_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    return xr_compile_state_fail(registry_state(registry),failure);
}
XR_FUNC XrCompileState *xa_symbol_registry_state(XaSymbolRegistry *registry) {
    return registry_state(registry);
}
XR_FUNC uint64_t xa_symbol_registry_next_id(const XaSymbolRegistry *registry) {
    return registry ? registry->next_id : 0;
}
XR_FUNC XrCompileResourceStatus xa_symbol_registry_retain(XaSymbolRegistry *registry) {
    if (!registry) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    uint64_t current=atomic_load_explicit(&registry->references,memory_order_relaxed);
    for (;;) {
        if (current==UINT64_MAX) return xr_compile_state_fail(registry->state,XR_COMPILE_RESOURCE_BUDGET);
        if (atomic_compare_exchange_weak_explicit(&registry->references,&current,current+1,
                memory_order_relaxed,memory_order_relaxed)) return XR_COMPILE_RESOURCE_OK;
    }
}
XR_FUNC void xa_symbol_registry_release(XaSymbolRegistry *registry) {
    if (!registry) return;
    uint64_t old=atomic_fetch_sub_explicit(&registry->references,1,memory_order_acq_rel);
    XR_CHECK(old,"Symbol registry release requires a live reference");
    if (old!=1) return;
    XrCompileState *state=registry->state;
    XrTypePoolPin pool=registry->type_pool;
    xr_compile_state_free(registry->slots);
    xr_compile_state_free(registry);
    xr_type_pool_unpin(&pool);
    xr_compile_state_release(state);
}
XR_FUNC XrCompileResourceStatus xa_symbol_registry_new(XrCompileState *state,
    XrTypePool *pool, XaSymbolRegistry **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!output || *output || !pool || pool->state != state || !pool->initialized)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XaSymbolRegistry), &memory);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    status = xr_compile_state_retain(state);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    XaSymbolRegistry *registry = memory;
    status = xr_type_pool_pin(state, pool, &registry->type_pool);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_release(state);
        xr_compile_state_free(registry);
        return status;
    }
    registry->state = state;
    registry->next_id = 1;
    atomic_init(&registry->references, 1);
    *output = registry;
    return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrTypePool *xa_symbol_registry_type_pool(XaSymbolRegistry *registry) {
    return registry ? registry->type_pool.pool : NULL;
}
XR_FUNC XaSymbol *xa_symbol_registry_lookup(XaSymbolRegistry *registry,uint32_t id) {
    if (xr_compile_state_status(registry_state(registry))!=XR_COMPILE_RESOURCE_OK) return NULL;
    if (xr_compile_state_work(registry->state,1)!=XR_COMPILE_RESOURCE_OK) return NULL;
    return id && id<registry->capacity ? registry->slots[id] : NULL;
}
static XrCompileResourceStatus reserve_birth(XaSymbolRegistry *registry,
    XaSymbol ***slots,size_t *capacity) {
    *slots=registry->slots;*capacity=registry->capacity;
    if (registry->next_id<registry->capacity) return XR_COMPILE_RESOURCE_OK;
    size_t bound=registry->capacity ? registry->capacity : 16;
    while (registry->next_id>=bound) {
        if (bound>SIZE_MAX/2) return xr_compile_state_fail(registry->state,XR_COMPILE_RESOURCE_BUDGET);
        bound*=2;
    }
    if (bound>SIZE_MAX/sizeof(XaSymbol *)) return xr_compile_state_fail(registry->state,XR_COMPILE_RESOURCE_BUDGET);
    void *memory=NULL;
    XrCompileResourceStatus status=xr_compile_state_calloc(registry->state,bound,sizeof(XaSymbol *),&memory);
    if (status==XR_COMPILE_RESOURCE_OK)
        status=xr_compile_state_copy(registry->state,memory,registry->slots,registry->capacity*sizeof(XaSymbol *));
    if (status!=XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(memory);return status; }
    *slots=memory;*capacity=bound;return status;
}
XR_FUNC XaSymbol *xa_symbol_new(XaSymbolRegistry *registry,const char *name,XaSymbolKind kind) {
    XrCompileState *state=registry_state(registry);
    if (xr_compile_state_status(state)!=XR_COMPILE_RESOURCE_OK) return NULL;
    if (!name || kind<XA_SYM_VARIABLE || kind>XA_SYM_TYPE_ALIAS) { (void) invalid_registry(registry);return NULL; }
    if (registry->next_id>UINT32_MAX) { (void) xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BUDGET);return NULL; }
    void *memory=NULL;char *copy=NULL;XaSymbol **slots=NULL;size_t capacity=0;
    XrCompileResourceStatus status=xr_compile_state_calloc(state,1,sizeof(SymbolOwner),&memory);
    if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_strdup(state,name,&copy);
    if (status==XR_COMPILE_RESOURCE_OK) status=reserve_birth(registry,&slots,&capacity);
    if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_work(state,1);
    if (status==XR_COMPILE_RESOURCE_OK) status=xa_symbol_registry_retain(registry);
    if (status!=XR_COMPILE_RESOURCE_OK) {
        if (slots!=registry->slots) xr_compile_state_free(slots);
        xr_compile_state_free(copy);xr_compile_state_free(memory);return NULL;
    }
    SymbolOwner *owner=memory;owner->registry=registry;atomic_init(&owner->references,1);
    owner->birth_id=(uint32_t)registry->next_id;
    XaSymbol *symbol=&owner->symbol;symbol->id=owner->birth_id;symbol->name=copy;symbol->kind=kind;
    symbol->is_rebindable=kind==XA_SYM_VARIABLE || kind==XA_SYM_PARAMETER;
    symbol->links.resource_state=state;
    symbol->links.binding_use=XA_BINDING_UNINITIALIZED;symbol->links.root_id=symbol->id;
    symbol->links.root_alias=XA_ROOT_UNIQUE;
    symbol->links.binding_mutability=symbol->is_rebindable ? XA_BINDING_REBINDABLE : XA_BINDING_STABLE;
    symbol->links.value_capability=symbol->is_rebindable ? XA_CAP_MUTABLE : XA_CAP_UNKNOWN;
    symbol->links.storage_domain=symbol->is_rebindable ? XR_STORAGE_EXEC_LOCAL : XR_STORAGE_DOMAIN_UNKNOWN;
    symbol->links.return_ownership.param_index=-1;
    XaSymbol **old=registry->slots;registry->slots=slots;registry->capacity=capacity;++registry->next_id;
    if (old!=slots) xr_compile_state_free(old);
    return symbol;
}
static XrCompileResourceStatus retain_symbol(XaSymbol *symbol) {
    SymbolOwner *owner=symbol_owner(symbol);
    uint64_t current=atomic_load_explicit(&owner->references,memory_order_relaxed);
    for (;;) {
        if (current==UINT64_MAX) return xr_compile_state_fail(owner->registry->state,XR_COMPILE_RESOURCE_BUDGET);
        if (atomic_compare_exchange_weak_explicit(&owner->references,&current,current+1,
                memory_order_relaxed,memory_order_relaxed)) return XR_COMPILE_RESOURCE_OK;
    }
}
static void release_symbol(XaSymbol *symbol) {
    SymbolOwner *owner=symbol_owner(symbol);
    uint64_t old=atomic_fetch_sub_explicit(&owner->references,1,memory_order_acq_rel);
    XR_CHECK(old,"Symbol release requires a live reference");
    if (old!=1) return;
    XaSymbolRegistry *registry=owner->registry;
    xa_symbol_links_dispose_contents(&symbol->links);
    xr_compile_state_free((void *)symbol->name);xr_compile_state_free(owner);
    xa_symbol_registry_release(registry);
}
static void unbind_symbol(XaSymbol *symbol) {
    SymbolOwner *owner=symbol_owner(symbol);XaSymbolRegistry *registry=owner->registry;
    if (registry->slots[owner->birth_id]==symbol) registry->slots[owner->birth_id]=NULL;
}
XR_FUNC void xa_symbol_free(XaSymbol *symbol) {
    if (!symbol) return;
    SymbolOwner *owner=symbol_owner(symbol);
    if (symbol->scope || owner->creator_released) { (void) invalid_registry(owner->registry);return; }
    owner->creator_released=true;
    unbind_symbol(symbol);release_symbol(symbol);
}
XR_FUNC XrCompileResourceStatus xa_scope_resource_status(const XaScope *scope) {
    return xr_compile_state_status(scope ? registry_state(scope->registry) : NULL);
}
static void detach_scope_cleanup(XaScope *scope) {
    XaScope *parent=scope->parent;
    if (!parent) return;
    for (int i=0;i<parent->child_count;++i) if (parent->children[i]==scope) {
        memmove(parent->children+i,parent->children+i+1,
            (size_t)(parent->child_count-i-1)*sizeof(XaScope *));--parent->child_count;break;
    }
    scope->parent=NULL;
}
static XaScope *create_scope(XaSymbolRegistry *registry,XaScopeKind kind,XaScope *parent,
    XrHashMap *index,const char *key,void *value) {
    XrCompileState *state=registry_state(registry);
    if (xr_compile_state_status(state)!=XR_COMPILE_RESOURCE_OK) return NULL;
    XrOsIoPolicy policy=xr_compile_io_policy(xr_compile_state_resources(state));
    if (kind<XA_SCOPE_GLOBAL || kind>XA_SCOPE_LOOP || (parent && parent->registry!=registry) ||
        (index && (!key || !xr_hashmap_owned_uses_policy(index,&policy)))) {
        (void) invalid_registry(registry);return NULL;
    }
    if (parent && (parent->child_count<0 || parent->child_capacity<parent->child_count ||
        (parent->child_capacity && !parent->children))) { (void) invalid_registry(registry);return NULL; }
    void *memory=NULL;XaScope **children=parent ? parent->children : NULL;
    int capacity=parent ? parent->child_capacity : 0;
    XrCompileResourceStatus status=xr_compile_state_calloc(state,1,sizeof(XaScope),&memory);
    if (status!=XR_COMPILE_RESOURCE_OK) return NULL;
    XaScope *scope=memory;
    status=capture_io(registry,xr_hashmap_owned_new(&policy,(XrHashMap **)&scope->symbols));
    if (status==XR_COMPILE_RESOURCE_OK)
        status=capture_io(registry,xr_hashmap_owned_new(&policy,(XrHashMap **)&scope->aliases));
    bool grow=parent && parent->child_count==parent->child_capacity;
    if (grow && status==XR_COMPILE_RESOURCE_OK) {
        if (capacity>INT_MAX/2) status=xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BUDGET);
        else {
            capacity=capacity ? capacity*2 : 4;void *staged=NULL;
            status=(size_t)capacity>SIZE_MAX/sizeof(XaScope *) ?
                xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BUDGET) :
                xr_compile_state_alloc(state,(size_t)capacity*sizeof(XaScope *),&staged);
            if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_copy(state,staged,children,
                (size_t)parent->child_count*sizeof(XaScope *));
            if (status==XR_COMPILE_RESOURCE_OK) children=staged;
            else xr_compile_state_free(staged);
        }
    }
    if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_work(state,4);
    bool retained=false;
    if (status==XR_COMPILE_RESOURCE_OK) {
        status=xa_symbol_registry_retain(registry);retained=status==XR_COMPILE_RESOURCE_OK;
    }
    if (status==XR_COMPILE_RESOURCE_OK && index)
        status=capture_io(registry,xr_hashmap_owned_set(index,key,value));
    if (status!=XR_COMPILE_RESOURCE_OK) {
        if (parent && children!=parent->children) xr_compile_state_free(children);
        xr_hashmap_owned_free(scope->symbols);xr_hashmap_owned_free(scope->aliases);
        xr_compile_state_free(scope);
        if (retained) xa_symbol_registry_release(registry);
        return NULL;
    }
    scope->registry=registry;scope->kind=kind;scope->parent=parent;
    if (parent) {
        XaScope **old=parent->children;parent->children=children;parent->child_capacity=capacity;
        parent->children[parent->child_count++]=scope;if (old!=children) xr_compile_state_free(old);
    }
    return scope;
}
XR_FUNC XaScope *xa_scope_new(XaSymbolRegistry *registry,XaScopeKind kind,XaScope *parent) {
    return create_scope(registry,kind,parent,NULL,NULL,NULL);
}
XR_FUNC XaScope *xa_scope_new_indexed(XaSymbolRegistry *registry,XaScopeKind kind,
    XaScope *parent,XrHashMap *index,const char *key,void *value) {
    if (xr_compile_state_status(registry_state(registry))!=XR_COMPILE_RESOURCE_OK) return NULL;
    if (!index) { (void) invalid_registry(registry);return NULL; }
    return create_scope(registry,kind,parent,index,key,value);
}
XR_FUNC XrCompileResourceStatus xa_symbol_registry_copy_origin(XaSymbolRegistry *registry,
    const char *text,const char **output) {
    XrCompileState *state=registry_state(registry);
    XrCompileResourceStatus status=xr_compile_state_status(state);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    if (!text || !output || *output) return invalid_registry(registry);
    char *copy=xr_pool_strdup(registry->type_pool.pool,text);
    status=xr_compile_state_status(state);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    if (!copy) return xr_compile_state_fail(state,XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    status=xr_compile_state_work(state,1);
    if (status==XR_COMPILE_RESOURCE_OK) *output=copy;
    return status;
}
static void free_scope_name(const char *key,void *value,void *context) {
    (void) value;(void) context;xr_compile_state_free((void *)key);
}
static void free_scope_leaf(XaScope *scope) {
    XaSymbolRegistry *registry=scope->registry;
    xr_hashmap_owned_free(scope->aliases);scope->aliases=NULL;
    ScopeAlias *alias=scope->alias_owners;
    while (alias) {
        ScopeAlias *next=alias->next;release_symbol(alias->symbol);
        xr_compile_state_free(alias->name);xr_compile_state_free(alias);alias=next;
    }
    XaSymbol *symbol=scope->owned_symbols;
    while (symbol) {
        XaSymbol *next=symbol->scope_owned_next;symbol->scope=NULL;symbol->scope_owned_next=NULL;
        symbol->scope_owned_previous=NULL;
        xa_symbol_free(symbol);symbol=next;
    }
    xr_hashmap_owned_dispose(scope->symbols,free_scope_name,NULL);
    xr_compile_state_free(scope->children);xr_compile_state_free(scope);
    xa_symbol_registry_release(registry);
}
XR_FUNC void xa_scope_free(XaScope *scope) {
    if (!scope) return;
    detach_scope_cleanup(scope);
    XaScope *cursor=scope;
    while (cursor) {
        if (cursor->child_count) { cursor=cursor->children[--cursor->child_count];continue; }
        XaScope *parent=cursor->parent;free_scope_leaf(cursor);cursor=parent;
    }
}
XR_FUNC XrCompileResourceStatus xa_scope_add_symbol(XaScope *scope,XaSymbol *symbol) {
    XrCompileResourceStatus status=xa_scope_resource_status(scope);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    XaSymbolRegistry *registry=scope->registry;
    if (!symbol) return invalid_registry(registry);
    SymbolOwner *owner=symbol_owner(symbol);
    if (owner->registry!=registry || owner->creator_released || owner->birth_id!=symbol->id ||
        (symbol->scope && symbol->scope!=scope) || !symbol->name) return invalid_registry(registry);
    const char *stored_key=NULL;void *active=NULL;
    status=capture_io(registry,xr_hashmap_owned_get_pair(scope->symbols,symbol->name,
        &stored_key,&active));
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    if (stored_key && !active) return invalid_registry(registry);
    char *new_key=NULL;
    if (!stored_key) status=xr_compile_state_strdup(registry->state,symbol->name,&new_key);
    if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_work(registry->state,6);
    if (status==XR_COMPILE_RESOURCE_OK)
        status=capture_io(registry,xr_hashmap_owned_set(scope->symbols,
            stored_key?stored_key:new_key,symbol));
    if (status!=XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(new_key);return status; }
    if (!symbol->scope) {
        symbol->scope=scope;symbol->scope_owned_next=scope->owned_symbols;
        symbol->scope_owned_previous=NULL;
        if (scope->owned_symbols) scope->owned_symbols->scope_owned_previous=symbol;
        scope->owned_symbols=symbol;
    }
    registry->slots[owner->birth_id]=symbol;return status;
}
XR_FUNC XrCompileResourceStatus xa_scope_remove_owned_symbol(XaScope *scope,XaSymbol *symbol) {
    XrCompileResourceStatus status=xa_scope_resource_status(scope);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    XaSymbolRegistry *registry=scope->registry;
    if (!symbol) return invalid_registry(registry);
    SymbolOwner *owner=symbol_owner(symbol);
    if (owner->registry!=registry || owner->creator_released || owner->birth_id!=symbol->id ||
        symbol->scope!=scope || !symbol->name) return invalid_registry(registry);
    XaSymbol *previous=symbol->scope_owned_previous,*next=symbol->scope_owned_next;
    if ((previous && previous->scope!=scope) || (next && next->scope!=scope) ||
        (previous?previous->scope_owned_next:scope->owned_symbols)!=symbol ||
        (next && next->scope_owned_previous!=symbol)) return invalid_registry(registry);
    const char *stored_key=NULL;void *active=NULL;
    status=capture_io(registry,xr_hashmap_owned_get_pair(scope->symbols,symbol->name,
        &stored_key,&active));
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    status=xr_compile_state_work(registry->state,6);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    if (active==symbol) {
        bool deleted=false;
        status=capture_io(registry,xr_hashmap_owned_delete(scope->symbols,stored_key,&deleted));
        if (status!=XR_COMPILE_RESOURCE_OK) return status;
        XR_CHECK(deleted,"An active owned symbol must retain its name entry");
    }
    if (previous) previous->scope_owned_next=next;
    else scope->owned_symbols=next;
    if (next) next->scope_owned_previous=previous;
    symbol->scope=NULL;symbol->scope_owned_next=NULL;symbol->scope_owned_previous=NULL;
    unbind_symbol(symbol);
    if (active==symbol) xr_compile_state_free((void *)stored_key);
    return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC bool xa_scope_remove_symbol(XaScope *scope,const char *name) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return false;
    XaSymbolRegistry *registry=scope->registry;void *found=NULL;
    if (capture_io(registry,xr_hashmap_owned_get(scope->symbols,name,&found))!=XR_COMPILE_RESOURCE_OK || !found)
        return false;
    return xa_scope_remove_owned_symbol(scope,found)==XR_COMPILE_RESOURCE_OK;
}

typedef struct ScopeWalkFrame {
    XaScope *scope;
    size_t next_child;
} ScopeWalkFrame;

XR_FUNC XrCompileResourceStatus xa_scope_clear_class_base_references(
    XaScope *root,const XrClassInfo *target) {
    XrCompileResourceStatus status=xa_scope_resource_status(root);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    XaSymbolRegistry *registry=root->registry;
    if (!target) return invalid_registry(registry);
    XrCompileState *state=registry->state;
    size_t capacity=16,count=1;void *memory=NULL;
    status=xr_compile_state_calloc(state,capacity,sizeof(ScopeWalkFrame),&memory);
    if (status!=XR_COMPILE_RESOURCE_OK) return status;
    ScopeWalkFrame *frames=memory;
    /* Clear actual owners before checking shared views. A view of a locally
     * owned descriptor then observes the same invalidation. A remaining foreign
     * descriptor cannot be mutated here and keeps the target from retiring. */
    for (unsigned verify=0;verify<2 && status==XR_COMPILE_RESOURCE_OK;++verify) {
        count=1;frames[0]=(ScopeWalkFrame){root,0};
        while (count && status==XR_COMPILE_RESOURCE_OK) {
        ScopeWalkFrame *frame=&frames[count-1];XaScope *scope=frame->scope;
        if (!frame->next_child) {
            status=xr_compile_state_work(state,1);
            if (status!=XR_COMPILE_RESOURCE_OK) break;
            if (scope->child_count<0 || scope->child_capacity<scope->child_count ||
                (scope->child_count && !scope->children)) { status=invalid_registry(registry);break; }
            for (XaSymbol *symbol=scope->owned_symbols;symbol;symbol=symbol->scope_owned_next) {
                status=xr_compile_state_work(state,1);
                if (status!=XR_COMPILE_RESOURCE_OK) break;
                XrClassInfo *info=symbol->links.class_info;
                if (verify && info==target && !symbol->links.owns_class_info) {
                    status=invalid_registry(registry);break;
                }
                if (info && info->base==target) {
                    if (verify) { status=invalid_registry(registry);break; }
                    if (symbol->links.owns_class_info) info->base=NULL;
                }
            }
            if (status!=XR_COMPILE_RESOURCE_OK) break;
        }
        if (frame->next_child==(size_t)scope->child_count) { --count;continue; }
        status=xr_compile_state_work(state,1);
        if (status!=XR_COMPILE_RESOURCE_OK) break;
        XaScope *child=scope->children[frame->next_child++];
        if (!child || child->registry!=registry || child->parent!=scope) {
            status=invalid_registry(registry);break;
        }
        if (count==capacity) {
            if (capacity>SIZE_MAX/(2*sizeof(ScopeWalkFrame))) {
                status=xr_compile_state_fail(state,XR_COMPILE_RESOURCE_BUDGET);break;
            }
            memory=NULL;status=xr_compile_state_calloc(state,capacity*2,sizeof(ScopeWalkFrame),&memory);
            if (status!=XR_COMPILE_RESOURCE_OK) break;
            status=xr_compile_state_copy(state,memory,frames,count*sizeof(ScopeWalkFrame));
            if (status!=XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(memory);break; }
            xr_compile_state_free(frames);frames=memory;capacity*=2;
        }
        frames[count++]=(ScopeWalkFrame){child,0};
    }
    }
    xr_compile_state_free(frames);return status;
}

static void unlink_alias(XaScope *scope,ScopeAlias *alias) {
    if (alias->previous) alias->previous->next=alias->next;
    else scope->alias_owners=alias->next;
    if (alias->next) alias->next->previous=alias->previous;
}
XR_FUNC bool xa_scope_set_alias(XaScope *scope,const char *name,XaSymbol *symbol) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return false;
    XaSymbolRegistry *registry=scope->registry;XrCompileState *state=registry->state;
    if (!symbol || symbol_owner(symbol)->registry!=registry) { (void) invalid_registry(registry);return false; }
    void *found=NULL;
    if (capture_io(registry,xr_hashmap_owned_get(scope->aliases,name,&found))!=XR_COMPILE_RESOURCE_OK) return false;
    ScopeAlias *alias=found;
    if (alias) {
        if (alias->symbol==symbol) return true;
        if (xr_compile_state_work(state,1)!=XR_COMPILE_RESOURCE_OK || retain_symbol(symbol)!=XR_COMPILE_RESOURCE_OK)
            return false;
        XaSymbol *old=alias->symbol;alias->symbol=symbol;release_symbol(old);return true;
    }
    void *memory=NULL;XrCompileResourceStatus status=xr_compile_state_calloc(state,1,sizeof(ScopeAlias),&memory);
    if (status!=XR_COMPILE_RESOURCE_OK) return false;
    alias=memory;status=xr_compile_state_strdup(state,name,&alias->name);
    if (status==XR_COMPILE_RESOURCE_OK) status=xr_compile_state_work(state,3);
    if (status==XR_COMPILE_RESOURCE_OK) status=retain_symbol(symbol);
    bool retained=status==XR_COMPILE_RESOURCE_OK;
    if (retained) status=capture_io(registry,xr_hashmap_owned_set(scope->aliases,alias->name,alias));
    if (status!=XR_COMPILE_RESOURCE_OK) {
        if (retained) release_symbol(symbol);
        xr_compile_state_free(alias->name);xr_compile_state_free(alias);return false;
    }
    alias->symbol=symbol;alias->next=scope->alias_owners;
    if (alias->next) alias->next->previous=alias;
    scope->alias_owners=alias;return true;
}
XR_FUNC bool xa_scope_remove_alias(XaScope *scope,const char *name) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return false;
    XaSymbolRegistry *registry=scope->registry;void *found=NULL;
    if (capture_io(registry,xr_hashmap_owned_get(scope->aliases,name,&found))!=XR_COMPILE_RESOURCE_OK || !found)
        return false;
    if (xr_compile_state_work(registry->state,3)!=XR_COMPILE_RESOURCE_OK) return false;
    bool deleted=false;
    if (capture_io(registry,xr_hashmap_owned_delete(scope->aliases,name,&deleted))!=XR_COMPILE_RESOURCE_OK || !deleted)
        return false;
    ScopeAlias *alias=found;unlink_alias(scope,alias);release_symbol(alias->symbol);
    xr_compile_state_free(alias->name);xr_compile_state_free(alias);return true;
}
XR_FUNC XaSymbol *xa_scope_lookup_local(XaScope *scope,const char *name) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return NULL;
    void *symbol=NULL;
    if (capture_io(scope->registry,xr_hashmap_owned_get(scope->symbols,name,&symbol))!=XR_COMPILE_RESOURCE_OK)
        return NULL;
    if (symbol) return symbol;
    void *found=NULL;
    if (capture_io(scope->registry,xr_hashmap_owned_get(scope->aliases,name,&found))!=XR_COMPILE_RESOURCE_OK)
        return NULL;
    return found ? ((ScopeAlias *)found)->symbol : NULL;
}
XR_FUNC XaSymbol *xa_scope_lookup(XaScope *scope,const char *name) {
    while (scope) {
        XaSymbol *symbol=xa_scope_lookup_local(scope,name);
        if (symbol || xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return symbol;
        scope=scope->parent;
    }
    return NULL;
}
XR_FUNC XaSymbol *xa_scope_lookup_by_id(XaScope *scope,uint32_t id) {
    return scope ? xa_symbol_registry_lookup(scope->registry,id) : NULL;
}
typedef struct SymbolCollect {
    XrCompileState *state;XaSymbol **symbols;uint32_t capacity,index;
} SymbolCollect;
static void collect_active_symbol(const char *key,void *value,void *context) {
    (void) key;SymbolCollect *collect=context;
    if (xr_compile_state_work(collect->state,1)!=XR_COMPILE_RESOURCE_OK) return;
    if (collect->index>=collect->capacity) { (void) xr_compile_state_fail(collect->state,XR_COMPILE_RESOURCE_BAD_ARGUMENT);return; }
    collect->symbols[collect->index++]=value;
}
XR_FUNC XaSymbol **xa_scope_get_all_symbols(XaScope *scope,int *count) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return NULL;
    XaSymbolRegistry *registry=scope->registry;
    if (!count) { (void) invalid_registry(registry);return NULL; }
    uint32_t capacity=xr_hashmap_count(scope->symbols);
    if (capacity>INT_MAX) { (void) xr_compile_state_fail(registry->state,XR_COMPILE_RESOURCE_BUDGET);return NULL; }
    if (!capacity) { *count=0;return NULL; }
    void *memory=NULL;
    if (xr_compile_state_alloc(registry->state,(size_t)capacity*sizeof(XaSymbol *),&memory)!=XR_COMPILE_RESOURCE_OK)
        return NULL;
    SymbolCollect collect={registry->state,memory,capacity,0};
    XrCompileResourceStatus status=capture_io(registry,
        xr_hashmap_owned_foreach(scope->symbols,collect_active_symbol,&collect));
    if (status!=XR_COMPILE_RESOURCE_OK || collect.index!=capacity) {
        if (status==XR_COMPILE_RESOURCE_OK) (void) invalid_registry(registry);
        xr_compile_state_free(memory);return NULL;
    }
    *count=(int)collect.index;return memory;
}
XR_FUNC int xa_scope_count_symbols(XaScope *scope) {
    if (xa_scope_resource_status(scope)!=XR_COMPILE_RESOURCE_OK) return 0;
    uint32_t count=xr_hashmap_count(scope->symbols);
    if (count>INT_MAX) { (void) xr_compile_state_fail(scope->registry->state,XR_COMPILE_RESOURCE_BUDGET);return 0; }
    return (int)count;
}
