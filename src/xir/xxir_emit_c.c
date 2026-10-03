/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_c.c - Portable C11 statements from target-bound scalar XIR
 *
 * KEY CONCEPT:
 *   Emission chooses spelling only; scalar behavior and frame layout already
 *   belong to the shared runtime and the immutable Lowered artifact. A fault
 *   site inside a protected block lands through the runtime helper the VM uses.
 */

#include "xxir_emit_c.h"
#include "xxir_program.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_value_place.h"
#include "xxir_scalar.h"
#include "xxir_compile_memory.h"
#include "../aot/xi_cgen_verify_output.h"
#include <stdarg.h>
#include <limits.h>

#include "xxir_emit_buffer.inc.c"

static void emit_reject(CBuffer *buffer, XrXirStatus status) {
    if (buffer->status == XR_XIR_OK) buffer->status = status;
}

static const char *comparison_symbol(XrXirOp op) {
    switch (op) {
    case XR_XIR_EQ_INT: return "==";
    case XR_XIR_NE_INT: return "!=";
    case XR_XIR_LT_INT: return "<";
    case XR_XIR_LE_INT: return "<=";
    case XR_XIR_GT_INT: return ">";
    case XR_XIR_GE_INT: return ">=";
    default: return NULL;
    }
}

static int emit_arithmetic_operation(XrXirOp op) {
    switch (op) {
    case XR_XIR_ADD_INT: return XR_XIR_ARITH_ADD;
    case XR_XIR_SUB_INT: return XR_XIR_ARITH_SUB;
    case XR_XIR_MUL_INT: return XR_XIR_ARITH_MUL;
    case XR_XIR_DIV_INT: return XR_XIR_ARITH_DIV;
    case XR_XIR_REM_INT: return XR_XIR_ARITH_REM;
    case XR_XIR_AND_INT: return XR_XIR_ARITH_AND;
    case XR_XIR_OR_INT: return XR_XIR_ARITH_OR;
    case XR_XIR_XOR_INT: return XR_XIR_ARITH_XOR;
    case XR_XIR_SHL_INT: return XR_XIR_ARITH_SHL;
    case XR_XIR_SHR_INT: return XR_XIR_ARITH_SHR;
    default: return -1;
    }
}

static uint32_t emit_block(CBuffer *buffer, const XrXirFunction *function, uint32_t instruction) {
    uint32_t low = 0, high = function->block_count;
    while (low + 1 < high && emit_work(buffer, 1)) {
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first <= instruction) low = middle;
        else high = middle;
    }
    return low;
}
static bool emit_has_cleanup(CBuffer *buffer, const XrXirFunction *function) {
    for (uint32_t i = 0; i < function->instruction_count && emit_work(buffer, 1); ++i)
        if (function->instructions[i].op == XR_XIR_CLEANUP_REGISTER) return true;
    return false;
}
/* Handler selector slot (UINT32_MAX when unbound) and resume point, if protected. */
static bool emit_protection(CBuffer *buffer, const XrXirFunction *function, const XrXirFunctionLayout *layout,
    uint32_t index, uint32_t *destination, uint32_t *handler_pc) {
    uint32_t handler = function->blocks[emit_block(buffer, function, index)].panic;
    if (!handler) return false;
    uint32_t first = function->blocks[handler].first;
    *destination = function->instructions[first].type == XR_XIR_UNIT ? UINT32_MAX :
        layout->offsets[function->parameter_count + first];
    *handler_pc = first + 1;
    return true;
}
static void emit_fault_return(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t index, const char *action) {
    uint32_t destination = 0, handler_pc = 0;
    if (!emit_protection(buffer, function, layout, index, &destination, &handler_pc)) {
        append(buffer, "return %s;", action); return;
    }
    const XrXirBlock *block = &function->blocks[emit_block(buffer, function, index)];
    uint32_t target = function->blocks[block->panic].frontier;
    if (block->frontier != target)
        append(buffer, "{ XrXirAction pending = %s; if (xr_xir_call_panic_action(&pending)) { "
            "state->exit_target = %uu; state->exit_pc = %uu; state->exit_destination = %uu; state->leaving = true; "
            "pending.kind = XR_XIR_ACTION_LEAVE; pending.flags = XR_XIR_ACTION_LEAVE_PANIC; } return pending; }",
            action, target, handler_pc, destination);
    else append(buffer, "return xr_xir_instance_panic_land(view, state->frame, %s, %uu, %uu, &state->pc);",
        action, destination, handler_pc);
}

static void emit_numeric_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t index, bool resumable) {
    const XrXirInstruction *op = &function->instructions[index];
    const char *frame = resumable ? "state->frame" : "frame";
    uint32_t destination = layout->offsets[function->parameter_count + index];
    uint32_t left = layout->offsets[op->args[0]];
    const char *comparison = comparison_symbol(op->op);
    XrXirType input = xr_xir_operand_type(function, op->args[0]);
    append(buffer, "    {\n");
    if (!comparison) append(buffer, "    int64_t temporary = 0;\n");
    if (comparison) {
        append(buffer, "    int ordering = 0;\n    XrXirRunStatus numeric_status = "
            "xr_xir_integer_compare(xr_xir_integer_format((XrXirType) %uu), "
            "xr_xir_scalar_load(%s, %uu), xr_xir_scalar_load(%s, %uu), &ordering);\n",
            (uint32_t) input, frame, left, frame, layout->offsets[op->args[1]]);
    } else if (op->op == XR_XIR_CONVERT_NUMBER) {
        append(buffer, "    XrXirRunStatus numeric_status = xr_xir_number_convert("
            "(XrXirType) %uu, (XrXirType) %uu, "
            "xr_xir_scalar_load(%s, %uu), &temporary);\n", (uint32_t) input, (uint32_t) op->type, frame, left);
    } else if (op->op == XR_XIR_NEG_FLOAT) {
        append(buffer, "    XrXirRunStatus numeric_status = xr_xir_float_negative((XrXirType) %uu, "
            "xr_xir_scalar_load(%s, %uu), &temporary);\n", (uint32_t) input, frame, left);
    } else if (op->op >= XR_XIR_ADD_FLOAT && op->op <= XR_XIR_DIV_FLOAT) {
        append(buffer, "    XrXirRunStatus numeric_status = xr_xir_float_binary((XrXirType) %uu, "
            "(XrXirFloatOperation) %uu, xr_xir_scalar_load(%s, %uu), xr_xir_scalar_load(%s, %uu), &temporary);\n",
            (uint32_t) input, (uint32_t) (op->op - XR_XIR_ADD_FLOAT), frame, left, frame, layout->offsets[op->args[1]]);
    } else if (op->op >= XR_XIR_EQ_FLOAT && op->op <= XR_XIR_GE_FLOAT) {
        append(buffer, "    XrXirRunStatus numeric_status = xr_xir_float_relation((XrXirType) %uu, "
            "(XrXirFloatRelation) %uu, xr_xir_scalar_load(%s, %uu), xr_xir_scalar_load(%s, %uu), &temporary);\n",
            (uint32_t) input, (uint32_t) (op->op - XR_XIR_EQ_FLOAT), frame, left, frame, layout->offsets[op->args[1]]);
    } else {
        append(buffer, "    XrXirRunStatus numeric_status = xr_xir_integer_arithmetic("
            "xr_xir_integer_format((XrXirType) %uu), (XrXirArithmetic) %d, "
            "xr_xir_scalar_load(%s, %uu), xr_xir_scalar_load(%s, %uu), &temporary);\n",
            (uint32_t) op->type, emit_arithmetic_operation(op->op), frame, left, frame, layout->offsets[op->args[1]]);
    }
    if (resumable) {
        append(buffer, "    if (numeric_status != XR_XIR_RUN_OK) ");
        emit_fault_return(buffer, function, layout, index, op->op == XR_XIR_REM_INT ?
            "xr_xir_call_numeric_fault(numeric_status, true)" : "xr_xir_call_numeric_fault(numeric_status, false)");
        append(buffer, "\n");
    }
    else append(buffer, "    if (numeric_status != XR_XIR_RUN_OK) { status = numeric_status; goto xr_done; }\n");
    append(buffer, "    xr_xir_scalar_store(%s, %uu, ", frame, destination);
    if (comparison) append(buffer, "ordering %s 0", comparison);
    else append(buffer, "temporary");
    append(buffer, ");\n    }\n");
}

static bool symbol_prefix_valid(CBuffer *buffer, const char *prefix) {
    if (!prefix)
        return false;
    for (size_t i = 0; i < 65 && emit_work(buffer, 1); ++i) {
        unsigned char c = (unsigned char) prefix[i];
        if (!c)
            return i != 0;
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i && c >= '0' && c <= '9'))
            return false;
    }
    return false;
}

static void emit_constant(CBuffer *buffer, int64_t value) {
    if (value == INT64_MIN)
        append(buffer, "(-INT64_C(9223372036854775807) - INT64_C(1))");
    else if (value < 0)
        append(buffer, "(-INT64_C(%llu))", (unsigned long long) -value);
    else
        append(buffer, "INT64_C(%llu)", (unsigned long long) value);
}

