/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_checked_resource.inc.c - Resolve immutable catalog authority before disk
 */
/* Takes ownership of canonical/logical whether or not a binding is selected. */
static void checked_resource_select(ModuleWork *work, XrModuleResolver *resolver,
    const XrModuleIdentityAuthority *authority, char *canonical, char *logical,
    XrModuleId *output, bool *matched) {
    size_t count = 0;
    const XrModuleResourceBinding *bindings = xr_xir_compile_library_catalog_resources_v2(resolver->config.catalog,&count);
    const XrModuleResourceBinding *selected = NULL;
    for (size_t i = 0; i < count && module_work(work,1); ++i) {
        if (bindings[i].canonical && module_equal(work,bindings[i].canonical,canonical)) {
            if (selected) { module_status(work,XR_MODULE_INVALID); break; }
            selected = &bindings[i];
        }
    }
    if (!selected || work->status != XR_MODULE_OK) goto release;
    *matched = true;
    if (!selected->checked || selected->authority.kind != authority->kind ||
        !module_equal(work,selected->authority.namespace_id,authority->namespace_id) ||
        !module_equal(work,selected->authority.physical_root,authority->physical_root) ||
        !selected->logical_path || !module_equal(work,selected->logical_path,logical) || !selected->source_locator) {
        module_status(work,XR_MODULE_INVALID); goto release;
    }
    output->kind = authority->kind == XR_MODULE_IDENTITY_STDLIB ? XR_MOD_STDLIB :
        authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    output->canonical = canonical; canonical = NULL; output->logical_path = logical; logical = NULL;
    output->source_path = module_dup(work,selected->source_locator);
    copy_authority(work,authority,&output->authority);
    output->representation = XR_MODULE_CHECKED_LIBRARY; output->resource = selected;
release:
    xr_compile_resources_free(canonical); xr_compile_resources_free(logical);
}
static void resolve_checked_stdlib(ModuleWork *work, XrModuleResolver *resolver, const char *specifier,
    XrModuleId *output, bool *matched) {
    char name[256], logical[XR_PATH_MAX];
    if (!resolver->config.stdlib_path || !stdlib_submodule_path(work,specifier+4,name,sizeof(name))) return;
    if (!module_format_buffer(work,logical,sizeof(logical),"%s.xr",specifier+4)) return;
    char *root = NULL; if (!realpath_into(work,resolver->config.stdlib_path,&root)) return;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB,name,root};
    char *canonical = NULL, *copy = NULL;
    module_status(work,xr_compile_module_identity_from_logical(work->resources,&authority,logical,&canonical));
    copy = module_dup(work,logical);
    if (work->status == XR_MODULE_OK) checked_resource_select(work,resolver,&authority,canonical,copy,output,matched);
    else { xr_compile_resources_free(canonical); xr_compile_resources_free(copy); }
    xr_compile_resources_free(root);
}
static void resolve_checked_resource(ModuleWork *work, XrModuleResolver *resolver, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority, XrModuleId *output, bool *matched) {
    *matched = false;
    if (!resolver->config.catalog) return;
    if (!module_authority(work,authority)) return;
    if (module_prefix(work,specifier,"std/")) { resolve_checked_stdlib(work,resolver,specifier,output,matched); return; }
    if (!importer || !module_prefix(work,specifier,"./")) return;
    XrOsIoPolicy policy = xr_compile_io_policy(work->resources); char *directory = NULL;
    if (work->status != XR_MODULE_OK || !module_io(work,xr_path_dirname_owned(&policy,importer,&directory))) return;
    size_t length = module_length(work,specifier+2);
    bool extension = length >= 3 && module_equal(work,specifier+2+length-3,".xr");
    char locator[XR_PATH_MAX];
    module_format_buffer(work,locator,sizeof(locator),"%s/%s%s",directory,specifier+2,extension ? "" : ".xr");
    xr_compile_resources_free(directory);
    char *canonical = NULL, *logical = NULL;
    if (work->status == XR_MODULE_OK) module_status(work,xr_compile_module_identity_from_source(
        work->resources,authority,locator,&canonical,&logical));
    if (work->status == XR_MODULE_OK) checked_resource_select(work,resolver,authority,canonical,logical,output,matched);
    else { xr_compile_resources_free(canonical); xr_compile_resources_free(logical); }
}
