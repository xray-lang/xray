/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xmodule_overlay.h"
#include "xmodule_compile_internal.h"
#include "../base/xhashmap.h"
#include <ctype.h>

struct XrModuleOverlay {
    XrCompileResources *resources;
    XrHashMap *index, *physical;
    char **keys, **paths;
    XrModuleOverlayEntry *entries;
    size_t initialized, count;
};
static void overlay_entry_free(XrModuleOverlayEntry *entry) {
    xr_compile_resources_free((char *)entry->canonical);
    xr_compile_resources_free((char *)entry->source.logical_path);
    xr_compile_resources_free((char *)entry->source.source_path);
    xr_compile_resources_free((char *)entry->source.text);
    xr_compile_resources_free((char *)entry->source.authority.namespace_id);
    xr_compile_resources_free((char *)entry->source.authority.physical_root);
}
XR_FUNC void xr_compile_module_overlay_free(XrModuleOverlay *overlay) {
    if (!overlay) return;
    /* The map borrows keys and entries, and is destroyed before either owner. */
    xr_hashmap_owned_free(overlay->index);
    xr_hashmap_owned_free(overlay->physical);
    for(size_t i=0;i<overlay->initialized;++i) {
        xr_compile_resources_free(overlay->keys?overlay->keys[i]:NULL);
        xr_compile_resources_free(overlay->paths?overlay->paths[i]:NULL);
    }
    xr_compile_resources_free(overlay->keys);xr_compile_resources_free(overlay->paths);
    for (size_t i = 0; i < overlay->initialized; ++i) overlay_entry_free(&overlay->entries[i]);
    xr_compile_resources_free(overlay->entries);
    xr_compile_resources_free(overlay);
}
static bool overlay_entry_copy(ModuleWork *work, const XrModuleOverlayInput *input,
    XrModuleOverlayEntry *entry) {
    if (!input->text || !module_authority(work, &input->authority)) return module_status(work, XR_MODULE_INVALID);
    if (input->length == SIZE_MAX) return module_status(work, XR_MODULE_BUDGET);
    char *canonical = NULL, *physical = NULL, *logical = NULL;
    module_status(work, xr_compile_module_identity_from_logical(work->resources,
        &input->authority, input->logical_path, &canonical));
    if (work->status == XR_MODULE_OK) {
        if (input->authority.kind == XR_MODULE_IDENTITY_MEMORY) {
            if (input->source_path) module_status(work, XR_MODULE_INVALID);
        } else {
            module_status(work, xr_compile_module_identity_from_source(work->resources,
                &input->authority, input->source_path, &physical, &logical));
            if (work->status == XR_MODULE_OK && !module_equal(work, canonical, physical))
                module_status(work, XR_MODULE_INVALID);
        }
    }
    xr_compile_resources_free(physical); xr_compile_resources_free(logical);
    if (work->status != XR_MODULE_OK) { xr_compile_resources_free(canonical); return false; }
    entry->canonical = canonical;
    entry->source.authority.kind = input->authority.kind;
    entry->source.authority.namespace_id = module_dup(work, input->authority.namespace_id);
    entry->source.authority.physical_root = module_dup(work, input->authority.physical_root);
    entry->source.logical_path = module_dup(work, input->logical_path);
    entry->source.source_path = module_dup(work, input->source_path);
    char *text = module_calloc(work, input->length + 1, 1);
    entry->source.text = text; entry->source.length = input->length;
    for (size_t i = 0; i < input->length && module_work(work, 1); ++i) {
        if (!input->text[i]) { module_status(work, XR_MODULE_INVALID); break; }
    }
    if (work->status == XR_MODULE_OK) module_copy(work, text, input->text, input->length);
    return work->status == XR_MODULE_OK;
}
static char *overlay_key(ModuleWork *work,const char *canonical,const XrModuleIdentityAuthority *authority) {
    const char *root=authority->physical_root?authority->physical_root:"";
    size_t length=module_length(work,root);
    char decimal[sizeof(size_t)*3+1];size_t cursor=sizeof(decimal)-1;
    if (!module_work(work,1)) return NULL;
    decimal[cursor]=0;
    do {
        if (!module_work(work,3)) return NULL;
        decimal[--cursor]=(char)('0'+length%10);length/=10;
    } while (length);
    return module_format(work,"%s:%s%s",decimal+cursor,root,canonical);
}
static char *overlay_path(ModuleWork *work,const char *path) {
    char *key=module_dup(work,path);
    if(!key)return NULL;
    for(size_t i=0;module_work(work,1)&&key[i];++i) {
#ifdef XR_OS_WINDOWS
        if(key[i]=='\\')key[i]='/';
#endif
    }
    return key;
}
XR_FUNC XrModuleStatus xr_compile_module_overlay_new(XrCompileResources *resources,
    const XrModuleOverlayInput *inputs, size_t count, XrModuleOverlay **output) {
    if (!resources || !output || *output || (count && !inputs)) return XR_MODULE_INVALID;
    ModuleWork work = {resources, XR_MODULE_OK};
    XrModuleOverlay *overlay = module_calloc(&work, 1, sizeof(*overlay));
    if (!overlay) return work.status;
    overlay->resources = resources;
    if (count) overlay->entries = module_calloc(&work, count, sizeof(*overlay->entries));
    if(count) {
        overlay->keys=module_calloc(&work,count,sizeof(*overlay->keys));
        overlay->paths=module_calloc(&work,count,sizeof(*overlay->paths));
    }
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    if (work.status == XR_MODULE_OK) module_io(&work, xr_hashmap_owned_new(&policy, &overlay->index));
    if (work.status == XR_MODULE_OK) module_io(&work, xr_hashmap_owned_new(&policy, &overlay->physical));
    for (size_t i = 0; i < count && module_work(&work, 1); ++i) {
        XrModuleOverlayEntry *entry = &overlay->entries[i];
        overlay->initialized = i + 1;
        if (!overlay_entry_copy(&work, &inputs[i], entry)) break;
        void *existing = NULL;
        overlay->keys[i]=overlay_key(&work,entry->canonical,&entry->source.authority);
        if(work.status!=XR_MODULE_OK)break;
        if (!module_io(&work, xr_hashmap_owned_get(overlay->index, overlay->keys[i], &existing))) break;
        if (existing) { module_status(&work, XR_MODULE_INVALID); break; }
        if (!module_io(&work, xr_hashmap_owned_set(overlay->index, overlay->keys[i], entry))) break;
        if(entry->source.source_path) {
            overlay->paths[i]=overlay_path(&work,entry->source.source_path);
            if(work.status!=XR_MODULE_OK)break;
            existing=NULL;
            if(!module_io(&work,xr_hashmap_owned_get(overlay->physical,overlay->paths[i],&existing)))break;
            if(existing) {
                const XrModuleOverlayEntry *other=existing;
                if(other->source.length!=entry->source.length ||
                    !module_equal(&work,other->source.text,entry->source.text)) {module_status(&work,XR_MODULE_INVALID);break;}
            } else if(!module_io(&work,xr_hashmap_owned_set(overlay->physical,overlay->paths[i],entry)))break;
        }
    }
    if (work.status != XR_MODULE_OK) { xr_compile_module_overlay_free(overlay); return work.status; }
    overlay->count = count; *output = overlay; return XR_MODULE_OK;
}
XR_FUNC XrCompileResources *xr_compile_module_overlay_resources(const XrModuleOverlay *overlay) {
    return overlay ? overlay->resources : NULL;
}
XR_FUNC const XrModuleOverlayEntry *xr_compile_module_overlay_entries(const XrModuleOverlay *overlay, size_t *count) {
    if (count) *count = overlay ? overlay->count : 0;
    return overlay ? overlay->entries : NULL;
}
XR_FUNC XrModuleStatus xr_compile_module_overlay_lookup(const XrModuleOverlay *overlay,
    const char *canonical, const XrModuleIdentityAuthority *authority,
    const XrModuleOverlayEntry **output) {
    if (!overlay || !canonical || !output) return XR_MODULE_INVALID;
    ModuleWork work = {overlay->resources, XR_MODULE_OK};
    if (!module_authority(&work, authority)) return work.status;
    void *value = NULL;
    char *key=overlay_key(&work,canonical,authority);
    if(work.status==XR_MODULE_OK)module_io(&work, xr_hashmap_owned_get(overlay->index, key, &value));
    xr_compile_resources_free(key);
    const XrModuleOverlayEntry *entry = value;
    if(!entry) for(size_t i=0;i<overlay->count&&module_work(&work,1);++i)
        if(module_equal(&work,overlay->entries[i].canonical,canonical)) {module_status(&work,XR_MODULE_INVALID);break;}
    if (entry && work.status == XR_MODULE_OK) {
        const XrModuleIdentityAuthority *stored = &entry->source.authority;
        if (stored->kind != authority->kind ||
            !module_equal(&work, stored->namespace_id, authority->namespace_id) ||
            !module_equal(&work, stored->physical_root, authority->physical_root))
            module_status(&work, XR_MODULE_INVALID);
    }
    if (work.status == XR_MODULE_OK) *output = entry;
    return work.status;
}

