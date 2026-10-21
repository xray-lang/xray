/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir.c - Owned scalar SSA construction and immutable queries
 *
 * KEY CONCEPT:
 *   Vectors own their elements and refer to each other by stable indices.
 *   A failed build can only be destroyed, never published for execution.
 */
#include "xlir_internal.h"
#include "xlir_verify.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

static void lir_status_diagnostic(XrLirDiagnostic *diagnostic, XrLirStatus status) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = status;
    const char *message = "LIR construction failed";
    if (status == XR_LIR_OUT_OF_MEMORY)
        message = "LIR allocation failed";
    else if (status == XR_LIR_INVALID_ARGUMENT)
        message = "invalid LIR construction argument";
    else if (status == XR_LIR_INVALID_STATE)
        message = "LIR module or block is not writable";
    else if (status == XR_LIR_UNSUPPORTED)
        message = "LIR construction is unsupported";
    else if (status == XR_LIR_OK)
        message = "";
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
}

static XrLirStatus lir_fail(XrLirModule *m, XrLirStatus status) {
    if (m && !m->finished && m->failure == XR_LIR_OK) {
        m->failure = status;
        lir_status_diagnostic(&m->failure_diagnostic, status);
    }
    return status;
}

/* Failed instructions transfer their already copied path without allocating.
 * Published-instruction diagnostics instead borrow storage owned by the graph. */
static XrLirStatus lir_failure_details(XrLirModule *m, XrLirStatus status,
                                       const XrLirDiagnostic *diagnostic,
                                       XrLirInstructionData *pending) {
    lir_fail(m, status);
    if (diagnostic && diagnostic->message[0])
        m->failure_diagnostic = *diagnostic;
    m->failure_diagnostic.status = m->failure;
    if (pending && pending->view.source.path) {
        m->failure_path = (char *) pending->view.source.path;
        m->failure_diagnostic.source = pending->view.source;
        pending->view.source.path = NULL;
    }
    return m->failure;
}

static XrLirStatus lir_writable(XrLirModule *m) {
    if (!m)
        return XR_LIR_INVALID_ARGUMENT;
    if (m->finished)
        return XR_LIR_INVALID_STATE;
    return m->failure;
}

static void *lir_allocate(XrLirModule *m, size_t count, size_t size) {
    if (!count)
        return NULL;
    if (!size || count > SIZE_MAX / size) {
        lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        return NULL;
    }
    void *memory = xr_malloc(count * size);
    if (!memory)
        lir_fail(m, XR_LIR_OUT_OF_MEMORY);
    return memory;
}

/* Growth copies only initialized elements and publishes after allocation. */
static void *lir_reserve(XrLirModule *m, void *old, uint32_t used, uint32_t *capacity,
                         size_t size) {
    if (used < *capacity)
        return old;
    if (used == UINT32_MAX) {
        lir_fail(m, XR_LIR_UNSUPPORTED);
        return NULL;
    }
    uint32_t next = *capacity < 4 ? 4 : (*capacity > UINT32_MAX / 2 ? UINT32_MAX : *capacity * 2);
    void *memory = lir_allocate(m, next, size);
    if (!memory)
        return NULL;
    if (used)
        memcpy(memory, old, (size_t) used * size);
    xr_free(old);
    *capacity = next;
    return memory;
}

static void *lir_copy(XrLirModule *m, const void *source, size_t count, size_t size) {
    void *memory = lir_allocate(m, count, size);
    if (memory)
        memcpy(memory, source, count * size);
    return memory;
}

static bool lir_types_valid(const XrLirType *types, uint32_t count) {
    if (count && !types)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!xr_lir_internal_type_valid(types[i], false))
            return false;
    return true;
}

static XrLirFunctionData *lir_function(XrLirModule *m, XrLirFunction handle) {
    if (!m || handle.module != m || !handle.id || handle.id > m->function_count)
        return NULL;
    return &m->functions[handle.id - 1];
}

static XrLirFunctionData *lir_block_function(XrLirModule *m, XrLirBlock handle) {
    XrLirFunctionData *f = lir_function(m, (XrLirFunction) {handle.module, handle.function});
    if (!f || !handle.id || handle.id > f->block_count)
        return NULL;
    return f;
}