static void emit_edge(CBuffer *buffer, const XrXirFunction *function,
                      const XrXirFunctionLayout *layout, uint32_t instruction,
                      uint32_t target, bool resumable) {
    uint32_t low = 0, high = function->block_count;
    while (low + 1 < high && emit_work(buffer, 1)) {
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first <= instruction) low = middle;
        else high = middle;
    }
    const char *frame = resumable ? "state->frame" : "frame";
    uint32_t first = function->blocks[target].first, end = first;
    while (end < function->instruction_count && emit_work(buffer, 1) && function->instructions[end].op == XR_XIR_PHI) {
        const XrXirInstruction *phi = &function->instructions[end];
        uint32_t source = UINT32_MAX;
        for (uint32_t a = 0; a < phi->args[1] && emit_work(buffer, 1); a += 2)
            if (function->operands[phi->args[0] + a] == low) {
                source = function->operands[phi->args[0] + a + 1]; break;
            }
        if (buffer->status != XR_XIR_OK) return;
        if (source == UINT32_MAX) { emit_reject(buffer, XR_XIR_BAD_STRUCTURE); return; }
        uint32_t scratch = layout->offsets[function->parameter_count + end] + 8;
        if (xr_xir_type_is_owned(buffer->types, phi->type)) {
            if (!resumable) { emit_reject(buffer, XR_XIR_BAD_STAGE); return; }
            append(buffer, "        if (xr_xir_owned_slot_copy(%s, %uu, view->arena, (XrXirType) %u, "
                "xr_xir_scalar_load(%s, %uu)) != XR_XIR_VALUE_OK) goto limit;\n",
                frame, scratch, (uint32_t) phi->type, frame, layout->offsets[source]);
        } else append(buffer, "        xr_xir_scalar_store(%s, %uu, xr_xir_scalar_load(%s, %uu));\n",
            frame, scratch, frame, layout->offsets[source]);
        ++end;
    }
    for (uint32_t i = first; i < end && emit_work(buffer, 1); ++i) {
        const XrXirInstruction *phi = &function->instructions[i];
        uint32_t destination = layout->offsets[function->parameter_count + i];
        if (xr_xir_type_is_owned(buffer->types, phi->type))
            append(buffer, "        { XrXirValue value = {%uu, 0, xr_xir_scalar_load(%s, %uu)};\n"
                "        xr_xir_scalar_store(%s, %uu, 0);\n"
                "        xr_xir_owned_slot_move(%s, %uu, &value); }\n",
                (uint32_t) phi->type, frame, destination + 8, frame, destination + 8, frame, destination);
        else append(buffer, "        xr_xir_scalar_store(%s, %uu, xr_xir_scalar_load(%s, %uu));\n"
            "        xr_xir_scalar_store(%s, %uu, 0);\n", frame, destination, frame, destination + 8, frame, destination + 8);
    }
    if (resumable) append(buffer, "        state->pc = %uu;\n", first);
    else append(buffer, "        goto xr_block_%u;\n", target);
}

static void emit_branch(CBuffer *buffer, const XrXirFunction *function,
                        const XrXirFunctionLayout *layout, uint32_t index, bool resumable) {
    const XrXirInstruction *op = &function->instructions[index];
    append(buffer, "    if (xr_xir_scalar_load(%s, %uu)) {\n",
        resumable ? "state->frame" : "frame", layout->offsets[op->args[0]]);
    emit_edge(buffer, function, layout, index, op->targets[0], resumable);
    append(buffer, "    } else {\n");
    emit_edge(buffer, function, layout, index, op->targets[1], resumable);
    append(buffer, "    }\n");
}

static void emit_instruction(CBuffer *buffer, const XrXirFunction *function,
                             const XrXirFunctionLayout *layout, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "    if (!xr_xir_scalar_step(context)) { status = XR_XIR_RUN_STEP_LIMIT; goto xr_done; }\n");
    switch (op->op) {
    case XR_XIR_PHI: break;
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_INT:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
        break;
    case XR_XIR_LOCAL_UNINIT: break;
    case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_COPY:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu));\n",
               destination, layout->offsets[op->args[0]]);
        break;
    case XR_XIR_SCALAR_LOCAL_WRITE:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu));\n",
               layout->offsets[op->args[0]], layout->offsets[op->args[1]]);
        break;
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT:
    case XR_XIR_SHL_INT: case XR_XIR_SHR_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT:
    case XR_XIR_EQ_INT: case XR_XIR_NE_INT: case XR_XIR_LT_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT:
    case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_CONVERT_NUMBER:
        emit_numeric_step(buffer, function, layout, index, false);
        break;
    case XR_XIR_JUMP:
        emit_edge(buffer, function, layout, index, op->targets[0], false);
        break;
    case XR_XIR_BRANCH:
        emit_branch(buffer, function, layout, index, false);
        break;
    case XR_XIR_RETURN:
        append(buffer, "    result->type = %uu;\n", (uint32_t) function->result);
        if (function->result != XR_XIR_UNIT)
            append(buffer, "    result->payload = xr_xir_scalar_load(frame, %uu);\n",
                   layout->offsets[op->args[0]]);
        append(buffer, "    goto xr_done;\n");
        break;
    default:
        emit_reject(buffer, XR_XIR_BAD_STAGE);
        break;
    }
}

static void emit_function(CBuffer *buffer, const XrXirArtifact *artifact,
                          const char *prefix, uint32_t index) {
    const XrXirFunction *function = &xr_xir_compile_artifact_module(artifact)->functions[index];
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, index);
    append(buffer, "\nXR_FUNC XrXirRunStatus %s_f%u(XrXirRunContext *context, "
           "const XrXirValue *arguments, uint32_t argument_count, XrXirValue *result) {\n"
           "    void *frame = NULL;\n    XrXirRunStatus status;\n", prefix, index);
    append(buffer, "    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;\n"
           "    *result = (XrXirValue) {0, 0, 0};\n"
           "    if (!context || argument_count != %uu || (argument_count && !arguments)) "
           "return XR_XIR_RUN_BAD_ARGUMENT;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count && emit_work(buffer, 1); ++p)
        append(buffer, "    if (!xr_xir_value_argument(&arguments[%u], NULL, (XrXirType) %u)) "
               "return XR_XIR_RUN_BAD_ARGUMENT;\n", p, (uint32_t) function->parameters[p]);
    append(buffer, "    status = xr_xir_scalar_frame_begin(context, %uu, &frame);\n"
           "    if (status != XR_XIR_RUN_OK) return status;\n", layout->frame_bytes);
    for (uint32_t p = 0; p < function->parameter_count && emit_work(buffer, 1); ++p)
        append(buffer, "    xr_xir_scalar_store(frame, %uu, arguments[%u].payload);\n",
               layout->offsets[p], p);
    append(buffer, "    goto xr_block_0;\n");
    for (uint32_t b = 0; b < function->block_count && emit_work(buffer, 1); ++b) {
        append(buffer, "xr_block_%u:\n", b);
        const XrXirBlock *block = &function->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count && emit_work(buffer, 1); ++i)
            emit_instruction(buffer, function, layout, i);
    }
    append(buffer, "xr_done:\n    xr_xir_scalar_frame_end(context, %uu, frame);\n"
           "    if (status != XR_XIR_RUN_OK) *result = (XrXirValue) {0, 0, 0};\n"
           "    return status;\n}\n", layout->frame_bytes);
}

XR_FUNC XrXirStatus xr_xir_compile_emit_leaf_c(const XrXirArtifact *artifact, const char *symbol_prefix,
                        size_t byte_limit, XrXirCSource *output) {
    if (!output || output->text || output->length)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED)
        return XR_XIR_BAD_STAGE;
    CBuffer buffer = {NULL, 0, 0, byte_limit, XR_XIR_OK, module->types, xr_xir_compile_artifact_context(artifact)};
    if (!symbol_prefix_valid(&buffer, symbol_prefix))
        return buffer.status == XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : buffer.status;
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, NULL);
    if (status != XR_XIR_OK)
        return status;
    for (uint32_t f = 0; f < module->function_count && emit_work(&buffer, 1); ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (xr_xir_type_is_owned(module->types, function->result)) return XR_XIR_BAD_STAGE;
        for (uint32_t p = 0; p < function->parameter_count && emit_work(&buffer, 1); ++p)
            if (xr_xir_type_is_owned(module->types, function->parameters[p])) return XR_XIR_BAD_STAGE;
    }
    if (module->declarations) for (uint32_t f = 0; f < module->function_count && emit_work(&buffer, 1); ++f)
        if (module->declarations->functions[f].cleanup_owner) return XR_XIR_BAD_STAGE;
    append(&buffer, "#include \"xir/xxir_float.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_VALUE_ABI_VERSION == %uu, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirValue) == 16, \"XIR boundary size\");\n"
           "_Static_assert(_Alignof(XrXirValue) == 8, \"XIR boundary alignment\");\n"
           "_Static_assert(offsetof(XrXirValue, payload) == 8, \"XIR payload offset\");\n",
           XR_XIR_VALUE_ABI_VERSION);
    for (uint32_t f = 0; f < module->function_count && emit_work(&buffer, 1); ++f)
        emit_function(&buffer, artifact, symbol_prefix, f);
    if (buffer.status != XR_XIR_OK) {
        xr_compile_resources_free(buffer.text);
        return buffer.status;
    }
    XiCgenVerifyStatus verified = xr_compile_cgen_verify_output_or_ice(buffer.context->resources, buffer.text, buffer.length, symbol_prefix);
    if (verified != XI_CGEN_VERIFY_PASSED) {
        xr_compile_resources_free(buffer.text);
        return verified == XI_CGEN_VERIFY_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            verified == XI_CGEN_VERIFY_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    }
    output->text = buffer.text;
    output->length = buffer.length;
    return XR_XIR_OK;
}