XR_FUNC XrModuleStatus xr_compile_module_overlay_bind(const XrModuleOverlay *overlay,
    const XrModuleIdentityAuthority *authority,const char *path,const XrModuleOverlayEntry **out) {
    if(!overlay||!path||!out)return XR_MODULE_INVALID;
    ModuleWork work={overlay->resources,XR_MODULE_OK};char *canonical=NULL,*logical=NULL,*key=NULL;
    module_status(&work,xr_compile_module_identity_from_source(work.resources,authority,path,&canonical,&logical));
    if(work.status==XR_MODULE_OK)key=overlay_path(&work,path);
    void *entry=NULL;
    if(work.status==XR_MODULE_OK)module_io(&work,xr_hashmap_owned_get(overlay->physical,key,&entry));
    /* Windows identity permits insensitive authority prefixes, but source
     * logical names remain exact. An ambiguous spelling must not silently
     * choose disk over edited text; reject without asserting OS equivalence. */
#ifdef XR_OS_WINDOWS
    if(!entry)for(size_t i=0;i<overlay->count&&module_work(&work,1);++i) {
        const char *other=overlay->paths[i];if(!other)continue;
        size_t n=0;bool ambiguous=true;
        for(;;++n) {
            if(!module_work(&work,2)){ambiguous=false;break;}
            if(tolower((unsigned char)key[n])!=tolower((unsigned char)other[n])){ambiguous=false;break;}
            if(!key[n])break;
        }
        if(ambiguous){module_status(&work,XR_MODULE_INVALID);break;}
    }
#endif
    xr_compile_resources_free(key);xr_compile_resources_free(canonical);xr_compile_resources_free(logical);
    if(work.status==XR_MODULE_OK)*out=entry;
    return work.status;
}
