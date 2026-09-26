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
 *   belong to the shared runtime and the immutable Lowered artifact.
 */

#include "xxir_emit_c.h"
#include "xxir_callable.h"
#include "xxir_scalar.h"
#include "../base/xmalloc.h"
#include "../aot/xi_cgen_verify_output.h"
#include <stdarg.h>
#include <limits.h>

typedef struct CBuffer {
    char *text;
    size_t length, capacity, limit;
    XrXirStatus status;
} CBuffer;

static const char *comparison_symbol(XrXirOp op) {
    switch (op) {
    case XR_XIR_EQ_I64: return "==";
    case XR_XIR_NE_I64: return "!=";
    case XR_XIR_LT_I64: return "<";
    case XR_XIR_LE_I64: return "<=";
    case XR_XIR_GT_I64: return ">";
    case XR_XIR_GE_I64: return ">=";
    default: return NULL;
    }
}

static int emit_arithmetic_operation(XrXirOp op) {
    switch (op) {
    case XR_XIR_ADD_I64: return XR_XIR_ARITH_ADD;
    case XR_XIR_SUB_I64: return XR_XIR_ARITH_SUB;
    case XR_XIR_MUL_I64: return XR_XIR_ARITH_MUL;
    case XR_XIR_DIV_I64: return XR_XIR_ARITH_DIV;
    case XR_XIR_REM_I64: return XR_XIR_ARITH_REM;
    case XR_XIR_AND_I64: return XR_XIR_ARITH_AND;
    case XR_XIR_OR_I64: return XR_XIR_ARITH_OR;
    case XR_XIR_XOR_I64: return XR_XIR_ARITH_XOR;
    case XR_XIR_SHL_I64: return XR_XIR_ARITH_SHL;
    case XR_XIR_SHR_I64: return XR_XIR_ARITH_SHR;
    default: return -1;
    }
}

static void append(CBuffer *buffer, const char *format, ...) {
    if (buffer->status != XR_XIR_OK)
        return;
    va_list args, measure;
    va_start(args, format);
    va_copy(measure, args);
    int count = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (count < 0) {
        buffer->status = XR_XIR_BAD_STRUCTURE;
    } else if ((size_t) count >= buffer->limit ||
               buffer->length > buffer->limit - (size_t) count - 1) {
        buffer->status = XR_XIR_BUDGET;
    } else {
        size_t required = buffer->length + (size_t) count + 1;
        if (required > buffer->capacity) {
            size_t capacity = buffer->capacity ? buffer->capacity : 128;
            while (capacity < required && capacity <= buffer->limit / 2)
                capacity *= 2;
            if (capacity < required || capacity > buffer->limit)
                capacity = required;
            char *replacement = xr_realloc(buffer->text, capacity);
            if (!replacement)
                buffer->status = XR_XIR_OUT_OF_MEMORY;
            else {
                buffer->text = replacement;
                buffer->capacity = capacity;
            }
        }
        if (buffer->status == XR_XIR_OK) {
            int written = vsnprintf(buffer->text + buffer->length,
                                    buffer->capacity - buffer->length, format, args);
            if (written != count)
                buffer->status = XR_XIR_BAD_STRUCTURE;
            else
                buffer->length += (size_t) count;
        }
    }
    va_end(args);
}

