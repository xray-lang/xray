/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_declarations.c - Bounded module graphs and owned declaration snapshots
 *
 * KEY CONCEPT:
 *   A single declaration validator defines closure, permissions and init order.
 */

#include "xxir_types.h"
#include "xxir_implementation.h"
#include "../base/xmalloc.h"
#include "../shared/xr_utf8_core.h"

static bool declaration_spend(uint64_t *budget, uint64_t amount) {
    if (amount > *budget) return false;
    *budget -= amount;
    return true;
}
/* Types, identity metadata and signature structure precede this pass.  A
 * derived receiver is concrete; its prefix correspondence is checked against
 * the independently verified original declaration by provenance admission. */
XrXirStatus xr_xir_method_signature_verify(const XrXirModule *module, uint32_t index,
    XrXirBudget *budget) {
    if (!module || !budget || index >= module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->declarations) return XR_XIR_OK;
    if (!module->declarations->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[index];
    if (!declaration_spend(&budget->work,1)) return XR_XIR_BUDGET;
    if (identity->method_kind > XR_XIR_MEMBER_HELPER ||
        (!!identity->nominal_owner != (identity->method_kind != XR_XIR_NON_MEMBER)))
        return XR_XIR_BAD_STRUCTURE;
    if (!identity->nominal_owner) return XR_XIR_OK;
    const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
    uint32_t owner = identity->nominal_owner-1;
    if (!table || owner >= table->count || (!table->declarations && !table->identities))
        return XR_XIR_BAD_STRUCTURE;
    if (identity->method_kind == XR_XIR_MEMBER_HELPER) return XR_XIR_OK;
    uint32_t count = table->declarations ? table->declarations[owner].parameter_count : table->identities[owner].arity;
    if (!module->provenance && count > (module->generics ? module->generics[index].parameter_count : 0))
        return XR_XIR_BAD_TYPE;
    const XrXirFunction *function = &module->functions[index];
    XrXirType subject;
    if (identity->method_kind == XR_XIR_READ_METHOD) {
        if (!function->parameter_count || !function->parameters) return XR_XIR_BAD_TYPE;
        subject = function->parameters[0];
    } else if (identity->method_kind == XR_XIR_CONSTRUCTOR) {
        subject = function->result;
    } else return XR_XIR_OK;
    const XrXirTypeNode *receiver = xr_xir_type_node(module->types,subject);
    if (!receiver || receiver->kind != XR_XIR_TYPE_NOMINAL ||
        receiver->nominal.declaration != owner || receiver->nominal.argument_count != count ||
        (count && !receiver->nominal.arguments)) return XR_XIR_BAD_TYPE;
    if (!module->provenance) {
        if (!declaration_spend(&budget->work,count)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < count; ++a)
            if (receiver->nominal.arguments[a] != (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+a))
                return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static int module_name_compare(const XrXirSourceModule *a, const XrXirSourceModule *b) {
    uint32_t count = a->name_length < b->name_length ? a->name_length : b->name_length;
    int order = memcmp(a->name, b->name, count);
    return order ? order : a->name_length < b->name_length ? -1 : a->name_length != b->name_length;
}
bool xr_xir_module_imports(const XrXirDeclarations *d, uint32_t from, uint32_t target) {
    if (from == target) return true;
    const XrXirSourceModule *module = &d->modules[from];
    for (uint32_t i = 0; i < module->dependency_count; ++i)
        if (module->dependencies[i] == target) return true;
    return false;
}
#include "xxir_nominal_access.inc.c"
XrXirStatus xr_xir_declarations_order(const XrXirDeclarations *d, uint32_t *order, uint64_t *work) {
    uint8_t *done = xr_calloc(d->module_count, 1);
    if (!done) return XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t step = 0; step < d->module_count; ++step) {
        uint32_t best = UINT32_MAX;
        for (uint32_t m = 0; m < d->module_count; ++m) {
            if (!declaration_spend(work, 1)) { status = XR_XIR_BUDGET; goto finish; }
            if (done[m]) continue;
            bool ready = true;
            const XrXirSourceModule *module = &d->modules[m];
            for (uint32_t i = 0; i < module->dependency_count; ++i) {
                if (!declaration_spend(work, 1)) { status = XR_XIR_BUDGET; goto finish; }
                if (!done[module->dependencies[i]]) ready = false;
            }
            if (ready) {
                if (!declaration_spend(work, module->name_length)) { status = XR_XIR_BUDGET; goto finish; }
                if (best == UINT32_MAX || module_name_compare(module, &d->modules[best]) < 0) best = m;
            }
        }
        if (best == UINT32_MAX) { status = XR_XIR_BAD_STRUCTURE; goto finish; }
        done[best] = 1;
        order[step] = best;
    }
 finish:
    xr_free(done);
    return status;
}
static XrXirStatus declaration_modules(const XrXirDeclarations *d, uint32_t functions,
                                       uint64_t *bytes, uint64_t *work) {
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *module = &d->modules[m];
        if (!declaration_spend(bytes, module->name_length + (uint64_t) module->dependency_count * 4) ||
            !declaration_spend(work, module->name_length + (uint64_t) module->dependency_count + m + 1))
            return XR_XIR_BUDGET;
        if (!module->name || !module->name_length || module->initializer >= functions ||
            (module->dependency_count && !module->dependencies) || module->dependency_count >= d->module_count ||
            memchr(module->name, 0, module->name_length) ||
            xr_utf8_core_scan_strict((const uint8_t *) module->name, module->name_length).error != XR_UTF8_OK)
            return XR_XIR_BAD_STRUCTURE;
        if (d->functions[module->initializer].module != m || d->functions[module->initializer].exported ||
            d->functions[module->initializer].nominal_owner || module->initializer == d->entry_function) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < m; ++p) {
            if (!declaration_spend(work, module->name_length)) return XR_XIR_BUDGET;
            if (module_name_compare(module, &d->modules[p]) == 0) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t i = 0; i < module->dependency_count; ++i) {
            uint32_t target = module->dependencies[i];
            if (target >= d->module_count || target == m) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t p = 0; p < i; ++p) {
                if (!declaration_spend(work, 1)) return XR_XIR_BUDGET;
                if (module->dependencies[p] == target) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_declarations_verify(const XrXirDeclarations *d, const XrXirTypes *types, uint32_t functions, XrXirLinkageKind kind,
                                      XrXirBudget *budget) {
    if (kind != XR_XIR_PROGRAM && kind != XR_XIR_LIBRARY) return XR_XIR_BAD_STRUCTURE;
    if (!d) return kind == XR_XIR_PROGRAM ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    uint64_t *bytes = &budget->metadata_bytes, *work = &budget->work;
    uint64_t fixed = sizeof(*d) + (uint64_t) d->module_count * sizeof(*d->modules) +
        (uint64_t) functions * sizeof(*d->functions) + (uint64_t) d->slot_count * sizeof(*d->slots) +
        (uint64_t) d->literal_count * sizeof(*d->literals);
    if (fixed > SIZE_MAX || !declaration_spend(bytes, fixed) ||
        !declaration_spend(work, (uint64_t) d->module_count + functions + d->slot_count + d->literal_count))
        return XR_XIR_BUDGET;
    if (!d->module_count || d->module_count > functions || !d->modules || !d->functions ||
        (kind == XR_XIR_PROGRAM ? (d->root_module >= d->module_count || d->entry_function >= functions) :
         (d->root_module != UINT32_MAX || d->entry_function != UINT32_MAX)) ||
        (d->slot_count && !d->slots) || (d->literal_count && !d->literals)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < functions; ++i)
        if (d->functions[i].module >= d->module_count || d->functions[i].exported > 1 ||
            d->functions[i].method_kind > XR_XIR_MEMBER_HELPER ||
            (!!d->functions[i].nominal_owner != (d->functions[i].method_kind != XR_XIR_NON_MEMBER)) ||
            (d->functions[i].promises & ~XR_XIR_FUNCTION_NO_SUSPEND)) return XR_XIR_BAD_STRUCTURE;
    if (kind == XR_XIR_PROGRAM && (d->functions[d->entry_function].module != d->root_module ||
        d->functions[d->entry_function].nominal_owner)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = declaration_modules(d, functions, bytes, work);
    if (status != XR_XIR_OK) return status;
    for (uint32_t i = 0; i < functions; ++i) {
        const XrXirFunctionIdentity *identity = &d->functions[i];
        if (identity->cleanup_owner) {
            if (identity->cleanup_owner > i || identity->exported || i == d->entry_function)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirFunctionIdentity *parent = &d->functions[identity->cleanup_owner - 1];
            if (identity->module != parent->module || identity->nominal_owner != parent->nominal_owner ||
                identity->member_access != parent->member_access) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t m = 0; m < d->module_count; ++m) {
                if (!declaration_spend(work, 1)) return XR_XIR_BUDGET;
                if (d->modules[m].initializer == i) return XR_XIR_BAD_STRUCTURE;
            }
        }
        uint32_t owner = d->functions[i].nominal_owner;
        uint32_t access = d->functions[i].member_access;
        if (access > XR_XIR_MEMBER_PROTECTED ||
            (access && (!owner || d->functions[i].exported))) return XR_XIR_BAD_STRUCTURE;
        if (!owner) continue;
        if (!types || !types->nominals || owner > types->nominals->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirNominalTable *table = types->nominals;
        if (!table->declarations && !table->identities) return XR_XIR_BAD_STRUCTURE;
        XrXirLiteral name = table->declarations ? table->declarations[owner - 1].module :
            table->identities[owner - 1].module;
        const XrXirSourceModule *module = &d->modules[d->functions[i].module];
        if (!declaration_spend(work, name.length)) return XR_XIR_BUDGET;
        if (!name.bytes || name.length != module->name_length || memcmp(name.bytes, module->name, name.length)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; i < d->slot_count; ++i) {
        const XrXirSlot *slot = &d->slots[i];
        if (slot->module >= d->module_count || slot->mutable > 1 ||
            (slot->mutable && slot->module != d->root_module)) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_is_cell(types, slot->type) || (slot->type != XR_XIR_UNIT && slot->type != XR_XIR_BOOL && !xr_xir_type_is_number((XrXirType) slot->type) && !xr_xir_type_is_owned(types, slot->type)))
            return XR_XIR_BAD_TYPE;
        if (xr_xir_type_span(types, slot->type)) return XR_XIR_BAD_TYPE;
        XrXirModule scope = {XR_XIR_BUILT, NULL, functions, d, NULL, types, NULL, kind, NULL};
        status = xr_xir_type_access(&scope, d->modules[slot->module].initializer, slot->type, budget);
        if (status != XR_XIR_OK) return status;
        if (slot->mutable && slot->type == XR_XIR_ATOMIC_I64) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; i < d->literal_count; ++i) {
        const XrXirLiteral *literal = &d->literals[i];
        if (!declaration_spend(bytes, literal->length) || !declaration_spend(work, literal->length))
            return XR_XIR_BUDGET;
        if ((literal->length && !literal->bytes) ||
            xr_utf8_core_scan_strict((const uint8_t *) literal->bytes, literal->length).error != XR_UTF8_OK)
            return XR_XIR_BAD_STRUCTURE;
    }
    /* The temporary order and the traversal's done bitmap overlap. */
    uint64_t scratch = (uint64_t)d->module_count * (sizeof(uint32_t) + 1);
    if (scratch > SIZE_MAX || scratch > budget->scratch_bytes) return XR_XIR_BUDGET;
    budget->scratch_bytes -= scratch;
    uint32_t *order = xr_calloc(d->module_count, sizeof(*order));
    if (!order) { budget->scratch_bytes += scratch; return XR_XIR_OUT_OF_MEMORY; }
    status = xr_xir_declarations_order(d, order, work);
    xr_free(order); budget->scratch_bytes += scratch;
    return status;
}
static void *declaration_copy(const void *input, size_t bytes) {
    if (!bytes) return NULL;
    void *copy = xr_malloc(bytes);
    if (copy) memcpy(copy, input, bytes);
    return copy;
}
void xr_xir_declarations_free(XrXirDeclarations *d) {
    if (!d) return;
    xr_xir_implementations_free((XrXirImplementationTable *)d->implementations);
    if (d->modules) for (uint32_t i = 0; i < d->module_count; ++i) {
        xr_free((void *) d->modules[i].name);
        xr_free((void *) d->modules[i].dependencies);
    }
    if (d->literals) for (uint32_t i = 0; i < d->literal_count; ++i) xr_free((void *) d->literals[i].bytes);
    xr_free((void *) d->modules); xr_free((void *) d->functions);
    xr_free((void *) d->slots); xr_free((void *) d->literals); xr_free(d);
}
XrXirStatus xr_xir_declarations_clone(const XrXirDeclarations *source, uint32_t functions,
                                     XrXirDeclarations **output) {
    *output = NULL;
    if (!source) return XR_XIR_OK;
    XrXirDeclarations *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    *copy = *source;
    copy->implementations = NULL;
    copy->modules = xr_calloc(source->module_count, sizeof(*source->modules));
    copy->functions = declaration_copy(source->functions, (size_t) functions * sizeof(*source->functions));
    copy->slots = declaration_copy(source->slots, (size_t) source->slot_count * sizeof(*source->slots));
    copy->literals = source->literal_count ? xr_calloc(source->literal_count, sizeof(*source->literals)) : NULL;
    if (!copy->modules || !copy->functions || (source->slot_count && !copy->slots) ||
        (source->literal_count && !copy->literals)) goto failed;
    for (uint32_t i = 0; i < source->module_count; ++i) {
        XrXirSourceModule *to = &((XrXirSourceModule *) copy->modules)[i];
        const XrXirSourceModule *from = &source->modules[i];
        *to = *from;
        to->name = declaration_copy(from->name, from->name_length);
        to->dependencies = declaration_copy(from->dependencies, (size_t) from->dependency_count * 4);
        if (!to->name || (from->dependency_count && !to->dependencies)) goto failed;
    }
    for (uint32_t i = 0; i < source->literal_count; ++i) {
        XrXirLiteral *to = &((XrXirLiteral *) copy->literals)[i];
        const XrXirLiteral *from = &source->literals[i];
        to->length = from->length;
        to->bytes = declaration_copy(from->bytes, from->length);
        if (from->length && !to->bytes) goto failed;
    }
    XrXirImplementationTable *implementations = NULL;
    XrXirStatus status = xr_xir_implementations_copy_verified(source->implementations, &implementations);
    if (status != XR_XIR_OK) { xr_xir_declarations_free(copy); return status; }
    copy->implementations = implementations;
    *output = copy;
    return XR_XIR_OK;
 failed:
    xr_xir_declarations_free(copy);
    return XR_XIR_OUT_OF_MEMORY;
}
