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
#include "xxir_compile_memory.h"
#include "../base/xchecks.h"

static bool program_value_type(const XrXirProgramSpec *spec, XrXirType type) {
    if (xr_xir_type_is_cell(spec->types, type)) type = xr_xir_cell_element(spec->types, type);
    return type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || xr_xir_type_is_owned(spec->types, type);
}
static XrXirStatus program_shape(const XrXirCompileContext *context, const XrXirProgramSpec *spec) {
    if (!spec || !spec->entries || !spec->entry_count || !spec->declarations ||
        (!!spec->code.owner != !!spec->code.release)) return XR_XIR_BAD_STRUCTURE;
    if (spec->abi_version != XR_XIR_PROGRAM_ABI_VERSION ||
        spec->target.architecture != XR_XIR_ARCH_X86_64 || spec->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return XR_XIR_BAD_LAYOUT;
    if (spec->entry_count > 65535 || spec->entry_count > context->limits.functions) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (spec->entries[i].abi_version != XR_XIR_CALL_ABI_VERSION) return XR_XIR_BAD_LAYOUT;
    }
    if (spec->declarations->implementations || (spec->types && spec->types->interfaces)) return XR_XIR_BAD_STAGE;
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, spec->types);
    if (status != XR_XIR_OK) return status;
    if (spec->types) for (uint32_t t = 0; t < spec->types->count; ++t) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (spec->types->nodes[t].parameter_span) return XR_XIR_BAD_TYPE;
    }
    status = xr_xir_compile_declarations_verify(context, spec->declarations, spec->types,
        spec->entry_count, XR_XIR_PROGRAM);
    if (status != XR_XIR_OK) return status;
    for (uint32_t s = 0; s < spec->declarations->slot_count; ++s) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirSlot *slot = &spec->declarations->slots[s];
        if (xr_xir_type_is_cell(spec->types, slot->type) ||
            (slot->type != XR_XIR_UNIT && !program_value_type(spec, slot->type)) ||
            (xr_xir_type_is_callable(spec->types, slot->type) &&
             slot->module != spec->declarations->root_module)) return XR_XIR_BAD_TYPE;
    }
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *entry = &spec->entries[i];
        if (spec->declarations->functions[i].test_role &&
            (entry->parameter_count || entry->result != XR_XIR_UNIT)) return XR_XIR_BAD_TYPE;
        if ((entry->flags & ~XR_XIR_ENTRY_EXIT) ||
            entry->cleanup_owner != spec->declarations->functions[i].cleanup_owner ||
            (entry->cleanup_owner && (entry->result != XR_XIR_UNIT ||
             !(spec->entries[entry->cleanup_owner - 1].flags & XR_XIR_ENTRY_EXIT)))) return XR_XIR_BAD_STRUCTURE;
        if (!entry->resume || (entry->parameter_count && !entry->parameters)) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_is_cell(spec->types, entry->result) ||
            (entry->result != XR_XIR_UNIT && !program_value_type(spec, entry->result))) return XR_XIR_BAD_TYPE;
        if (entry->parameter_count > context->limits.parameters) return XR_XIR_BUDGET;
        for (uint32_t p = 0; p < entry->parameter_count; ++p) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            /* A cell parameter is a ref parameter: only the program's own functions can build one. */
            if (!program_value_type(spec, entry->parameters[p])) return XR_XIR_BAD_TYPE;
        }
    }
    const XrXirDeclarations *d = spec->declarations;
    const XrXirCallEntry *entry = &spec->entries[d->entry_function];
    if (entry->parameter_count || entry->result != XR_XIR_I64) return XR_XIR_BAD_TYPE;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *init = &spec->entries[d->modules[m].initializer];
        if (init->parameter_count || init->result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static void program_dispose(XrXirProgram *program) {
    if (program->code.release) program->code.release(program->code.owner);
    if (program->entries) for (uint32_t i = 0; i < program->entry_count; ++i)
        xr_compile_resources_free((void *)program->entries[i].parameters);
    xr_compile_resources_free(program->entries);
    xr_xir_compile_type_arena_drop(program->arena);
    xr_xir_compile_declarations_free(program->declarations);
    xr_compile_resources_free(program->order);
    xr_compile_resources_free(program->active_modules);
    xr_compile_resources_free(program->module_slots);
    xr_compile_resources_free(program);
}
static XrXirStatus program_execution_order(XrXirProgram *program) {
    const XrXirCompileContext *context = &program->context;
    const XrXirDeclarations *d = program->declarations;
    XrXirStatus status = xr_xir_compile_declarations_order(context, d, program->order);
    if (status != XR_XIR_OK) return status;
    program->active_modules[d->root_module] = 1;
    for (uint32_t i = d->module_count; i > 0; --i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        uint32_t id = program->order[i - 1];
        if (!program->active_modules[id]) continue;
        const XrXirSourceModule *module = &d->modules[id];
        for (uint32_t dep = 0; dep < module->dependency_count; ++dep) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            program->active_modules[module->dependencies[dep]] = 1;
        }
    }
    for (uint32_t i = 0; i < d->module_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        uint32_t id = program->order[i];
        if (program->active_modules[id]) program->order[program->initialization_count++] = id;
    }
    return XR_XIR_OK;
}
static XrXirStatus program_copy_entries(XrXirProgram *program, const XrXirProgramSpec *spec) {
    const XrXirCompileContext *context = &program->context;
    XrXirStatus status = XR_XIR_OK;
    program->entries = xir_compile_calloc(context, spec->entry_count, sizeof(*program->entries), &status);
    for (uint32_t i = 0; i < spec->entry_count && status == XR_XIR_OK; ++i) {
        if (!xir_compile_work(context, sizeof(*program->entries))) return XR_XIR_BUDGET;
        program->entries[i] = spec->entries[i];
        program->entries[i].parameters = NULL;
        uint64_t bytes = (uint64_t)spec->entries[i].parameter_count * sizeof(XrXirType);
        if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
        program->entries[i].parameters = xir_compile_copy(context, spec->entries[i].parameters, (size_t)bytes, &status);
    }
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_program_seal(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, XrXirProgram **output) {
    if (!output || *output || !xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = program_shape(context, spec);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_compile_program_proof_verify(context, spec, &spec->proof);
    if (status != XR_XIR_OK) return status;
    XrXirProgram *program = xir_compile_calloc(context, 1, sizeof(*program), &status);
    if (!program) return status;
    atomic_init(&program->references, 1);
    program->context = *context;
    program->entry_count = spec->entry_count;
    status = program_copy_entries(program, spec);
    program->order = xir_compile_calloc(context, spec->declarations->module_count, sizeof(*program->order), &status);
    program->active_modules = xir_compile_calloc(context, spec->declarations->module_count, 1, &status);
    program->module_slots = xir_compile_calloc(context, spec->declarations->module_count, sizeof(*program->module_slots), &status);
    if (status != XR_XIR_OK) goto failed;
    if (spec->types) {
        XrXirValueStatus value_status = xr_xir_compile_type_arena_new(context, spec->types, &program->arena);
        if (value_status != XR_XIR_VALUE_OK) {
            status = value_status == XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
                value_status == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_BAD_TYPE : XR_XIR_BUDGET;
            goto failed;
        }
        program->types = xr_xir_compile_type_arena_types(program->arena);
    }
    status = xr_xir_compile_declarations_clone(context, spec->declarations, spec->entry_count, &program->declarations);
    if (status != XR_XIR_OK) goto failed;
    status = program_execution_order(program);
    if (status != XR_XIR_OK) goto failed;
    for (uint32_t i = 0; i < program->declarations->slot_count; ++i) {
        if (!xir_compile_work(context, 1)) { status = XR_XIR_BUDGET; goto failed; }
        ++program->module_slots[program->declarations->slots[i].module];
    }
    program->code = spec->code;
    *output = program;
    return XR_XIR_OK;
failed:
    program_dispose(program);
    return status;
}
XR_FUNC bool xr_xir_compile_program_retain(XrXirProgram *program) {
    if (!program) return false;
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}
XR_FUNC void xr_xir_compile_program_drop(XrXirProgram *program) {
    if (!program) return;
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(count, "program reference underflow");
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count - 1,
                memory_order_acq_rel, memory_order_relaxed)) break;
    }
    if (count == 1) program_dispose(program);
}