static bool symbol_prefix_valid(const char *prefix) {
    if (!prefix || !*prefix)
        return false;
    for (size_t i = 0; i < 65; ++i) {
        unsigned char c = (unsigned char) prefix[i];
        if (!c)
            return true;
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
    while (low + 1 < high) {
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first <= instruction) low = middle;
        else high = middle;
    }
    const char *frame = resumable ? "state->frame" : "frame";
    uint32_t first = function->blocks[target].first, end = first;
    while (end < function->instruction_count && function->instructions[end].op == XR_XIR_PHI) {
        const XrXirInstruction *phi = &function->instructions[end];
        uint32_t source = UINT32_MAX;
        for (uint32_t a = 0; a < phi->args[1]; a += 2)
            if (function->operands[phi->args[0] + a] == low) {
                source = function->operands[phi->args[0] + a + 1]; break;
            }
        if (source == UINT32_MAX) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
        uint32_t scratch = layout->offsets[function->parameter_count + end] + 8;
        if (xr_xir_type_is_owned(phi->type)) {
            if (!resumable) { buffer->status = XR_XIR_BAD_STAGE; return; }
            append(buffer, "        if (xr_xir_owned_slot_copy(%s, %uu, (XrXirType) %u, "
                "xr_xir_scalar_load(%s, %uu)) != XR_XIR_VALUE_OK) goto limit;\n",
                frame, scratch, (uint32_t) phi->type, frame, layout->offsets[source]);
        } else append(buffer, "        xr_xir_scalar_store(%s, %uu, xr_xir_scalar_load(%s, %uu));\n",
            frame, scratch, frame, layout->offsets[source]);
        ++end;
    }
    for (uint32_t i = first; i < end; ++i) {
        const XrXirInstruction *phi = &function->instructions[i];
        uint32_t destination = layout->offsets[function->parameter_count + i];
        if (xr_xir_type_is_owned(phi->type))
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
    case XR_XIR_CONST_I64:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
        break;
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
    case XR_XIR_ADD_I64: case XR_XIR_SUB_I64: case XR_XIR_MUL_I64:
    case XR_XIR_AND_I64: case XR_XIR_OR_I64: case XR_XIR_XOR_I64:
    case XR_XIR_SHL_I64: case XR_XIR_SHR_I64:
    case XR_XIR_DIV_I64: case XR_XIR_REM_I64:
        append(buffer, "    status = xr_xir_scalar_arithmetic((XrXirArithmetic) %d, xr_xir_scalar_load(frame, %uu), "
               "xr_xir_scalar_load(frame, %uu), &temporary);\n"
               "    if (status != XR_XIR_RUN_OK) goto xr_done;\n"
               "    xr_xir_scalar_store(frame, %uu, temporary);\n",
               emit_arithmetic_operation(op->op), layout->offsets[op->args[0]], layout->offsets[op->args[1]], destination);
        break;
    case XR_XIR_EQ_I64:
    case XR_XIR_LT_I64: case XR_XIR_NE_I64: case XR_XIR_LE_I64:
    case XR_XIR_GT_I64: case XR_XIR_GE_I64:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu) %s "
               "xr_xir_scalar_load(frame, %uu));\n", destination,
               layout->offsets[op->args[0]], comparison_symbol(op->op),
               layout->offsets[op->args[1]]);
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
        buffer->status = XR_XIR_BAD_STAGE;
        break;
    }
}

static void emit_function(CBuffer *buffer, const XrXirArtifact *artifact,
                          const char *prefix, uint32_t index) {
    const XrXirFunction *function = &xr_xir_artifact_module(artifact)->functions[index];
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, index);
    append(buffer, "\nXR_FUNC XrXirRunStatus %s_f%u(XrXirRunContext *context, "
           "const XrXirValue *arguments, uint32_t argument_count, XrXirValue *result) {\n"
           "    void *frame = NULL;\n    XrXirRunStatus status;\n", prefix, index);
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        if (emit_arithmetic_operation(function->instructions[i].op) >= 0) {
            append(buffer, "    int64_t temporary;\n");
            break;
        }
    append(buffer, "    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;\n"
           "    *result = (XrXirValue) {0, 0, 0};\n"
           "    if (!context || argument_count != %uu || (argument_count && !arguments)) "
           "return XR_XIR_RUN_BAD_ARGUMENT;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count; ++p)
        append(buffer, "    if (!xr_xir_value_argument(&arguments[%u], (XrXirType) %u)) "
               "return XR_XIR_RUN_BAD_ARGUMENT;\n", p, (uint32_t) function->parameters[p]);
    append(buffer, "    status = xr_xir_scalar_frame_begin(context, %uu, &frame);\n"
           "    if (status != XR_XIR_RUN_OK) return status;\n", layout->frame_bytes);
    for (uint32_t p = 0; p < function->parameter_count; ++p)
        append(buffer, "    xr_xir_scalar_store(frame, %uu, arguments[%u].payload);\n",
               layout->offsets[p], p);
    append(buffer, "    goto xr_block_0;\n");
    for (uint32_t b = 0; b < function->block_count; ++b) {
        append(buffer, "xr_block_%u:\n", b);
        const XrXirBlock *block = &function->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i)
            emit_instruction(buffer, function, layout, i);
    }
    append(buffer, "xr_done:\n    xr_xir_scalar_frame_end(context, %uu, frame);\n"
           "    if (status != XR_XIR_RUN_OK) *result = (XrXirValue) {0, 0, 0};\n"
           "    return status;\n}\n", layout->frame_bytes);
}

