/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_exports.inc.c - Validated Program native C boundaries
 */

static const char *export_c_type(uint16_t type, bool result, char storage[32]) {
    if (type == XR_CORE_TYPE_BOOL)
        return "bool";
    if (xr_core_spec_integer_type(type) || (result && type == XR_CORE_TYPE_VOID))
        return type_c_name(type, storage);
    return NULL;
}

static bool export_symbol_valid(const char *symbol) {
    if (!symbol || !symbol[0] || symbol[0] == '_' ||
        strncmp(symbol, "xr_aot_", 7u) == 0 || strncmp(symbol, "XrAot", 5u) == 0 ||
        strncmp(symbol, "XR_", 3u) == 0)
        return false;
    static const char *const reserved[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
        "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
        "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "bool",
        "true", "false", "main", "abort", "malloc", "calloc", "realloc", "free", "NULL",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t",
        "uint64_t", "size_t", "ptrdiff_t", "memcpy", "memset", "memcmp", "strlen"
    };
    for (size_t index = 0u; index < sizeof(reserved) / sizeof(reserved[0]); ++index)
        if (strcmp(symbol, reserved[index]) == 0)
            return false;
    for (size_t index = 0u; symbol[index]; ++index) {
        unsigned char byte = (unsigned char) symbol[index];
        bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (index >= 255u || (!letter && byte != '_' &&
                             !(index != 0u && byte >= '0' && byte <= '9')))
            return false;
    }
    return true;
}

static bool export_signature_valid(const XrValidatedFunction *function) {
    char storage[32];
    if (function->has_receiver || function->coroutine_safepoint_count ||
        function->error_type_id != XR_CORE_TYPE_VOID ||
        function->panic_type_id != XR_CORE_TYPE_VOID ||
        !export_c_type(function->result_type_id, true, storage))
        return false;
    for (uint32_t index = 0u; index < function->parameter_count; ++index)
        if (function->parameter_modes[index] != XR_PARAM_READ ||
            !export_c_type(function->parameter_types[index], false, storage))
            return false;
    return true;
}

/* A plain C caller has no instance, provider bindings or suspension protocol.
 * Visit exactly the possible callees using the emitter's existing target facts.
 * The temporary work queue is bounded by the immutable function table. */
static void export_enqueue(uint32_t function, uint8_t *seen, uint32_t *queue, uint32_t *end) {
    if (!seen[function]) {
        seen[function] = 1u;
        queue[(*end)++] = function;
    }
}

static bool export_visit_instruction(const XrBackendIR *ir, const XrValidatedFunction *function,
                                      const XrValidatedInstruction *instruction, uint8_t *seen,
                                      uint32_t *queue, uint32_t *end) {
    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_MODULE_SLOT ||
        instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION ||
        instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_COROUTINE_CALL ||
        instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_COROUTINE_SUSPEND)
        return false;
    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION)
        export_enqueue(instruction->immediate.function_id, seen, queue, end);
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE) {
        uint16_t type = function->value_types[instruction->operands[0]];
        for (uint32_t target = 0u; target < ir->program->function_count; ++target)
            if (callable_type_can_target(ir, type, target))
                export_enqueue(target, seen, queue, end);
    }
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
        uint32_t interface_id = 0u;
        if (!witness_signature(ir, function, instruction, &interface_id))
            return false;
        for (uint32_t index = 0u; index < ir->program->conformance_count; ++index) {
            const XrValidatedConformance *row = &ir->program->conformances[index];
            if (row->interface_id == interface_id)
                export_enqueue(row->slot_function_ids[instruction->immediate.u32], seen, queue, end);
        }
    }
    return true;
}

