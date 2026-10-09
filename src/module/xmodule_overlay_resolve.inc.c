/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
/* Included after the resolver's ordinary owned-copy helpers. */
XR_FUNC XrModuleStatus xr_compile_module_resolver_set_overlay(XrModuleResolver *resolver,
    const XrModuleOverlayInput *inputs, size_t count) {
    if (!resolver || !resolver->resources || resolver->overlay || resolver->resolution_started ||
        xr_hashmap_count(resolver->cache)) return XR_MODULE_INVALID;
    ModuleWork work = {resolver->resources, XR_MODULE_OK}; XrModuleOverlay *overlay = NULL;
    module_status(&work, xr_compile_module_overlay_new(resolver->resources, inputs, count, &overlay));
    size_t bindings_count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources_v2(resolver->config.catalog, &bindings_count);
    for (size_t i = 0; i < bindings_count && module_work(&work, 1); ++i) {
        const XrModuleOverlayEntry *found = NULL;
        module_status(&work, xr_compile_module_overlay_lookup(overlay, bindings[i].canonical, &bindings[i].authority, &found));
        if (found) module_status(&work, XR_MODULE_INVALID);
        if(work.status==XR_MODULE_OK && bindings[i].source_locator) {
            module_status(&work,xr_compile_module_overlay_bind(overlay,&bindings[i].authority,bindings[i].source_locator,&found));
            if(found)module_status(&work,XR_MODULE_INVALID);
        }
    }
    if (work.status != XR_MODULE_OK) { xr_compile_module_overlay_free(overlay); return work.status; }
    resolver->overlay = overlay; return XR_MODULE_OK;
}
/* Fold only the import's logical components. Drive/UNC/physical-root syntax is
 * handled by the existing identity owner before reaching this helper. Suffix
 * concatenation precedes folding, matching the existing disk candidate order. */
static char *overlay_relative_logical(ModuleWork *work, const char *importer,
    const char *relative, const char *suffix) {
    size_t importer_length = module_length(work, importer), directory = 0;
    for (size_t i = 0; i < importer_length && module_work(work, 1); ++i)
        if (importer[i] == '/') directory = i;
    char *raw = module_format(work, "%s%s", relative, suffix);
    size_t length = raw ? module_length(work, raw) : 0;
    if (directory > SIZE_MAX-2 || length > SIZE_MAX-directory-2) module_status(work, XR_MODULE_BUDGET);
    char *logical = module_alloc(work, directory+length+2); size_t used = directory;
    if (logical) module_copy(work, logical, importer, directory);
    for (size_t start = 0; start < length && work->status == XR_MODULE_OK;) {
        size_t end = start;
        while (end < length && module_work(work, 1) && raw[end] != '/' && raw[end] != '\\') ++end;
        if (work->status != XR_MODULE_OK) break;
        size_t count = end-start; bool current = false, parent = false;
        if (count == 1) { if (!module_work(work, 1)) break; current = raw[start] == '.'; }
        else if (count == 2) {
            if (!module_work(work, 2)) break;
            char first = raw[start], second = raw[start+1]; parent = first == '.' && second == '.';
        }
        if (current) { /* Current directory has no component. */ }
        else if (parent) {
            if (!used) { module_status(work, XR_MODULE_INVALID); break; }
            while (used && module_work(work, 1) && logical[used-1] != '/') --used;
            if (used) --used;
        } else if (count) {
            if (used && module_work(work, 1)) logical[used++] = '/';
            if (!module_copy(work, logical+used, raw+start, count)) break;
            used += count;
        }
        start = end < length ? end+1 : end;
    }
    if (work->status == XR_MODULE_OK && module_work(work, 1)) logical[used] = 0;
    xr_compile_resources_free(raw);
    if (work->status != XR_MODULE_OK) { xr_compile_resources_free(logical); return NULL; }
    return logical;
}
static bool overlay_relative_select(ModuleWork *work, XrModuleResolver *resolver,
    const XrModuleIdentityAuthority *authority, const char *importer_logical,
    const char *relative, const char *suffix, const XrModuleOverlayEntry **output) {
    char *logical = overlay_relative_logical(work, importer_logical, relative, suffix);
    char *path=NULL;
    if(work->status==XR_MODULE_OK)path=module_format(work,"%s/%s",authority->physical_root,logical);
    if(work->status==XR_MODULE_OK)module_status(work,xr_compile_module_overlay_bind(resolver->overlay,authority,path,output));
    xr_compile_resources_free(path);xr_compile_resources_free(logical);
    return work->status==XR_MODULE_OK;
}
/* Authority was established by descriptor/lockfile/project resolution. This
 * helper changes only the selected text source, never the module identity. */
static bool overlay_named_select(ModuleWork *work,XrModuleResolver *resolver,
    const XrModuleIdentityAuthority *authority,const char *path,XrModuleId *out) {
    if(!resolver->overlay)return false;
    const XrModuleOverlayEntry *entry=NULL;
    module_status(work,xr_compile_module_overlay_bind(resolver->overlay,authority,path,&entry));
    if(!entry||work->status!=XR_MODULE_OK)return false;
    out->kind=authority->kind==XR_MODULE_IDENTITY_STDLIB?XR_MOD_STDLIB:
        authority->kind==XR_MODULE_IDENTITY_PACKAGE?XR_MOD_PACKAGE:XR_MOD_FILE;
    out->source_path=module_dup(work,path);
    copy_authority(work,authority,&out->authority);
    if(work->status==XR_MODULE_OK)module_status(work,xr_compile_module_identity_from_source(
        work->resources,authority,path,&out->canonical,&out->logical_path));
    return work->status==XR_MODULE_OK;
}