static void lir_instruction_free(XrLirInstructionData *instruction) {
    xr_free((void *) instruction->view.operands);
    for (uint32_t i = 0; i < 2; ++i)
        xr_free((void *) instruction->view.edges[i].arguments);
    xr_free((void *) instruction->view.source.path);
}

static void lir_function_free(XrLirFunctionData *f) {
    for (uint32_t b = 0; b < f->block_count; ++b) {
        XrLirBlockData *block = &f->blocks[b];
        for (uint32_t i = 0; i < block->instruction_count; ++i)
            lir_instruction_free(&block->instructions[i]);
        xr_free(block->parameters);
        xr_free(block->instructions);
    }
    xr_free(f->blocks);
    xr_free(f->values);
    xr_free(f->name);
    xr_free(f->parameters);
    xr_free(f->parameter_types);
}

XR_FUNC void xr_lir_module_destroy(XrLirModule *m) {
    if (!m)
        return;
    for (uint32_t i = 0; i < m->function_count; ++i)
        lir_function_free(&m->functions[i]);
    for (uint32_t i = 0; i < m->intrinsic_count; ++i)
        xr_free((void *) m->intrinsics[i].parameters);
    xr_free(m->functions);
    xr_free(m->intrinsics);
    xr_free(m->failure_path);
    xr_free(m);
}

