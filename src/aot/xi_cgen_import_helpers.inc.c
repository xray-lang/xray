/* ========== Cross-module Import Resolution ========== */

/* Derive the relative import path from exporter to importer directory.
 * E.g. "/a/b/math.xr" from dir "/a/b" becomes "./math".  Caller must free. */
static char *cg_derive_import_string(const char *target_path, const char *importer_dir) {
    size_t dir_len = strlen(importer_dir);
    if (strncmp(target_path, importer_dir, dir_len) == 0 && target_path[dir_len] == '/') {
        const char *filename = target_path + dir_len + 1;
        size_t flen = strlen(filename);
        if (flen > 3 && strcmp(filename + flen - 3, ".xr") == 0)
            flen -= 3;
        char *result = (char *) xr_malloc(2 + flen + 1);
        if (!result)
            return NULL;
        result[0] = '.';
        result[1] = '/';
        memcpy(result + 2, filename, flen);
        result[2 + flen] = '\0';
        return result;
    }
    const char *base = strrchr(target_path, '/');
    base = base ? base + 1 : target_path;
    size_t blen = strlen(base);
    if (blen > 3 && strcmp(base + blen - 3, ".xr") == 0)
        blen -= 3;
    char *result = (char *) xr_malloc(2 + blen + 1);
    if (!result)
        return NULL;
    result[0] = '.';
    result[1] = '/';
    memcpy(result + 2, base, blen);
    result[2 + blen] = '\0';
    return result;
}

/* Add one entry to the internal import table. */
static void cg_add_import(XiCgenCtx *ctx, const char *module_path, const char *member_name,
                          const char *target_mod_name, int shared_slot, const XiFunc *target_func,
                          const XiClassData *target_class, const XiFunc *exporter_func) {
    if (ctx->nimports >= CG_MAX_IMPORTS)
        return;
    CgImportEntry *e = &ctx->imports[ctx->nimports++];
    e->module_path = module_path;
    e->member_name = member_name;
    e->target_mod_name = target_mod_name;
    e->shared_slot = shared_slot;
    e->target_func = target_func;
    e->target_class = target_class;
    e->exporter_func = exporter_func;
}

XR_FUNC void xi_cgen_resolve_module_imports(XiCgenCtx *ctx, XiModule **modules, int nmodules) {
    XR_DCHECK(ctx != NULL, "xi_cgen_resolve_module_imports: NULL ctx");
    ctx->all_modules = modules;
    ctx->all_nmodules = modules && nmodules > 0 ? nmodules : 0;
    ctx->nshared_native_exports = 0;
    memset(ctx->shared_native_exports, 0, sizeof(ctx->shared_native_exports));
    if (!modules || nmodules <= 1)
        return;

    ctx->nimports = 0;
    memset(ctx->imports, 0, sizeof(ctx->imports));

    for (int exporter = 0; exporter < nmodules; exporter++) {
        XiModule *emod = modules[exporter];
        if (!emod || emod->nexports == 0)
            continue;

        for (int importer = 0; importer < nmodules; importer++) {
            if (importer == exporter)
                continue;
            XR_DCHECK(modules[importer] != NULL,
                      "xi_cgen_resolve_module_imports: NULL importer module");

            /* Derive importer directory from its path */
            char importer_dir[1024];
            const char *imp_path = modules[importer]->path;
            if (!imp_path)
                continue;
            strncpy(importer_dir, imp_path, sizeof(importer_dir) - 1);
            importer_dir[sizeof(importer_dir) - 1] = '\0';
            char *slash = strrchr(importer_dir, '/');
            if (slash)
                *slash = '\0';

            char *import_str = cg_derive_import_string(emod->path, importer_dir);
            if (!import_str)
                continue;

            for (uint16_t ei = 0; ei < emod->nexports; ei++) {
                const XiModuleExport *exp = &emod->exports[ei];
                const XiFunc *target_fn = exp->function;
                const XiClassData *target_cd = exp->class_data;

                /* For class exports, resolve constructor if not already set */
                if (target_cd && !target_fn && target_cd->methods) {
                    for (uint16_t mi = 0; mi < target_cd->nmethod; mi++) {
                        if (target_cd->methods[mi].is_constructor && target_cd->child_idx &&
                            mi < target_cd->ninst + target_cd->nstat) {
                            uint16_t idx = target_cd->child_idx[mi];
                            if (idx < emod->init->nchildren) {
                                target_fn = emod->init->children[idx];
                                break;
                            }
                        }
                    }
                }

                cg_add_import(ctx, import_str, exp->name, emod->name, (int) exp->shared_slot,
                              target_fn, target_cd, emod->init);
            }
            /* import_str is kept in the table for the short-lived AOT process. */
        }
    }

    cg_prepare_sync_go_targets_for_modules(ctx, modules, nmodules);
}
