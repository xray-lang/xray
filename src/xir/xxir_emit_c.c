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
#include "../base/xmalloc.h"
#include "../aot/xi_cgen_verify_output.h"
#include <stdarg.h>
#include <limits.h>

typedef struct CBuffer {
    char *text;
    size_t length, capacity, limit;
    XrXirStatus status;
} CBuffer;

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

static void emit_instruction(CBuffer *buffer, const XrXirFunction *function,
                             const XrXirFunctionLayout *layout, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "    if (!xr_xir_scalar_step(context)) { status = XR_XIR_RUN_STEP_LIMIT; goto xr_done; }\n");
    switch (op->op) {
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_I64:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
        break;
    case XR_XIR_SCALAR_COPY:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu));\n",
               destination, layout->offsets[op->args[0]]);
        break;
    case XR_XIR_ADD_I64:
        append(buffer, "    status = xr_xir_scalar_add(xr_xir_scalar_load(frame, %uu), "
               "xr_xir_scalar_load(frame, %uu), &temporary);\n"
               "    if (status != XR_XIR_RUN_OK) goto xr_done;\n"
               "    xr_xir_scalar_store(frame, %uu, temporary);\n",
               layout->offsets[op->args[0]], layout->offsets[op->args[1]], destination);
        break;
    case XR_XIR_EQ_I64:
    case XR_XIR_LT_I64:
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu) %s "
               "xr_xir_scalar_load(frame, %uu));\n", destination,
               layout->offsets[op->args[0]], op->op == XR_XIR_EQ_I64 ? "==" : "<",
               layout->offsets[op->args[1]]);
        break;
    case XR_XIR_JUMP:
        append(buffer, "    goto xr_block_%u;\n", op->targets[0]);
        break;
    case XR_XIR_BRANCH:
        append(buffer, "    if (xr_xir_scalar_load(frame, %uu)) goto xr_block_%u; "
               "else goto xr_block_%u;\n", layout->offsets[op->args[0]],
               op->targets[0], op->targets[1]);
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
           "const XrXirScalar *arguments, uint32_t argument_count, XrXirScalar *result) {\n"
           "    void *frame = NULL;\n    XrXirRunStatus status;\n", prefix, index);
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        if (function->instructions[i].op == XR_XIR_ADD_I64) {
            append(buffer, "    int64_t temporary;\n");
            break;
        }
    append(buffer, "    if (!result) return XR_XIR_RUN_BAD_ARGUMENT;\n"
           "    *result = (XrXirScalar) {0, 0, 0};\n"
           "    if (!context || argument_count != %uu || (argument_count && !arguments)) "
           "return XR_XIR_RUN_BAD_ARGUMENT;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count; ++p)
        append(buffer, "    if (!xr_xir_scalar_argument(&arguments[%u], (XrXirType) %u)) "
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
           "    if (status != XR_XIR_RUN_OK) *result = (XrXirScalar) {0, 0, 0};\n"
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
    CBuffer buffer = {NULL, 0, 0, byte_limit, XR_XIR_OK};
    append(&buffer, "#include \"xir/xxir_scalar.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_SCALAR_ABI_VERSION == 1u, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirScalar) == 16, \"XIR boundary size\");\n"
           "_Static_assert(_Alignof(XrXirScalar) == 8, \"XIR boundary alignment\");\n"
           "_Static_assert(offsetof(XrXirScalar, payload) == 8, \"XIR payload offset\");\n");
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

static void emit_resume_step(CBuffer *buffer, const XrXirModule *module,
                            const XrXirFunction *function, const XrXirFunctionLayout *layout,
                            uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t destination = layout->offsets[function->parameter_count + index];
    append(buffer, "    case %uu:\n        state->pc = %uu;\n", index, index + 1);
    switch (op->op) {
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_I64:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, ", destination);
        emit_constant(buffer, op->immediate);
        append(buffer, ");\n");
        break;
    case XR_XIR_SCALAR_COPY:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, "
               "xr_xir_scalar_load(state->frame, %uu));\n", destination, layout->offsets[op->args[0]]);
        break;
    case XR_XIR_ADD_I64:
        append(buffer, "        if (xr_xir_scalar_add(xr_xir_scalar_load(state->frame, %uu), "
               "xr_xir_scalar_load(state->frame, %uu), &temporary) != XR_XIR_RUN_OK)\n"
               "            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, "
               "{XR_XIR_I64, 0, XR_XIR_CALL_OVERFLOW}};\n"
               "        xr_xir_scalar_store(state->frame, %uu, temporary);\n",
               layout->offsets[op->args[0]], layout->offsets[op->args[1]], destination);
        break;
    case XR_XIR_EQ_I64:
    case XR_XIR_LT_I64:
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, "
               "xr_xir_scalar_load(state->frame, %uu) %s xr_xir_scalar_load(state->frame, %uu));\n",
               destination, layout->offsets[op->args[0]], op->op == XR_XIR_EQ_I64 ? "==" : "<",
               layout->offsets[op->args[1]]);
        break;
    case XR_XIR_JUMP:
        append(buffer, "        state->pc = %uu;\n", function->blocks[op->targets[0]].first);
        break;
    case XR_XIR_BRANCH:
        append(buffer, "        state->pc = xr_xir_scalar_load(state->frame, %uu) ? %uu : %uu;\n",
               layout->offsets[op->args[0]], function->blocks[op->targets[0]].first,
               function->blocks[op->targets[1]].first);
        break;
    case XR_XIR_CALL: {
        const XrXirFunction *callee = &module->functions[op->immediate];
        append(buffer, "        state->waiting = true; state->destination = %uu; state->expected = %uu;\n",
               destination, (uint32_t) op->type);
        for (uint32_t p = 0; p < callee->parameter_count; ++p)
            append(buffer, "        state->arguments[%u] = (XrXirScalar) {%uu, 0, "
                   "xr_xir_scalar_load(state->frame, %uu)};\n", p, (uint32_t) callee->parameters[p],
                   layout->offsets[op->args[p]]);
        append(buffer, "        return (XrXirAction) {XR_XIR_ACTION_CALL, %uu, state->arguments, %uu, {0, 0, 0}};\n",
               (uint32_t) op->immediate, callee->parameter_count);
        return;
    }
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
    if (layout->frame_bytes > UINT32_MAX - 64u) {
        buffer->status = XR_XIR_BUDGET;
        return;
    }
    append(buffer, "typedef struct %s_state_%u {\n"
           "    uint32_t pc, destination, expected; bool initialized, waiting;\n"
           "    XrXirScalar arguments[2]; unsigned char frame[%u];\n"
           "} %s_state_%u;\n", prefix, index, layout->frame_bytes ? layout->frame_bytes : 1, prefix, index);
    append(buffer, "XR_FUNC XrXirAction %s_f%u(XrXirCallView *view) {\n"
           "    %s_state_%u *state = view->state;\n", prefix, index, prefix, index);
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        if (function->instructions[i].op == XR_XIR_ADD_I64) {
            append(buffer, "    int64_t temporary;\n");
            break;
        }
    append(buffer, "    if (!state->initialized) {\n"
           "        if (view->argument_count != %uu) goto invalid;\n", function->parameter_count);
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        append(buffer, "        if (!xr_xir_scalar_argument(&view->arguments[%u], (XrXirType) %u)) goto invalid;\n",
               p, (uint32_t) function->parameters[p]);
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
           "        } else if (!xr_xir_scalar_argument(&view->inbox.value, (XrXirType) state->expected)) goto invalid;\n"
           "        if (state->destination != UINT32_MAX)\n"
           "            xr_xir_scalar_store(state->frame, state->destination, view->inbox.value.payload);\n"
           "    }\n    switch (state->pc) {\n");
    for (uint32_t i = 0; i < function->instruction_count; ++i)
        emit_resume_step(buffer, module, function, layout, i);
    append(buffer, "    default: break;\n    }\ninvalid:\n"
           "    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};\n}\n");
    if (function->parameter_count) {
        append(buffer, "static const XrXirType %s_parameters_%u[] = {", prefix, index);
        for (uint32_t p = 0; p < function->parameter_count; ++p)
            append(buffer, "%s(XrXirType) %u", p ? ", " : "", (uint32_t) function->parameters[p]);
        append(buffer, "};\n");
    }
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
    append(&buffer, "#include \"xir/xxir_call.h\"\n"
           "#if !defined(XR_ARCH_X86_64)\n#error XIR_target_mismatch\n#endif\n"
           "_Static_assert(XR_XIR_CALL_ABI_VERSION == 1u, \"XIR call ABI\");\n"
           "_Static_assert(XR_XIR_SCALAR_ABI_VERSION == 1u, \"XIR scalar ABI\");\n"
           "_Static_assert(sizeof(XrXirScalar) == 16, \"XIR scalar size\");\n"
           "_Static_assert(_Alignof(XrXirScalar) == 8, \"XIR scalar alignment\");\n"
           "_Static_assert(offsetof(XrXirScalar, payload) == 8, \"XIR payload offset\");\n");
    for (uint32_t f = 0; f < module->function_count && buffer.status == XR_XIR_OK; ++f)
        emit_resume_function(&buffer, artifact, symbol_prefix, f);
    append(&buffer, "XR_DATADEF const XrXirCallEntry %s_entries[] = {\n", symbol_prefix);
    for (uint32_t f = 0; f < module->function_count && buffer.status == XR_XIR_OK; ++f) {
        const XrXirFunction *function = &module->functions[f];
        append(&buffer, "    {XR_XIR_CALL_ABI_VERSION, ");
        if (function->parameter_count) append(&buffer, "%s_parameters_%u", symbol_prefix, f);
        else append(&buffer, "NULL");
        append(&buffer, ", %uu, (XrXirType) %u, (uint32_t) sizeof(%s_state_%u), %s_f%u, NULL, NULL},\n",
               function->parameter_count, (uint32_t) function->result, symbol_prefix, f, symbol_prefix, f);
    }
    append(&buffer, "};\n");
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
