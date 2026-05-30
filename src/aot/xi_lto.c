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
#include "../base/xmalloc.h"
#include <string.h>

static XiFunc *resolve_export(const XiLtoContext *ctx, const char *name) {
    if (!ctx || !name)
        return NULL;
    for (uint32_t i = 0; i < ctx->nexports; i++) {
        if (ctx->export_names[i] && strcmp(ctx->export_names[i], name) == 0)
            return ctx->export_funcs[i];
    }
    return NULL;
}

XR_FUNC bool xi_lto_context_init(XiLtoContext *ctx, XiModule **modules, uint32_t nmodules) {
    if (!ctx || !modules || nmodules == 0)
        return false;

    memset(ctx, 0, sizeof(*ctx));
    ctx->modules = modules;
    ctx->nmodules = nmodules;

    uint32_t cap = 16;
    ctx->export_funcs = (XiFunc **) xr_malloc(cap * sizeof(XiFunc *));
    ctx->export_names = (const char **) xr_malloc(cap * sizeof(const char *));
    if (!ctx->export_funcs || !ctx->export_names) {
        xi_lto_context_free(ctx);
        return false;
    }

    for (uint32_t mi = 0; mi < nmodules; mi++) {
        XiModule *mod = modules[mi];
        if (!mod)
            continue;
        for (uint16_t ei = 0; ei < mod->nexports; ei++) {
            XiModuleExport *exp = &mod->exports[ei];
            if (!exp->function || !exp->name)
                continue;
            if (ctx->nexports >= cap) {
                cap *= 2;
                XiFunc **nf = (XiFunc **) xr_realloc(ctx->export_funcs, cap * sizeof(XiFunc *));
                const char **nn =
                    (const char **) xr_realloc(ctx->export_names, cap * sizeof(const char *));
                if (!nf || !nn) {
                    xi_lto_context_free(ctx);
                    return false;
                }
                ctx->export_funcs = nf;
                ctx->export_names = nn;
            }
            ctx->export_funcs[ctx->nexports] = exp->function;
            ctx->export_names[ctx->nexports] = exp->name;
            ctx->nexports++;
        }
    }

    return true;
}

XR_FUNC void xi_lto_context_free(XiLtoContext *ctx) {
    if (!ctx)
        return;
    xr_free(ctx->export_funcs);
    xr_free(ctx->export_names);
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

            XiFunc *target = resolve_export(ctx, ref->member_name);
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
