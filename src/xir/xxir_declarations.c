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
#include "xxir_native_callable.h"
#include "xxir_effect_contract_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_lower_control.h"
#include "xxir_type_scratch_internal.h"
#include "xxir_implementation.h"
#include "../base/xmalloc.h"
#include "../shared/xr_utf8_core.h"
#include <limits.h>


/* Types, identity metadata and signature structure precede this pass.  A
 * derived receiver is concrete; its prefix correspondence is checked against
 * the independently verified original declaration by provenance admission. */
XrXirStatus xr_xir_compile_method_signature_verify(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t index) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!module || !budget || index >= module->function_count || !module->functions)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->declarations) return XR_XIR_OK;
    if (!module->declarations->functions) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity = &module->declarations->functions[index];

    if (identity->method_kind > XR_XIR_MATERIALIZER ||
        (!!identity->nominal_owner != (identity->method_kind != XR_XIR_NON_MEMBER)))
        return XR_XIR_BAD_STRUCTURE;
    if (identity->native_callable) return xir_native_callable_verify(module,index);
    if (!identity->nominal_owner) return XR_XIR_OK;
    const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
    uint32_t owner = identity->nominal_owner-1;
    if (!table || owner >= table->count || (!table->declarations && !table->identities))
        return XR_XIR_BAD_STRUCTURE;
    if (identity->method_kind == XR_XIR_MEMBER_HELPER || identity->method_kind==XR_XIR_MATERIALIZER) return XR_XIR_OK;
    uint32_t count = table->declarations ? table->declarations[owner].parameter_count : table->identities[owner].arity;
    if (!xir_effect_evidence_is_closed(module) && count > (module->generics ? module->generics[index].parameter_count : 0))
        return XR_XIR_BAD_TYPE;
    const XrXirFunction *function = &module->functions[index];
    XrXirType subject;
    if (identity->method_kind == XR_XIR_READ_METHOD) {
        if (!function->parameter_count || !function->parameters) return XR_XIR_BAD_TYPE;
        subject = function->parameters[0];
        /* A ref receiver is the caller's mutable cell of the owner value. */
        if (xr_xir_type_is_cell(module->types, subject)) subject = xr_xir_cell_element(module->types, subject);
    } else if (identity->method_kind == XR_XIR_CONSTRUCTOR) {
        subject = function->result;
    } else return XR_XIR_OK;
    const XrXirTypeNode *receiver = xr_xir_type_node(module->types,subject);
    if (!receiver || receiver->kind != XR_XIR_TYPE_NOMINAL ||
        receiver->nominal.declaration != owner || receiver->nominal.argument_count != count ||
        (count && !receiver->nominal.arguments)) return XR_XIR_BAD_TYPE;
    if (!xir_effect_evidence_is_closed(module)) {

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
XR_FUNC bool xr_xir_module_same_scope(const XrXirDeclarations *d, uint32_t from, uint32_t target) {
    if (!d || !d->modules || from >= d->module_count || target >= d->module_count) return false;
    uint32_t a = d->modules[from].lexical_scope, b = d->modules[target].lexical_scope;
    if (a > d->module_count || b > d->module_count) return false;
    if (a && d->modules[a - 1].lexical_scope) return false;
    if (b && d->modules[b - 1].lexical_scope) return false;
    return (a ? a - 1 : from) == (b ? b - 1 : target);
}
XR_FUNC XrXirStatus xr_xir_compile_declarations_active(const XrXirCompileContext *context,
    const XrXirDeclarations *d,uint32_t *order,uint8_t *active,uint32_t *count) {
    if(!xir_compile_context_valid(context) || !d || !d->module_count || !d->modules || !order || !active || !count || *count ||
        d->root_module>=d->module_count)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xr_xir_compile_declarations_order(context,d,order);
    if(status!=XR_XIR_OK)return status;
    active[d->root_module]=1;
    for(uint32_t i=d->module_count;i>0;--i) {

        uint32_t id=order[i-1];if(!active[id])continue;
        const XrXirSourceModule *module=&d->modules[id];
        for(uint32_t dep=0;dep<module->dependency_count;++dep) {

            active[module->dependencies[dep]]=1;
        }
    }
    for(uint32_t i=0;i<d->module_count;++i) {

        uint32_t id=order[i];if(active[id])order[(*count)++]=id;
    }
    return XR_XIR_OK;
}
bool xr_xir_module_imports(const XrXirDeclarations *d, uint32_t from, uint32_t target) {
    if (from == target) return true;
    const XrXirSourceModule *module = &d->modules[from];
    for (uint32_t i = 0; i < module->dependency_count; ++i)
        if (module->dependencies[i] == target) return true;
    return false;
}
#include "xxir_nominal_access.inc.c"
XrXirStatus xr_xir_compile_declarations_order(const XrXirCompileContext *compile_context, const XrXirDeclarations *d, uint32_t *order) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    if (!d || !order || !d->module_count || !d->modules) return XR_XIR_BAD_STRUCTURE;
    uint32_t *result = xir_compile_calloc(compile_context, d->module_count, sizeof(*result), &allocation_status);
    uint8_t *done = xir_compile_calloc(compile_context, d->module_count, 1, &allocation_status);
    if (!done) { xr_compile_resources_free(result); return allocation_status; }
    XrXirStatus status = XR_XIR_OK;
    for (uint32_t step = 0; step < d->module_count; ++step) {
        uint32_t best = UINT32_MAX;
        for (uint32_t m = 0; m < d->module_count; ++m) {

            if (done[m]) continue;
            bool ready = true;
            const XrXirSourceModule *module = &d->modules[m];
            if((module->name_length && !module->name) || module->dependency_count>d->module_count ||
                (module->dependency_count && !module->dependencies)){status=XR_XIR_BAD_STRUCTURE;goto finish;}
            for (uint32_t i = 0; i < module->dependency_count; ++i) {

                uint32_t dependency=module->dependencies[i];
                if(dependency>=d->module_count){status=XR_XIR_BAD_STRUCTURE;goto finish;}
                if (!done[dependency]) ready = false;
            }
            if (ready) {

                if (best == UINT32_MAX || module_name_compare(module, &d->modules[best]) < 0) best = m;
            }
        }
        if (best == UINT32_MAX) { status = XR_XIR_BAD_STRUCTURE; goto finish; }
        done[best] = 1;
        result[step] = best;
    }
 finish:
    if (status == XR_XIR_OK) {
        size_t bytes = (size_t)d->module_count * sizeof(*result);
        memcpy(order, result, bytes);
    }
    xr_compile_resources_free(result);
    xr_compile_resources_free(done);
    return status;
}
static XrXirStatus declaration_modules(const XrXirDeclarations *d, uint32_t functions) {
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *module = &d->modules[m];
        if (!(module->name_length + (uint64_t) module->dependency_count * 4 <= SIZE_MAX))
            return XR_XIR_BUDGET;
        if (module->lexical_scope > d->module_count ||
            (module->lexical_scope && (module->lexical_scope == m + 1 ||
                d->modules[module->lexical_scope - 1].lexical_scope))) return XR_XIR_BAD_STRUCTURE;
        if (!module->name || !module->name_length || module->initializer >= functions ||
            (module->dependency_count && !module->dependencies) || module->dependency_count >= d->module_count ||
            memchr(module->name, 0, module->name_length) ||
            xr_utf8_core_scan_strict((const uint8_t *) module->name, module->name_length).error != XR_UTF8_OK)
            return XR_XIR_BAD_STRUCTURE;
        if (d->functions[module->initializer].module != m || d->functions[module->initializer].exported ||
            d->functions[module->initializer].test_role ||
            (d->functions[module->initializer].promises & XR_XIR_FUNCTION_GENERATOR) ||
            d->functions[module->initializer].nominal_owner || module->initializer == d->entry_function) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < m; ++p) {

            if (module_name_compare(module, &d->modules[p]) == 0) return XR_XIR_BAD_STRUCTURE;
        }
        for (uint32_t i = 0; i < module->dependency_count; ++i) {
            uint32_t target = module->dependencies[i];
            if (target >= d->module_count || target == m) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t p = 0; p < i; ++p) {

                if (module->dependencies[p] == target) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_declarations_verify(const XrXirCompileContext *compile_context, const XrXirDeclarations *d, const XrXirTypes *types, uint32_t functions, XrXirLinkageKind kind) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (kind != XR_XIR_PROGRAM && kind != XR_XIR_LIBRARY) return XR_XIR_BAD_STRUCTURE;
    if (!d) return kind == XR_XIR_PROGRAM ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *work = budget;
    uint64_t fixed = sizeof(*d) + (uint64_t) d->module_count * sizeof(*d->modules) +
        (uint64_t) functions * sizeof(*d->functions) + (uint64_t) d->slot_count * sizeof(*d->slots) +
        (uint64_t) d->literal_count * sizeof(*d->literals);
    if ((fixed > SIZE_MAX) || (!(fixed <= SIZE_MAX)))
        return XR_XIR_BUDGET;
    if (!d->module_count || d->module_count > functions || !d->modules || !d->functions ||
        (kind == XR_XIR_PROGRAM ? (d->root_module >= d->module_count || d->entry_function >= functions) :
         (d->root_module != UINT32_MAX || d->entry_function != UINT32_MAX)) ||
        (d->slot_count && !d->slots) || (d->literal_count && !d->literals)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < functions; ++i)
        if (d->functions[i].module >= d->module_count || d->functions[i].exported > 1 ||
            d->functions[i].method_kind > XR_XIR_MATERIALIZER ||
            (d->functions[i].native_callable && !xr_native_callable_by_id(d->functions[i].native_callable)) ||
            d->functions[i].test_role > XR_XIR_TEST_ROLE_AFTER_EACH ||
            (d->functions[i].test_role != XR_XIR_TEST_ROLE_TEST && d->functions[i].test_timeout_seconds) ||
            d->functions[i].test_timeout_seconds > INT_MAX ||
            (d->functions[i].test_role && (d->functions[i].nominal_owner || d->functions[i].cleanup_owner)) ||
            (!!d->functions[i].nominal_owner != (d->functions[i].method_kind != XR_XIR_NON_MEMBER)) ||
            (d->functions[i].promises & ~(XR_XIR_FUNCTION_NO_SUSPEND | XR_XIR_FUNCTION_GENERATOR)) ||
            ((d->functions[i].promises & XR_XIR_FUNCTION_GENERATOR) &&
             (d->functions[i].cleanup_owner || d->functions[i].test_role ||
              (d->functions[i].promises & XR_XIR_FUNCTION_NO_SUSPEND)))) return XR_XIR_BAD_STRUCTURE;
    if (kind == XR_XIR_PROGRAM && (d->functions[d->entry_function].module != d->root_module ||
        d->functions[d->entry_function].nominal_owner || d->functions[d->entry_function].test_role ||
        (d->functions[d->entry_function].promises & XR_XIR_FUNCTION_GENERATOR))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = declaration_modules(d, functions);
    if (status != XR_XIR_OK) return status;
    for (uint32_t m=0;m<d->module_count;++m) {

        if (d->modules[m].storage>XR_XIR_MODULE_BIRTH_STORAGE ||
            (d->modules[m].storage==XR_XIR_MODULE_BIRTH_STORAGE &&
                (kind!=XR_XIR_PROGRAM || m==d->root_module))) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; i < functions; ++i) {
        const XrXirFunctionIdentity *identity = &d->functions[i];
        if (identity->cleanup_owner) {
            if (identity->cleanup_owner > i || identity->exported || i == d->entry_function)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirFunctionIdentity *parent = &d->functions[identity->cleanup_owner - 1];
            if (identity->module != parent->module || identity->nominal_owner != parent->nominal_owner ||
                identity->member_access != parent->member_access) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t m = 0; m < d->module_count; ++m) {

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

        if (!name.bytes || name.length != module->name_length || memcmp(name.bytes, module->name, name.length)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; i < d->slot_count; ++i) {
        const XrXirSlot *slot = &d->slots[i];
        if (slot->module >= d->module_count || slot->mutable > 1) return XR_XIR_BAD_STRUCTURE;
        bool cell = xr_xir_type_is_cell(types, slot->type);
        XrXirType logical = cell ? xr_xir_cell_element(types, slot->type) : slot->type;
        if (cell != !!slot->mutable || (logical != XR_XIR_UNIT && logical != XR_XIR_BOOL &&
            logical != XR_XIR_RUNE && !xr_xir_type_is_number(logical) &&
            !xr_xir_type_is_owned(types, logical))) return XR_XIR_BAD_TYPE;
        if (xr_xir_type_span(types, slot->type)) return XR_XIR_BAD_TYPE;
        XrXirModule scope = {XR_XIR_BUILT, NULL, functions, d, NULL, types, NULL, kind, NULL};
        status = xr_xir_compile_type_access(budget, &scope, d->modules[slot->module].initializer, slot->type);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t i = 0; i < d->literal_count; ++i) {
        const XrXirLiteral *literal = &d->literals[i];

        if ((literal->length && !literal->bytes) ||
            xr_utf8_core_scan_strict((const uint8_t *) literal->bytes, literal->length).error != XR_UTF8_OK)
            return XR_XIR_BAD_STRUCTURE;
    }
    /* The temporary order and the traversal's done bitmap overlap. */
    uint64_t scratch = (uint64_t)d->module_count * (sizeof(uint32_t) + 1);
    if (scratch > SIZE_MAX) return XR_XIR_BUDGET;

    uint32_t *order = xir_compile_calloc(compile_context, d->module_count, sizeof(*order), &allocation_status);
    if (!order) {  return allocation_status; }
    status = xr_xir_compile_declarations_order(work, d, order);
    xr_compile_resources_free(order);
    return status;
}
static XrXirStatus declaration_poll(XrCompileRequestControl *control) {
    return control ? xir_compile_resource_status(xr_compile_request_control_poll(control)) : XR_XIR_OK;
}
static void *declaration_copy(const XrXirCompileContext *compile_context, const void *input,
    size_t bytes, XrXirStatus *allocation_status, XrCompileRequestControl *control) {
    if (!control) return xir_compile_copy(compile_context,input,bytes,allocation_status);
    if (*allocation_status!=XR_XIR_OK || !bytes) return NULL;
    if (!input) { *allocation_status=XR_XIR_BAD_STRUCTURE;return NULL; }
    void *memory=xir_compile_alloc(compile_context,bytes,allocation_status);
    if (!memory) return NULL;
    *allocation_status=xir_compile_resource_status(xir_lower_copy_span(control,memory,input,bytes));
    if (*allocation_status!=XR_XIR_OK) { xr_compile_resources_free(memory);return NULL; }
    return memory;
}
void xr_xir_compile_declarations_free(XrXirDeclarations *d) {
    if (!d) return;
    xr_xir_compile_implementations_free((XrXirImplementationTable *)d->implementations);
    if (d->modules) for (uint32_t i = 0; i < d->module_count; ++i) {
        xr_compile_resources_free((void *) d->modules[i].name);
        xr_compile_resources_free((void *) d->modules[i].dependencies);
    }
    if (d->literals) for (uint32_t i = 0; i < d->literal_count; ++i) xr_compile_resources_free((void *) d->literals[i].bytes);
    xr_compile_resources_free((void *) d->modules); xr_compile_resources_free((void *) d->functions);
    xr_compile_resources_free((void *) d->slots); xr_compile_resources_free((void *) d->literals); xr_compile_resources_free(d);
}
XrXirStatus xr_xir_compile_declarations_clone(const XrXirCompileContext *compile_context, const XrXirDeclarations *source, uint32_t functions, XrCompileRequestControl *control, XrXirDeclarations **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus allocation_status = XR_XIR_OK;

    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    if (!source) { *output = NULL; return XR_XIR_OK; }

    allocation_status=declaration_poll(control);if (allocation_status!=XR_XIR_OK) return allocation_status;
    XrXirDeclarations *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    *copy = *source;
    copy->implementations = NULL;
    copy->modules=NULL;copy->functions=NULL;copy->slots=NULL;copy->literals=NULL;
    copy->modules = xir_compile_calloc(compile_context, source->module_count, sizeof(*source->modules), &allocation_status);
    copy->functions = declaration_copy(compile_context, source->functions, (size_t) functions * sizeof(*source->functions), &allocation_status,control);
    copy->slots = declaration_copy(compile_context, source->slots, (size_t) source->slot_count * sizeof(*source->slots), &allocation_status,control);
    copy->literals = source->literal_count ? xir_compile_calloc(compile_context, source->literal_count, sizeof(*source->literals), &allocation_status) : NULL;
    if (!copy->modules || !copy->functions || (source->slot_count && !copy->slots) ||
        (source->literal_count && !copy->literals)) goto failed;
    for (uint32_t i = 0; i < source->module_count; ++i) {
        allocation_status=declaration_poll(control);if (allocation_status!=XR_XIR_OK) goto failed;
        XrXirSourceModule *to = &((XrXirSourceModule *) copy->modules)[i];
        const XrXirSourceModule *from = &source->modules[i];
        *to = *from;to->name=NULL;to->dependencies=NULL;
        to->name = declaration_copy(compile_context, from->name, from->name_length, &allocation_status,control);
        to->dependencies = declaration_copy(compile_context, from->dependencies, (size_t) from->dependency_count * 4, &allocation_status,control);
        if (!to->name || (from->dependency_count && !to->dependencies)) goto failed;
    }
    for (uint32_t i = 0; i < source->literal_count; ++i) {
        allocation_status=declaration_poll(control);if (allocation_status!=XR_XIR_OK) goto failed;
        XrXirLiteral *to = &((XrXirLiteral *) copy->literals)[i];
        const XrXirLiteral *from = &source->literals[i];
        to->length = from->length;
        to->bytes = declaration_copy(compile_context, from->bytes, from->length, &allocation_status,control);
        if (from->length && !to->bytes) goto failed;
    }
    allocation_status=declaration_poll(control);if (allocation_status!=XR_XIR_OK) goto failed;
    XrXirImplementationTable *implementations = NULL;
    XrXirStatus status = xr_xir_compile_implementations_copy_verified(compile_context, source->implementations, control, &implementations);
    if (status != XR_XIR_OK) { xr_xir_compile_declarations_free(copy); return status; }
    copy->implementations = implementations;
    allocation_status=declaration_poll(control);if (allocation_status!=XR_XIR_OK) goto failed;
    *output = copy;
    return XR_XIR_OK;
 failed:
    xr_xir_compile_declarations_free(copy);
    return allocation_status;
}
