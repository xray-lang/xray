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
#include "../ir/xi_opt.h"
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

static XiFunc *lto_export_slot_in_module(const XiModule *mod, int slot) {
    if (!mod || slot < 0 || slot >= mod->nslots || !mod->slot_funcs)
        return NULL;
    return mod->slot_funcs[slot];
}

static struct XrType *lto_const_binding_type(const XiModule *module, int slot) {
    if (!module || !module->init || slot < 0)
        return NULL;
    const XiFunc *init = module->init;
    for (uint32_t bi = 0; bi < init->nblocks; bi++) {
        const XiBlock *block = init->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *store = block->values[vi];
            if (!store || store->op != XI_SET_SHARED || store->aux_int != slot || store->nargs < 1)
                continue;
            const XiValue *source = store->args[0];
            for (uint8_t depth = 0; source && depth < 8; depth++) {
                if (!((source->op == XI_BOX || source->op == XI_UNBOX ||
                       xi_copy_is_identity_alias(source) ||
                       xi_op_is_identity_forward(source->op)) &&
                      source->nargs >= 1))
                    break;
                source = source->args[0];
            }
            if (source && source->op == XI_CONST && source->type)
                return source->type;
        }
    }
    return NULL;
}

static const XiConstLiteral *lto_imported_scalar_literal(const XiLtoContext *ctx,
                                                         const XiModule *owner_module,
                                                         const XiValue *value,
                                                         struct XrType **out_binding_type) {
    if (out_binding_type)
        *out_binding_type = NULL;
    if (!ctx || !owner_module || !value)
        return NULL;

    const XiImportRef *ref = NULL;
    if (value->op == XI_GET_SHARED) {
        int64_t slot = value->aux_int;
        if (slot < 0 || slot >= owner_module->nslots || !owner_module->slot_imports)
            return NULL;
        ref = owner_module->slot_imports[slot];
    } else if (value->op == XI_IMPORT_REF) {
        ref = (const XiImportRef *) value->aux;
    } else {
        return NULL;
    }
    if (!ref || ref->resolved_mod_index < 0 || ref->resolved_shared_slot < 0 ||
        (uint32_t) ref->resolved_mod_index >= ctx->nmodules)
        return NULL;

    const XiModule *module = ctx->modules[ref->resolved_mod_index];
    int slot = ref->resolved_shared_slot;
    if (!module || !module->slot_const_literals || slot < 0 || slot >= module->nslots)
        return NULL;
    const XiConstLiteral *literal = &module->slot_const_literals[slot];
    if (literal->data_weak || literal->data_mutable)
        return NULL;
    switch (literal->kind) {
        case XI_CONST_LITERAL_NULL:
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_STRING:
            if (out_binding_type) {
                *out_binding_type = lto_const_binding_type(module, slot);
                if (!*out_binding_type) {
                    for (uint16_t i = 0; i < module->nexports; i++) {
                        if (module->exports[i].shared_slot == (uint16_t) slot &&
                            module->exports[i].value_type) {
                            *out_binding_type = module->exports[i].value_type;
                            break;
                        }
                    }
                }
            }
            return literal;
        default:
            return NULL;
    }
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
        const XiModule *module = ctx->modules[ref->resolved_mod_index];
        XiFunc *target = lto_export_slot_in_module(module, ref->resolved_shared_slot);
        if (!target)
            target = lto_export_in_module(module, ref->member_name);
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

static uint32_t resolve_bindings_in_func(XiFunc *f, const XiLtoContext *ctx,
                                         const XiModule *owner_module,
                                         bool *representations_dirty) {
    uint32_t n = 0;
    if (!f || !ctx)
        return 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            struct XrType *binding_type = NULL;
            const XiConstLiteral *literal =
                lto_imported_scalar_literal(ctx, owner_module, v, &binding_type);
            if (literal && xi_rewrite_value_to_const_literal(v, literal)) {
                if (binding_type)
                    v->type = binding_type;
                if (representations_dirty)
                    *representations_dirty = true;
                n++;
                continue;
            }

            if (v->op != XI_CALL || v->nargs < 1)
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
            n +=
                resolve_bindings_in_func(f->children[ci], ctx, owner_module, representations_dirty);
    }

    return n;
}

XR_FUNC uint32_t xi_lto_resolve_bindings(XiFunc *f, const XiLtoContext *ctx) {
    if (!f || !ctx)
        return 0;
    const XiFunc *root = f;
    while (root->parent_func)
        root = root->parent_func;
    const XiModule *owner_module = f->module;
    if (!owner_module) {
        for (uint32_t mi = 0; mi < ctx->nmodules; mi++) {
            if (ctx->modules[mi] && ctx->modules[mi]->init == root) {
                owner_module = ctx->modules[mi];
                break;
            }
        }
    }
    bool representations_dirty = false;
    uint32_t resolved = resolve_bindings_in_func(f, ctx, owner_module, &representations_dirty);
    if (representations_dirty) {
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        xi_opt_refresh_representations_with_policy(f, &policy);
    }
    return resolved;
}

XR_FUNC uint32_t xi_lto_link_modules(XiLtoContext *ctx) {
    if (!ctx)
        return 0;

    uint32_t total = 0;
    for (uint32_t mi = 0; mi < ctx->nmodules; mi++) {
        XiModule *mod = ctx->modules[mi];
        if (!mod || !mod->init)
            continue;
        bool representations_dirty = false;
        total += resolve_bindings_in_func(mod->init, ctx, mod, &representations_dirty);
        if (representations_dirty) {
            XiRepPolicy policy = xi_rep_policy_native_boundary();
            xi_opt_refresh_representations_with_policy(mod->init, &policy);
        }
    }
    return total;
}