static void emit_instance_step(CBuffer *buffer, const XrXirModule *module,
                               const XrXirFunction *function, const XrXirInstruction *op,
                               const XrXirFunctionLayout *layout, uint32_t destination) {
    append(buffer, "        { XrXirCallStatus status = XR_XIR_CALL_READY;\n");
    if (op->op != XR_XIR_CELL_WRITE) append(buffer, "        XrXirValue value = {0};\n");
    switch (op->op) {
    case XR_XIR_CELL_LOCAL_WRITE: {
        uint32_t offset = layout->offsets[op->args[0]];
        XrXirType type = xr_xir_operand_type(function, op->args[0]);
        append(buffer, "        XrXirValue cell = {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        XrXirValue incoming = {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        if (cell.payload) status = xr_xir_instance_cell_write(view, &cell, &incoming);\n"
            "        else { status = xr_xir_instance_cell(view, (XrXirType)%u, &incoming, &value);\n"
            "            if (status == XR_XIR_CALL_READY) xr_xir_owned_slot_move(state->frame, %uu, &value); }\n",
            (uint32_t)type, offset, (uint32_t)xr_xir_operand_type(function, op->args[1]),
            layout->offsets[op->args[1]], (uint32_t)type, offset);
        break;
    }
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE:
        append(buffer, "        XrXirValue left = {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
            (uint32_t) xr_xir_operand_type(function, op->args[0]), layout->offsets[op->args[0]]);
        if (op->op == XR_XIR_CELL_WRITE) {
            append(buffer, "        XrXirValue right = {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
                "        status = xr_xir_instance_cell_write(view, &left, &right);\n",
                (uint32_t) xr_xir_operand_type(function, op->args[1]), layout->offsets[op->args[1]]);
        } else if (op->op == XR_XIR_CELL_NEW)
            append(buffer, "        status = xr_xir_instance_cell(view, (XrXirType) %u, &left, &value);\n", (uint32_t) op->type);
        else append(buffer, "        status = xr_xir_instance_cell_read(view, &left, &value);\n");
        break;
    case XR_XIR_FUNCTION_WEAKEN:
        append(buffer, "        XrXirValue input = {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
            (uint32_t)xr_xir_operand_type(function, op->args[0]), layout->offsets[op->args[0]]);
        append(buffer, "        status = xr_xir_instance_weaken_function(view, (XrXirType)%u, &input, &value);\n", (uint32_t)op->type);
        break;
    case XR_XIR_FUNCTION_REF:
        for (uint32_t p = 0; p < op->args[1] && emit_work(buffer, 1); ++p) {
            uint32_t id = function->operands[op->args[0] + p];
            append(buffer, "state->arguments[%u] = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
                p, (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
        }
        append(buffer, "status = xr_xir_instance_function(view, (XrXirType) %u, %uu, %s, %uu, &value);\n",
            (uint32_t) op->type, (uint32_t) op->immediate, op->args[1] ? "state->arguments" : "NULL", op->args[1]); break;
    case XR_XIR_CONST_STRING:
    case XR_XIR_SLOT_LOAD:
        append(buffer, "        status = %s(view, %uu, &value);\n",
            op->op == XR_XIR_CONST_STRING ? "xr_xir_instance_literal" : "xr_xir_instance_slot_read",
            (uint32_t) op->immediate);
        break;
    case XR_XIR_SLOT_INIT:
    case XR_XIR_SLOT_STORE:
        if (xr_xir_slot_is_unit(module->declarations->slots,module->declarations->slot_count,op))
            append(buffer,"        value = (XrXirValue) {0};\n");
        else append(buffer, "        value = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
            (uint32_t) module->declarations->slots[op->immediate].type, layout->offsets[op->args[0]]);
        append(buffer,"        status = xr_xir_instance_slot_write(view, %uu, &value, %s);\n",
            (uint32_t) op->immediate, op->op == XR_XIR_SLOT_INIT ? "true" : "false");
        break;
    case XR_XIR_ATOMIC_I64_NEW:
        append(buffer, "        status = xr_xir_instance_atomic(view, xr_xir_scalar_load(state->frame, %uu), &value);\n",
            layout->offsets[op->args[0]]);
        break;
    case XR_XIR_ATOMIC_I64_LOAD:
    case XR_XIR_ATOMIC_I64_FETCH_ADD:
        append(buffer, "        XrXirValue atomic = {XR_XIR_ATOMIC_I64, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        value.type = XR_XIR_I64;\n        if (!%s(&atomic, ", layout->offsets[op->args[0]],
            op->op == XR_XIR_ATOMIC_I64_LOAD ? "xr_xir_atomic_i64_load" : "xr_xir_atomic_i64_fetch_add");
        if (op->op == XR_XIR_ATOMIC_I64_FETCH_ADD)
            append(buffer, "xr_xir_scalar_load(state->frame, %uu), ", layout->offsets[op->args[1]]);
        append(buffer, "&value.payload)) status = XR_XIR_CALL_BAD_STATE;\n");
        break;
    default: emit_reject(buffer, XR_XIR_BAD_STAGE); break;
    }
    append(buffer, "        if (status != XR_XIR_CALL_READY) return (XrXirAction) "
        "{XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};\n");
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT)
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};\n");
}

static void emit_resume_call(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination) {
    append(buffer, "        { uint32_t callee = %uu; XrXirValue target = {0};\n", (uint32_t) op->immediate);
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        uint32_t id = (uint32_t) op->immediate;
        append(buffer, "        target = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        XrXirCallStatus status = xr_xir_instance_resolve_function(view, &target, &callee);\n"
            "        if (status != XR_XIR_CALL_READY) return (XrXirAction) "
            "{XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {0}, 0};\n",
            (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
    }
    if (op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT) {
        uint32_t normal = function->blocks[op->targets[0]].first, error = function->blocks[op->targets[1]].first;
        bool discard = function->instructions[normal].op == XR_XIR_INVOKE_DISCARD;
        destination = discard || op->type == XR_XIR_UNIT ? UINT32_MAX : layout->offsets[function->parameter_count + normal];
        append(buffer,"        state->discard = %s;\n",discard ? "true" : "false");
        append(buffer, "        state->invoking = true; state->normal_pc = %uu; state->error_pc = %uu; state->error_destination = %uu;\n",
            normal + (discard || op->type != XR_XIR_UNIT), error + 1, layout->offsets[function->parameter_count + error]);
    } else append(buffer, "        state->invoking = false;\n");
    uint32_t panic_destination = 0, handler_pc = 0;
    bool protected_call = emit_protection(buffer, function, layout, (uint32_t) (op - function->instructions),
        &panic_destination, &handler_pc);
    if (protected_call) {
        uint32_t block = emit_block(buffer, function, (uint32_t)(op - function->instructions));
        append(buffer, "        state->panic_pc = %uu; state->panic_destination = %uu;\n", handler_pc, panic_destination);
        if (emit_has_cleanup(buffer, function)) append(buffer, "        state->panic_frontier = %uu;\n",
            function->blocks[function->blocks[block].panic].frontier);
    }
    append(buffer, "        state->waiting = true; state->destination = %uu; state->expected = %uu;\n",
        destination, (uint32_t) op->type);
    for (uint32_t p = 0; p < op->args[1] && emit_work(buffer, 1); ++p) {
        uint32_t id = function->operands[op->args[0] + p];
        append(buffer, "        state->arguments[%u] = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
            p, (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
    }
    append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_CALL, callee, %s, %uu, target, {0}, %s}; }\n",
        op->args[1] ? "state->arguments" : "NULL", op->args[1], protected_call ? "XR_XIR_ACTION_PROTECTED" : "0u");
}

static void emit_value(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t id) {
    append(buffer, "(XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)}",
        (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
}
static void emit_value_receiver(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t id) {
    append(buffer, "        XrXirValueReceiver receiver = {0}; receiver.type = (XrXirType) %u;\n",
        (uint32_t) xr_xir_operand_type(function, id));
    XrXirPlaceKind kind = xr_xir_place_kind(function, id);
    if (kind == XR_XIR_PLACE_NONE) {
        append(buffer, "        receiver.value = ");
        emit_value(buffer, function, layout, id);
        append(buffer, ";\n");
    } else if (kind == XR_XIR_PLACE_LOCAL) {
        append(buffer, "        receiver.kind = XR_XIR_ROOT_LOCAL; receiver.local_payload = state->frame + %uu;\n",
            layout->offsets[id]);
    } else {
        const XrXirInstruction *place = &function->instructions[id - function->parameter_count];
        if (kind == XR_XIR_PLACE_CELL) {
            append(buffer, "        receiver.kind = XR_XIR_ROOT_CELL; receiver.value = ");
            emit_value(buffer, function, layout, place->args[0]);
            append(buffer, ";\n");
        } else append(buffer, "        receiver.kind = XR_XIR_ROOT_SLOT; receiver.slot = %uu;\n",
            (uint32_t) place->immediate);
    }
}
#include "xxir_emit_path.inc.c"
static void emit_array_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination, uint32_t index) {
    append(buffer, "        { XrXirCallStatus status; XrXirFaultDetail fault = {0};\n");
    if (op->type != XR_XIR_UNIT) append(buffer, "        XrXirValue value = {0};\n");
    if (op->op == XR_XIR_ARRAY_NEW) {
        for (uint32_t i = 0; i < op->args[1] && emit_work(buffer, 1); ++i) {
            append(buffer, "        state->arguments[%u] = ", i);
            emit_value(buffer, function, layout, function->operands[op->args[0] + i]);
            append(buffer, ";\n");
        }
        append(buffer, "        status = xr_xir_instance_array_new(view, (XrXirType) %u, %s, %uu, &value);\n",
            (uint32_t) op->type, op->args[1] ? "state->arguments" : "NULL", op->args[1]);
    } else {
        bool setting = op->op == XR_XIR_ARRAY_SET, pushing = op->op == XR_XIR_ARRAY_PUSH;
        const uint32_t *args = setting ? &function->operands[op->args[0]] : op->args;
        emit_value_receiver(buffer, function, layout, args[0]);
        if (setting || pushing) {
            append(buffer, "        XrXirValue element = ");
            emit_value(buffer, function, layout, args[setting ? 2 : 1]);
            append(buffer, ";\n");
        }
        append(buffer, "        status = %s(view, &receiver, ",
            setting || pushing ? "xr_xir_instance_array_write" : "xr_xir_instance_array_read");
        if (setting || op->op == XR_XIR_ARRAY_GET)
            append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[args[1]]);
        else append(buffer, "0");
        if (setting || pushing) append(buffer, ", &element, %s, &fault);\n", pushing ? "true" : "false");
        else append(buffer, ", %s, &value, &fault);\n", op->op == XR_XIR_ARRAY_LEN ? "true" : "false");
    }
    append(buffer, "        if (status != XR_XIR_CALL_READY) ");
    emit_fault_return(buffer, function, layout, index,
        "(XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {fault, {0}}, 0}");
    append(buffer, "\n");
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT)
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}

