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
    return (!a && !b) || (a && b && !strcmp(a, b));
}

/* Takes ownership of canonical and logical even when no binding is selected. */
static XrModuleStatus checked_resource_select(XrModuleResolver *r, const char *specifier,
    const XrModuleIdentityAuthority *authority, char *canonical, char *logical,
    XrModuleId *output, char **error, bool *matched) {
    const XrModuleResourceBinding *selected = NULL;
    for (size_t i = 0; i < r->config.resource_count; ++i) {
        const XrModuleResourceBinding *binding = &r->config.resources[i];
        if (binding->canonical && !strcmp(binding->canonical, canonical)) {
            if (selected) {
                xr_free(canonical); xr_free(logical); return XR_MODULE_INVALID;
            }
            selected = binding;
        }
    }
    if (!selected) { xr_free(canonical); xr_free(logical); return XR_MODULE_OK; }
    *matched = true;
    if (authority->kind != XR_MODULE_IDENTITY_STDLIB &&
        (!specifier[2] || strchr(specifier + 2, '/') || strchr(specifier + 2, '\\'))) {
        xr_free(canonical); xr_free(logical);
        if (error) *error = xr_strdup("Checked catalog supports only direct relative imports");
        return XR_MODULE_INVALID;
    }
    if (!selected->checked || selected->authority.kind != authority->kind ||
        !resource_optional_equal(selected->authority.namespace_id, authority->namespace_id) ||
        !resource_optional_equal(selected->authority.physical_root, authority->physical_root) ||
        !selected->logical_path || strcmp(selected->logical_path, logical) || !selected->source_locator) {
        xr_free(canonical); xr_free(logical);
        if (error) *error = xr_strdup("Checked resource authority binding is inconsistent");
        return XR_MODULE_INVALID;
    }
    XrModuleId owned = {0};
    owned.kind = authority->kind == XR_MODULE_IDENTITY_STDLIB ? XR_MOD_STDLIB :
        authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    owned.canonical = canonical;
    owned.logical_path = logical;
    owned.source_path = xr_strdup(selected->source_locator);
    owned.authority.kind = authority->kind;
    owned.authority.namespace_id = authority->namespace_id ? xr_strdup(authority->namespace_id) : NULL;
    owned.authority.physical_root = authority->physical_root ? xr_strdup(authority->physical_root) : NULL;
    owned.representation = XR_MODULE_CHECKED_LIBRARY;
    owned.resource = selected;
    if (!owned.source_path || (authority->namespace_id && !owned.authority.namespace_id) ||
        (authority->physical_root && !owned.authority.physical_root)) {
        xr_module_id_cleanup(&owned); return XR_MODULE_OUT_OF_MEMORY;
    }
    *output = owned;
    return XR_MODULE_OK;
}

static XrModuleStatus resolve_checked_stdlib(XrModuleResolver *r, const char *specifier,
    XrModuleId *output, char **error, bool *matched) {
    char name[256], logical[XR_PATH_MAX];
    if (!r->config.stdlib_path || !stdlib_submodule_path(specifier + 4, name, sizeof(name)))
        return XR_MODULE_OK;
    int length = snprintf(logical, sizeof(logical), "%s.xr", specifier + 4);
    if (length < 0) return XR_MODULE_INVALID;
    if ((size_t) length >= sizeof(logical)) return XR_MODULE_BUDGET;
    /* The configured root supplies authority after the source has been removed. */
    XrPathStatus path_status;
    char *root = xr_realpath(r->config.stdlib_path, &path_status);
    if (!root) return xr_module_status_from_path(path_status);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB, name, root};
    char *canonical = NULL, *owned_logical = NULL;
    XrModuleStatus status = xr_module_identity_from_logical(&authority, logical, &canonical);
    if (status == XR_MODULE_OK) {
        owned_logical = xr_strdup(logical);
        if (!owned_logical) status = XR_MODULE_OUT_OF_MEMORY;
    }
    if (status != XR_MODULE_OK) { xr_free(canonical); xr_free(root); return status; }
    status = checked_resource_select(r, specifier, &authority, canonical, owned_logical,
        output, error, matched);
    xr_free(root);
    return status;
}

static XrModuleStatus resolve_checked_resource(XrModuleResolver *r, const char *specifier,
    const char *importer, const XrModuleIdentityAuthority *authority,
    XrModuleId *output, char **error, bool *matched) {
    *matched = false;
    if (!r->config.resource_count) return XR_MODULE_OK;
    if (!r->config.resources || !authority || !xr_module_identity_authority_valid(authority))
        return XR_MODULE_INVALID;
    if (!strncmp(specifier, "std/", 4))
        return resolve_checked_stdlib(r, specifier, output, error, matched);
    if (!importer || strncmp(specifier, "./", 2)) return XR_MODULE_OK;
    char *directory = xr_path_dirname(importer);
    if (!directory) return XR_MODULE_OUT_OF_MEMORY;
    char locator[XR_PATH_MAX];
    size_t name_length = strlen(specifier + 2);
    bool extension = name_length >= 3 && !strcmp(specifier + 2 + name_length - 3, ".xr");
    int n = snprintf(locator, sizeof(locator), "%s/%s%s", directory, specifier + 2, extension ? "" : ".xr");
    xr_free(directory);
    if (n < 0) return XR_MODULE_INVALID;
    if ((size_t) n >= sizeof(locator)) return XR_MODULE_BUDGET;
    char *canonical = NULL, *logical = NULL;
    XrModuleStatus status = xr_module_identity_from_source(authority, locator, &canonical, &logical);
    if (status != XR_MODULE_OK) return status;
    return checked_resource_select(r, specifier, authority, canonical, logical, output, error, matched);
}