static XrLirStatus lir_registry_copy(XrLirModule *m, const XrLirRegistry *registry) {
    if (!registry || !registry->count)
        return XR_LIR_OK;
    if (!registry->entries)
        return XR_LIR_INVALID_ARGUMENT;
    m->intrinsics = lir_allocate(m, registry->count, sizeof(*m->intrinsics));
    if (!m->intrinsics)
        return m->failure;
    memset(m->intrinsics, 0, (size_t) registry->count * sizeof(*m->intrinsics));
    m->intrinsic_count = registry->count;
    for (uint32_t i = 0; i < registry->count; ++i) {
        XrLirIntrinsic entry = registry->entries[i];
        if (!entry.id || !entry.scalar_sync ||
            !lir_types_valid(entry.parameters, entry.parameter_count) ||
            !xr_lir_internal_type_valid(entry.result, true))
            return XR_LIR_UNSUPPORTED;
        for (uint32_t j = 0; j < i; ++j)
            if (m->intrinsics[j].id == entry.id)
                return XR_LIR_INVALID_ARGUMENT;
        XrLirType *parameters =
            lir_copy(m, entry.parameters, entry.parameter_count, sizeof(*parameters));
        if (entry.parameter_count && !parameters)
            return m->failure;
        entry.parameters = parameters;
        m->intrinsics[i] = entry;
    }
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_module_create(const XrLirRegistry *registry, XrLirModule **output) {
    if (!output || *output)
        return XR_LIR_INVALID_ARGUMENT;
    XrLirModule *m = xr_malloc(sizeof(*m));
    if (!m)
        return XR_LIR_OUT_OF_MEMORY;
    memset(m, 0, sizeof(*m));
    XrLirStatus status = lir_registry_copy(m, registry);
    if (status != XR_LIR_OK) {
        xr_lir_module_destroy(m);
        return status;
    }
    *output = m;
    return XR_LIR_OK;
}

static bool lir_function_spec_valid(const XrLirFunctionSpec *spec) {
    if (!spec || !spec->name || !spec->name_length || spec->name_length == SIZE_MAX ||
        memchr(spec->name, 0, spec->name_length) ||
        !lir_types_valid(spec->parameters, spec->parameter_count) ||
        !xr_lir_internal_type_valid(spec->result, true))
        return false;
    if (spec->kind == XR_LIR_FUNCTION_DEFINITION)
        return spec->extern_kind == XR_LIR_EXTERN_NONE;
    return spec->kind == XR_LIR_FUNCTION_EXTERNAL && spec->extern_kind == XR_LIR_EXTERN_PRINT_I64 &&
           spec->name_length == sizeof("xrt_print_i64") - 1 &&
           !memcmp(spec->name, "xrt_print_i64", sizeof("xrt_print_i64") - 1) &&
           spec->parameter_count == 1 && spec->parameters[0] == XR_LIR_I64 &&
           spec->result == XR_LIR_UNIT;
}

static bool lir_signature_copy(XrLirModule *m, const XrLirFunctionSpec *spec,
                               XrLirFunctionData *f) {
    f->name = lir_allocate(m, spec->name_length + 1, 1);
    if (!f->name)
        return false;
    memcpy(f->name, spec->name, spec->name_length);
    f->name[spec->name_length] = 0;
    f->name_length = spec->name_length;
    f->parameter_count = spec->parameter_count;
    f->result = spec->result;
    f->kind = spec->kind;
    f->extern_kind = spec->extern_kind;
    if (!f->parameter_count)
        return true;
    f->parameter_types =
        lir_copy(m, spec->parameters, f->parameter_count, sizeof(*f->parameter_types));
    f->parameters = lir_allocate(m, f->parameter_count, sizeof(*f->parameters));
    f->values = lir_allocate(m, f->parameter_count, sizeof(*f->values));
    if (!f->parameter_types || !f->parameters || !f->values)
        return false;
    f->value_count = f->value_capacity = f->parameter_count;
    for (uint32_t i = 0; i < f->parameter_count; ++i) {
        f->parameters[i] = i + 1;
        f->values[i] = (XrLirValueData) {f->parameter_types[i], 0, i, true};
    }
    return true;
}

XR_FUNC XrLirStatus xr_lir_declare_function(XrLirModule *m, const XrLirFunctionSpec *spec,
                                            XrLirFunction *output) {
    if (output)
        *output = (XrLirFunction) {0};
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    if (!output || !lir_function_spec_valid(spec))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    for (uint32_t i = 0; i < m->function_count; ++i)
        if (m->functions[i].name_length == spec->name_length &&
            !memcmp(m->functions[i].name, spec->name, spec->name_length))
            return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirFunctionData function = {0};
    if (!lir_signature_copy(m, spec, &function)) {
        lir_function_free(&function);
        return m->failure;
    }
    void *grown = lir_reserve(m, m->functions, m->function_count, &m->function_capacity,
                              sizeof(*m->functions));
    if (!grown) {
        lir_function_free(&function);
        return m->failure;
    }
    m->functions = grown;
    m->functions[m->function_count++] = function;
    *output = (XrLirFunction) {m, m->function_count};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_define_function(XrLirModule *m, XrLirFunction handle) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_function(m, handle);
    if (!f || f->kind != XR_LIR_FUNCTION_DEFINITION || f->defined)
        return lir_fail(m, XR_LIR_INVALID_STATE);
    f->defined = true;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_function_parameter(XrLirModule *m, XrLirFunction handle,
                                              uint32_t ordinal, XrLirValue *output) {
    if (output)
        *output = (XrLirValue) {0};
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_function(m, handle);
    if (!output || !f || ordinal >= f->parameter_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    *output = (XrLirValue) {m, handle.id, f->parameters[ordinal]};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_add_block(XrLirModule *m, XrLirFunction handle, const XrLirType *types,
                                     uint32_t count, XrLirBlock *output) {
    if (output)
        *output = (XrLirBlock) {0};
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_function(m, handle);
    if (!output || !f || !f->defined || !lir_types_valid(types, count))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    if (count > UINT32_MAX - f->value_count)
        return lir_fail(m, XR_LIR_UNSUPPORTED);
    uint32_t *parameters = lir_allocate(m, count, sizeof(*parameters));
    if (count && !parameters)
        return m->failure;
    for (uint32_t i = 0; i < count; ++i) {
        void *grown =
            lir_reserve(m, f->values, f->value_count, &f->value_capacity, sizeof(*f->values));
        if (!grown) {
            xr_free(parameters);
            return m->failure;
        }
        f->values = grown;
        f->values[f->value_count] = (XrLirValueData) {types[i], f->block_count + 1, i, true};
        parameters[i] = ++f->value_count;
    }
    void *grown = lir_reserve(m, f->blocks, f->block_count, &f->block_capacity, sizeof(*f->blocks));
    if (!grown) {
        xr_free(parameters);
        return m->failure;
    }
    f->blocks = grown;
    f->blocks[f->block_count++] =
        (XrLirBlockData) {.parameters = parameters, .parameter_count = count};
    *output = (XrLirBlock) {m, handle.id, f->block_count};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_block_parameter(XrLirModule *m, XrLirBlock handle, uint32_t ordinal,
                                           XrLirValue *output) {
    if (output)
        *output = (XrLirValue) {0};
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_block_function(m, handle);
    if (!output || !f || ordinal >= f->blocks[handle.id - 1].parameter_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    *output = (XrLirValue) {m, handle.function, f->blocks[handle.id - 1].parameters[ordinal]};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_set_entry(XrLirModule *m, XrLirFunction handle, XrLirBlock block) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_function(m, handle);
    if (!f || block.module != m || block.function != handle.id || !block.id ||
        block.id > f->block_count || f->entry || f->blocks[block.id - 1].parameter_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    f->entry = block.id;
    return XR_LIR_OK;
}

static bool lir_value_handle_valid(const XrLirModule *m, uint32_t function, XrLirValue v) {
    return v.module == m && v.function == function && v.id &&
           v.id <= m->functions[function - 1].value_count;
}

static uint32_t *lir_copy_values(XrLirModule *m, uint32_t function, const XrLirValue *values,
                                 uint32_t count) {
    if (!count)
        return NULL;
    if (!values) {
        lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        return NULL;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (!lir_value_handle_valid(m, function, values[i])) {
            lir_fail(m, XR_LIR_INVALID_ARGUMENT);
            return NULL;
        }
    uint32_t *ids = lir_allocate(m, count, sizeof(*ids));
    if (!ids)
        return NULL;
    for (uint32_t i = 0; i < count; ++i)
        ids[i] = values[i].id;
    return ids;
}

static bool lir_source_copy(XrLirModule *m, XrLirSource source, XrLirSource *output) {
    if (!source.path && !source.path_length && !source.line && !source.column && !source.end_line &&
        !source.end_column)
        return true;
    if (!source.path || !source.path_length || source.path_length == SIZE_MAX ||
        memchr(source.path, 0, source.path_length) || !source.line || !source.column ||
        source.end_line < source.line || !source.end_column ||
        (source.end_line == source.line && source.end_column < source.column)) {
        lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        return false;
    }
    char *path = lir_allocate(m, source.path_length + 1, 1);
    if (!path)
        return false;
    memcpy(path, source.path, source.path_length);
    path[source.path_length] = 0;
    source.path = path;
    *output = source;
    return true;
}

static bool lir_instruction_copy(XrLirModule *m, uint32_t function, const XrLirInstruction *input,
                                 XrLirInstructionData *output) {
    XrLirInstructionView *v = &output->view;
    v->op = input->op;
    v->type = input->type;
    v->predicate = input->predicate;
    v->bits = input->bits;
    v->intrinsic = input->intrinsic;
    v->operand_count = input->operand_count;
    v->edge_count = input->edge_count;
    if (input->edge_count > 2) {
        lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        return false;
    }
    if (input->op == XR_LIR_CALL) {
        if (!lir_function(m, input->callee)) {
            lir_fail(m, XR_LIR_INVALID_ARGUMENT);
            return false;
        }
        v->callee = input->callee.id;
    } else if (input->callee.module || input->callee.id) {
        lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        return false;
    }
    v->operands = lir_copy_values(m, function, input->operands, input->operand_count);
    if (input->operand_count && !v->operands)
        return false;
    for (uint32_t e = 0; e < input->edge_count; ++e) {
        XrLirBlock target = input->edges[e].target;
        if (target.module != m || target.function != function || !target.id ||
            target.id > m->functions[function - 1].block_count) {
            lir_fail(m, XR_LIR_INVALID_ARGUMENT);
            return false;
        }
        v->edges[e].target = target.id;
        v->edges[e].argument_count = input->edges[e].argument_count;
        v->edges[e].arguments =
            lir_copy_values(m, function, input->edges[e].arguments, input->edges[e].argument_count);
        if (input->edges[e].argument_count && !v->edges[e].arguments)
            return false;
    }
    return true;
}

static XrLirStatus lir_emit(XrLirModule *m, XrLirBlock handle, const XrLirInstruction *input,
                            unsigned category, XrLirValue *output) {
    if (output)
        *output = (XrLirValue) {0};
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK)
        return status;
    XrLirFunctionData *f = lir_block_function(m, handle);
    if (!f || !input)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirBlockData *block = &f->blocks[handle.id - 1];
    XrLirInstructionData instruction = {0};
    XrLirDiagnostic diagnostic = {0};
    if (!lir_source_copy(m, input->source, &instruction.view.source)) {
        status = m->failure;
        goto failed;
    }
    if (block->terminated) {
        status = XR_LIR_INVALID_STATE;
        goto failed;
    }
    bool terminal = xr_lir_internal_terminal(input->op);
    if ((category == 0 && !output) || terminal != (category == 2) ||
        (!terminal && (input->type == XR_LIR_UNIT) != (category == 1))) {
        status = XR_LIR_INVALID_ARGUMENT;
        goto failed;
    }
    if (!lir_instruction_copy(m, handle.function, input, &instruction)) {
        status = m->failure;
        goto failed;
    }
    status = xr_lir_internal_instruction_check(m, handle.function, &instruction.view, &diagnostic);
    if (status != XR_LIR_OK)
        goto failed;
    if (category == 0) {
        void *grown =
            lir_reserve(m, f->values, f->value_count, &f->value_capacity, sizeof(*f->values));
        if (!grown) {
            status = m->failure;
            goto failed;
        }
        f->values = grown;
    }
    void *grown = lir_reserve(m, block->instructions, block->instruction_count,
                              &block->instruction_capacity, sizeof(*block->instructions));
    if (!grown) {
        status = m->failure;
        goto failed;
    }
    block->instructions = grown;
    if (category == 0) {
        f->values[f->value_count] =
            (XrLirValueData) {input->type, handle.id, block->instruction_count, false};
        instruction.view.result = ++f->value_count;
        *output = (XrLirValue) {m, handle.function, f->value_count};
    }
    block->instructions[block->instruction_count++] = instruction;
    block->terminated = terminal;
    XR_DCHECK(block->instruction_count <= block->instruction_capacity, "instruction capacity");
    return XR_LIR_OK;
failed:
    diagnostic.function = handle.function;
    diagnostic.block = handle.id;
    diagnostic.instruction = block->instruction_count + 1;
    status = lir_failure_details(m, status, &diagnostic, &instruction);
    m->failure_diagnostic.function = handle.function;
    m->failure_diagnostic.block = handle.id;
    m->failure_diagnostic.instruction = block->instruction_count + 1;
    lir_instruction_free(&instruction);
    return status;
}

XR_FUNC XrLirStatus xr_lir_emit_value(XrLirModule *m, XrLirBlock block,
                                      const XrLirInstruction *instruction, XrLirValue *output) {
    return lir_emit(m, block, instruction, 0, output);
}
XR_FUNC XrLirStatus xr_lir_emit_effect(XrLirModule *m, XrLirBlock block,
                                       const XrLirInstruction *instruction) {
    return lir_emit(m, block, instruction, 1, NULL);
}
XR_FUNC XrLirStatus xr_lir_set_terminator(XrLirModule *m, XrLirBlock block,
                                          const XrLirInstruction *instruction) {
    return lir_emit(m, block, instruction, 2, NULL);
}

XR_FUNC XrLirStatus xr_lir_module_finish(XrLirModule *m, XrLirDiagnostic *diagnostic) {
    XrLirStatus status = lir_writable(m);
    XrLirDiagnostic local = {0};
    XrLirDiagnostic *result = diagnostic ? diagnostic : &local;
    lir_status_diagnostic(result, status);
    if (status != XR_LIR_OK) {
        if (m && m->failure != XR_LIR_OK)
            *result = m->failure_diagnostic;
        return status;
    }
    for (uint32_t f = 0; f < m->function_count; ++f) {
        XrLirFunctionData *function = &m->functions[f];
        if (function->kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        if (!function->defined || !function->entry) {
            xr_lir_internal_error(result, "function definition or entry is missing", f + 1, 0, 0,
                                  NULL);
            return lir_failure_details(m, XR_LIR_VERIFY_ERROR, result, NULL);
        }
        for (uint32_t b = 0; b < function->block_count; ++b)
            if (!function->blocks[b].terminated) {
                xr_lir_internal_error(result, "block has no terminator", f + 1, b + 1, 0, NULL);
                return lir_failure_details(m, XR_LIR_VERIFY_ERROR, result, NULL);
            }
    }
#if XR_DEBUG
    status = xr_lir_verify(m, result);
    if (status != XR_LIR_OK)
        return lir_failure_details(m, status, result, NULL);
#endif
    m->finished = true;
    return XR_LIR_OK;
}

static bool lir_readable(const XrLirModule *m) {
    return m && m->finished && m->failure == XR_LIR_OK;
}
XR_FUNC uint32_t xr_lir_intrinsic_count(const XrLirModule *m) {
    return lir_readable(m) ? m->intrinsic_count : 0;
}
XR_FUNC XrLirStatus xr_lir_intrinsic_at(const XrLirModule *m, uint32_t ordinal,
                                        XrLirIntrinsic *out) {
    if (out)
        *out = (XrLirIntrinsic) {0};
    if (!out || !lir_readable(m) || ordinal >= m->intrinsic_count)
        return XR_LIR_INVALID_ARGUMENT;
    *out = m->intrinsics[ordinal];
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_intrinsic_view(const XrLirModule *m, uint32_t id, XrLirIntrinsic *out) {
    if (out)
        *out = (XrLirIntrinsic) {0};
    if (!out || !lir_readable(m) || !id)
        return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < m->intrinsic_count; ++i)
        if (m->intrinsics[i].id == id)
            return xr_lir_intrinsic_at(m, i, out);
    return XR_LIR_INVALID_ARGUMENT;
}
XR_FUNC uint32_t xr_lir_function_count(const XrLirModule *m) {
    return lir_readable(m) ? m->function_count : 0;
}
XR_FUNC XrLirStatus xr_lir_function_at(const XrLirModule *m, uint32_t ordinal, XrLirFunction *out) {
    if (out)
        *out = (XrLirFunction) {0};
    if (!out || !lir_readable(m) || ordinal >= m->function_count)
        return XR_LIR_INVALID_ARGUMENT;
    *out = (XrLirFunction) {m, ordinal + 1};
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_function_view(const XrLirModule *m, XrLirFunction handle,
                                         XrLirFunctionView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !lir_readable(m) || handle.module != m || !handle.id ||
        handle.id > m->function_count)
        return XR_LIR_INVALID_ARGUMENT;
    const XrLirFunctionData *f = &m->functions[handle.id - 1];
    *out = (XrLirFunctionView) {f->name,        f->name_length,     f->parameter_types,
                                f->parameters,  f->parameter_count, f->block_count,
                                f->value_count, f->entry,           f->result,
                                f->kind,        f->extern_kind};
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_block_at(const XrLirModule *m, XrLirFunction handle, uint32_t ordinal,
                                    XrLirBlock *out) {
    if (out)
        *out = (XrLirBlock) {0};
    XrLirFunctionView view;
    if (!out || xr_lir_function_view(m, handle, &view) != XR_LIR_OK || ordinal >= view.block_count)
        return XR_LIR_INVALID_ARGUMENT;
    *out = (XrLirBlock) {m, handle.id, ordinal + 1};
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_block_view(const XrLirModule *m, XrLirBlock handle,
                                      XrLirBlockView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !lir_readable(m) || handle.module != m || !handle.function ||
        handle.function > m->function_count)
        return XR_LIR_INVALID_ARGUMENT;
    const XrLirFunctionData *f = &m->functions[handle.function - 1];
    if (!handle.id || handle.id > f->block_count)
        return XR_LIR_INVALID_ARGUMENT;
    const XrLirBlockData *b = &f->blocks[handle.id - 1];
    *out = (XrLirBlockView) {b->parameters, b->parameter_count, b->instruction_count};
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_instruction_view(const XrLirModule *m, XrLirBlock block,
                                            uint32_t ordinal, XrLirInstructionView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    XrLirBlockView view;
    if (!out || xr_lir_block_view(m, block, &view) != XR_LIR_OK ||
        ordinal >= view.instruction_count)
        return XR_LIR_INVALID_ARGUMENT;
    *out = m->functions[block.function - 1].blocks[block.id - 1].instructions[ordinal].view;
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_value_type(const XrLirModule *m, XrLirValue value, XrLirType *out) {
    if (out)
        *out = XR_LIR_TYPE_INVALID;
    if (!out || !lir_readable(m) || !value.function || value.function > m->function_count ||
        !lir_value_handle_valid(m, value.function, value))
        return XR_LIR_INVALID_ARGUMENT;
    *out = m->functions[value.function - 1].values[value.id - 1].type;
    return XR_LIR_OK;
}