static void emit_class_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination) {
    append(buffer,"        { XrXirValue value={0}; XrXirValueStatus status;\n");
    if(op->op == XR_XIR_CLASS_NEW) {
        for(uint32_t i=0;i<op->args[1];++i) {
            append(buffer,"        state->arguments[%u] = ",i);
            emit_value(buffer,function,layout,function->operands[op->args[0]+i]);append(buffer,";\n");
        }
        append(buffer,"        status=xr_xir_class_new((XrXirType)%uu,%s,%uu,xr_xir_call_admission(view),&value);\n",
            (uint32_t)op->type,op->args[1]?"state->arguments":"NULL",op->args[1]);
    } else {
        append(buffer,"        XrXirValue receiver = ");emit_value(buffer,function,layout,op->args[0]);append(buffer,";\n");
        if(op->op == XR_XIR_CLASS_SET) {
            append(buffer,"        value = ");emit_value(buffer,function,layout,op->args[1]);append(buffer,";\n");
            append(buffer,"        status=xr_xir_class_set(&receiver,%uu,&value,xr_xir_call_admission(view));\n",(uint32_t)op->immediate);
        } else append(buffer,"        status=xr_xir_value_admit(&receiver,(XrXirType)receiver.type,xr_xir_call_admission(view));\n"
            "        if(status == XR_XIR_VALUE_OK) status=xr_xir_class_get(&receiver,%uu,xr_xir_call_admission(view),&value);\n",(uint32_t)op->immediate);
    }
    append(buffer,"        if(status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
        "            status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
        "            status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n");
    if(op->op != XR_XIR_CLASS_SET) {
        if(xr_xir_type_is_owned(buffer->types,op->type))
            append(buffer,"        xr_xir_owned_slot_move(state->frame,%uu,&value);\n",destination);
        else append(buffer,"        xr_xir_scalar_store(state->frame,%uu,value.payload);\n",destination);
    }
    append(buffer,"        }\n        return (XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};\n");
}

static void emit_nominal_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination) {
    if (op->op == XR_XIR_STRUCT_SET) {
        append(buffer, "        {\n"); emit_value_receiver(buffer, function, layout, op->args[0]);
        append(buffer, "        XrXirValue value = "); emit_value(buffer, function, layout, op->args[1]);
        append(buffer, ";\n        XrXirCallStatus status = xr_xir_instance_struct_write(view, &receiver, %uu, &value);\n"
            "        if (status != XR_XIR_CALL_READY) return xr_xir_call_fault(\n"
            "            status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
            "            status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n", (uint32_t) op->immediate);
        return;
    }
    append(buffer, "        { XrXirValue value = {0}; XrXirValueStatus status;\n");
    if (op->op == XR_XIR_STRUCT_NEW || op->op == XR_XIR_ENUM_NEW) {
        for (uint32_t i = 0; i < op->args[1] && emit_work(buffer, 1); ++i) {
            append(buffer, "        state->arguments[%u] = ", i);
            emit_value(buffer, function, layout, function->operands[op->args[0] + i]);
            append(buffer, ";\n");
        }
        if (op->op == XR_XIR_ENUM_NEW)
            append(buffer, "        status = xr_xir_enum_new((XrXirType) %uu, %uu, %s, %uu, xr_xir_call_admission(view), &value);\n",
                (uint32_t) op->type, (uint32_t) op->immediate, op->args[1] ? "state->arguments" : "NULL", op->args[1]);
        else append(buffer, "        status = xr_xir_struct_new((XrXirType) %uu, %s, %uu, xr_xir_call_admission(view), &value);\n",
            (uint32_t) op->type, op->args[1] ? "state->arguments" : "NULL", op->args[1]);
    } else {
        append(buffer, "        XrXirValue receiver = "); emit_value(buffer, function, layout, op->args[0]);
        if (op->op == XR_XIR_ERROR_ERASE)
            append(buffer, ";\n        status = xr_xir_error_erase(&receiver, xr_xir_call_admission(view), &value);\n");
        else if (op->op == XR_XIR_ERROR_NARROW)
            append(buffer, ";\n        status = xr_xir_error_narrow(&receiver, (XrXirType) %uu, xr_xir_call_admission(view), &value);\n", (uint32_t) op->type);
        else if (op->op == XR_XIR_ERROR_IS)
            append(buffer, ";\n        bool matches = false; status = xr_xir_error_is(&receiver, (XrXirType) %uu, xr_xir_call_admission(view), &matches);\n"
                "        value = (XrXirValue) {XR_XIR_BOOL, 0, matches};\n", (uint32_t) op->immediate);
        else if (op->op == XR_XIR_ENUM_TAG)
            append(buffer, ";\n        uint32_t variant = 0; status = xr_xir_enum_variant(&receiver, &variant);\n"
                "        value = (XrXirValue) {XR_XIR_I64, 0, variant};\n");
        else if (op->op == XR_XIR_ENUM_GET)
            append(buffer, ";\n        status = xr_xir_enum_get(&receiver, %uu, %uu, xr_xir_call_admission(view), &value);\n",
                (uint32_t) op->immediate, op->args[1]);
        else append(buffer, ";\n        status = xr_xir_struct_get(&receiver, %uu, xr_xir_call_admission(view), &value);\n",
            (uint32_t) op->immediate);
    }
    append(buffer, "        if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
        "            status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
        "            status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n");
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}

#include "xxir_emit_cleanup.inc.c"