XrXirStatus xr_xir_emit_leaf_c(const XrXirArtifact *artifact, const char *symbol_prefix,
                        size_t byte_limit, XrXirCSource *output) {
    if (!output)
        return XR_XIR_BAD_STRUCTURE;
    *output = (XrXirCSource) {NULL, 0};
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED)
        return XR_XIR_BAD_STAGE;
    if (!symbol_prefix_valid(symbol_prefix))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_artifact_verify(artifact, NULL, NULL);
    if (status != XR_XIR_OK)
        return status;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (xr_xir_type_is_owned(function->result)) return XR_XIR_BAD_STAGE;
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            if (xr_xir_type_is_owned(function->parameters[p])) return XR_XIR_BAD_STAGE;
    }
    CBuffer buffer = {NULL, 0, 0, byte_limit, XR_XIR_OK};
    append(&buffer, "#include \"xir/xxir_scalar.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_VALUE_ABI_VERSION == 5u, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirValue) == 16, \"XIR boundary size\");\n"
           "_Static_assert(_Alignof(XrXirValue) == 8, \"XIR boundary alignment\");\n"
           "_Static_assert(offsetof(XrXirValue, payload) == 8, \"XIR payload offset\");\n");
    for (uint32_t f = 0; f < module->function_count && buffer.status == XR_XIR_OK; ++f)
        emit_function(&buffer, artifact, symbol_prefix, f);
    if (buffer.status != XR_XIR_OK) {
        xr_free(buffer.text);
        return buffer.status;
    }
    xi_cgen_verify_output_or_ice(buffer.text, buffer.length, symbol_prefix);
    output->text = buffer.text;
    output->length = buffer.length;
    return XR_XIR_OK;
}

static void emit_instance_step(CBuffer *buffer, const XrXirModule *module,
                               const XrXirFunction *function, const XrXirInstruction *op,
                               const XrXirFunctionLayout *layout, uint32_t destination) {
    append(buffer, "        { XrXirValue value = {0}; XrXirCallStatus status = XR_XIR_CALL_READY;\n");
    switch (op->op) {
    case XR_XIR_FUNCTION_REF:
        for (uint32_t p = 0; p < op->args[1]; ++p) {
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
        append(buffer, "        value = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        status = xr_xir_instance_slot_write(view, %uu, &value, %s);\n",
            (uint32_t) module->declarations->slots[op->immediate].type, layout->offsets[op->args[0]],
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
    default: buffer->status = XR_XIR_BAD_STAGE; break;
    }
    append(buffer, "        if (status != XR_XIR_CALL_READY) return (XrXirAction) "
        "{XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}};\n");
    if (xr_xir_type_is_owned(op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT)
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}};\n");
}

static void emit_resume_call(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination) {
    append(buffer, "        { uint32_t callee = %uu; XrXirValue target = {0};\n", (uint32_t) op->immediate);
    if (op->op == XR_XIR_CALL_INDIRECT) {
        uint32_t id = (uint32_t) op->immediate;
        append(buffer, "        target = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n"
            "        XrXirCallStatus status = xr_xir_instance_resolve_function(view, &target, &callee);\n"
            "        if (status != XR_XIR_CALL_READY) return (XrXirAction) "
            "{XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}};\n",
            (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
    }
    append(buffer, "        state->waiting = true; state->destination = %uu; state->expected = %uu;\n",
        destination, (uint32_t) op->type);
    for (uint32_t p = 0; p < op->args[1]; ++p) {
        uint32_t id = function->operands[op->args[0] + p];
        append(buffer, "        state->arguments[%u] = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
            p, (uint32_t) xr_xir_operand_type(function, id), layout->offsets[id]);
    }
    append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_CALL, callee, %s, %uu, target}; }\n",
        op->args[1] ? "state->arguments" : "NULL", op->args[1]);
}

