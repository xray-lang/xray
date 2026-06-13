/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_lto.c - Cross-module LTO for Xi IR
 */

#include "xi_lto.h"
#include "../ir/xi.h"
#include <string.h>

/* Resolve an exported function within a single module by member name. */
static XiFunc *lto_export_in_module(const XiModule *mod, const char *name) {
    if (!mod || !name)
        return NULL;
    for (uint16_t ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        if (exp->function && exp->name && strcmp(exp->name, name) == 0)
            return exp->function;
    }
    return NULL;
}

/* Module-aware import resolution.
 *
 * The import's target module identity is resolved before LTO runs
 * (xi_resolve_imports fills resolved_mod_index), so direct binding keys on
 * that identity, never on a global by-name match.  A name-only lookup would
 * misbind when two modules export the same member name (e.g. both export
 * `run`): a flattened table returns whichever entry appears first and
 * silently calls the wrong module's function.  Resolution therefore stays
 * scoped to the resolved module, with a module-path match as the only
 * fallback. */
static XiFunc *lto_resolve_import(const XiLtoContext *ctx, const XiImportRef *ref) {
    if (!ctx || !ref || !ref->member_name)
        return NULL;

    if (ref->resolved_mod_index >= 0 && (uint32_t) ref->resolved_mod_index < ctx->nmodules) {
        XiFunc *target =
            lto_export_in_module(ctx->modules[ref->resolved_mod_index], ref->member_name);
        if (target)
            return target;
    }

    if (ref->module_path) {
        for (uint32_t mi = 0; mi < ctx->nmodules; mi++) {
            const XiModule *mod = ctx->modules[mi];
            if (!mod)
                continue;
            if ((mod->path && strcmp(mod->path, ref->module_path) == 0) ||
                (mod->name && strcmp(mod->name, ref->module_path) == 0))
                return lto_export_in_module(mod, ref->member_name);
        }
    }

    return NULL;
}

XR_FUNC bool xi_lto_context_init(XiLtoContext *ctx, XiModule **modules, uint32_t nmodules) {
    if (!ctx || !modules || nmodules == 0)
        return false;

    memset(ctx, 0, sizeof(*ctx));
    ctx->modules = modules;
    ctx->nmodules = nmodules;
    return true;
}

XR_FUNC void xi_lto_context_free(XiLtoContext *ctx) {
    if (!ctx)
        return;
    memset(ctx, 0, sizeof(*ctx));
}

static uint32_t resolve_in_func(XiFunc *f, const XiLtoContext *ctx) {
    uint32_t n = 0;
    if (!f || !ctx)
        return 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CALL || v->nargs < 1)
                continue;

            XiValue *callee = v->args[0];
            if (!callee || callee->op != XI_IMPORT_REF || !callee->aux)
                continue;

            XiImportRef *ref = (XiImportRef *) callee->aux;
            if (!ref->member_name)
                continue;

            XiFunc *target = lto_resolve_import(ctx, ref);
            if (!target)
                continue;

            XiValue *closure = xi_value_new(f, blk, XI_CLOSURE_NEW, v->type, 0);
            if (!closure)
                continue;
            closure->aux = target;
            v->args[0] = closure;
            n++;
        }
    }

    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (f->children[ci])
            n += resolve_in_func(f->children[ci], ctx);
    }

    return n;
}

XR_FUNC uint32_t xi_lto_resolve_calls(XiFunc *f, const XiLtoContext *ctx) {
    return resolve_in_func(f, ctx);
}

XR_FUNC uint32_t xi_lto_link_modules(XiLtoContext *ctx) {
    if (!ctx)
        return 0;

    uint32_t total = 0;
    for (uint32_t mi = 0; mi < ctx->nmodules; mi++) {
        XiModule *mod = ctx->modules[mi];
        if (!mod || !mod->init)
            continue;
        total += resolve_in_func(mod->init, ctx);
    }
    return total;
}
