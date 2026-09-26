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
        append(buffer, "    xr_xir_scalar_store(frame, %uu, xr_xir_scalar_load(frame, %uu) == "
               "xr_xir_scalar_load(frame, %uu));\n", destination,
               layout->offsets[op->args[0]], layout->offsets[op->args[1]]);
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

XrXirStatus xr_xir_emit_c(const XrXirArtifact *artifact, const char *symbol_prefix,
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

void xr_xir_c_source_free(XrXirCSource *source) {
    if (!source)
        return;
    xr_free(source->text);
    *source = (XrXirCSource) {NULL, 0};
}