static void emit_io_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    switch (op->op) {
    case XR_XIR_STRING_INDEX_OF: case XR_XIR_STRING_LAST_INDEX_OF: {
        const uint32_t *args = op->op == XR_XIR_STRING_INDEX_OF ? &function->operands[op->args[0]] : op->args;
        append(buffer, "        { XrXirValue left = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        XrXirValue right = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        int64_t result = 0; XrXirValueStatus status;\n", layout->offsets[args[0]], layout->offsets[args[1]]);
        if (op->op == XR_XIR_STRING_INDEX_OF) {
            append(buffer, "        int64_t start = xr_xir_scalar_load(state->frame, %uu);\n"
                "        status = xr_xir_string_index_of(&left, &right, start, &result);\n"
                "        if (status == XR_XIR_VALUE_BOUNDS) { int64_t length = 0;\n"
                "            if (xr_xir_string_length(&left, &length) != XR_XIR_VALUE_OK) goto limit;\n"
                "            ", layout->offsets[args[2]]);
            emit_fault_return(buffer, function, layout, index, "xr_xir_call_bounds(start, length)");
            append(buffer, " }\n");
        } else append(buffer, "        status = xr_xir_string_last_index_of(&left, &right, &result);\n");
        append(buffer, "        if (status != XR_XIR_VALUE_OK) goto limit;\n"
            "        xr_xir_scalar_store(state->frame, %uu, result); }\n", destination);
        break;
    }
    case XR_XIR_STRING_CONTAINS: case XR_XIR_STRING_STARTS_WITH: case XR_XIR_STRING_ENDS_WITH:
        append(buffer, "        { XrXirValue left = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        XrXirValue right = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        bool result = false; if (!%s(&left, &right, &result)) goto limit;\n"
            "        xr_xir_scalar_store(state->frame, %uu, result); }\n",
            layout->offsets[op->args[0]], layout->offsets[op->args[1]],
            op->op == XR_XIR_STRING_CONTAINS ? "xr_xir_string_contains" :
            op->op == XR_XIR_STRING_STARTS_WITH ? "xr_xir_string_starts_with" : "xr_xir_string_ends_with", destination);
        break;
    case XR_XIR_STRING_LEN: case XR_XIR_EQ_STRING: case XR_XIR_NE_STRING:
        append(buffer, "        { XrXirValue left = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        int64_t result = 0; XrXirValueStatus status;\n", layout->offsets[op->args[0]]);
        if (op->op == XR_XIR_STRING_LEN)
            append(buffer, "        status = xr_xir_string_length(&left, &result);\n");
        else {
            append(buffer, "        XrXirValue right = {XR_XIR_STRING, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
                "        bool equal = false; status = xr_xir_string_equal(&left, &right, &equal) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;\n"
                "        result = %sequal;\n", layout->offsets[op->args[1]], op->op == XR_XIR_EQ_STRING ? "" : "!");
        }
        append(buffer, "        if (status != XR_XIR_VALUE_OK) goto limit;\n"
            "        xr_xir_scalar_store(state->frame, %uu, result); }\n", destination);
        break;
    case XR_XIR_OUTPUT:
    case XR_XIR_WRITE_STREAM:
    case XR_XIR_PRINT: {
        uint32_t count = op->op == XR_XIR_PRINT ? op->args[1] : 1;
        for (uint32_t p = 0; p < count && emit_work(buffer, 1); ++p) {
            uint32_t id = op->op == XR_XIR_PRINT ? function->operands[op->args[0] + p] : op->args[p];
            XrXirType type = id < function->parameter_count ? function->parameters[id] :
                function->instructions[id - function->parameter_count].type;
            append(buffer, "        state->arguments[%u] = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
                   p, (uint32_t) type, layout->offsets[id]);
        }
        if (op->op == XR_XIR_WRITE_STREAM)
            append(buffer, "        state->waiting = true; state->destination = %uu; state->expected = XR_XIR_BOOL;\n", destination);
        append(buffer, "        return (XrXirAction) {%s, %uu, %s, %uu, {0}, {0}, 0};\n",
            op->op == XR_XIR_WRITE_STREAM ? "XR_XIR_ACTION_WRITE_STREAM" : "XR_XIR_ACTION_OUTPUT",
            op->op == XR_XIR_PRINT ? 3u : (uint32_t) op->immediate, count ? "state->arguments" : "NULL", count);
        return;
    }
    default: emit_reject(buffer, XR_XIR_BAD_STAGE); return;
    }
    append(buffer, "        return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
/* Clock reads and UTC offsets are synchronous instance services. A range
 * rejection is a language fault; a provider failure aborts the activation. */
static void emit_time_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "        { int64_t reading = 0;\n        XrXirCallStatus time_status = ");
    if (op->op == XR_XIR_CLOCK_NANOS)
        append(buffer, "xr_xir_instance_clock_ns(view, (XrXirClockKind) %uu, &reading);\n", (uint32_t) op->immediate);
    else append(buffer, "xr_xir_instance_utc_offset(view, xr_xir_scalar_load(state->frame, %uu), &reading);\n",
        layout->offsets[op->args[0]]);
    append(buffer, "        if (time_status == XR_XIR_CALL_NUMERIC_RANGE) ");
    emit_fault_return(buffer, function, layout, index, "xr_xir_call_fault(XR_XIR_RUN_NUMERIC_RANGE)");
    append(buffer, "\n        if (time_status == XR_XIR_CALL_HOST_ERROR) return xr_xir_call_fault(XR_XIR_RUN_HOST_ERROR);\n"
        "        if (time_status != XR_XIR_CALL_READY) goto invalid;\n"
        "        xr_xir_scalar_store(state->frame, %uu, reading); }\n", destination);
}
static void emit_resume_step(CBuffer *buffer, const XrXirModule *module,
                            const XrXirFunction *function, const XrXirFunctionLayout *layout,
                            uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "    case %uu:\n        state->pc = %uu;\n", index, index + 1);
    if (op->op == XR_XIR_STRING_INDEX_OF || op->op == XR_XIR_STRING_LAST_INDEX_OF ||
        (op->op >= XR_XIR_STRING_CONTAINS && op->op <= XR_XIR_STRING_ENDS_WITH) ||
        op->op == XR_XIR_STRING_LEN || op->op == XR_XIR_EQ_STRING || op->op == XR_XIR_NE_STRING ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT) {
        emit_io_step(buffer, function, layout, index); return;
    }
    if (op->op == XR_XIR_CLEANUP_REGISTER || op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR) {
        emit_cleanup_step(buffer, function, layout, index); return;
    }
    if (xr_xir_op_uses_value_path(function, op)) {
        emit_path_step(buffer, function, op, layout, destination, index); return;
    }
    if (op->op == XR_XIR_CLASS_NEW || op->op == XR_XIR_CLASS_GET || op->op == XR_XIR_CLASS_SET) {
        emit_class_step(buffer,function,op,layout,destination); return;
    }
    if ((op->op >= XR_XIR_STRUCT_NEW && op->op <= XR_XIR_STRUCT_SET) ||
        (op->op >= XR_XIR_ENUM_NEW && op->op <= XR_XIR_ENUM_GET) || op->op == XR_XIR_ERROR_ERASE ||
        op->op == XR_XIR_ERROR_IS || op->op == XR_XIR_ERROR_NARROW) {
        emit_nominal_step(buffer, function, op, layout, destination); return;
    }
    if (op->op >= XR_XIR_ARRAY_NEW && op->op <= XR_XIR_ARRAY_LEN) {
        emit_array_step(buffer, function, op, layout, destination, index);
        return;
    }
    if ((op->op >= XR_XIR_CONST_STRING && op->op <= XR_XIR_ATOMIC_I64_FETCH_ADD) || op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_FUNCTION_WEAKEN ||
        (op->op >= XR_XIR_CELL_NEW && op->op <= XR_XIR_CELL_WRITE) || op->op == XR_XIR_CELL_LOCAL_WRITE) {
        emit_instance_step(buffer, module, function, op, layout, destination);
        return;
    }
    switch (op->op) {
    case XR_XIR_PHI: case XR_XIR_CELL_PLACE: case XR_XIR_SLOT_PLACE:
    case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE: break;
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_FLOAT:
    case XR_XIR_CONST_INT:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
        break;
    case XR_XIR_LOCAL_UNINIT:
        if (xr_xir_type_is_owned(module->types, op->type))
            append(buffer, "        xr_xir_owned_slot_clear(state->frame, %uu);\n", destination);
        break;
    case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ:
    case XR_XIR_SCALAR_COPY:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, "
               "xr_xir_scalar_load(state->frame, %uu));\n", destination, layout->offsets[op->args[0]]);
        break;
    case XR_XIR_SCALAR_LOCAL_WRITE:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, xr_xir_scalar_load(state->frame, %uu));\n",
               layout->offsets[op->args[0]], layout->offsets[op->args[1]]);
        break;
    case XR_XIR_OWNED_LOCAL_WRITE:
        append(buffer, "        if (xr_xir_owned_slot_copy(state->frame, %uu, view->arena, (XrXirType) %u, "
               "xr_xir_scalar_load(state->frame, %uu)) != XR_XIR_VALUE_OK) goto limit;\n",
               layout->offsets[op->args[0]], (uint32_t) function->instructions[op->args[0] - function->parameter_count].type,
               layout->offsets[op->args[1]]);
        break;
    case XR_XIR_OWNED_LOCAL_NEW:
    case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING:
        append(buffer, "        { XrXirValueStatus status = %s(state->frame, %uu, ",
               op->op != XR_XIR_CONCAT_STRING ? "xr_xir_owned_slot_copy" : "xr_xir_string_slot_concat", destination);
        if (op->op != XR_XIR_CONCAT_STRING) append(buffer, "view->arena, (XrXirType) %u, ", (uint32_t) op->type);
        append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[0]]);
        if (op->op == XR_XIR_CONCAT_STRING)
            append(buffer, ", xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[1]]);
        append(buffer, ");\n        if (status != XR_XIR_VALUE_OK)\n"
               "            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, "
               "{XR_XIR_I64, 0, status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT}, {0}, 0}; }\n");
        break;
    case XR_XIR_ADD_INT: case XR_XIR_SUB_INT: case XR_XIR_MUL_INT:
    case XR_XIR_AND_INT: case XR_XIR_OR_INT: case XR_XIR_XOR_INT:
    case XR_XIR_SHL_INT: case XR_XIR_SHR_INT:
    case XR_XIR_DIV_INT: case XR_XIR_REM_INT:
    case XR_XIR_EQ_INT: case XR_XIR_NE_INT: case XR_XIR_LT_INT:
    case XR_XIR_LE_INT: case XR_XIR_GT_INT: case XR_XIR_GE_INT:
    case XR_XIR_NEG_FLOAT: case XR_XIR_EQ_FLOAT: case XR_XIR_NE_FLOAT:
    case XR_XIR_LT_FLOAT: case XR_XIR_LE_FLOAT: case XR_XIR_GT_FLOAT: case XR_XIR_GE_FLOAT:
    case XR_XIR_ADD_FLOAT: case XR_XIR_SUB_FLOAT: case XR_XIR_MUL_FLOAT: case XR_XIR_DIV_FLOAT:
    case XR_XIR_CONVERT_NUMBER:
        emit_numeric_step(buffer, function, layout, index, true);
        break;
    case XR_XIR_JUMP:
        emit_edge(buffer, function, layout, index, op->targets[0], true);
        break;
    case XR_XIR_BRANCH:
        emit_branch(buffer, function, layout, index, true);
        break;
    case XR_XIR_CALL: case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE: case XR_XIR_INVOKE_INDIRECT:
        emit_resume_call(buffer, function, op, layout, destination);
        return;
    case XR_XIR_INVOKE_RESULT: case XR_XIR_INVOKE_ERROR: case XR_XIR_INVOKE_DISCARD: case XR_XIR_PANIC_CATCH:
        append(buffer, "        goto invalid;\n"); return;
    case XR_XIR_PANIC_CODE:
        append(buffer, "        { XrXirValue info = "); emit_value(buffer, function, layout, op->args[0]);
        append(buffer, ";\n        XrXirFaultDetail detail;\n        if (!xr_xir_panic_info_detail(&info, &detail)) goto invalid;\n"
            "        xr_xir_scalar_store(state->frame, %uu, (int64_t) detail.code); }\n", destination);
        break;
    case XR_XIR_PANIC_MESSAGE:
        append(buffer, "        { XrXirValue info = "); emit_value(buffer, function, layout, op->args[0]);
        append(buffer, ";\n        XrXirValue message = {0};\n"
            "        XrXirValueStatus status = xr_xir_panic_info_message(&info, &message);\n"
            "        if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
            "            status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
            "            status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        xr_xir_owned_slot_move(state->frame, %uu, &message); }\n", destination);
        break;
    case XR_XIR_SUSPEND:
        append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0, 0, 0}, {0}, 0};\n");
        return;
    case XR_XIR_TIMER_AFTER_MS:
        append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_TIMER, 0, NULL, 0, "
            "{XR_XIR_I64, 0, xr_xir_scalar_load(state->frame, %uu)}, {0}, 0};\n", layout->offsets[op->args[0]]);
        return;
    case XR_XIR_CLOCK_NANOS: case XR_XIR_UTC_OFFSET_AT:
        emit_time_step(buffer, function, layout, index);
        break;
    case XR_XIR_NULLABLE_NONE: case XR_XIR_NULLABLE_SOME:
        append(buffer,"        { XrXirValue output = {0};\n");
        if (op->op == XR_XIR_NULLABLE_SOME) {
            append(buffer,"        XrXirValue payload = "); emit_value(buffer,function,layout,op->args[0]);
            append(buffer,";\n");
        }
        append(buffer,
            "        XrXirValueStatus status = xr_xir_nullable_new((XrXirType)%uu,%s,\n"
            "            xr_xir_call_admission(view),&output);\n"
            "        if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
            "            status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
            "            status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        xr_xir_owned_slot_move(state->frame,%uu,&output); }\n",
            (uint32_t)op->type,op->op == XR_XIR_NULLABLE_SOME ? "&payload" : "NULL",destination);
        break;
    case XR_XIR_EQUAL:
        append(buffer,"        { XrXirValue left = "); emit_value(buffer,function,layout,op->args[0]);
        append(buffer,"; XrXirValue right = "); emit_value(buffer,function,layout,op->args[1]);
        append(buffer,";\n        XrXirValueAdmission *admission = xr_xir_call_admission(view);\n"
            "        bool equal = false;\n"
            "        XrXirValueStatus status = xr_xir_value_equal(&left,&right,(XrXirType)left.type,admission,&equal);\n"
            "        if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault(\n"
            "            status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :\n"
            "            status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        xr_xir_scalar_store(state->frame,%uu,%sequal); }\n",destination,op->immediate ? "!" : "");
        break;
    case XR_XIR_ASSERT_CONDITION:
        append(buffer, "        if (!xr_xir_scalar_load(state->frame, %uu)) {\n"
            "            XrXirValue message = ", layout->offsets[op->args[0]]);
        emit_value(buffer, function, layout, op->args[1]);
        append(buffer, ";\n            ");
        emit_fault_return(buffer, function, layout, index, "xr_xir_call_assertion(&message)");
        append(buffer, "\n        }\n");
        break;
    case XR_XIR_MATCH_FAIL:
        append(buffer, "        ");
        emit_fault_return(buffer, function, layout, index, "xr_xir_call_match_failure()");
        append(buffer, "\n");
        return;
    case XR_XIR_THROW:
    case XR_XIR_RETURN: {
        uint32_t type = op->op == XR_XIR_THROW ? (uint32_t) xr_xir_operand_type(function, op->args[0]) : (uint32_t) function->result;
        if (op->op == XR_XIR_THROW && type == XR_XIR_ERROR) {
            append(buffer, "        { XrXirValue erased = "); emit_value(buffer, function, layout, op->args[0]);
            append(buffer, "; XrXirValue concrete = {0};\n"
                "        if (!xr_xir_error_borrow(&erased, &concrete)) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);\n"
                "        return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, concrete, {0}, 0}; }\n");
            return;
        }
        append(buffer, "        return (XrXirAction) {%s, 0, NULL, 0, {%uu, 0, ",
               op->op == XR_XIR_THROW ? "XR_XIR_ACTION_THROW" : "XR_XIR_ACTION_RETURN", type);
        if (type == XR_XIR_UNIT) append(buffer, "0");
        else append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[0]]);
        append(buffer, "}, {0}, 0};\n");
        return;
    }
    default:
        emit_reject(buffer, XR_XIR_BAD_STAGE);
        return;
    }
    append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}, {0}, 0};\n");
}

