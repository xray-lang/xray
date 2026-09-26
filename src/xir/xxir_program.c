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

static XrXirStatus program_shape(const XrXirProgramSpec *spec, uint64_t *bytes, uint64_t *work) {
    if (!spec || !spec->entries || !spec->entry_count || spec->entry_count > 65535 ||
        !spec->declarations || (!!spec->code.owner != !!spec->code.release)) return XR_XIR_BAD_STRUCTURE;
    if (spec->abi_version != XR_XIR_PROGRAM_ABI_VERSION ||
        spec->target.architecture != XR_XIR_ARCH_X86_64 || spec->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return XR_XIR_BAD_LAYOUT;
    uint64_t fixed = sizeof(XrXirProgram) + (uint64_t) spec->entry_count * sizeof(XrXirCallEntry) +
        (uint64_t) spec->declarations->module_count * sizeof(uint32_t) * 2;
    if (fixed > *bytes || fixed > SIZE_MAX) return XR_XIR_BUDGET;
    *bytes -= fixed;
    XrXirStatus status = xr_xir_declarations_verify(spec->declarations, spec->entry_count, bytes, work);
    if (status != XR_XIR_OK) return status;
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        const XrXirCallEntry *entry = &spec->entries[i];
        if (entry->abi_version != XR_XIR_CALL_ABI_VERSION) return XR_XIR_BAD_LAYOUT;
        if (!entry->resume || (entry->parameter_count && !entry->parameters)) return XR_XIR_BAD_STRUCTURE;
        if (entry->result != XR_XIR_UNIT && entry->result != XR_XIR_BOOL && entry->result != XR_XIR_I64 &&
            !xr_xir_type_is_owned(entry->result)) return XR_XIR_BAD_TYPE;
        uint64_t parameter_bytes = (uint64_t) entry->parameter_count * sizeof(XrXirType);
        if (parameter_bytes > *bytes || parameter_bytes > SIZE_MAX || entry->parameter_count > *work)
            return XR_XIR_BUDGET;
        *bytes -= parameter_bytes; *work -= entry->parameter_count;
        for (uint32_t p = 0; p < entry->parameter_count; ++p)
            if (entry->parameters[p] != XR_XIR_BOOL && entry->parameters[p] != XR_XIR_I64 &&
                !xr_xir_type_is_owned(entry->parameters[p])) return XR_XIR_BAD_TYPE;
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
    xr_xir_declarations_free(program->declarations);
    xr_free(program->order);
    xr_free(program->module_slots);
    xr_free(program);
}
XrXirStatus xr_xir_program_seal(const XrXirProgramSpec *spec, uint64_t byte_limit, XrXirProgram **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    uint64_t work = UINT64_C(16000000);
    XrXirStatus status = program_shape(spec, &byte_limit, &work);
    if (status != XR_XIR_OK) return status;
    XrXirProgram *program = xr_calloc(1, sizeof(*program));
    if (!program) return XR_XIR_OUT_OF_MEMORY;
    atomic_init(&program->references, 1);
    program->entry_count = spec->entry_count;
    program->entries = xr_calloc(spec->entry_count, sizeof(*program->entries));
    program->order = xr_calloc(spec->declarations->module_count, sizeof(*program->order));
    program->module_slots = xr_calloc(spec->declarations->module_count, sizeof(*program->module_slots));
    if (!program->entries || !program->order || !program->module_slots) {
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
    status = xr_xir_declarations_clone(spec->declarations, spec->entry_count, &program->declarations);
    if (status != XR_XIR_OK) goto failed;
    status = xr_xir_declarations_order(program->declarations, program->order, &work);
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