static void emit_resume_step(CBuffer *buffer, const XrXirModule *module,
                            const XrXirFunction *function, const XrXirFunctionLayout *layout,
                            uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "    case %uu:\n        state->pc = %uu;\n", index, index + 1);
    if ((op->op >= XR_XIR_CONST_STRING && op->op <= XR_XIR_ATOMIC_I64_FETCH_ADD) || op->op == XR_XIR_FUNCTION_REF) {
        emit_instance_step(buffer, module, function, op, layout, destination);
        return;
    }
    switch (op->op) {
    case XR_XIR_PHI: break;
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_I64:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
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
        append(buffer, "        if (xr_xir_owned_slot_copy(state->frame, %uu, (XrXirType) %u, "
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
        if (op->op != XR_XIR_CONCAT_STRING) append(buffer, "(XrXirType) %u, ", (uint32_t) op->type);
        append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[0]]);
        if (op->op == XR_XIR_CONCAT_STRING)
            append(buffer, ", xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[1]]);
        append(buffer, ");\n        if (status != XR_XIR_VALUE_OK)\n"
               "            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, "
               "{XR_XIR_I64, 0, status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT}}; }\n");
        break;
    case XR_XIR_OUTPUT:
    case XR_XIR_WRITE_STREAM:
    case XR_XIR_PRINT: {
        uint32_t count = op->op == XR_XIR_PRINT ? op->args[1] : 1;
        for (uint32_t p = 0; p < count; ++p) {
            uint32_t id = op->op == XR_XIR_PRINT ? function->operands[op->args[0] + p] : op->args[p];
            XrXirType type = id < function->parameter_count ? function->parameters[id] :
                function->instructions[id - function->parameter_count].type;
            append(buffer, "        state->arguments[%u] = (XrXirValue) {%uu, 0, xr_xir_scalar_load(state->frame, %uu)};\n",
                   p, (uint32_t) type, layout->offsets[id]);
        }
        if (op->op == XR_XIR_WRITE_STREAM)
            append(buffer, "        state->waiting = true; state->destination = %uu; state->expected = XR_XIR_BOOL;\n", destination);
        append(buffer, "        return (XrXirAction) {%s, %uu, %s, %uu, {0}};\n",
            op->op == XR_XIR_WRITE_STREAM ? "XR_XIR_ACTION_WRITE_STREAM" : "XR_XIR_ACTION_OUTPUT",
            op->op == XR_XIR_PRINT ? 3u : (uint32_t) op->immediate, count ? "state->arguments" : "NULL", count);
        return;
    }
    case XR_XIR_ADD_I64: case XR_XIR_SUB_I64: case XR_XIR_MUL_I64:
    case XR_XIR_AND_I64: case XR_XIR_OR_I64: case XR_XIR_XOR_I64:
    case XR_XIR_SHL_I64: case XR_XIR_SHR_I64:
    case XR_XIR_DIV_I64: case XR_XIR_REM_I64:
        append(buffer, "        if (xr_xir_scalar_arithmetic((XrXirArithmetic) %d, xr_xir_scalar_load(state->frame, %uu), "
               "xr_xir_scalar_load(state->frame, %uu), &temporary) != XR_XIR_RUN_OK)\n"
               "            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, "
               "{XR_XIR_I64, 0, XR_XIR_CALL_DIVIDE_BY_ZERO}};\n"
               "        xr_xir_scalar_store(state->frame, %uu, temporary);\n",
               emit_arithmetic_operation(op->op), layout->offsets[op->args[0]], layout->offsets[op->args[1]], destination);
        break;
    case XR_XIR_EQ_I64:
    case XR_XIR_LT_I64: case XR_XIR_NE_I64: case XR_XIR_LE_I64:
    case XR_XIR_GT_I64: case XR_XIR_GE_I64:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, "
               "xr_xir_scalar_load(state->frame, %uu) %s xr_xir_scalar_load(state->frame, %uu));\n",
               destination, layout->offsets[op->args[0]], comparison_symbol(op->op),
               layout->offsets[op->args[1]]);
        break;
    case XR_XIR_JUMP:
        emit_edge(buffer, function, layout, index, op->targets[0], true);
        break;
    case XR_XIR_BRANCH:
        emit_branch(buffer, function, layout, index, true);
        break;
    case XR_XIR_CALL: case XR_XIR_CALL_INDIRECT:
        emit_resume_call(buffer, function, op, layout, destination);
        return;
    case XR_XIR_SUSPEND:
        append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0, 0, 0}};\n");
        return;
    case XR_XIR_THROW:
    case XR_XIR_RETURN: {
        uint32_t type = op->op == XR_XIR_THROW ? XR_XIR_I64 : (uint32_t) function->result;
        append(buffer, "        return (XrXirAction) {%s, 0, NULL, 0, {%uu, 0, ",
               op->op == XR_XIR_THROW ? "XR_XIR_ACTION_THROW" : "XR_XIR_ACTION_RETURN", type);
        if (type == XR_XIR_UNIT) append(buffer, "0");
        else append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[op->args[0]]);
        append(buffer, "}};\n");
        return;
    }
    default:
        buffer->status = XR_XIR_BAD_STAGE;
        return;
    }
    append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}};\n");
}