static void emit_resume_function(CBuffer *buffer, const XrXirArtifact *artifact,
                                  const char *prefix, uint32_t index) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirFunction *function = &module->functions[index];
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, index);
    if ((uint64_t) layout->frame_bytes + (uint64_t) layout->outgoing_count * sizeof(XrXirValue) +
        (uint64_t) layout->path_count * sizeof(XrXirValuePathStep) > UINT32_MAX - 128u) {
        emit_reject(buffer, XR_XIR_BUDGET);
        return;
    }
    append(buffer, "typedef struct %s_state_%u {\n"
           "    uint32_t pc, destination, expected, normal_pc, error_pc, error_destination, panic_pc, panic_destination;\n"
           "    bool initialized, waiting, invoking, discard;\n", prefix, index);
    if (emit_has_cleanup(buffer, function)) append(buffer,
        "    uint32_t frontier, cleanup_parent, exit_target, exit_pc, exit_destination, leave_instruction, panic_frontier;\n"
        "    bool cleanup_waiting, leaving;\n");
    if (layout->outgoing_count) append(buffer, "    XrXirValue arguments[%u];\n", layout->outgoing_count);
    if (layout->path_count) append(buffer, "    XrXirValuePathStep path_steps[%u];\n", layout->path_count);
    append(buffer, "    unsigned char frame[%u];\n} %s_state_%u;\n",
           layout->frame_bytes ? layout->frame_bytes : 1, prefix, index);
    append(buffer, "XR_FUNC XrXirAction %s_f%u(XrXirCallView *view) {\n"
           "    %s_state_%u *state = view->state;\n", prefix, index, prefix, index);
    emit_cleanup_entry(buffer, function, layout);
    append(buffer, "    if (!state->initialized) {\n"
           "        if (view->argument_count != %uu) goto invalid;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count && emit_work(buffer, 1); ++p) {
        append(buffer, "        if (!xr_xir_value_argument(&view->arguments[%u], view->arena, (XrXirType) %u)) goto invalid;\n",
               p, (uint32_t) function->parameters[p]);
        if (xr_xir_type_is_owned(module->types, function->parameters[p]))
            append(buffer, "        if (xr_xir_owned_slot_copy(state->frame, %uu, view->arena, (XrXirType) %u, view->arguments[%u].payload) "
                   "!= XR_XIR_VALUE_OK) goto limit;\n", layout->offsets[p], (uint32_t) function->parameters[p], p);
        else
            append(buffer, "        xr_xir_scalar_store(state->frame, %uu, view->arguments[%u].payload);\n",
                   layout->offsets[p], p);
    }
    append(buffer, "        state->initialized = true;\n    }\n"
           "    if (state->waiting) {\n        state->waiting = false;\n"
           "        uint32_t panic_pc = state->panic_pc; state->panic_pc = 0;\n"
           "        if (xr_xir_call_panic_status(view->inbox.status)) {\n"
           "            if (!panic_pc) goto invalid;\n"
           "            state->invoking = false;\n");
    if (emit_has_cleanup(buffer, function)) append(buffer,
           "            if (state->frontier != state->panic_frontier) {\n"
           "                state->exit_target = state->panic_frontier; state->exit_pc = panic_pc;\n"
           "                state->exit_destination = state->panic_destination; state->leaving = true;\n"
           "                return (XrXirAction){XR_XIR_ACTION_LEAVE, 0, NULL, 0, {XR_XIR_I64, 0, view->inbox.status},\n"
           "                    view->inbox.panic, XR_XIR_ACTION_LEAVE_PANIC};\n            }\n");
    append(buffer, "            return xr_xir_instance_panic_land(view, state->frame, (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, "
           "{XR_XIR_I64, 0, view->inbox.status}, view->inbox.panic, 0}, state->panic_destination, panic_pc, &state->pc);\n"
           "        }\n"
           "        XrXirValue inbox = view->inbox.value;\n"
           "        bool discarded = false;\n"
           "        if (state->invoking) {\n"
           "            bool error = view->inbox.status == XR_XIR_CALL_THROWN;\n"
           "            if (!error && view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;\n"
           "            state->pc = error ? state->error_pc : state->normal_pc; state->invoking = false;\n"
           "            if (error) { state->expected = XR_XIR_ERROR; state->destination = state->error_destination; inbox.type = XR_XIR_ERROR; }\n"
           "            else if (state->discard) {\n"
           "                if (xr_xir_call_discard_inbox(view, (XrXirType)state->expected) != XR_XIR_CALL_READY) goto invalid;\n"
           "                discarded = true;\n            }\n"
           "        } else if (view->inbox.status == XR_XIR_CALL_THROWN)\n"
           "            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, inbox, {0}, 0};\n"
           "        if (!discarded) {\n"
           "        if (view->inbox.status != XR_XIR_CALL_RETURNED && view->inbox.status != XR_XIR_CALL_THROWN) goto invalid;\n"
           "        if (state->expected == XR_XIR_UNIT) {\n"
           "            if (inbox.type || inbox.reserved || inbox.payload) goto invalid;\n"
           "        } else if (!xr_xir_value_argument(&inbox, view->arena, (XrXirType) state->expected)) goto invalid;\n"
           "        if (xr_xir_type_is_owned(xr_xir_compile_type_arena_types(view->arena), (XrXirType) state->expected)) {\n"
           "            if (xr_xir_owned_slot_copy(state->frame, state->destination, view->arena, (XrXirType) state->expected, inbox.payload) "
           "!= XR_XIR_VALUE_OK) goto limit;\n"
           "        } else if (state->destination != UINT32_MAX)\n"
           "            xr_xir_scalar_store(state->frame, state->destination, inbox.payload);\n"
           "        }\n    }\n    switch (state->pc) {\n");
    for (uint32_t i = 0; i < function->instruction_count && emit_work(buffer, 1); ++i)
        emit_resume_step(buffer, module, function, layout, i);
    append(buffer, "    default: break;\n    }\ninvalid:\n"
           "    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}, {0}, 0};\n"
           "limit:\n    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}, {0}, 0};\n}\n");
    append(buffer, "static void %s_release_%u(XrXirCallView *view, XrXirCallStatus reason) {\n"
           "    (void) reason; (void) view;\n", prefix, index);
    if (layout->owned_count) {
        append(buffer, "    %s_state_%u *state = view->state;\n", prefix, index);
        for (uint32_t i = layout->owned_count; i > 0 && emit_work(buffer, 1); --i)
            append(buffer, "    xr_xir_owned_slot_clear(state->frame, %uu);\n", layout->owned_offsets[i - 1]);
    }
    append(buffer, "}\n");
    if (function->parameter_count) {
        append(buffer, "static const XrXirType %s_parameters_%u[] = {", prefix, index);
        for (uint32_t p = 0; p < function->parameter_count && emit_work(buffer, 1); ++p)
            append(buffer, "%s(XrXirType) %u", p ? ", " : "", (uint32_t) function->parameters[p]);
        append(buffer, "};\n");
    }
}

