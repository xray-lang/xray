/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program.c - Validated immutable program and code lease ownership
 *
 * KEY CONCEPT:
 *   Instances retain immutable descriptors until execution and cleanup finish.
 */

#include "xxir_program_internal.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

static bool program_value_type(const XrXirProgramSpec *spec, XrXirType type) {
    if (xr_xir_type_is_cell(spec->types, type)) type = xr_xir_cell_element(spec->types, type);
    return type == XR_XIR_BOOL || xr_xir_type_is_number(type) || xr_xir_type_is_owned(spec->types, type);
}
static XrXirStatus program_shape(const XrXirProgramSpec *spec, uint64_t *bytes, uint64_t *work) {
    if (!spec || !spec->entries || !spec->entry_count || spec->entry_count > 65535 ||
        !spec->declarations || (!!spec->code.owner != !!spec->code.release)) return XR_XIR_BAD_STRUCTURE;
    if (spec->abi_version != XR_XIR_PROGRAM_ABI_VERSION ||
        spec->target.architecture != XR_XIR_ARCH_X86_64 || spec->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return XR_XIR_BAD_LAYOUT;
    if (spec->declarations->implementations || (spec->types && spec->types->interfaces))
        return XR_XIR_BAD_STAGE;
    uint64_t fixed = sizeof(XrXirProgram) + (uint64_t) spec->entry_count * sizeof(XrXirCallEntry) +
        (uint64_t) spec->declarations->module_count * (sizeof(uint32_t) * 2 + 1);
    if (fixed > *bytes || fixed > SIZE_MAX) return XR_XIR_BUDGET;
    *bytes -= fixed;
    XrXirBudget signature_budget = {0};
    signature_budget.metadata_bytes = *bytes; signature_budget.work = *work;
    signature_budget.parameters = 65536;
    XrXirStatus status = xr_xir_types_structure_verify(spec->types, &signature_budget);
    if (status != XR_XIR_OK) return status;
    if (spec->types) for (uint32_t t = 0; t < spec->types->count; ++t)
        if (spec->types->nodes[t].parameter_span) return XR_XIR_BAD_TYPE;
    /* The runtime arena charges its actual header and owned descriptors against
     * the remaining metadata budget after the other program allocations. */
    *work = signature_budget.work;
    status = xr_xir_declarations_verify(spec->declarations, spec->types, spec->entry_count, bytes, work);
    if (status != XR_XIR_OK) return status;
    for (uint32_t s = 0; s < spec->declarations->slot_count; ++s) {
        const XrXirSlot *slot = &spec->declarations->slots[s];
        if (xr_xir_type_is_cell(spec->types, slot->type) || !program_value_type(spec, slot->type) || (xr_xir_type_is_callable(spec->types, slot->type) &&
            slot->module != spec->declarations->root_module)) return XR_XIR_BAD_TYPE;
    }
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        const XrXirCallEntry *entry = &spec->entries[i];
        if (entry->abi_version != XR_XIR_CALL_ABI_VERSION) return XR_XIR_BAD_LAYOUT;
        if ((entry->flags & ~XR_XIR_ENTRY_EXIT) ||
            entry->cleanup_owner != spec->declarations->functions[i].cleanup_owner ||
            (entry->cleanup_owner && (entry->result != XR_XIR_UNIT ||
                !(spec->entries[entry->cleanup_owner - 1].flags & XR_XIR_ENTRY_EXIT)))) return XR_XIR_BAD_STRUCTURE;
        if (!entry->resume || (entry->parameter_count && !entry->parameters)) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_is_cell(spec->types, entry->result) || (entry->result != XR_XIR_UNIT && !program_value_type(spec, entry->result))) return XR_XIR_BAD_TYPE;
        uint64_t parameter_bytes = (uint64_t) entry->parameter_count * sizeof(XrXirType);
        if (parameter_bytes > *bytes || parameter_bytes > SIZE_MAX || entry->parameter_count > *work)
            return XR_XIR_BUDGET;
        *bytes -= parameter_bytes; *work -= entry->parameter_count;
        for (uint32_t p = 0; p < entry->parameter_count; ++p) {
            if (!program_value_type(spec, entry->parameters[p]) ||
                (xr_xir_type_is_cell(spec->types, entry->parameters[p]) && spec->declarations->functions[i].exported)) return XR_XIR_BAD_TYPE;
        }
    }
    const XrXirDeclarations *d = spec->declarations;
    const XrXirCallEntry *entry = &spec->entries[d->entry_function];
    if (entry->parameter_count || entry->result != XR_XIR_I64) return XR_XIR_BAD_TYPE;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirCallEntry *init = &spec->entries[d->modules[m].initializer];
        if (init->parameter_count || init->result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static void program_dispose(XrXirProgram *program) {
    if (program->code.release) program->code.release(program->code.owner);
    if (program->entries) for (uint32_t i = 0; i < program->entry_count; ++i)
        xr_free((void *) program->entries[i].parameters);
    xr_free(program->entries);
    xr_xir_type_arena_drop(program->arena);
    xr_xir_declarations_free(program->declarations);
    xr_free(program->order);
    xr_free(program->active_modules);
    xr_free(program->module_slots);
    xr_free(program);
}
static XrXirStatus program_execution_order(XrXirProgram *program, uint64_t *work) {
    const XrXirDeclarations *d = program->declarations;
    XrXirStatus status = xr_xir_declarations_order(d, program->order, work);
    if (status != XR_XIR_OK) return status;
    program->active_modules[d->root_module] = 1;
    for (uint32_t i = d->module_count; i > 0; --i) {
        uint32_t id = program->order[i - 1];
        if (!*work) return XR_XIR_BUDGET;
        --*work;
        if (!program->active_modules[id]) continue;
        const XrXirSourceModule *module = &d->modules[id];
        if (module->dependency_count > *work) return XR_XIR_BUDGET;
        *work -= module->dependency_count;
        for (uint32_t dep = 0; dep < module->dependency_count; ++dep)
            program->active_modules[module->dependencies[dep]] = 1;
    }
    if (d->module_count > *work) return XR_XIR_BUDGET;
    *work -= d->module_count;
    for (uint32_t i = 0; i < d->module_count; ++i) {
        uint32_t id = program->order[i];
        if (program->active_modules[id]) program->order[program->initialization_count++] = id;
    }
    return XR_XIR_OK;
}
XrXirStatus xr_xir_program_seal(const XrXirProgramSpec *spec, XrXirProgramBudget limits, XrXirProgram **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    uint64_t byte_limit = limits.metadata_bytes, proof_limit = byte_limit;
    uint64_t work = limits.work;
    XrXirStatus status = program_shape(spec, &byte_limit, &work);
    if (status != XR_XIR_OK) return status;
    XrXirBudget phase = xr_xir_default_budget();
    phase.metadata_bytes = proof_limit / 7;
    phase.scratch_bytes = phase.metadata_bytes;
    phase.work = work / 16;
    work -= phase.work * 15;
    status = xr_xir_program_proof_verify(spec, &spec->proof, &phase, &phase, proof_limit, &work);
    if (status != XR_XIR_OK) return status;
    XrXirProgram *program = xr_calloc(1, sizeof(*program));
    if (!program) return XR_XIR_OUT_OF_MEMORY;
    atomic_init(&program->references, 1);
    program->entry_count = spec->entry_count;
    program->entries = xr_calloc(spec->entry_count, sizeof(*program->entries));
    program->order = xr_calloc(spec->declarations->module_count, sizeof(*program->order));
    program->active_modules = xr_calloc(spec->declarations->module_count, 1);
    program->module_slots = xr_calloc(spec->declarations->module_count, sizeof(*program->module_slots));
    if (!program->entries || !program->order || !program->active_modules || !program->module_slots) {
        status = XR_XIR_OUT_OF_MEMORY; goto failed;
    }
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        program->entries[i] = spec->entries[i];
        program->entries[i].parameters = NULL;
        if (spec->entries[i].parameter_count) {
            size_t size = (size_t) spec->entries[i].parameter_count * sizeof(XrXirType);
            XrXirType *types = xr_malloc(size);
            if (!types) { status = XR_XIR_OUT_OF_MEMORY; goto failed; }
            memcpy(types, spec->entries[i].parameters, size);
            program->entries[i].parameters = types;
        }
    }
    if (spec->types) {
        XrXirDomain *domain = NULL;
        XrXirValueStatus value_status = xr_xir_domain_new(byte_limit, &domain);
        if (value_status == XR_XIR_VALUE_OK) {
            XrXirBudget budget = {.parameters = 65536, .metadata_bytes = byte_limit,
                .scratch_bytes = byte_limit, .work = work};
            value_status = xr_xir_type_arena_new(domain, spec->types, &budget, &program->arena);
            if (value_status == XR_XIR_VALUE_OK) work = budget.work;
        }
        xr_xir_domain_drop(domain);
        if (value_status != XR_XIR_VALUE_OK) {
            status = value_status == XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
                value_status == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_BAD_TYPE : XR_XIR_BUDGET;
            goto failed;
        }
        program->types = xr_xir_type_arena_types(program->arena);
    }
    status = xr_xir_declarations_clone(spec->declarations, spec->entry_count, &program->declarations);
    if (status != XR_XIR_OK) goto failed;
    status = program_execution_order(program, &work);
    if (status != XR_XIR_OK) goto failed;
    for (uint32_t i = 0; i < program->declarations->slot_count; ++i)
        ++program->module_slots[program->declarations->slots[i].module];
    program->code = spec->code;
    *output = program;
    return XR_XIR_OK;
 failed:
    program_dispose(program);
    return status;
}
bool xr_xir_program_retain(XrXirProgram *program) {
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}
void xr_xir_program_drop(XrXirProgram *program) {
    if (!program) return;
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(count, "program reference underflow");
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count - 1,
                memory_order_acq_rel, memory_order_relaxed)) break;
    }
    if (count == 1) program_dispose(program);
}