static void emit_resume_function(CBuffer *buffer, const XrXirArtifact *artifact,
                                  const char *prefix, uint32_t index) {
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    const XrXirFunction *function = &module->functions[index];
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, index);
    if ((uint64_t) layout->frame_bytes + (uint64_t) layout->outgoing_count * sizeof(XrXirValue) > UINT32_MAX - 64u) {
        buffer->status = XR_XIR_BUDGET;
        return;
    }
    append(buffer, "typedef struct %s_state_%u {\n"
           "    uint32_t pc, destination, expected; bool initialized, waiting;\n", prefix, index);
    if (layout->outgoing_count) append(buffer, "    XrXirValue arguments[%u];\n", layout->outgoing_count);
    append(buffer, "    unsigned char frame[%u];\n} %s_state_%u;\n",
           layout->frame_bytes ? layout->frame_bytes : 1, prefix, index);
    append(buffer, "XR_FUNC XrXirAction %s_f%u(XrXirCallView *view) {\n"
           "    %s_state_%u *state = view->state;\n", prefix, index, prefix, index);
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        if (emit_arithmetic_operation(function->instructions[i].op) >= 0) {
            append(buffer, "    int64_t temporary;\n");
            break;
        }
    append(buffer, "    if (!state->initialized) {\n"
           "        if (view->argument_count != %uu) goto invalid;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        append(buffer, "        if (!xr_xir_value_argument(&view->arguments[%u], (XrXirType) %u)) goto invalid;\n",
               p, (uint32_t) function->parameters[p]);
        if (xr_xir_type_is_owned(function->parameters[p]))
            append(buffer, "        if (xr_xir_owned_slot_copy(state->frame, %uu, (XrXirType) %u, view->arguments[%u].payload) "
                   "!= XR_XIR_VALUE_OK) goto limit;\n", layout->offsets[p], (uint32_t) function->parameters[p], p);
        else
            append(buffer, "        xr_xir_scalar_store(state->frame, %uu, view->arguments[%u].payload);\n",
                   layout->offsets[p], p);
    }
    append(buffer, "        state->initialized = true;\n    }\n"
           "    if (state->waiting) {\n        state->waiting = false;\n"
           "        if (view->inbox.status == XR_XIR_CALL_THROWN)\n"
           "            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value};\n"
           "        if (view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;\n"
           "        if (state->expected == XR_XIR_UNIT) {\n"
           "            if (view->inbox.value.type || view->inbox.value.reserved || view->inbox.value.payload) goto invalid;\n"
           "        } else if (!xr_xir_value_argument(&view->inbox.value, (XrXirType) state->expected)) goto invalid;\n"
           "        if (xr_xir_type_is_owned((XrXirType) state->expected)) {\n"
           "            if (xr_xir_owned_slot_copy(state->frame, state->destination, (XrXirType) state->expected, view->inbox.value.payload) "
           "!= XR_XIR_VALUE_OK) goto limit;\n"
           "        } else if (state->destination != UINT32_MAX)\n"
           "            xr_xir_scalar_store(state->frame, state->destination, view->inbox.value.payload);\n"
           "    }\n    switch (state->pc) {\n");
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        emit_resume_step(buffer, module, function, layout, i);
    append(buffer, "    default: break;\n    }\ninvalid:\n"
           "    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};\n"
           "limit:\n    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}};\n}\n");
    append(buffer, "static void %s_cleanup_%u(XrXirCallView *view, XrXirCallStatus reason) {\n"
           "    (void) reason; (void) view;\n", prefix, index);
    if (layout->owned_count) {
        append(buffer, "    %s_state_%u *state = view->state;\n", prefix, index);
        for (uint32_t i = layout->owned_count; i > 0; --i)
            append(buffer, "    xr_xir_owned_slot_clear(state->frame, %uu);\n", layout->owned_offsets[i - 1]);
    }
    append(buffer, "}\n");
    if (function->parameter_count) {
        append(buffer, "static const XrXirType %s_parameters_%u[] = {", prefix, index);
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            append(buffer, "%s(XrXirType) %u", p ? ", " : "", (uint32_t) function->parameters[p]);
        append(buffer, "};\n");
    }
}