static void emit_bytes(CBuffer *buffer, const char *bytes, uint32_t length) {
    /* Every call initializes a file-scope table, so the compound array has
     * static storage. Unsigned elements preserve all bytes without narrowing
     * warnings or the C11 minimum limit on concatenated string literals. */
    append(buffer, "(const char *)(const unsigned char[]){");
    for (uint32_t i = 0; i < length && emit_work(buffer, 1); ++i) {
        unsigned int byte = (unsigned char) bytes[i];
        if (i % 16 == 0) append(buffer, "\n    ");
        append(buffer, "0x%02x,", byte);
    }
    append(buffer, "\n    0}");
}
static void emit_nominal_identities(CBuffer *buffer, const XrXirNominalTable *table, const char *prefix) {
    if (!table) return;
    for (uint32_t i = 0; i < table->count && emit_work(buffer, 1); ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        if (!d->field_count) continue;
        append(buffer, "static const XrXirNominalFieldIdentity %s_nominal_fields_%u[] = {\n", prefix, i);
        for (uint32_t f = 0; f < d->field_count && emit_work(buffer, 1); ++f) {
            append(buffer, "    {{"); emit_bytes(buffer, d->fields[f].name.bytes, d->fields[f].name.length);
            append(buffer, ", %uu}, %uu},\n", d->fields[f].name.length, d->fields[f].flags);
        }
        append(buffer, "};\n");
    }
    for (uint32_t i = 0; i < table->count && emit_work(buffer, 1); ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        if (!d->variant_count) continue;
        append(buffer, "static const XrXirNominalVariant %s_nominal_variants_%u[] = {\n", prefix, i);
        for (uint32_t v = 0; v < d->variant_count && emit_work(buffer, 1); ++v) {
            const XrXirNominalVariant *variant = &d->variants[v];
            append(buffer, "    {{"); emit_bytes(buffer, variant->name.bytes, variant->name.length);
            append(buffer, ", %uu}, %uu, %uu},\n", variant->name.length, variant->field_begin, variant->field_count);
        }
        append(buffer, "};\n");
    }
    append(buffer, "static const XrXirNominalIdentity %s_nominal_identities[] = {\n", prefix);
    for (uint32_t i = 0; i < table->count && emit_work(buffer, 1); ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        append(buffer, "    {{"); emit_bytes(buffer, d->module.bytes, d->module.length);
        append(buffer, ", %uu}, {", d->module.length); emit_bytes(buffer, d->name.bytes, d->name.length);
        append(buffer, ", %uu}, %uu, %uu, ", d->name.length, d->exported, d->arity);
        if (d->field_count) append(buffer, "%s_nominal_fields_%u", prefix, i); else append(buffer, "NULL");
        append(buffer, ", %uu, %uu, ", d->field_count, d->kind);
        if (d->variant_count) append(buffer, "%s_nominal_variants_%u", prefix, i); else append(buffer, "NULL");
        append(buffer, ", %uu, %uu},\n", d->variant_count, d->flags);
    }
    append(buffer, "};\nstatic const XrXirNominalTable %s_nominals = {NULL, %uu, %s_nominal_identities};\n",
        prefix, table->count, prefix);
}
static void emit_type_ids(CBuffer *buffer, const char *prefix, uint32_t index,
                          const char *name, const XrXirType *types, uint32_t count) {
    if (!count) return;
    append(buffer, "static const XrXirType %s_%s_%u[] = {", prefix, name, index);
    for (uint32_t i = 0; i < count && emit_work(buffer, 1); ++i) append(buffer, "%s(XrXirType) %uu", i ? ", " : "", (uint32_t) types[i]);
    append(buffer, "};\n");
}
static void emit_types(CBuffer *buffer, const XrXirTypes *types, const char *prefix) {
    if (!types) return;
    emit_nominal_identities(buffer, types->nominals, prefix);
    for (uint32_t i = 0; i < types->count && emit_work(buffer, 1); ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        emit_type_ids(buffer, prefix, i, "nominal_arguments", node->nominal.arguments, node->nominal.argument_count);
        emit_type_ids(buffer, prefix, i, "nominal_types", node->nominal.fields, node->nominal.field_count);
        if (!node->parameter_count) continue;
        append(buffer, "static const XrXirCallableParameter %s_type_parameters_%u[] = {", prefix, i);
        for (uint32_t p = 0; p < node->parameter_count && emit_work(buffer, 1); ++p)
            append(buffer, "%s{(XrXirType) %u, %uu}", p ? ", " : "", (uint32_t) node->parameters[p].type, node->parameters[p].mode);
        append(buffer, "};\n");
    }
    if (types->count) append(buffer, "static const XrXirTypeNode %s_type_nodes[] = {\n", prefix);
    for (uint32_t i = 0; i < types->count && emit_work(buffer, 1); ++i) {
        const XrXirTypeNode *node = &types->nodes[i];
        append(buffer, "    {%uu, (XrXirType) %u, ", node->kind, (uint32_t) node->element);
        if (node->parameter_count) append(buffer, "%s_type_parameters_%u", prefix, i); else append(buffer, "NULL");
        append(buffer, ", %uu, (XrXirType) %u, %uu, %uu, {%uu, ", node->parameter_count,
            (uint32_t) node->result, node->flags, node->parameter_span, node->nominal.declaration);
        if (node->nominal.argument_count) append(buffer, "%s_nominal_arguments_%u", prefix, i); else append(buffer, "NULL");
        append(buffer, ", %uu, ", node->nominal.argument_count);
        if (node->nominal.field_count) append(buffer, "%s_nominal_types_%u", prefix, i); else append(buffer, "NULL");
        append(buffer, ", %uu}},\n", node->nominal.field_count);
    }
    if (types->count) append(buffer, "};\n");
    append(buffer, "static const XrXirTypes %s_types = {", prefix);
    if (types->count) append(buffer, "%s_type_nodes", prefix); else append(buffer, "NULL");
    append(buffer, ", %uu, ", types->count);
    if (types->nominals) append(buffer, "&%s_nominals", prefix); else append(buffer, "NULL");
    append(buffer, ", NULL};\n");
}
static void emit_proof(CBuffer *buffer, const XrXirArtifact *artifact, const char *prefix) {
    XrXirProgramProof proof = xr_xir_compile_program_proof(artifact);
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    append(buffer, "static const uint8_t %s_checked[] = {", prefix);
    for (size_t i = 0; i < proof.length && emit_work(buffer, 1); ++i) {
        if (!(i % 16)) append(buffer, "\n    ");
        append(buffer, "%u,", (unsigned)proof.bytes[i]);
    }
    append(buffer, "\n};\nstatic const uint8_t %s_identity[32] = {", prefix);
    for (unsigned i = 0; i < 32 && emit_work(buffer, 1); ++i) append(buffer, "%u,", (unsigned)proof.identity[i]);
    append(buffer, "};\n");
    for (uint32_t f = 0; f < module->function_count && emit_work(buffer, 1); ++f) {
        const XrXirFunctionLayout *layout = &proof.layouts[f];
        append(buffer, "static const uint32_t %s_offsets_%u[] = {", prefix, f);
        for (uint32_t i = 0; i < layout->slot_count && emit_work(buffer, 1); ++i) append(buffer, "%uu,", layout->offsets[i]);
        append(buffer, "};\n");
        if (layout->owned_count) {
            append(buffer, "static const uint32_t %s_owned_%u[] = {", prefix, f);
            for (uint32_t i = 0; i < layout->owned_count && emit_work(buffer, 1); ++i) append(buffer, "%uu,", layout->owned_offsets[i]);
            append(buffer, "};\n");
        }
        if (module->functions[f].parameter_count) {
            append(buffer, "static const XrXirLayout %s_physical_%u[] = {", prefix, f);
            for (uint32_t i = 0; i < module->functions[f].parameter_count && emit_work(buffer, 1); ++i)
                append(buffer, "{%uu,%uu},", layout->parameters[i].size, layout->parameters[i].alignment);
            append(buffer, "};\n");
        }
    }
    append(buffer, "static const XrXirFunctionLayout %s_layouts[] = {\n", prefix);
    for (uint32_t f = 0; f < module->function_count && emit_work(buffer, 1); ++f) {
        const XrXirFunctionLayout *layout = &proof.layouts[f];
        append(buffer, "    {%uu,%uu,%s_offsets_%u,", layout->slot_count, layout->frame_bytes, prefix, f);
        if (module->functions[f].parameter_count) append(buffer, "%s_physical_%u,", prefix, f);
        else append(buffer, "NULL,");
        append(buffer, "{%uu,%uu},%uu,", layout->result.size, layout->result.alignment, layout->owned_count);
        if (layout->owned_count) append(buffer, "%s_owned_%u,", prefix, f);
        else append(buffer, "NULL,");
        append(buffer, "%uu,%uu},\n", layout->outgoing_count, layout->path_count);
    }
    append(buffer, "};\n");
}