static XrBackendStatus validate_c_exports(const XrBackendIR *ir, const XrBackendCExport *exports,
                                          uint32_t count, XrBackendDiagnostic *diagnostic) {
    if (!count)
        return XR_BACKEND_OK;
    if (!exports || count > ir->program->function_count)
        return XR_BACKEND_INVALID_INPUT;
    for (uint32_t index = 0u; index < count; ++index) {
        const XrBackendCExport *item = &exports[index];
        if (item->function_id >= ir->program->function_count || item->hidden > 1u ||
            item->header > 1u || item->reserved8[0] || item->reserved8[1] ||
            !export_symbol_valid(item->symbol))
            return XR_BACKEND_INVALID_INPUT;
        for (uint32_t prior = 0u; prior < index; ++prior)
            if (exports[prior].function_id == item->function_id ||
                strcmp(exports[prior].symbol, item->symbol) == 0)
                return XR_BACKEND_INVALID_INPUT;
        if (!export_signature_valid(&ir->program->functions[item->function_id])) {
            xr_backend_set_diagnostic(diagnostic, XR_BACKEND_BINDING_REJECTED,
                                      0u, item->function_id, 0u, 0u);
            return XR_BACKEND_BINDING_REJECTED;
        }
    }
    uint8_t *seen = xr_calloc(ir->program->function_count, sizeof(*seen));
    uint32_t *queue = xr_calloc(ir->program->function_count, sizeof(*queue));
    if (!seen || !queue) {
        xr_free(seen);
        xr_free(queue);
        return XR_BACKEND_OUT_OF_MEMORY;
    }
    uint32_t end = 0u;
    for (uint32_t index = 0u; index < count; ++index)
        export_enqueue(exports[index].function_id, seen, queue, &end);
    XrBackendStatus status = XR_BACKEND_OK;
    for (uint32_t next = 0u; next < end && status == XR_BACKEND_OK; ++next) {
        uint32_t id = queue[next];
        const XrValidatedFunction *function = &ir->program->functions[id];
        if (function->coroutine_safepoint_count ||
            (function->effect_mask & (XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_PROVIDER_CALL))) {
            xr_backend_set_diagnostic(diagnostic, XR_BACKEND_BINDING_REJECTED, 0u, id, 0u, 0u);
            status = XR_BACKEND_BINDING_REJECTED;
            break;
        }
        for (uint32_t block = 0u; block < function->block_count && status == XR_BACKEND_OK; ++block) {
            const XrValidatedBlock *row = &function->blocks[block];
            for (uint32_t index = 0u; index < row->instruction_count; ++index)
                if (!export_visit_instruction(ir, function, &row->instructions[index], seen, queue,
                                               &end)) {
                    xr_backend_set_diagnostic(diagnostic, XR_BACKEND_BINDING_REJECTED,
                                              row->instructions[index].operation_id, id, block, index);
                    status = XR_BACKEND_BINDING_REJECTED;
                    break;
                }
        }
    }
    xr_free(seen);
    xr_free(queue);
    return status;
}

static bool emit_c_export_signature(CBuffer *buffer, const XrValidatedFunction *function,
                                     const char *symbol) {
    char storage[32];
    if (!append_format(buffer, "%s %s(", export_c_type(function->result_type_id, true, storage),
                        symbol))
        return false;
    if (!function->parameter_count && !append_text(buffer, "void"))
        return false;
    for (uint32_t index = 0u; index < function->parameter_count; ++index)
        if (!append_format(buffer, "%s%s p%u", index ? ", " : "",
                            export_c_type(function->parameter_types[index], false, storage), index))
            return false;
    return append_text(buffer, ")");
}

static bool emit_c_export(CBuffer *buffer, const XrBackendIR *ir, const XrBackendCExport *item,
                           bool arena) {
    const XrValidatedFunction *function = &ir->program->functions[item->function_id];
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
    const char *visibility = machine->operating_system == XR_TARGET_OS_WINDOWS
                                 ? (item->hidden ? "" : "__declspec(dllexport) ")
                                 : (item->hidden ? "__attribute__((visibility(\"hidden\"))) "
                                                 : "__attribute__((visibility(\"default\"))) ");
    if (!append_text(buffer, visibility) || !emit_c_export_signature(buffer, function, item->symbol) ||
        !append_format(buffer, " {\n    XrAotContext context = {0};\n"
                                "    XrAotOutcome result = xr_aot_fn_%u(&context", item->function_id))
        return false;
    for (uint32_t index = 0u; index < function->parameter_count; ++index)
        if (!append_format(buffer, ", p%u", index))
            return false;
    if (!append_text(buffer, ");\n") ||
        (arena && !append_text(buffer, "    xr_aot_context_destroy(&context);\n")) ||
        !append_text(buffer, "    if (result.kind != UINT32_C(0)) abort();\n"))
        return false;
    if (function->result_type_id != XR_CORE_TYPE_VOID &&
        !append_format(buffer, "    return result.%s;\n", outcome_field(function->result_type_id)))
        return false;
    return append_text(buffer, "}\n\n");
}

static bool emit_c_exports(CBuffer *buffer, CBuffer *header, const XrBackendIR *ir,
                            const XrBackendCExport *exports, uint32_t count) {
    if (!count)
        return true;
    if (!append_text(buffer, "\n#include <stdbool.h>\n#include <stdlib.h>\n") ||
        !append_text(header, "#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n"
                             "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"))
        return false;
    bool checked = false, wrapping = false, arena = false, output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    for (uint32_t index = 0u; index < count; ++index) {
        const XrBackendCExport *item = &exports[index];
        if (!emit_c_export(buffer, ir, item, arena))
            return false;
        if (item->header &&
            (!emit_c_export_signature(header, &ir->program->functions[item->function_id], item->symbol) ||
             !append_text(header, ";\n")))
            return false;
    }
    return append_text(header, "#ifdef __cplusplus\n}\n#endif\n");
}