static void emit_bytes(CBuffer *buffer, const char *bytes, uint32_t length) {
    append(buffer, "\"");
    for (uint32_t i = 0; i < length && buffer->status == XR_XIR_OK; ++i)
        append(buffer, "\\x%02x", (unsigned int) (unsigned char) bytes[i]);
    append(buffer, "\"");
}
static void emit_callable_types(CBuffer *buffer, const XrXirCallableTypes *types, const char *prefix) {
    if (!types) return;
    for (uint32_t i = 0; i < types->count; ++i) {
        const XrXirCallableSignature *s = &types->signatures[i];
        if (!s->parameter_count) continue;
        append(buffer, "static const XrXirCallableParameter %s_callable_parameters_%u[] = {", prefix, i);
        for (uint32_t p = 0; p < s->parameter_count; ++p)
            append(buffer, "%s{(XrXirType) %u, %uu}", p ? ", " : "", (uint32_t) s->parameters[p].type, s->parameters[p].mode);
        append(buffer, "};\n");
    }
    append(buffer, "static const XrXirCallableSignature %s_callable_signatures[] = {\n", prefix);
    for (uint32_t i = 0; i < types->count; ++i) {
        const XrXirCallableSignature *s = &types->signatures[i];
        append(buffer, "    {");
        if (s->parameter_count) append(buffer, "%s_callable_parameters_%u", prefix, i);
        else append(buffer, "NULL");
        append(buffer, ", %uu, (XrXirType) %u, %uu, 0u},\n", s->parameter_count, (uint32_t) s->result, s->flags);
    }
    append(buffer, "};\nstatic const XrXirCallableTypes %s_callable_types = {%s_callable_signatures, %uu};\n",
        prefix, prefix, types->count);
}
static void emit_program(CBuffer *buffer, const XrXirModule *module, const char *prefix) {
    const XrXirDeclarations *d = module->declarations;
    if (!d) return;
    emit_callable_types(buffer, module->callables, prefix);
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *source = &d->modules[m];
        if (!source->dependency_count) continue;
        append(buffer, "static const uint32_t %s_dependencies_%u[] = {", prefix, m);
        for (uint32_t i = 0; i < source->dependency_count; ++i)
            append(buffer, "%s%uu", i ? ", " : "", source->dependencies[i]);
        append(buffer, "};\n");
    }
    append(buffer, "static const XrXirSourceModule %s_modules[] = {\n", prefix);
    for (uint32_t m = 0; m < d->module_count; ++m) {
        const XrXirSourceModule *source = &d->modules[m];
        append(buffer, "    {"); emit_bytes(buffer, source->name, source->name_length);
        append(buffer, ", %uu, ", source->name_length);
        if (source->dependency_count) append(buffer, "%s_dependencies_%u", prefix, m);
        else append(buffer, "NULL");
        append(buffer, ", %uu, %uu},\n", source->dependency_count, source->initializer);
    }
    append(buffer, "};\nstatic const XrXirFunctionIdentity %s_identities[] = {\n", prefix);
    for (uint32_t f = 0; f < module->function_count; ++f)
        append(buffer, "    {%uu, %uu},\n", d->functions[f].module, d->functions[f].exported);
    append(buffer, "};\n");
    if (d->slot_count) {
        append(buffer, "static const XrXirSlot %s_slots[] = {\n", prefix);
        for (uint32_t i = 0; i < d->slot_count; ++i)
            append(buffer, "    {%uu, (XrXirType) %u, %uu},\n", d->slots[i].module,
                (uint32_t) d->slots[i].type, d->slots[i].mutable);
        append(buffer, "};\n");
    }
    if (d->literal_count) {
        append(buffer, "static const XrXirLiteral %s_literals[] = {\n", prefix);
        for (uint32_t i = 0; i < d->literal_count; ++i) {
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
    append(buffer, ", %uu, %uu, %uu};\n", d->literal_count, d->root_module, d->entry_function);
    append(buffer, "_Static_assert(XR_XIR_PROGRAM_ABI_VERSION == 4u, \"XIR program ABI\");\n"
        "XR_DATADEF const XrXirProgramSpec %s_program = {XR_XIR_PROGRAM_ABI_VERSION, "
        "{XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, %s_entries, %uu, &%s_declarations, {NULL, NULL}, ",
        prefix, prefix, module->function_count, prefix);
    if (module->callables) append(buffer, "&%s_callable_types", prefix); else append(buffer, "NULL");
    append(buffer, "};\n");
}

XrXirStatus xr_xir_emit_c(const XrXirArtifact *artifact, const char *symbol_prefix,
                        size_t byte_limit, XrXirCSource *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = (XrXirCSource) {NULL, 0};
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (!symbol_prefix_valid(symbol_prefix)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_artifact_verify(artifact, NULL, NULL);
    if (status != XR_XIR_OK) return status;
    CBuffer buffer = {NULL, 0, 0, byte_limit, XR_XIR_OK};
    append(&buffer, "#include \"xir/xxir_program.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_CALL_ABI_VERSION == 8u, \"XIR call ABI\");\n"
           "_Static_assert(XR_XIR_VALUE_ABI_VERSION == 5u, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirValue) == 16, \"XIR scalar size\");\n"
           "_Static_assert(_Alignof(XrXirValue) == 8, \"XIR scalar alignment\");\n"
           "_Static_assert(offsetof(XrXirValue, payload) == 8, \"XIR payload offset\");\n");
    for (uint32_t f = 0; f < module->function_count && buffer.status == XR_XIR_OK; ++f)
        emit_resume_function(&buffer, artifact, symbol_prefix, f);
    append(&buffer, "XR_DATADEF const XrXirCallEntry %s_entries[] = {\n", symbol_prefix);
    for (uint32_t f = 0; f < module->function_count && buffer.status == XR_XIR_OK; ++f) {
        const XrXirFunction *function = &module->functions[f];
        append(&buffer, "    {XR_XIR_CALL_ABI_VERSION, ");
        if (function->parameter_count) append(&buffer, "%s_parameters_%u", symbol_prefix, f);
        else append(&buffer, "NULL");
        append(&buffer, ", %uu, (XrXirType) %u, (uint32_t) sizeof(%s_state_%u), %s_f%u, %s_cleanup_%u, NULL},\n",
               function->parameter_count, (uint32_t) function->result, symbol_prefix, f, symbol_prefix, f,
               symbol_prefix, f);
    }
    append(&buffer, "};\n");
    emit_program(&buffer, module, symbol_prefix);
    if (buffer.status != XR_XIR_OK) {
        xr_free(buffer.text);
        return buffer.status;
    }
    xi_cgen_verify_output_or_ice(buffer.text, buffer.length, symbol_prefix);
    *output = (XrXirCSource) {buffer.text, buffer.length};
    return XR_XIR_OK;
}

void xr_xir_c_source_free(XrXirCSource *source) {
    if (!source)
        return;
    xr_free(source->text);
    *source = (XrXirCSource) {NULL, 0};
}