static void emit_program(CBuffer *buffer, const XrXirArtifact *artifact, const char *prefix) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module->declarations;
    if (!d) return;
    emit_types(buffer, module->types, prefix);
    emit_proof(buffer, artifact, prefix);
    for (uint32_t m = 0; m < d->module_count && emit_work(buffer, 1); ++m) {
        const XrXirSourceModule *source = &d->modules[m];
        if (!source->dependency_count) continue;
        append(buffer, "static const uint32_t %s_dependencies_%u[] = {", prefix, m);
        for (uint32_t i = 0; i < source->dependency_count && emit_work(buffer, 1); ++i)
            append(buffer, "%s%uu", i ? ", " : "", source->dependencies[i]);
        append(buffer, "};\n");
    }
    append(buffer, "static const XrXirSourceModule %s_modules[] = {\n", prefix);
    for (uint32_t m = 0; m < d->module_count && emit_work(buffer, 1); ++m) {
        const XrXirSourceModule *source = &d->modules[m];
        append(buffer, "    {"); emit_bytes(buffer, source->name, source->name_length);
        append(buffer, ", %uu, ", source->name_length);
        if (source->dependency_count) append(buffer, "%s_dependencies_%u", prefix, m);
        else append(buffer, "NULL");
        append(buffer, ", %uu, %uu},\n", source->dependency_count, source->initializer);
    }
    append(buffer, "};\nstatic const XrXirFunctionIdentity %s_identities[] = {\n", prefix);
    for (uint32_t f = 0; f < module->function_count && emit_work(buffer, 1); ++f)
        append(buffer, "    {%uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu},\n", d->functions[f].module, d->functions[f].exported, d->functions[f].nominal_owner, d->functions[f].member_access, d->functions[f].cleanup_owner, d->functions[f].promises, d->functions[f].method_kind, d->functions[f].test_role, d->functions[f].test_timeout_seconds);
    append(buffer, "};\n");
    if (d->slot_count) {
        append(buffer, "static const XrXirSlot %s_slots[] = {\n", prefix);
        for (uint32_t i = 0; i < d->slot_count && emit_work(buffer, 1); ++i)
            append(buffer, "    {%uu, (XrXirType) %u, %uu},\n", d->slots[i].module,
                (uint32_t) d->slots[i].type, d->slots[i].mutable);
        append(buffer, "};\n");
    }
    if (d->literal_count) {
        append(buffer, "static const XrXirLiteral %s_literals[] = {\n", prefix);
        for (uint32_t i = 0; i < d->literal_count && emit_work(buffer, 1); ++i) {
            append(buffer, "    {"); emit_bytes(buffer, d->literals[i].bytes, d->literals[i].length);
            append(buffer, ", %uu},\n", d->literals[i].length);
        }
        append(buffer, "};\n");
    }
    append(buffer, "static const XrXirDeclarations %s_declarations = {%s_modules, %uu, %s_identities, ",
        prefix, prefix, d->module_count, prefix);
    if (d->slot_count) append(buffer, "%s_slots", prefix); else append(buffer, "NULL");
    append(buffer, ", %uu, ", d->slot_count);
    if (d->literal_count) append(buffer, "%s_literals", prefix); else append(buffer, "NULL");
    append(buffer, ", %uu, %uu, %uu, NULL};\n", d->literal_count, d->root_module, d->entry_function);
    append(buffer, "_Static_assert(sizeof(XrXirFunctionIdentity) == 36, \"XIR function identity stride\");\n"
        "_Static_assert(offsetof(XrXirFunctionIdentity, method_kind) == 24, \"XIR method role offset\");\n"
        "_Static_assert(offsetof(XrXirFunctionIdentity, test_role) == 28, \"XIR test role offset\");\n"
        "_Static_assert(offsetof(XrXirFunctionIdentity, test_timeout_seconds) == 32, \"XIR test timeout offset\");\n"
        "_Static_assert(offsetof(XrXirDeclarations, implementations) == 64, \"XIR implementation table offset\");\n"
        "_Static_assert(sizeof(XrXirDeclarations) == 72, \"XIR declaration stride\");\n"
        "_Static_assert(XR_XIR_PROGRAM_ABI_VERSION == %uu, \"XIR program ABI\");\n"
        "XR_DATADEF const XrXirProgramSpec %s_program = {XR_XIR_PROGRAM_ABI_VERSION, "
        "{XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, %s_entries, %uu, &%s_declarations, {NULL, NULL}, ",
        XR_XIR_PROGRAM_ABI_VERSION, prefix, prefix, module->function_count, prefix);
    if (module->types) append(buffer, "&%s_types", prefix); else append(buffer, "NULL");
    append(buffer, ", {%s_checked, sizeof(%s_checked), %s_identity, %s_layouts}};\n",
        prefix, prefix, prefix, prefix);
}

XR_FUNC XrXirStatus xr_xir_compile_emit_c(const XrXirArtifact *artifact, const char *symbol_prefix,
                        size_t byte_limit, XrXirCSource *output) {
    if (!output || output->text || output->length) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    CBuffer buffer = {NULL, 0, 0, byte_limit, XR_XIR_OK, module->types, xr_xir_compile_artifact_context(artifact)};
    if (!symbol_prefix_valid(&buffer, symbol_prefix))
        return buffer.status == XR_XIR_OK ? XR_XIR_BAD_STRUCTURE : buffer.status;
    XrXirStatus status = xr_xir_compile_artifact_verify(artifact, NULL);
    if (status != XR_XIR_OK) return status;
    append(&buffer, "#include \"xir/xxir_program.h\"\n#include \"xir/xxir_float.h\"\n"
           "#include \"xir/xxir_instance_value.h\"\n#include \"xir/xxir_struct.h\"\n#include \"xir/xxir_class.h\"\n#include \"xir/xxir_enum.h\"\n#include \"xir/xxir_error.h\"\n"
           "#include \"xir/xxir_panic.h\"\n#include \"xir/xxir_equal.h\"\n#include \"xir/xxir_nullable.h\"\n"
           "#include \"xir/xxir_types.h\"\n#include \"xir/xxir_type_arena.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_CALL_ABI_VERSION == %uu, \"XIR call ABI\");\n"
           "_Static_assert(XR_XIR_VALUE_ABI_VERSION == %uu, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirValue) == 16, \"XIR scalar size\");\n"
           "_Static_assert(_Alignof(XrXirValue) == 8, \"XIR scalar alignment\");\n"
           "_Static_assert(offsetof(XrXirValue, payload) == 8, \"XIR payload offset\");\n",
           XR_XIR_CALL_ABI_VERSION, XR_XIR_VALUE_ABI_VERSION);
    append(&buffer,
           "_Static_assert(sizeof(XrXirFaultDetail) == 24 && _Alignof(XrXirFaultDetail) == 8, \"XIR fault layout\");\n"
           "_Static_assert(offsetof(XrXirFaultDetail, index) == 8 && offsetof(XrXirFaultDetail, length) == 16, \"XIR fault offsets\");\n"
           "_Static_assert(sizeof(XrXirPanicPayload) == 40 && _Alignof(XrXirPanicPayload) == 8, \"XIR panic layout\");\n"
           "_Static_assert(offsetof(XrXirPanicPayload, detail) == 0 && offsetof(XrXirPanicPayload, message) == 24, \"XIR panic offsets\");\n"
           "_Static_assert(sizeof(XrXirAction) == 88 && _Alignof(XrXirAction) == 8, \"XIR action layout\");\n"
           "_Static_assert(offsetof(XrXirAction, value) == 24 && offsetof(XrXirAction, panic) == 40 && "
           "offsetof(XrXirAction, flags) == 80, \"XIR action offsets\");\n"
           "_Static_assert(sizeof(XrXirCallResult) == 72 && _Alignof(XrXirCallResult) == 8, \"XIR result layout\");\n"
           "_Static_assert(offsetof(XrXirCallResult, value) == 8 && offsetof(XrXirCallResult, wake) == 24 && "
           "offsetof(XrXirCallResult, panic) == 32, \"XIR result offsets\");\n");
    for (uint32_t f = 0; f < module->function_count && emit_work(&buffer, 1); ++f)
        emit_resume_function(&buffer, artifact, symbol_prefix, f);
    append(&buffer, "XR_DATADEF const XrXirCallEntry %s_entries[] = {\n", symbol_prefix);
    for (uint32_t f = 0; f < module->function_count && emit_work(&buffer, 1); ++f) {
        const XrXirFunction *function = &module->functions[f];
        append(&buffer, "    {XR_XIR_CALL_ABI_VERSION, ");
        if (function->parameter_count) append(&buffer, "%s_parameters_%u", symbol_prefix, f);
        else append(&buffer, "NULL");
        uint32_t owner = 0, flags = 0;
        if (module->declarations) {
            owner = module->declarations->functions[f].cleanup_owner;
            for (uint32_t child = f + 1; child < module->function_count && emit_work(&buffer, 1); ++child)
                if (module->declarations->functions[child].cleanup_owner == f + 1) flags = XR_XIR_ENTRY_EXIT;
        }
        append(&buffer, ", %uu, (XrXirType) %u, (uint32_t) sizeof(%s_state_%u), %s_f%u, %s_release_%u, NULL, %uu, %uu},\n",
               function->parameter_count, (uint32_t) function->result, symbol_prefix, f, symbol_prefix, f,
               symbol_prefix, f, flags, owner);
    }
    append(&buffer, "};\n");
    emit_program(&buffer, artifact, symbol_prefix);
    if (buffer.status != XR_XIR_OK) {
        xr_compile_resources_free(buffer.text);
        return buffer.status;
    }
    XiCgenVerifyStatus verified = xr_compile_cgen_verify_output_or_ice(buffer.context->resources, buffer.text, buffer.length, symbol_prefix);
    if (verified != XI_CGEN_VERIFY_PASSED) {
        xr_compile_resources_free(buffer.text);
        return verified == XI_CGEN_VERIFY_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            verified == XI_CGEN_VERIFY_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    }
    *output = (XrXirCSource) {buffer.text, buffer.length};
    return XR_XIR_OK;
}

XR_FUNC void xr_xir_compile_c_source_free(XrXirCSource *source) {
    if (!source)
        return;
    xr_compile_resources_free(source->text);
    *source = (XrXirCSource) {NULL, 0};
}
