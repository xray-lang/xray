/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_checked_resource.inc.c - Exact checked resource resolution
 *
 * KEY CONCEPT:
 *   An exact compiler-owned resource binding is checked before source probing.
 *   A matched but inconsistent Checked resource never falls back to source.
 */
static bool resource_optional_equal(const char *a, const char *b) {
    return (!a && !b) || (a && b && !strcmp(a,b));
}
static int checked_resource_select(XrModuleResolver *r, const char *specifier,
    const XrModuleIdentityAuthority *authority, char *canonical, char *logical,
    XrModuleId *output, char **error) {
    const XrModuleResourceBinding *selected=NULL;
    for(size_t i=0;i<r->config.resource_count;++i){
        const XrModuleResourceBinding *binding=&r->config.resources[i];
        if(binding->canonical && !strcmp(binding->canonical,canonical)) {
            if(selected){xr_free(canonical);xr_free(logical);return -1;}selected=binding;
        }
    }
    if(!selected){xr_free(canonical);xr_free(logical);return 0;}
    if (authority->kind != XR_MODULE_IDENTITY_STDLIB &&
        (!specifier[2] || strchr(specifier+2,'/') || strchr(specifier+2,'\\'))) {
        xr_free(canonical);xr_free(logical);
        if(error)*error=xr_strdup("Checked catalog supports only direct relative imports");
        return -1;
    }
    if(!selected->checked || selected->authority.kind!=authority->kind ||
        !resource_optional_equal(selected->authority.namespace_id,authority->namespace_id) ||
        !resource_optional_equal(selected->authority.physical_root,authority->physical_root) ||
        !selected->logical_path || strcmp(selected->logical_path,logical) || !selected->source_locator) {
        xr_free(canonical);xr_free(logical);
        if(error)*error=xr_strdup("Checked resource authority binding is inconsistent");
        return -1;
    }
    output->kind=authority->kind==XR_MODULE_IDENTITY_STDLIB?XR_MOD_STDLIB:
        authority->kind==XR_MODULE_IDENTITY_PACKAGE?XR_MOD_PACKAGE:XR_MOD_FILE;
    output->canonical=canonical;output->logical_path=logical;
    output->source_path=xr_strdup(selected->source_locator);
    output->authority.kind=authority->kind;
    output->authority.namespace_id=authority->namespace_id?xr_strdup(authority->namespace_id):NULL;
    output->authority.physical_root=authority->physical_root?xr_strdup(authority->physical_root):NULL;
    output->representation=XR_MODULE_CHECKED_LIBRARY;output->resource=selected;
    if(!output->source_path || (authority->namespace_id&&!output->authority.namespace_id) ||
        (authority->physical_root&&!output->authority.physical_root)) {xr_module_id_cleanup(output);return -1;}
    return 1;
}

static int resolve_checked_stdlib(XrModuleResolver *r, const char *specifier,
    XrModuleId *output, char **error) {
    char name[256], logical[XR_PATH_MAX];
    if (!r->config.stdlib_path || !stdlib_submodule_path(specifier + 4, name, sizeof(name)))
        return 0;
    int length = snprintf(logical, sizeof(logical), "%s.xr", specifier + 4);
    if (length <= 0 || (size_t)length >= sizeof(logical)) return -1;
    /* The configured stdlib root supplies authority even after its module source
     * has been removed. Import spelling never grants a caller's SCRIPT authority. */
    char *root = xr_realpath(r->config.stdlib_path);
    if (!root) return -1;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB, name, root};
    char *canonical = NULL, *owned_logical = NULL;
    bool valid = xr_module_identity_from_logical(&authority, logical, &canonical);
    if (valid) owned_logical = xr_strdup(logical);
    if (!valid || !owned_logical) { xr_free(canonical); xr_free(root); return -1; }
    int result = checked_resource_select(r, specifier, &authority, canonical, owned_logical, output, error);
    xr_free(root);
    return result;
}

static int resolve_checked_resource(XrModuleResolver *r, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority,
    XrModuleId *output, char **error) {
    if (!r->config.resource_count) return 0;
    if (!r->config.resources || !authority || !xr_module_identity_authority_valid(authority)) return -1;
    if (!strncmp(specifier, "std/", 4)) return resolve_checked_stdlib(r, specifier, output, error);
    if (!importer || strncmp(specifier,"./",2)) return 0;
    char *directory=xr_path_dirname(importer);
    if (!directory) return -1;
    char locator[XR_PATH_MAX];
    size_t name_length=strlen(specifier+2);
    bool extension=name_length>=3&&!strcmp(specifier+2+name_length-3,".xr");
    int n=snprintf(locator,sizeof(locator),"%s/%s%s",directory,specifier+2,extension?"":".xr");xr_free(directory);
    if(n<=0||(size_t)n>=sizeof(locator))return -1;
    char *canonical=NULL,*logical=NULL;
    if(!xr_module_identity_from_source(authority,locator,&canonical,&logical))return -1;
    return checked_resource_select(r,specifier,authority,canonical,logical,output,error);
}
