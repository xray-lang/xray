/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_c.c - Mechanical portable-C11 spelling of XrBackendIR
 */

#include "xr_backend_ir_internal.h"

#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../core/xr_core_spec_gen.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../runtime/abi/xr_builtin_provider_contract.h"
#include "../xi_cgen_verify_output.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define XR_GENERATED_C_MAX_SIZE (64u * 1024u * 1024u)

typedef struct CBuffer {
    char *bytes;
    size_t size;
    size_t capacity;
    bool failed;
} CBuffer;

static bool reserve(CBuffer *buffer, size_t extra) {
    if (buffer->failed || extra > XR_GENERATED_C_MAX_SIZE ||
        buffer->size > XR_GENERATED_C_MAX_SIZE - extra) {
        buffer->failed = true;
        return false;
    }
    size_t required = buffer->size + extra + 1u;
    if (required <= buffer->capacity)
        return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < required) {
        if (capacity > XR_GENERATED_C_MAX_SIZE / 2u) {
            capacity = XR_GENERATED_C_MAX_SIZE + 1u;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > XR_GENERATED_C_MAX_SIZE) {
        buffer->failed = true;
        return false;
    }
    char *grown = xr_realloc(buffer->bytes, capacity);
    if (!grown) {
        buffer->failed = true;
        return false;
    }
    buffer->bytes = grown;
    buffer->capacity = capacity;
    return true;
}

static bool append_text(CBuffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (!reserve(buffer, length))
        return false;
    memcpy(buffer->bytes + buffer->size, text, length);
    buffer->size += length;
    buffer->bytes[buffer->size] = '\0';
    return true;
}

static bool append_format(CBuffer *buffer, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list measure;
    va_copy(measure, arguments);
    int length = vsnprintf(NULL, 0u, format, measure);
    va_end(measure);
    if (length < 0 || !reserve(buffer, (size_t) length)) {
        va_end(arguments);
        return false;
    }
    (void) vsnprintf(buffer->bytes + buffer->size, (size_t) length + 1u, format, arguments);
    va_end(arguments);
    buffer->size += (size_t) length;
    return true;
}

static const char *type_c_name(uint16_t type_id, char storage[32]) {
    switch (type_id) {
        case XR_CORE_TYPE_BOOL:
            return "uint8_t";
        case XR_CORE_TYPE_I64:
            return "int64_t";
        case XR_CORE_TYPE_U32:
        case XR_CORE_TYPE_ERROR:
        case XR_CORE_TYPE_PANIC_INFO:
            return "uint32_t";
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return "uint16_t";
        case XR_CORE_TYPE_VOID:
            return "void";
        default:
            if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return NULL;
            (void) snprintf(storage, 32u, "XrAotType%u", type_id);
            return storage;
    }
}

static uint32_t outcome_value_kind(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_VOID:
            return 0u;
        case XR_CORE_TYPE_BOOL:
            return 1u;
        case XR_CORE_TYPE_I64:
            return 2u;
        case XR_CORE_TYPE_U32:
            return 3u;
        case XR_CORE_TYPE_ERROR:
            return 4u;
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return 6u;
        default:
            return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? 5u : UINT32_MAX;
    }
}

static bool emit_type_definition(CBuffer *buffer, const XrBackendIR *ir, uint32_t index,
                                 uint8_t *state) {
    if (state[index] == 2u)
        return true;
    if (state[index] == 1u)
        return false;
    state[index] = 1u;
    const XrValidatedType *type = &ir->program->types[index];
    for (uint32_t field = 0; field < type->field_count; ++field) {
        uint16_t child = type->field_types[field];
        if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
            return false;
    }
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        for (uint32_t field = 0; field < type->variants[variant].payload_count; ++field) {
            uint16_t child = type->variants[variant].payload_types[field];
            if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                !emit_type_definition(buffer, ir, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                return false;
        }
    }
    if (!append_format(buffer, "struct XrAotType%u {\n", type->type_id))
        return false;
    if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        if (type->field_count == 0u && !append_text(buffer, "    uint8_t xr_unit;\n"))
            return false;
        for (uint32_t field = 0; field < type->field_count; ++field) {
            char storage[32];
            const char *name = type_c_name(type->field_types[field], storage);
            if (!name || !append_format(buffer, "    %s f%u;\n", name, field))
                return false;
        }
    } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (!append_text(buffer, "    uint32_t tag;\n    union {\n"))
            return false;
        for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            if (!append_text(buffer, "        struct {\n"))
                return false;
            if (row->payload_count == 0u && !append_text(buffer, "            uint8_t empty;\n"))
                return false;
            for (uint32_t field = 0; field < row->payload_count; ++field) {
                char storage[32];
                const char *name = type_c_name(row->payload_types[field], storage);
                if (!name || !append_format(buffer, "            %s f%u;\n", name, field))
                    return false;
            }
            if (!append_format(buffer, "        } case_%u;\n", variant))
                return false;
        }
        if (!append_text(buffer, "    } payload;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_VIEW) {
        if (!append_text(buffer, "    void *data;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (!append_text(buffer, "    uint32_t function_id;\n    void *capture;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (!append_text(
                buffer,
                "    uint16_t concrete_type_id;\n    uint32_t conformance_id;\n    void *data;\n"))
            return false;
    } else {
        return false;
    }
    if (!append_text(buffer, "};\n\n"))
        return false;
    state[index] = 2u;
    return true;
}

static bool emit_type_definitions(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t index = 0; index < ir->program->type_count; ++index) {
        if (!append_format(buffer, "typedef struct XrAotType%u XrAotType%u;\n",
                           ir->program->types[index].type_id, ir->program->types[index].type_id))
            return false;
    }
    if (ir->program->type_count != 0u && !append_text(buffer, "\n"))
        return false;
    uint8_t *state =
        xr_calloc(ir->program->type_count ? ir->program->type_count : 1u, sizeof(uint8_t));
    if (!state)
        return false;
    bool emitted = true;
    for (uint32_t index = 0; emitted && index < ir->program->type_count; ++index)
        emitted = emit_type_definition(buffer, ir, index, state);
    xr_free(state);
    return emitted;
}

static const char *outcome_field(uint16_t type_id) {
    switch (type_id) {
        case XR_CORE_TYPE_BOOL:
            return "boolean";
        case XR_CORE_TYPE_I64:
            return "i64";
        case XR_CORE_TYPE_U32:
            return "u32";
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            return "u16";
        case XR_CORE_TYPE_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static bool conformance_id(const XrValidatedProgram *program, uint16_t concrete_type_id,
                           uint32_t interface_id, uint32_t *id_out) {
    if (!program || !id_out)
        return false;
    for (uint32_t index = 0; index < program->conformance_count; ++index) {
        const XrValidatedConformance *row = &program->conformances[index];
        if (row->implementor_type_id == concrete_type_id && row->interface_id == interface_id) {
            *id_out = index;
            return true;
        }
    }
    return false;
}

static void scan_helpers(const XrBackendIR *ir, bool *checked, bool *wrapping, bool *arena,
                         bool *output) {
    *checked = false;
    *wrapping = false;
    *arena = false;
    *output = false;
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *fn = &ir->functions[function];
        for (uint32_t block = 0; block < fn->block_count; ++block) {
            const XrBackendBlock *row = &fn->blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrBackendInstruction *op = &row->instructions[instruction];
                if (op->operation_id == XR_CORE_OP_CORE_ADD_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_SUB_I64 ||
                    op->operation_id == XR_CORE_OP_CORE_MUL_I64) {
                    if (op->immediate.u32 == 0u)
                        *checked = true;
                    else
                        *wrapping = true;
                }
                if (op->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
                    (op->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK && op->operand_count != 0u))
                    *arena = true;
                if (op->operation_id == XR_CORE_OP_CORE_OUTPUT_GROUP_I64)
                    *output = true;
            }
        }
    }
}

typedef struct XrAotHostedClockBindings {
    bool realtime;
    bool monotonic;
    bool process_cpu;
    bool utc_offset;
    uint32_t realtime_requirement;
    uint32_t realtime_operation;
    uint32_t monotonic_requirement;
    uint32_t monotonic_operation;
    uint32_t process_cpu_requirement;
    uint32_t process_cpu_operation;
    uint32_t utc_offset_requirement;
    uint32_t utc_offset_operation;
} XrAotHostedClockBindings;

typedef struct XrAotHostedPipeBindings {
    bool open;
    bool close;
    uint32_t open_requirement;
    uint32_t open_operation;
    uint32_t close_requirement;
    uint32_t close_operation;
} XrAotHostedPipeBindings;

static XrAotHostedClockBindings hosted_clock_bindings(const XrBackendIR *ir);
static XrAotHostedPipeBindings hosted_pipe_bindings(const XrBackendIR *ir);

static bool emit_prelude(CBuffer *buffer, const XrBackendIR *ir, bool standalone_main) {
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    bool output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    XrAotHostedClockBindings clock = hosted_clock_bindings(ir);
    XrAotHostedPipeBindings pipe = hosted_pipe_bindings(ir);
    bool host_clock = standalone_main &&
                      (clock.realtime || clock.monotonic || clock.process_cpu ||
                       clock.utc_offset);
    bool host_pipe = standalone_main && (pipe.open || pipe.close);
    if (((host_clock || host_pipe) &&
         !append_text(buffer, "#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)\n"
                              "#define _POSIX_C_SOURCE 200809L\n"
                              "#endif\n")) ||
        !append_text(buffer, "#include <stdint.h>\n"
                             "#include <limits.h>\n"
                             "#include <stddef.h>\n") ||
        (arena && !append_text(buffer, "#include <stdlib.h>\n")) ||
        ((host_clock || host_pipe) && !append_text(buffer, "#include <time.h>\n"
                                                           "#if defined(_WIN32)\n"
                                                           "#ifndef WIN32_LEAN_AND_MEAN\n"
                                                           "#define WIN32_LEAN_AND_MEAN\n"
                                                           "#endif\n"
                                                           "#include <windows.h>\n"
                                                           "#else\n"
                                                           "#include <fcntl.h>\n"
                                                           "#include <unistd.h>\n"
                                                           "#endif\n")) ||
        (output && standalone_main &&
         !append_text(buffer, "#include <stdio.h>\n"
                              "#if defined(_WIN32)\n"
                              "#include <fcntl.h>\n"
                              "#include <io.h>\n"
                              "#endif\n")) ||
        !append_text(buffer, "\n"))
        return false;
    if (!append_text(
            buffer, "typedef int (*XrAotProviderCallI64Unary)(void *context, uint32_t requirement, "
                    "uint32_t operation, int64_t argument, int64_t *result);\n"
                    "typedef int (*XrAotProviderCallI64Nullary)(void *context, uint32_t "
                    "requirement, uint32_t operation, int64_t *result);\n"
                    "typedef int (*XrAotProviderCallBoolI64Unary)(void *context, uint32_t "
                    "requirement, uint32_t operation, int64_t argument, uint8_t *result);\n"
                    "typedef int (*XrAotProviderCallOptionalI64PairNullary)(void *context, "
                    "uint32_t requirement, uint32_t operation, uint8_t *present, "
                    "int64_t *first, int64_t *second);\n"
                    "typedef int (*XrAotProviderOutputWrite)(void *context, uint32_t "
                    "requirement, uint32_t operation, const uint8_t *bytes, size_t size);\n\n"))
        return false;
    if (arena) {
        if (!append_text(buffer,
                         "typedef union XrAotAllocation XrAotAllocation;\n"
                         "union XrAotAllocation {\n"
                         "    struct { XrAotAllocation *next; } link;\n"
                         "    max_align_t alignment;\n"
                         "};\n"
                         "typedef struct XrAotContext {\n"
                         "    XrAotAllocation *allocations;\n"
                         "    void *provider_context;\n"
                         "    XrAotProviderCallI64Unary provider_call_i64_unary;\n"
                         "    XrAotProviderCallI64Nullary provider_call_i64_nullary;\n"
                         "    XrAotProviderCallBoolI64Unary provider_call_bool_i64_unary;\n"
                         "    XrAotProviderCallOptionalI64PairNullary "
                         "provider_call_optional_i64_pair_nullary;\n"
                         "    XrAotProviderOutputWrite provider_output_write;\n"
                         "} XrAotContext;\n\n"
                         "static inline void *xr_aot_alloc(XrAotContext *context, size_t size) "
                         "{\n"
                         "    if (!context || size > SIZE_MAX - sizeof(XrAotAllocation)) "
                         "return NULL;\n"
                         "    XrAotAllocation *allocation = "
                         "(XrAotAllocation *)malloc(sizeof(XrAotAllocation) + size);\n"
                         "    if (!allocation) return NULL;\n"
                         "    allocation->link.next = context->allocations;\n"
                         "    context->allocations = allocation;\n"
                         "    return (void *)(allocation + 1);\n"
                         "}\n"
                         "static inline void xr_aot_context_destroy(XrAotContext *context) "
                         "{\n"
                         "    XrAotAllocation *allocation = context->allocations;\n"
                         "    while (allocation) {\n"
                         "        XrAotAllocation *next = allocation->link.next;\n"
                         "        free(allocation);\n"
                         "        allocation = next;\n"
                         "    }\n"
                         "    context->allocations = NULL;\n"
                         "}\n\n"))
            return false;
    } else if (!append_text(buffer, "typedef struct XrAotContext {\n"
                                    "    void *provider_context;\n"
                                    "    XrAotProviderCallI64Unary provider_call_i64_unary;\n"
                                    "    XrAotProviderCallI64Nullary provider_call_i64_nullary;\n"
                                    "    XrAotProviderCallBoolI64Unary "
                                    "provider_call_bool_i64_unary;\n"
                                    "    XrAotProviderCallOptionalI64PairNullary "
                                    "provider_call_optional_i64_pair_nullary;\n"
                                    "    XrAotProviderOutputWrite provider_output_write;\n"
                                    "} XrAotContext;\n\n")) {
        return false;
    }
    if (!append_text(buffer, "typedef struct XrAotOutcome {\n"
                             "    uint32_t kind;\n"
                             "    uint32_t value_kind;\n"
                             "    uint32_t trap;\n"
                             "    uint32_t error;\n"
                             "    uint8_t boolean;\n"
                             "    int64_t i64;\n"
                             "    uint32_t u32;\n"
                             "    uint16_t u16;\n"
                             "} XrAotOutcome;\n\n"
                             "static XrAotOutcome xr_aot_make(uint32_t kind, uint32_t value_kind, "
                             "uint32_t trap) {\n"
                             "    XrAotOutcome result = {0};\n"
                             "    result.kind = kind;\n"
                             "    result.value_kind = value_kind;\n"
                             "    result.trap = trap;\n"
                             "    return result;\n"
                             "}\n\n"))
        return false;
    if (wrapping &&
        !append_text(buffer, "static int64_t xr_aot_i64_from_bits(uint64_t bits) {\n"
                             "    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;\n"
                             "    return -(int64_t)(~bits) - INT64_C(1);\n"
                             "}\n\n"))
        return false;
    if (output &&
        !append_text(buffer,
                     "static size_t xr_aot_format_i64_line(int64_t value, uint8_t output[22]) "
                     "{\n"
                     "    uint8_t reverse[20];\n"
                     "    size_t count = 0;\n"
                     "    uint64_t magnitude = value < 0 ? UINT64_C(0) - (uint64_t)value : "
                     "(uint64_t)value;\n"
                     "    do {\n"
                     "        reverse[count++] = (uint8_t)('0' + magnitude % UINT64_C(10));\n"
                     "        magnitude /= UINT64_C(10);\n"
                     "    } while (magnitude != 0);\n"
                     "    size_t cursor = 0;\n"
                     "    if (value < 0) output[cursor++] = (uint8_t)'-';\n"
                     "    while (count != 0) output[cursor++] = reverse[--count];\n"
                     "    output[cursor++] = (uint8_t)'\\n';\n"
                     "    return cursor;\n"
                     "}\n\n"))
        return false;
    if (output && standalone_main &&
        !append_text(buffer,
                     "static int xr_aot_host_output_write(void *context, uint32_t requirement, "
                     "uint32_t operation, const uint8_t *bytes, size_t size) {\n"
                     "    (void)context;\n"
                     "    (void)requirement;\n"
                     "    (void)operation;\n"
                     "#if defined(_WIN32)\n"
                     "    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;\n"
                     "#endif\n"
                     "    if ((!bytes && size != 0) || fwrite(bytes, 1, size, stdout) != size) "
                     "return 1;\n"
                     "    return 0;\n"
                     "}\n\n"))
        return false;
    if (host_pipe && pipe.open &&
        (!append_text(buffer,
                      "static int xr_aot_host_pipe_open(void *context, uint32_t requirement, "
                      "uint32_t operation, uint8_t *present, int64_t *first, int64_t *second) {\n"
                      "    (void)context;\n") ||
         !append_format(buffer,
                        "    if (requirement != UINT32_C(%u) || operation != UINT32_C(%u) || "
                        "!present || !first || !second) return 1;\n",
                        pipe.open_requirement, pipe.open_operation) ||
         !append_text(
             buffer, "    *present = UINT8_C(0);\n"
                     "    *first = INT64_C(0);\n"
                     "    *second = INT64_C(0);\n"
                     "#if defined(_WIN32)\n"
                     "    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, FALSE};\n"
                     "    HANDLE read_handle = NULL;\n"
                     "    HANDLE write_handle = NULL;\n"
                     "    if (!CreatePipe(&read_handle, &write_handle, &attributes, 0)) return 0;\n"
                     "    if (!SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0) || "
                     "!SetHandleInformation(write_handle, HANDLE_FLAG_INHERIT, 0)) {\n"
                     "        CloseHandle(read_handle);\n"
                     "        CloseHandle(write_handle);\n"
                     "        return 0;\n"
                     "    }\n"
                     "    *first = (int64_t)(intptr_t)read_handle;\n"
                     "    *second = (int64_t)(intptr_t)write_handle;\n"
                     "#else\n"
                     "    int handles[2];\n"
                     "    if (pipe(handles) != 0) return 0;\n"
                     "    int read_flags = fcntl(handles[0], F_GETFD);\n"
                     "    int write_flags = fcntl(handles[1], F_GETFD);\n"
                     "    if (read_flags < 0 || write_flags < 0 || "
                     "fcntl(handles[0], F_SETFD, read_flags | FD_CLOEXEC) != 0 || "
                     "fcntl(handles[1], F_SETFD, write_flags | FD_CLOEXEC) != 0) {\n"
                     "        close(handles[0]);\n"
                     "        close(handles[1]);\n"
                     "        return 0;\n"
                     "    }\n"
                     "    *first = (int64_t)handles[0];\n"
                     "    *second = (int64_t)handles[1];\n"
                     "#endif\n"
                     "    *present = UINT8_C(1);\n"
                     "    return 0;\n"
                     "}\n\n")))
        return false;
    if (host_pipe && pipe.close &&
        (!append_text(buffer,
                      "static int xr_aot_host_pipe_close(void *context, uint32_t requirement, "
                      "uint32_t operation, int64_t argument, uint8_t *result) {\n"
                      "    (void)context;\n") ||
         !append_format(buffer,
                        "    if (requirement != UINT32_C(%u) || operation != UINT32_C(%u) || "
                        "!result) return 1;\n",
                        pipe.close_requirement, pipe.close_operation) ||
         !append_text(buffer,
                      "#if defined(_WIN32)\n"
                      "    *result = CloseHandle((HANDLE)(intptr_t)argument) ? UINT8_C(1) : "
                      "UINT8_C(0);\n"
                      "#else\n"
                      "    *result = argument >= INT_MIN && argument <= INT_MAX && "
                      "close((int)argument) == 0 ? UINT8_C(1) : UINT8_C(0);\n"
                      "#endif\n"
                      "    return 0;\n"
                      "}\n\n")))
        return false;
    if (host_clock && clock.realtime &&
        !append_text(buffer,
                     "static inline uint64_t xr_aot_host_realtime_nanos(void) {\n"
                     "#if defined(_WIN32)\n"
                     "    FILETIME value;\n"
                     "    ULARGE_INTEGER ticks;\n"
                     "    GetSystemTimePreciseAsFileTime(&value);\n"
                     "    ticks.LowPart = value.dwLowDateTime;\n"
                     "    ticks.HighPart = value.dwHighDateTime;\n"
                     "    return (ticks.QuadPart - UINT64_C(116444736000000000)) * "
                     "UINT64_C(100);\n"
                     "#else\n"
                     "    struct timespec value;\n"
                     "    if (clock_gettime(CLOCK_REALTIME, &value) != 0) return UINT64_C(0);\n"
                     "    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + "
                     "(uint64_t)value.tv_nsec;\n"
                     "#endif\n"
                     "}\n\n"))
        return false;
    if (host_clock && clock.monotonic &&
        !append_text(buffer,
                     "static inline uint64_t xr_aot_host_monotonic_nanos(void) {\n"
                     "#if defined(_WIN32)\n"
                     "    LARGE_INTEGER frequency;\n"
                     "    LARGE_INTEGER counter;\n"
                     "    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 "
                     "|| !QueryPerformanceCounter(&counter)) return UINT64_C(0);\n"
                     "    uint64_t scale = (uint64_t)frequency.QuadPart;\n"
                     "    uint64_t ticks = (uint64_t)counter.QuadPart;\n"
                     "    return (ticks / scale) * UINT64_C(1000000000) + "
                     "((ticks % scale) * UINT64_C(1000000000)) / scale;\n"
                     "#else\n"
                     "    struct timespec value;\n"
                     "    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return UINT64_C(0);\n"
                     "    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + "
                     "(uint64_t)value.tv_nsec;\n"
                     "#endif\n"
                     "}\n\n"))
        return false;
    if (host_clock && clock.process_cpu &&
        !append_text(buffer,
                     "static inline uint64_t xr_aot_host_process_cpu_nanos(void) {\n"
                     "#if defined(_WIN32)\n"
                     "    FILETIME creation, exit_time, kernel, user;\n"
                     "    ULARGE_INTEGER kernel_ticks, user_ticks;\n"
                     "    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, "
                     "&kernel, &user)) return UINT64_C(0);\n"
                     "    kernel_ticks.LowPart = kernel.dwLowDateTime;\n"
                     "    kernel_ticks.HighPart = kernel.dwHighDateTime;\n"
                     "    user_ticks.LowPart = user.dwLowDateTime;\n"
                     "    user_ticks.HighPart = user.dwHighDateTime;\n"
                     "    return (kernel_ticks.QuadPart + user_ticks.QuadPart) * UINT64_C(100);\n"
                     "#else\n"
                     "#if defined(CLOCK_PROCESS_CPUTIME_ID)\n"
                     "    struct timespec value;\n"
                     "    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0)\n"
                     "        return (uint64_t)value.tv_sec * UINT64_C(1000000000) + "
                     "(uint64_t)value.tv_nsec;\n"
                     "#endif\n"
                     "    return (uint64_t)clock() * (UINT64_C(1000000000) / CLOCKS_PER_SEC);\n"
                     "#endif\n"
                     "}\n\n"))
        return false;
    if (host_clock && clock.utc_offset &&
        !append_text(buffer,
                     "static inline int xr_aot_host_utc_offset_at(int64_t seconds, "
                     "int64_t *result) {\n"
                     "    if (!result) return 1;\n"
                     "    time_t probe = (time_t)seconds;\n"
                     "    struct tm local_value;\n"
                     "    struct tm utc_value;\n"
                     "#if defined(_WIN32)\n"
                     "    if (probe < (time_t)86400) probe = (time_t)86400;\n"
                     "    if (localtime_s(&local_value, &probe) != 0 || "
                     "gmtime_s(&utc_value, &probe) != 0) return 1;\n"
                     "#else\n"
                     "    if (!localtime_r(&probe, &local_value) || "
                     "!gmtime_r(&probe, &utc_value)) return 1;\n"
                     "#endif\n"
                     "    local_value.tm_isdst = 0;\n"
                     "    utc_value.tm_isdst = 0;\n"
                     "    *result = (int64_t)(difftime(mktime(&local_value), "
                     "mktime(&utc_value)) / 60.0);\n"
                     "    return 0;\n"
                     "}\n\n"))
        return false;
    if (host_clock && (clock.realtime || clock.monotonic || clock.process_cpu)) {
        if (!append_text(buffer,
                         "static int xr_aot_host_clock_nullary(void *context, "
                         "uint32_t requirement, uint32_t operation, int64_t *result) {\n"
                         "    (void)context;\n"
                         "    if (!result) return 1;\n"))
            return false;
        if (clock.realtime &&
            !append_format(buffer,
                           "    if (requirement == UINT32_C(%u) && operation == "
                           "UINT32_C(%u)) { *result = (int64_t)xr_aot_host_realtime_nanos(); "
                           "return 0; }\n",
                           clock.realtime_requirement, clock.realtime_operation))
            return false;
        if (clock.monotonic &&
            !append_format(buffer,
                           "    if (requirement == UINT32_C(%u) && operation == "
                           "UINT32_C(%u)) { *result = (int64_t)xr_aot_host_monotonic_nanos(); "
                           "return 0; }\n",
                           clock.monotonic_requirement, clock.monotonic_operation))
            return false;
        if (clock.process_cpu &&
            !append_format(buffer,
                           "    if (requirement == UINT32_C(%u) && operation == "
                           "UINT32_C(%u)) { *result = (int64_t)xr_aot_host_process_cpu_nanos(); "
                           "return 0; }\n",
                           clock.process_cpu_requirement, clock.process_cpu_operation))
            return false;
        if (!append_text(buffer, "    return 1;\n}\n\n"))
            return false;
    }
    if (host_clock && clock.utc_offset &&
        (!append_text(buffer,
                      "static int xr_aot_host_clock_unary(void *context, uint32_t requirement, "
                      "uint32_t operation, int64_t argument, int64_t *result) {\n"
                      "    (void)context;\n") ||
         !append_format(buffer,
                        "    if (requirement == UINT32_C(%u) && operation == UINT32_C(%u)) "
                        "return xr_aot_host_utc_offset_at(argument, result);\n",
                        clock.utc_offset_requirement, clock.utc_offset_operation) ||
         !append_text(buffer, "    return 1;\n}\n\n")))
        return false;
    if (checked &&
        !append_text(buffer, "int xr_aot_checked_add(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if ((right > 0 && left > INT64_MAX - right) || "
                             "(right < 0 && left < INT64_MIN - right)) return 0;\n"
                             "    *out = left + right;\n"
                             "    return 1;\n"
                             "}\n"
                             "int xr_aot_checked_sub(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if ((right < 0 && left > INT64_MAX + right) || "
                             "(right > 0 && left < INT64_MIN + right)) return 0;\n"
                             "    *out = left - right;\n"
                             "    return 1;\n"
                             "}\n"
                             "int xr_aot_checked_mul(int64_t left, int64_t right, int64_t *out) {\n"
                             "    if (left == 0 || right == 0) { *out = 0; return 1; }\n"
                             "    if ((left == -1 && right == INT64_MIN) || "
                             "(right == -1 && left == INT64_MIN)) return 0;\n"
                             "    if (left > 0) {\n"
                             "        if ((right > 0 && left > INT64_MAX / right) || "
                             "(right < 0 && right < INT64_MIN / left)) return 0;\n"
                             "    } else if ((right > 0 && left < INT64_MIN / right) || "
                             "(right < 0 && left < INT64_MAX / right)) return 0;\n"
                             "    *out = left * right;\n"
                             "    return 1;\n"
                             "}\n\n"))
        return false;
    return emit_type_definitions(buffer, ir);
}

static const XrBackendInstruction *coroutine_suspension_instruction(
    const XrBackendFunction *function, uint32_t safepoint_id, uint32_t *block_id_out) {
    const XrBackendInstruction *found = NULL;
    for (uint32_t block = 0u; function && block < function->block_count; ++block) {
        const XrBackendBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0u; instruction < row->instruction_count; ++instruction) {
            const XrBackendInstruction *candidate = &row->instructions[instruction];
            bool matches =
                (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD &&
                 candidate->immediate.u32 == safepoint_id) ||
                (candidate->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
                 candidate->immediate.coroutine_call.safepoint_id == safepoint_id);
            if (!matches)
                continue;
            if (found)
                return NULL;
            found = candidate;
            if (block_id_out)
                *block_id_out = block;
        }
    }
    return found;
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool find_program_provider_operation(const XrValidatedProgram *program,
                                            const char *contract_key,
                                            const char *operation_key,
                                            uint32_t *requirement_out,
                                            uint32_t *operation_out) {
    XrFingerprint digest;
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    if (!program || !contract_key || !operation_key || !requirement_out || !operation_out ||
        !xr_stable_id_from_key(contract_key, &contract_id, &digest) ||
        !xr_stable_id_from_key(operation_key, &operation_id, &digest))
        return false;
    for (uint32_t requirement = 0u;
         requirement < xr_validated_program_provider_requirement_count(program);
         ++requirement) {
        XrProgramProviderRequirementView view = {0};
        if (!xr_validated_program_provider_requirement(program, requirement, &view) ||
            !stable_id_equal(view.contract_id, contract_id))
            continue;
        for (uint32_t operation = 0u; operation < view.operation_count; ++operation) {
            if (!stable_id_equal(view.operation_ids[operation], operation_id))
                continue;
            *requirement_out = requirement;
            *operation_out = operation;
            return true;
        }
    }
    return false;
}

static XrAotHostedClockBindings hosted_clock_bindings(const XrBackendIR *ir) {
    XrAotHostedClockBindings bindings = {0};
    const XrValidatedProgram *program = ir ? ir->program : NULL;
    bindings.realtime = find_program_provider_operation(
        program, XR_PROVIDER_CLOCK_CONTRACT_KEY,
        XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY, &bindings.realtime_requirement,
        &bindings.realtime_operation);
    bindings.monotonic = find_program_provider_operation(
        program, XR_PROVIDER_CLOCK_CONTRACT_KEY,
        XR_PROVIDER_CLOCK_MONOTONIC_NANOS_OPERATION_KEY, &bindings.monotonic_requirement,
        &bindings.monotonic_operation);
    bindings.process_cpu = find_program_provider_operation(
        program, XR_PROVIDER_CLOCK_CONTRACT_KEY,
        XR_PROVIDER_CLOCK_PROCESS_CPU_NANOS_OPERATION_KEY,
        &bindings.process_cpu_requirement, &bindings.process_cpu_operation);
    bindings.utc_offset = find_program_provider_operation(
        program, XR_PROVIDER_CLOCK_CONTRACT_KEY,
        XR_PROVIDER_CLOCK_UTC_OFFSET_MINUTES_AT_OPERATION_KEY,
        &bindings.utc_offset_requirement, &bindings.utc_offset_operation);
    return bindings;
}

static XrAotHostedPipeBindings hosted_pipe_bindings(const XrBackendIR *ir) {
    XrAotHostedPipeBindings bindings = {0};
    const XrValidatedProgram *program = ir ? ir->program : NULL;
    bindings.open = find_program_provider_operation(program, XR_PROVIDER_IO_CONTRACT_KEY,
                                                    XR_PROVIDER_IO_PIPE_OPEN_OPERATION_KEY,
                                                    &bindings.open_requirement,
                                                    &bindings.open_operation);
    bindings.close = find_program_provider_operation(program, XR_PROVIDER_IO_CONTRACT_KEY,
                                                     XR_PROVIDER_IO_PIPE_CLOSE_OPERATION_KEY,
                                                     &bindings.close_requirement,
                                                     &bindings.close_operation);
    return bindings;
}

static bool emit_coroutine_frame_definition(CBuffer *buffer, const XrBackendIR *ir,
                                             uint32_t function_id, uint8_t *state) {
    if (state[function_id] == 2u)
        return true;
    if (state[function_id] == 1u)
        return false;
    state[function_id] = 1u;
    const XrBackendFunction *function = &ir->functions[function_id];
    for (uint32_t safepoint = 0u; safepoint < function->coroutine_safepoint_count;
         ++safepoint) {
        const XrBackendInstruction *instruction =
            coroutine_suspension_instruction(function, safepoint, NULL);
        if (!instruction)
            return false;
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
            !emit_coroutine_frame_definition(
                buffer, ir, instruction->immediate.coroutine_call.function_id, state))
            return false;
    }
    if (!append_format(buffer, "struct XrAotCoroutineFrame%u {\n    uint32_t state;\n",
                       function_id))
        return false;
    for (uint32_t parameter = 0u; parameter < function->parameter_count; ++parameter) {
        char storage[32];
        const char *type = type_c_name(function->parameter_types[parameter], storage);
        if (!type || function->parameter_modes[parameter] != XR_PARAM_READ ||
            !append_format(buffer, "    %s parameter_%u;\n", type, parameter))
            return false;
    }
    for (uint32_t safepoint = 0u; safepoint < function->coroutine_safepoint_count;
         ++safepoint) {
        const XrBackendCoroutineSafepoint *point = &function->coroutine_safepoints[safepoint];
        const XrBackendInstruction *instruction =
            coroutine_suspension_instruction(function, safepoint, NULL);
        if (!instruction)
            return false;
        for (uint32_t live = 0u; live < point->live_value_count; ++live) {
            char storage[32];
            const char *type = type_c_name(function->value_types[point->live_value_ids[live]],
                                           storage);
            if (!type || !append_format(buffer, "    %s live_%u_%u;\n", type, safepoint, live))
                return false;
        }
        if (instruction->operation_id == XR_CORE_OP_CORE_COROUTINE_CALL_SEALED &&
            (!append_format(buffer, "    XrAotCoroutineFrame%u child_%u;\n",
                            instruction->immediate.coroutine_call.function_id, safepoint) ||
             !append_format(buffer, "    uint8_t child_active_%u;\n", safepoint)))
            return false;
    }
    if (!append_text(buffer, "};\n\n"))
        return false;
    state[function_id] = 2u;
    return true;
}

static bool emit_coroutine_frames(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t function_id = 0u; function_id < ir->function_count; ++function_id) {
        if (ir->functions[function_id].coroutine_safepoint_count != 0u &&
            !append_format(buffer,
                           "typedef struct XrAotCoroutineFrame%u XrAotCoroutineFrame%u;\n",
                           function_id, function_id))
            return false;
    }
    if (!append_text(buffer, "\n"))
        return false;
    uint8_t *state = xr_calloc(ir->function_count ? ir->function_count : 1u, sizeof(*state));
    if (!state)
        return false;
    bool emitted = true;
    for (uint32_t function_id = 0u; emitted && function_id < ir->function_count; ++function_id) {
        if (ir->functions[function_id].coroutine_safepoint_count != 0u)
            emitted = emit_coroutine_frame_definition(buffer, ir, function_id, state);
    }
    xr_free(state);
    return emitted;
}

static bool emit_function_signature(CBuffer *buffer, const XrBackendFunction *function,
                                    const XrBackendIR *ir, uint32_t function_id, bool prototype) {
    const char *suffix = function->coroutine_safepoint_count != 0u ? "_step" : "";
    if (!append_format(buffer, "XrAotOutcome xr_aot_fn_%u%s(XrAotContext *xr_ctx", function_id,
                       suffix))
        return false;
    if (function->coroutine_safepoint_count != 0u &&
        !append_format(buffer, ", XrAotCoroutineFrame%u *frame", function_id))
        return false;
    bool aggregate_result =
        xr_validated_program_type(ir->program, function->result_type_id) != NULL;
    bool has_error = function->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = function->panic_type_id != XR_CORE_TYPE_VOID;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        char storage[32];
        const char *name = type_c_name(function->parameter_types[parameter], storage);
        if (!name)
            return false;
        const char *pointer = function->parameter_modes[parameter] == XR_PARAM_REF ? " *" : "";
        if (!append_format(buffer, ", %s%s p%u", name, pointer, parameter))
            return false;
    }
    if (aggregate_result) {
        char storage[32];
        const char *name = type_c_name(function->result_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_result", name))
            return false;
    }
    if (has_error) {
        char storage[32];
        const char *name = type_c_name(function->error_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_error", name))
            return false;
    }
    if (has_panic) {
        char storage[32];
        const char *name = type_c_name(function->panic_type_id, storage);
        if (!name || !append_format(buffer, ", %s *out_panic", name))
            return false;
    }
    return append_text(buffer, prototype ? ");\n" : ") {\n");
}

static bool emit_i64_literal(CBuffer *buffer, int64_t value) {
    if (value == INT64_MIN)
        return append_text(buffer, "(-INT64_C(9223372036854775807) - INT64_C(1))");
    if (value < 0)
        return append_format(buffer, "(-INT64_C(%llu))", (unsigned long long) (-(value + 1)) + 1u);
    return append_format(buffer, "INT64_C(%llu)", (unsigned long long) value);
}

static bool emit_parallel_edge(CBuffer *buffer, const XrBackendFunction *function,
                               const XrBackendInstruction *instruction, uint32_t successor_index,
                               uint32_t operand_start, uint32_t function_id) {
    uint32_t target_id = instruction->successors[successor_index];
    const XrBackendBlock *target = &function->blocks[target_id];
    if (!append_text(buffer, "{\n"))
        return false;
    for (uint32_t argument = 0; argument < target->argument_count; ++argument) {
        uint32_t source_value = instruction->operands[operand_start + argument];
        char storage[32];
        const char *type = type_c_name(target->argument_types[argument], storage);
        const char *pointer = target->argument_categories[argument] == XR_CORE_IR_PLACE ? " *" : "";
        if (!type || !append_format(buffer, "        %s%s edge_%u = v%u;\n", type, pointer,
                                    argument, source_value))
            return false;
    }
    for (uint32_t argument = 0; argument < target->argument_count; ++argument) {
        if (!append_format(buffer, "        v%u = edge_%u;\n", target->argument_ids[argument],
                           argument))
            return false;
    }
    return append_format(buffer, "        goto xr_f%u_b%u;\n    }\n", function_id, target_id);
}

static bool emit_return(CBuffer *buffer, const XrBackendFunction *function,
                        const XrBackendInstruction *instruction) {
    uint32_t kind = outcome_value_kind(function->result_type_id);
    if (kind == UINT32_MAX)
        return false;
    if (!append_format(buffer, "        { XrAotOutcome result = xr_aot_make(0, %u, 0);", kind))
        return false;
    if (instruction->operand_count != 0u) {
        if (function->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            if (!append_format(buffer, " *out_result = v%u;", instruction->operands[0]))
                return false;
            return append_text(buffer, " return result; }\n");
        }
        const char *field = outcome_field(function->result_type_id);
        if (!field || !append_format(buffer, " result.%s = v%u;", field, instruction->operands[0]))
            return false;
    }
    return append_text(buffer, " return result; }\n");
}

static bool emit_call(CBuffer *buffer, const XrBackendIR *ir,
                      const XrBackendInstruction *instruction, uint32_t instruction_id) {
    uint32_t callee_id = instruction->immediate.function_id;
    const XrBackendFunction *callee = &ir->functions[callee_id];
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(xr_ctx", instruction_id,
                       callee_id))
        return false;
    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
        if (!append_format(buffer, ", v%u", instruction->operands[operand]))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
        !append_format(buffer, ", &v%u", instruction->result_id))
        return false;
    if (!append_text(buffer, ")"))
        return false;
    if (!append_text(buffer, ";\n") ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
        if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
            return true;
        const char *field = outcome_field(callee->result_type_id);
        if (!field || !append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                     instruction_id, field))
            return false;
    }
    return true;
}

static const XrValidatedSignature *witness_signature(const XrBackendIR *ir,
                                                     const XrBackendFunction *function,
                                                     const XrBackendInstruction *instruction,
                                                     uint32_t *interface_id_out) {
    if (!ir || !function || !instruction || !interface_id_out || instruction->operand_count == 0u)
        return NULL;
    uint32_t receiver_value = instruction->operands[0];
    if (receiver_value >= function->value_count)
        return NULL;
    const XrValidatedType *receiver =
        xr_validated_program_type(ir->program, function->value_types[receiver_value]);
    if (!receiver || receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        receiver->interface_id >= ir->program->interface_count)
        return NULL;
    const XrValidatedInterface *interface_row = &ir->program->interfaces[receiver->interface_id];
    uint32_t slot = instruction->immediate.u32;
    if (slot >= interface_row->slot_count ||
        interface_row->slot_signature_ids[slot] >= ir->program->signature_count)
        return NULL;
    *interface_id_out = receiver->interface_id;
    return &ir->program->signatures[interface_row->slot_signature_ids[slot]];
}

static bool emit_witness_call_cases(CBuffer *buffer, const XrBackendIR *ir,
                                    const XrBackendInstruction *instruction,
                                    const XrValidatedSignature *signature, uint32_t interface_id,
                                    uint32_t instruction_id, const char *result_expression,
                                    const char *error_expression, const char *panic_expression) {
    uint32_t receiver_value = instruction->operands[0];
    uint32_t slot = instruction->immediate.u32;
    for (uint32_t conformance_id = 0; conformance_id < ir->program->conformance_count;
         ++conformance_id) {
        const XrValidatedConformance *conformance = &ir->program->conformances[conformance_id];
        if (conformance->interface_id != interface_id)
            continue;
        if (slot >= conformance->slot_count ||
            conformance->slot_function_ids[slot] >= ir->function_count)
            return false;
        uint32_t callee_id = conformance->slot_function_ids[slot];
        const XrBackendFunction *callee = &ir->functions[callee_id];
        if (callee->parameter_count != signature->parameter_count || callee->parameter_count == 0u)
            return false;
        char storage[32];
        const char *receiver_type = type_c_name(conformance->implementor_type_id, storage);
        if (!receiver_type ||
            !append_format(buffer,
                           "            case UINT32_C(%u):\n"
                           "                if (v%u.concrete_type_id != UINT16_C(%u)) break;\n"
                           "                call_%u = xr_aot_fn_%u(xr_ctx, ",
                           conformance_id, receiver_value, conformance->implementor_type_id,
                           instruction_id, callee_id))
            return false;
        if (callee->parameter_modes[0] == XR_PARAM_REF) {
            if (!append_format(buffer, "(%s *)v%u.data", receiver_type, receiver_value))
                return false;
        } else if (!append_format(buffer, "*(const %s *)v%u.data", receiver_type, receiver_value)) {
            return false;
        }
        for (uint32_t parameter = 1u; parameter < callee->parameter_count; ++parameter)
            if (!append_format(buffer, ", v%u", instruction->operands[parameter]))
                return false;
        if (result_expression && !append_format(buffer, ", %s", result_expression))
            return false;
        if (error_expression && !append_format(buffer, ", %s", error_expression))
            return false;
        if (panic_expression && !append_format(buffer, ", %s", panic_expression))
            return false;
        if (!append_text(buffer, ");\n                break;\n"))
            return false;
    }
    return append_text(buffer, "            default:\n                break;\n        }\n");
}

static bool emit_witness_call(CBuffer *buffer, const XrBackendIR *ir,
                              const XrBackendFunction *function,
                              const XrBackendInstruction *instruction, uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature || !append_format(buffer,
                                     "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                                     "        switch (v%u.conformance_id) {\n",
                                     instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    const char *result = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        instruction->result_id);
        result = result_expression;
    }
    if (!emit_witness_call_cases(buffer, ir, instruction, signature, interface_id, instruction_id,
                                 result, NULL, NULL) ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static const XrValidatedSignature *callable_signature(const XrBackendIR *ir,
                                                      const XrBackendFunction *function,
                                                      const XrBackendInstruction *instruction) {
    if (!ir || !function || !instruction || instruction->operand_count == 0u ||
        instruction->operands[0] >= function->value_count)
        return NULL;
    const XrValidatedType *callable =
        xr_validated_program_type(ir->program, function->value_types[instruction->operands[0]]);
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= ir->program->signature_count)
        return NULL;
    return &ir->program->signatures[callable->signature_id];
}

static bool callable_type_can_target(const XrBackendIR *ir, uint16_t callable_type,
                                     uint32_t target_function) {
    for (uint32_t function = 0; function < ir->function_count; ++function) {
        const XrBackendFunction *row = &ir->functions[function];
        for (uint32_t block = 0; block < row->block_count; ++block) {
            for (uint32_t instruction = 0; instruction < row->blocks[block].instruction_count;
                 ++instruction) {
                const XrBackendInstruction *candidate =
                    &row->blocks[block].instructions[instruction];
                if (candidate->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK &&
                    candidate->result_type_id == callable_type &&
                    candidate->immediate.function_id == target_function)
                    return true;
            }
        }
    }
    return false;
}

static bool emit_callable_call_cases(CBuffer *buffer, const XrBackendIR *ir,
                                     const XrBackendFunction *caller,
                                     const XrBackendInstruction *instruction,
                                     const XrValidatedSignature *signature, uint32_t instruction_id,
                                     const char *result_expression, const char *error_expression,
                                     const char *panic_expression) {
    uint32_t callable_value = instruction->operands[0];
    uint16_t callable_type = caller->value_types[callable_value];
    for (uint32_t target_id = 0; target_id < ir->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrBackendFunction *target = &ir->functions[target_id];
        const XrValidatedFunction *semantic_target = &ir->program->functions[target_id];
        uint32_t receiver_count = semantic_target->has_receiver ? 1u : 0u;
        if (target->parameter_count != signature->parameter_count + receiver_count ||
            !append_format(buffer,
                           "            case UINT32_C(%u):\n"
                           "                call_%u = xr_aot_fn_%u(xr_ctx",
                           target_id, instruction_id, target_id))
            return false;
        if (receiver_count != 0u) {
            char storage[32];
            const char *capture_type = type_c_name(target->parameter_types[0], storage);
            if (!capture_type ||
                !append_format(buffer, ", *(const %s *)v%u.capture", capture_type, callable_value))
                return false;
        }
        for (uint32_t parameter = 0u; parameter < signature->parameter_count; ++parameter)
            if (!append_format(buffer, ", v%u", instruction->operands[parameter + 1u]))
                return false;
        if (result_expression && !append_format(buffer, ", %s", result_expression))
            return false;
        if (error_expression && !append_format(buffer, ", %s", error_expression))
            return false;
        if (panic_expression && !append_format(buffer, ", %s", panic_expression))
            return false;
        if (!append_text(buffer, ");\n                break;\n"))
            return false;
    }
    return append_text(buffer, "            default:\n                break;\n        }\n");
}

static bool emit_callable_call(CBuffer *buffer, const XrBackendIR *ir,
                               const XrBackendFunction *function,
                               const XrBackendInstruction *instruction, uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature || !append_format(buffer,
                                     "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                                     "        switch (v%u.function_id) {\n",
                                     instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    const char *result = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        instruction->result_id);
        result = result_expression;
    }
    if (!emit_callable_call_cases(buffer, ir, function, instruction, signature, instruction_id,
                                  result, NULL, NULL) ||
        !append_format(buffer, "        if (call_%u.kind != 0) return call_%u;\n", instruction_id,
                       instruction_id))
        return false;
    if (instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
        signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return true;
    const char *field = outcome_field(signature->result_type_id);
    return field && append_format(buffer, "        v%u = call_%u.%s;\n", instruction->result_id,
                                  instruction_id, field);
}

static bool emit_invoke_edge(CBuffer *buffer, const XrBackendFunction *function,
                             const XrBackendInstruction *instruction, uint32_t successor_index,
                             uint32_t target_start, uint32_t operand_start, uint32_t function_id,
                             const char *implicit_expression) {
    const XrBackendBlock *target = &function->blocks[instruction->successors[successor_index]];
    if (!append_text(buffer, "{\n"))
        return false;
    if (target_start != 0u &&
        (!implicit_expression || !append_format(buffer, "            v%u = %s;\n",
                                                target->argument_ids[0], implicit_expression)))
        return false;
    for (uint32_t argument = target_start; argument < target->argument_count; ++argument) {
        uint32_t source_value = instruction->operands[operand_start + argument - target_start];
        char storage[32];
        const char *type = type_c_name(target->argument_types[argument], storage);
        const char *pointer = target->argument_categories[argument] == XR_CORE_IR_PLACE ? " *" : "";
        if (!type || !append_format(buffer, "            %s%s edge_%u = v%u;\n", type, pointer,
                                    argument, source_value))
            return false;
    }
    for (uint32_t argument = target_start; argument < target->argument_count; ++argument) {
        if (!append_format(buffer, "            v%u = edge_%u;\n", target->argument_ids[argument],
                           argument))
            return false;
    }
    return append_format(buffer, "            goto xr_f%u_b%u;\n        }\n", function_id,
                         instruction->successors[successor_index]);
}

static bool emit_invoke(CBuffer *buffer, const XrBackendIR *ir, const XrBackendFunction *function,
                        const XrBackendInstruction *instruction, uint32_t function_id,
                        uint32_t instruction_id) {
    const XrBackendFunction *callee = &ir->functions[instruction->immediate.function_id];
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(callee->error_type_id, storage);
        const char *initializer =
            callee->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer, "        XrAotOutcome call_%u = xr_aot_fn_%u(xr_ctx", instruction_id,
                       instruction->immediate.function_id))
        return false;
    for (uint32_t parameter = 0; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, ", v%u", instruction->operands[parameter]))
            return false;
    }
    if (callee->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        if (!append_format(buffer, ", &v%u", normal->argument_ids[0]))
            return false;
    }
    if (has_error) {
        if (!append_format(buffer, ", &invoke_error_%u", instruction_id))
            return false;
    }
    if (has_panic && !append_format(buffer, ", &invoke_panic_%u", instruction_id))
        return false;
    if (!append_text(buffer, ");\n"))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && callee->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(callee->result_type_id);
        if (!field)
            return false;
        snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id, field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        snprintf(normal_expression, sizeof(normal_expression), "v%u", normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit,
                          callee->parameter_count, function_id, normal_value))
        return false;
    uint32_t operand = callee->parameter_count + normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_witness_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                const XrBackendFunction *function,
                                const XrBackendInstruction *instruction, uint32_t function_id,
                                uint32_t instruction_id) {
    uint32_t interface_id = 0u;
    const XrValidatedSignature *signature =
        witness_signature(ir, function, instruction, &interface_id);
    if (!signature)
        return false;
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = signature->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(signature->error_type_id, storage);
        const char *initializer =
            signature->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer,
                       "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                       "        switch (v%u.conformance_id) {\n",
                       instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    char error_expression[64];
    char panic_expression[64];
    const char *result = NULL;
    const char *error = NULL;
    const char *panic = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        normal->argument_ids[0]);
        result = result_expression;
    }
    if (has_error) {
        (void) snprintf(error_expression, sizeof(error_expression), "&invoke_error_%u",
                        instruction_id);
        error = error_expression;
    }
    if (has_panic) {
        (void) snprintf(panic_expression, sizeof(panic_expression), "&invoke_panic_%u",
                        instruction_id);
        panic = panic_expression;
    }
    if (!emit_witness_call_cases(buffer, ir, instruction, signature, interface_id, instruction_id,
                                 result, error, panic))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && signature->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(signature->result_type_id);
        if (!field)
            return false;
        (void) snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id,
                        field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        (void) snprintf(normal_expression, sizeof(normal_expression), "v%u",
                        normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit,
                          signature->parameter_count, function_id, normal_value))
        return false;
    uint32_t operand = signature->parameter_count + normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_callable_invoke(CBuffer *buffer, const XrBackendIR *ir,
                                 const XrBackendFunction *function,
                                 const XrBackendInstruction *instruction, uint32_t function_id,
                                 uint32_t instruction_id) {
    const XrValidatedSignature *signature = callable_signature(ir, function, instruction);
    if (!signature)
        return false;
    const XrBackendBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = signature->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    bool has_error = signature->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = signature->panic_type_id != XR_CORE_TYPE_VOID;
    if (has_error) {
        char storage[32];
        const char *type = type_c_name(signature->error_type_id, storage);
        const char *initializer =
            signature->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "        %s invoke_error_%u = %s;\n", type,
                                    instruction_id, initializer))
            return false;
    }
    if (has_panic &&
        !append_format(buffer, "        uint32_t invoke_panic_%u = 0;\n", instruction_id))
        return false;
    if (!append_format(buffer,
                       "        XrAotOutcome call_%u = xr_aot_make(4, 0, 0);\n"
                       "        switch (v%u.function_id) {\n",
                       instruction_id, instruction->operands[0]))
        return false;
    char result_expression[64];
    char error_expression[64];
    char panic_expression[64];
    const char *result = NULL;
    const char *error = NULL;
    const char *panic = NULL;
    if (signature->result_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        (void) snprintf(result_expression, sizeof(result_expression), "&v%u",
                        normal->argument_ids[0]);
        result = result_expression;
    }
    if (has_error) {
        (void) snprintf(error_expression, sizeof(error_expression), "&invoke_error_%u",
                        instruction_id);
        error = error_expression;
    }
    if (has_panic) {
        (void) snprintf(panic_expression, sizeof(panic_expression), "&invoke_panic_%u",
                        instruction_id);
        panic = panic_expression;
    }
    if (!emit_callable_call_cases(buffer, ir, function, instruction, signature, instruction_id,
                                  result, error, panic))
        return false;

    char normal_expression[64];
    const char *normal_value = NULL;
    if (normal_implicit != 0u && signature->result_type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
        const char *field = outcome_field(signature->result_type_id);
        if (!field)
            return false;
        (void) snprintf(normal_expression, sizeof(normal_expression), "call_%u.%s", instruction_id,
                        field);
        normal_value = normal_expression;
    } else if (normal_implicit != 0u) {
        (void) snprintf(normal_expression, sizeof(normal_expression), "v%u",
                        normal->argument_ids[0]);
        normal_value = normal_expression;
    }
    uint32_t operand = signature->parameter_count + 1u;
    if (!append_format(buffer, "        if (call_%u.kind == 0) ", instruction_id) ||
        !emit_invoke_edge(buffer, function, instruction, 0u, normal_implicit, operand, function_id,
                          normal_value))
        return false;
    operand += normal->argument_count - normal_implicit;
    uint32_t successor = 1u;
    if (has_error) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_error_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 2) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
        operand += function->blocks[instruction->successors[successor]].argument_count - 1u;
        ++successor;
    }
    if (has_panic) {
        char expression[64];
        (void) snprintf(expression, sizeof(expression), "invoke_panic_%u", instruction_id);
        if (!append_format(buffer, "        if (call_%u.kind == 3) ", instruction_id) ||
            !emit_invoke_edge(buffer, function, instruction, successor, 1u, operand, function_id,
                              expression))
            return false;
    }
    return append_format(buffer, "        return call_%u;\n", instruction_id);
}

static bool emit_callable_copy(CBuffer *buffer, const XrBackendIR *ir,
                               const XrBackendInstruction *instruction) {
    uint16_t callable_type = instruction->result_type_id;
    uint32_t source_value = instruction->operands[0];
    uint32_t result_value = instruction->result_id;
    if (!append_format(buffer,
                       "        v%u = v%u;\n"
                       "        switch (v%u.function_id) {\n",
                       result_value, source_value, source_value))
        return false;
    for (uint32_t target_id = 0; target_id < ir->function_count; ++target_id) {
        if (!callable_type_can_target(ir, callable_type, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
        if (!append_format(buffer, "            case UINT32_C(%u):\n", target_id))
            return false;
        if (!target->has_receiver) {
            if (!append_format(buffer,
                               "                v%u.capture = NULL;\n"
                               "                break;\n",
                               result_value))
                return false;
            continue;
        }
        char storage[32];
        const char *capture_type = type_c_name(target->parameter_types[0], storage);
        if (!capture_type ||
            !append_format(buffer,
                           "                { %s *callable_capture_%u = "
                           "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n"
                           "                  if (!callable_capture_%u) return "
                           "xr_aot_make(4, 0, 0);\n"
                           "                  *callable_capture_%u = "
                           "*(const %s *)v%u.capture;\n"
                           "                  v%u.capture = (void *)callable_capture_%u; }\n"
                           "                break;\n",
                           capture_type, result_value, capture_type, capture_type, result_value,
                           result_value, capture_type, source_value, result_value, result_value))
            return false;
    }
    return append_text(buffer, "            default:\n"
                               "                return xr_aot_make(4, 0, 0);\n"
                               "        }\n");
}

static bool emit_coroutine_call(CBuffer *buffer, const XrBackendIR *ir,
                                const XrBackendFunction *function,
                                const XrBackendInstruction *instruction,
                                uint32_t function_id, uint32_t instruction_id) {
    uint32_t callee_id = instruction->immediate.coroutine_call.function_id;
    uint32_t safepoint_id = instruction->immediate.coroutine_call.safepoint_id;
    const XrBackendFunction *callee = &ir->functions[callee_id];
    const XrBackendCoroutineSafepoint *point =
        &function->coroutine_safepoints[safepoint_id];
    uint32_t implicit_result = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    if (!append_format(buffer,
                       "        if (!frame->child_active_%u) {\n"
                       "            XrAotCoroutineFrame%u child_zero_%u = {0};\n"
                       "            frame->child_%u = child_zero_%u;\n",
                       safepoint_id, callee_id, instruction_id, safepoint_id, instruction_id))
        return false;
    for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, "            frame->child_%u.parameter_%u = v%u;\n",
                           safepoint_id, parameter, instruction->operands[parameter]))
            return false;
    }
    if (!append_format(buffer,
                       "            frame->child_active_%u = UINT8_C(1);\n"
                       "        }\n"
                       "        XrAotOutcome child_%u = xr_aot_fn_%u_step(xr_ctx, "
                       "&frame->child_%u",
                       safepoint_id, instruction_id, callee_id, safepoint_id))
        return false;
    for (uint32_t parameter = 0u; parameter < callee->parameter_count; ++parameter) {
        if (!append_format(buffer, ", frame->child_%u.parameter_%u", safepoint_id, parameter))
            return false;
    }
    if (!append_text(buffer, ");\n"))
        return false;
    if (!append_format(buffer, "        if (child_%u.kind == UINT32_C(5)) {\n", instruction_id))
        return false;
    for (uint32_t live = 0u; live < point->live_value_count; ++live) {
        if (!append_format(buffer, "            frame->live_%u_%u = v%u;\n", safepoint_id,
                           live, instruction->operands[callee->parameter_count + live]))
            return false;
    }
    if (!append_format(buffer,
                       "            frame->state = UINT32_C(%u);\n"
                       "            return xr_aot_make(5, UINT32_C(%u), 0);\n"
                       "        }\n"
                       "        if (child_%u.kind != UINT32_C(0)) {\n"
                       "            frame->state = UINT32_MAX;\n"
                       "            return child_%u;\n"
                       "        }\n"
                       "        frame->child_active_%u = UINT8_C(0);\n",
                       point->resume_state_id, safepoint_id, instruction_id, instruction_id,
                       safepoint_id))
        return false;
    char result_expression[64];
    const char *result = NULL;
    if (implicit_result != 0u) {
        const char *field = outcome_field(callee->result_type_id);
        if (!field)
            return false;
        (void) snprintf(result_expression, sizeof(result_expression), "child_%u.%s",
                        instruction_id, field);
        result = result_expression;
    }
    return emit_invoke_edge(buffer, function, instruction, 0u, implicit_result,
                            callee->parameter_count, function_id, result);
}

static bool emit_instruction(CBuffer *buffer, const XrBackendIR *ir,
                             const XrBackendFunction *function,
                             const XrBackendInstruction *instruction, uint32_t function_id,
                             uint32_t instruction_id) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64: {
            const XrValidatedConstant *constant =
                &ir->constants[instruction->immediate.constant_id];
            if (!append_format(buffer, "        v%u = ", instruction->result_id) ||
                !emit_i64_literal(buffer, constant->value.i64))
                return false;
            return append_text(buffer, ";\n");
        }
        case XR_CORE_OP_CORE_CONSTANT_BOOL: {
            const XrValidatedConstant *constant =
                &ir->constants[instruction->immediate.constant_id];
            return append_format(buffer, "        v%u = UINT8_C(%u);\n", instruction->result_id,
                                 constant->value.boolean ? 1u : 0u);
        }
        case XR_CORE_OP_CORE_CONSTANT_TARGET_ENUM:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 instruction->immediate.u32);
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64: {
            const char *name = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64   ? "add"
                               : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64 ? "sub"
                                                                                      : "mul";
            const char *symbol = instruction->operation_id == XR_CORE_OP_CORE_ADD_I64   ? "+"
                                 : instruction->operation_id == XR_CORE_OP_CORE_SUB_I64 ? "-"
                                                                                        : "*";
            if (instruction->immediate.u32 == 0u)
                return append_format(buffer,
                                     "        if (!xr_aot_checked_%s(v%u, v%u, &v%u)) return "
                                     "xr_aot_make(1, 0, 1);\n",
                                     name, instruction->operands[0], instruction->operands[1],
                                     instruction->result_id);
            return append_format(buffer,
                                 "        v%u = xr_aot_i64_from_bits((uint64_t)v%u %s "
                                 "(uint64_t)v%u);\n",
                                 instruction->result_id, instruction->operands[0], symbol,
                                 instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_DIV_I64:
            return append_format(buffer,
                                 "        if (v%u == 0) return xr_aot_make(1, 0, 2);\n"
                                 "        if (v%u == INT64_MIN && v%u == -1) return "
                                 "xr_aot_make(1, 0, 3);\n"
                                 "        v%u = v%u / v%u;\n",
                                 instruction->operands[1], instruction->operands[0],
                                 instruction->operands[1], instruction->result_id,
                                 instruction->operands[0], instruction->operands[1]);
        case XR_CORE_OP_CORE_LOGICAL_NOT:
            return append_format(buffer, "        v%u = (uint8_t)!v%u;\n",
                                 instruction->result_id, instruction->operands[0]);
        case XR_CORE_OP_CORE_LOGICAL_AND:
        case XR_CORE_OP_CORE_LOGICAL_OR:
            return append_format(
                buffer, "        v%u = (uint8_t)(v%u %s v%u);\n", instruction->result_id,
                instruction->operands[0],
                instruction->operation_id == XR_CORE_OP_CORE_LOGICAL_AND ? "&&" : "||",
                instruction->operands[1]);
        case XR_CORE_OP_CORE_COMPARE_I64: {
            static const char *operators[] = {"==", "!=", "<", "<=", ">", ">="};
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 operators[instruction->immediate.u32], instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_COMPARE_TARGET_ENUM: {
            static const char *operators[] = {"==", "!="};
            return append_format(buffer, "        v%u = (uint8_t)(v%u %s v%u);\n",
                                 instruction->result_id, instruction->operands[0],
                                 operators[instruction->immediate.u32], instruction->operands[1]);
        }
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT:
            return true;
        case XR_CORE_OP_CORE_BRANCH:
            return emit_parallel_edge(buffer, function, instruction, 0u, 0u, function_id);
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
            uint32_t true_count = function->blocks[instruction->successors[0]].argument_count;
            if (!append_format(buffer, "        if (v%u) ", instruction->operands[0]) ||
                !emit_parallel_edge(buffer, function, instruction, 0u, 1u, function_id) ||
                !append_text(buffer, "        else "))
                return false;
            return emit_parallel_edge(buffer, function, instruction, 1u, 1u + true_count,
                                      function_id);
        }
        case XR_CORE_OP_CORE_RETURN:
            if (function->coroutine_safepoint_count != 0u &&
                !append_text(buffer, "        frame->state = UINT32_MAX;\n"))
                return false;
            return emit_return(buffer, function, instruction);
        case XR_CORE_OP_CORE_COROUTINE_YIELD: {
            uint32_t safepoint_id = instruction->immediate.u32;
            const XrBackendCoroutineSafepoint *safepoint =
                &function->coroutine_safepoints[safepoint_id];
            if (instruction->operand_count != safepoint->live_value_count)
                return false;
            for (uint32_t live = 0; live < instruction->operand_count; ++live) {
                if (!append_format(buffer, "        frame->live_%u_%u = v%u;\n", safepoint_id, live,
                                   instruction->operands[live]))
                    return false;
            }
            return append_format(buffer,
                                 "        frame->state = UINT32_C(%u);\n"
                                 "        return xr_aot_make(5, UINT32_C(%u), 0);\n",
                                 safepoint->resume_state_id, safepoint_id);
        }
        case XR_CORE_OP_CORE_COROUTINE_CALL_SEALED:
            return emit_coroutine_call(buffer, ir, function, instruction, function_id,
                                       instruction_id);
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT:
            return emit_call(buffer, ir, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT:
            return emit_callable_call(buffer, ir, function, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE:
            return emit_invoke(buffer, ir, function, instruction, function_id, instruction_id);
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
            return emit_callable_invoke(buffer, ir, function, instruction, function_id,
                                        instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT:
            return emit_witness_call(buffer, ir, function, instruction, instruction_id);
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            return emit_witness_invoke(buffer, ir, function, instruction, function_id,
                                       instruction_id);
        case XR_CORE_OP_CORE_TRAP:
            return append_text(buffer, "        return xr_aot_make(1, 0, 4);\n");
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            return append_format(buffer,
                                 "        *out_error = v%u;\n"
                                 "        return xr_aot_make(2, 0, 0);\n",
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            return append_format(buffer,
                                 "        *out_panic = v%u;\n"
                                 "        return xr_aot_make(3, 0, 0);\n",
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->pointer_width);
        case XR_CORE_OP_CORE_TARGET_OPERATING_SYSTEM:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->operating_system);
        case XR_CORE_OP_CORE_TARGET_ARCHITECTURE:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->architecture);
        case XR_CORE_OP_CORE_TARGET_NATIVE_ABI:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->native_abi);
        case XR_CORE_OP_CORE_TARGET_ENDIANNESS:
            return append_format(buffer, "        v%u = UINT16_C(%u);\n", instruction->result_id,
                                 ir->endianness);
        case XR_CORE_OP_CORE_PROVIDER_CALL: {
            uint16_t operand_type = instruction->operand_count == 1u
                                        ? function->value_types[instruction->operands[0]]
                                        : XR_CORE_TYPE_VOID;
            XrProviderLogicalCallKind call_kind = xr_validated_program_provider_call_kind(
                ir->program, instruction->result_type_id,
                instruction->operand_count == 1u ? &operand_type : NULL,
                instruction->operand_count);
            if (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_UNARY)
                return append_format(
                    buffer,
                    "        if (!xr_ctx->provider_call_i64_unary || "
                    "xr_ctx->provider_call_i64_unary(xr_ctx->provider_context, UINT32_C(%u), "
                    "UINT32_C(%u), v%u, &v%u) != 0) return xr_aot_make(1, 0, 7);\n",
                    instruction->immediate.provider_operation.requirement_index,
                    instruction->immediate.provider_operation.operation_index,
                    instruction->operands[0], instruction->result_id);
            if (call_kind == XR_PROVIDER_LOGICAL_CALL_I64_NULLARY)
                return append_format(
                    buffer,
                    "        if (!xr_ctx->provider_call_i64_nullary || "
                    "xr_ctx->provider_call_i64_nullary(xr_ctx->provider_context, UINT32_C(%u), "
                    "UINT32_C(%u), &v%u) != 0) return xr_aot_make(1, 0, 7);\n",
                    instruction->immediate.provider_operation.requirement_index,
                    instruction->immediate.provider_operation.operation_index,
                    instruction->result_id);
            if (call_kind == XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY)
                return append_format(
                    buffer,
                    "        {\n"
                    "            uint8_t xr_result = UINT8_C(0);\n"
                    "            if (!xr_ctx->provider_call_bool_i64_unary || "
                    "xr_ctx->provider_call_bool_i64_unary(xr_ctx->provider_context, "
                    "UINT32_C(%u), UINT32_C(%u), v%u, &xr_result) != 0) "
                    "return xr_aot_make(1, 0, 7);\n"
                    "            v%u = xr_result != UINT8_C(0);\n"
                    "        }\n",
                    instruction->immediate.provider_operation.requirement_index,
                    instruction->immediate.provider_operation.operation_index,
                    instruction->operands[0], instruction->result_id);
            if (call_kind == XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY) {
                uint16_t pair_type_id = XR_CORE_TYPE_VOID;
                if (!xr_validated_program_type_is_optional_i64_pair(
                        ir->program, instruction->result_type_id, &pair_type_id))
                    return false;
                return append_format(
                    buffer,
                    "        {\n"
                    "            uint8_t xr_present = UINT8_C(0);\n"
                    "            int64_t xr_first = INT64_C(0);\n"
                    "            int64_t xr_second = INT64_C(0);\n"
                    "            if (!xr_ctx->provider_call_optional_i64_pair_nullary || "
                    "xr_ctx->provider_call_optional_i64_pair_nullary(xr_ctx->provider_context, "
                    "UINT32_C(%u), UINT32_C(%u), &xr_present, &xr_first, &xr_second) != 0) "
                    "return xr_aot_make(1, 0, 7);\n"
                    "            if (xr_present) v%u = (XrAotType%u){.tag = UINT32_C(1), "
                    ".payload.case_1.f0 = (XrAotType%u){.f0 = xr_first, .f1 = xr_second}};\n"
                    "            else v%u = (XrAotType%u){.tag = UINT32_C(0), "
                    ".payload.case_0.empty = UINT8_C(0)};\n"
                    "        }\n",
                    instruction->immediate.provider_operation.requirement_index,
                    instruction->immediate.provider_operation.operation_index,
                    instruction->result_id, instruction->result_type_id, pair_type_id,
                    instruction->result_id, instruction->result_type_id);
            }
            return false;
        }
        case XR_CORE_OP_CORE_OUTPUT_GROUP_I64:
            return append_format(
                buffer,
                "        {\n"
                "            uint8_t xr_output[22];\n"
                "            size_t xr_output_size = xr_aot_format_i64_line(v%u, xr_output);\n"
                "            if (!xr_ctx->provider_output_write || "
                "xr_ctx->provider_output_write(xr_ctx->provider_context, UINT32_C(%u), "
                "UINT32_C(%u), xr_output, xr_output_size) != 0) "
                "return xr_aot_make(1, 0, 7);\n"
                "        }\n",
                instruction->operands[0],
                instruction->immediate.provider_operation.requirement_index,
                instruction->immediate.provider_operation.operation_index);
        case XR_CORE_OP_CORE_CALLABLE_PACK: {
            uint32_t target_id = instruction->immediate.function_id;
            if (instruction->operand_count == 0u)
                return append_format(buffer,
                                     "        v%u = (XrAotType%u){.function_id = "
                                     "UINT32_C(%u), .capture = NULL};\n",
                                     instruction->result_id, instruction->result_type_id,
                                     target_id);
            uint16_t capture_type = function->value_types[instruction->operands[0]];
            char storage[32];
            const char *name = type_c_name(capture_type, storage);
            if (!name ||
                !append_format(buffer,
                               "        %s *callable_capture_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(buffer,
                               "        if (!callable_capture_%u) return "
                               "xr_aot_make(4, 0, 0);\n"
                               "        *callable_capture_%u = v%u;\n",
                               instruction->result_id, instruction->result_id,
                               instruction->operands[0]))
                return false;
            return append_format(buffer,
                                 "        v%u = (XrAotType%u){.function_id = UINT32_C(%u), "
                                 ".capture = (void *)callable_capture_%u};\n",
                                 instruction->result_id, instruction->result_type_id, target_id,
                                 instruction->result_id);
        }
        case XR_CORE_OP_CORE_OWNER_COPY: {
            const XrValidatedType *type =
                xr_validated_program_type(ir->program, instruction->result_type_id);
            if (type && type->kind == XR_CORE_IR_TYPE_CALLABLE)
                return emit_callable_copy(buffer, ir, instruction);
            return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        }
        case XR_CORE_OP_CORE_OWNER_MOVE:
            return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_OWNER_DROP:
            return append_format(buffer, "        (void)v%u;\n", instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            return append_format(buffer,
                                 "        xr_place_%u = v%u;\n        v%u = &xr_place_%u;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->result_id, instruction->result_id);
        case XR_CORE_OP_CORE_PLACE_LOAD:
            return append_format(buffer, "        v%u = *v%u;\n", instruction->result_id,
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_PLACE_STORE:
            return append_format(buffer, "        *v%u = v%u;\n", instruction->operands[0],
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_PLACE_PROJECT:
            return append_format(buffer, "        v%u = &v%u->f%u;\n", instruction->result_id,
                                 instruction->operands[0], instruction->immediate.field_ordinal);
        case XR_CORE_OP_CORE_PLACE_TAKE:
            return append_format(buffer, "        v%u = *v%u;\n        v%u = NULL;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->operands[0]);
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
            char storage[32];
            const char *name = type_c_name(instruction->result_type_id, storage);
            if (!name ||
                !append_format(buffer, "        v%u = (%s){", instruction->result_id, name))
                return false;
            if (instruction->operand_count == 0u && !append_text(buffer, ".xr_unit = UINT8_C(0)"))
                return false;
            for (uint32_t field = 0; field < instruction->operand_count; ++field) {
                if (!append_format(buffer, "%s.f%u = v%u", field ? ", " : "", field,
                                   instruction->operands[field]))
                    return false;
            }
            return append_text(buffer, "};\n");
        }
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT:
            return append_format(buffer, "        v%u = v%u.f%u;\n", instruction->result_id,
                                 instruction->operands[0], instruction->immediate.field_ordinal);
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE:
            return append_format(buffer, "        v%u = v%u;\n        v%u.f%u = v%u;\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->result_id, instruction->immediate.field_ordinal,
                                 instruction->operands[1]);
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
            uint32_t variant = instruction->immediate.variant_ordinal;
            if (!append_format(buffer, "        v%u.tag = UINT32_C(%u);\n", instruction->result_id,
                               variant))
                return false;
            for (uint32_t field = 0; field < instruction->operand_count; ++field) {
                if (!append_format(buffer, "        v%u.payload.case_%u.f%u = v%u;\n",
                                   instruction->result_id, variant, field,
                                   instruction->operands[field]))
                    return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_VARIANT_TEST:
            return append_format(buffer, "        v%u = (uint8_t)(v%u.tag == UINT32_C(%u));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->immediate.variant_ordinal);
        case XR_CORE_OP_CORE_VARIANT_PROJECT:
            return append_format(
                buffer,
                "        if (v%u.tag != UINT32_C(%u)) return xr_aot_make(1, 0, 6);\n"
                "        v%u = v%u.payload.case_%u.f%u;\n",
                instruction->operands[0], instruction->immediate.variant_field.variant_ordinal,
                instruction->result_id, instruction->operands[0],
                instruction->immediate.variant_field.variant_ordinal,
                instruction->immediate.variant_field.field_ordinal);
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
            uint16_t concrete_type = function->value_types[instruction->operands[0]];
            const XrValidatedType *existential =
                xr_validated_program_type(ir->program, instruction->result_type_id);
            uint32_t conformance = 0u;
            if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                !conformance_id(ir->program, concrete_type, existential->interface_id,
                                &conformance))
                return false;
            if (function->value_categories[instruction->operands[0]] == XR_CORE_IR_PLACE)
                return append_format(
                    buffer,
                    "        v%u = (XrAotType%u){.concrete_type_id = UINT16_C(%u), "
                    ".conformance_id = UINT32_C(%u), .data = (void *)v%u};\n",
                    instruction->result_id, instruction->result_type_id, concrete_type, conformance,
                    instruction->operands[0]);
            char storage[32];
            const char *name = type_c_name(concrete_type, storage);
            if (!name ||
                !append_format(buffer,
                               "        %s *existential_payload_%u = "
                               "(%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n",
                               name, instruction->result_id, name, name) ||
                !append_format(buffer,
                               "        if (!existential_payload_%u) return "
                               "xr_aot_make(4, 0, 0);\n",
                               instruction->result_id) ||
                !append_format(buffer, "        *existential_payload_%u = v%u;\n",
                               instruction->result_id, instruction->operands[0]))
                return false;
            return append_format(buffer,
                                 "        v%u = (XrAotType%u){.concrete_type_id = UINT16_C(%u), "
                                 ".conformance_id = UINT32_C(%u), "
                                 ".data = (void *)existential_payload_%u};\n",
                                 instruction->result_id, instruction->result_type_id, concrete_type,
                                 conformance, instruction->result_id);
        }
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST:
            return append_format(buffer,
                                 "        v%u = (uint8_t)(v%u.concrete_type_id == "
                                 "UINT16_C(%u));\n",
                                 instruction->result_id, instruction->operands[0],
                                 instruction->immediate.type_id);
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
            char storage[32];
            const char *name = type_c_name(instruction->immediate.type_id, storage);
            if (!name)
                return false;
            if (instruction->result_category == XR_CORE_IR_PLACE)
                return append_format(buffer, "        v%u = (%s *)v%u.data;\n",
                                     instruction->result_id, name, instruction->operands[0]);
            return append_format(buffer, "        v%u = *(const %s *)v%u.data;\n",
                                 instruction->result_id, name, instruction->operands[0]);
        }
        default:
            return false;
    }
}

static bool emit_function(CBuffer *buffer, const XrBackendIR *ir, uint32_t function_id) {
    const XrBackendFunction *function = &ir->functions[function_id];
    if (!emit_function_signature(buffer, function, ir, function_id, false))
        return false;
    if (!append_text(buffer, "    (void)xr_ctx;\n"))
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    (void)p%u;\n", parameter))
            return false;
    }
    if (function->error_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    (void)out_error;\n"))
        return false;
    if (function->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    (void)out_panic;\n"))
        return false;
    for (uint32_t value = 0; value < function->value_count; ++value) {
        char storage[32];
        const char *type = type_c_name(function->value_types[value], storage);
        const char *initializer =
            function->value_types[value] >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        const char *pointer = function->value_categories[value] == XR_CORE_IR_PLACE ? " *" : "";
        const char *value_initializer =
            function->value_categories[value] == XR_CORE_IR_PLACE ? "0" : initializer;
        if (!type ||
            !append_format(buffer, "    %s%s v%u = %s;\n", type, pointer, value,
                           value_initializer) ||
            !append_format(buffer, "    (void)v%u;\n", value))
            return false;
        if (function->value_categories[value] == XR_CORE_IR_PLACE &&
            (!append_format(buffer, "    %s xr_place_%u = %s;\n", type, value, initializer) ||
             !append_format(buffer, "    (void)xr_place_%u;\n", value)))
            return false;
    }
    const XrBackendBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count)
        return false;
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        if (!append_format(buffer, "    v%u = p%u;\n", entry->argument_ids[parameter], parameter))
            return false;
    }
    if (function->coroutine_safepoint_count != 0u) {
        if (!append_text(buffer, "    if (!frame) return xr_aot_make(4, 0, 0);\n"
                                 "    switch (frame->state) {\n") ||
            !append_format(buffer, "        case UINT32_C(0): goto xr_f%u_b%u;\n", function_id,
                           function->entry_block))
            return false;
        for (uint32_t state = 1u; state < function->coroutine_state_count; ++state) {
            uint32_t resume_block = function->coroutine_states[state].continuation_block;
            const XrBackendCoroutineSafepoint *point = NULL;
            uint32_t safepoint_id = 0u;
            for (; safepoint_id < function->coroutine_safepoint_count; ++safepoint_id) {
                if (function->coroutine_safepoints[safepoint_id].resume_state_id == state) {
                    point = &function->coroutine_safepoints[safepoint_id];
                    break;
                }
            }
            const XrBackendInstruction *suspension =
                point ? coroutine_suspension_instruction(function, safepoint_id, NULL) : NULL;
            if (!point || !suspension ||
                !append_format(buffer, "        case UINT32_C(%u):\n", state))
                return false;
            for (uint32_t live = 0; live < point->live_value_count; ++live) {
                uint32_t target = suspension->operation_id == XR_CORE_OP_CORE_COROUTINE_YIELD
                                      ? function->blocks[resume_block].argument_ids[live]
                                      : suspension->operands[
                                            ir->functions[suspension->immediate.coroutine_call
                                                              .function_id]
                                                    .parameter_count +
                                                live];
                if (!append_format(buffer, "            v%u = frame->live_%u_%u;\n", target,
                                   safepoint_id, live))
                    return false;
            }
            if (!append_format(buffer, "            goto xr_f%u_b%u;\n", function_id, resume_block))
                return false;
        }
        if (!append_text(buffer, "        default: return xr_aot_make(4, 0, 0);\n    }\n"))
            return false;
    } else if (!append_format(buffer, "    goto xr_f%u_b%u;\n", function_id,
                              function->entry_block)) {
        return false;
    }
    uint32_t instruction_serial = 0u;
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        const XrBackendBlock *block = &function->blocks[block_id];
        if (!append_format(buffer, "xr_f%u_b%u:\n    ;\n", function_id, block_id))
            return false;
        for (uint32_t instruction = 0; instruction < block->instruction_count;
             ++instruction, ++instruction_serial) {
            if (!emit_instruction(buffer, ir, function, &block->instructions[instruction],
                                  function_id, instruction_serial))
                return false;
        }
    }
    return append_text(buffer, "}\n\n");
}

static bool emit_coroutine_entry_adapter(CBuffer *buffer, const XrBackendIR *ir) {
    const XrBackendFunction *entry = &ir->functions[ir->entry_function];
    if (entry->coroutine_safepoint_count == 0u)
        return true;
    if (entry->parameter_count != 0u ||
        (entry->result_type_id != XR_CORE_TYPE_VOID &&
         entry->result_type_id != XR_CORE_TYPE_I64) ||
        entry->error_type_id != XR_CORE_TYPE_VOID || entry->panic_type_id != XR_CORE_TYPE_VOID)
        return false;
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    bool output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    if (!append_format(buffer,
                       "typedef struct XrAotEntryCoroutineFrame {\n"
                       "    XrAotContext context;\n"
                       "    XrAotCoroutineFrame%u function;\n"
                       "} XrAotEntryCoroutineFrame;\n\n"
                       "typedef struct XrBackendNativeOutcome {\n"
                       "    uint32_t kind;\n"
                       "    int64_t value;\n"
                       "    uint32_t state_id;\n"
                       "    uint32_t safepoint_id;\n"
                       "} XrBackendNativeOutcome;\n\n"
                       "typedef struct XrBackendNativeExecutionId {\n"
                       "    uint8_t bytes[32];\n"
                       "} XrBackendNativeExecutionId;\n\n"
                       "typedef void (*XrBackendNativeInitialize)(void *frame);\n"
                       "typedef XrBackendNativeOutcome (*XrBackendNativeStep)(void *frame);\n"
                       "typedef void (*XrBackendNativeDrop)(void *frame);\n\n"
                       "typedef struct XrBackendNativeDescriptor {\n"
                       "    uint32_t schema_version;\n"
                       "    uint32_t reserved32;\n"
                       "    XrBackendNativeExecutionId execution_id;\n"
                       "    size_t frame_size;\n"
                       "    XrBackendNativeInitialize initialize;\n"
                       "    XrBackendNativeStep step;\n"
                       "    XrBackendNativeDrop drop;\n"
                       "} XrBackendNativeDescriptor;\n\n"
                       "size_t xr_aot_entry_coroutine_frame_size(void) {\n"
                       "    return sizeof(XrAotEntryCoroutineFrame);\n"
                       "}\n\n"
                       "void xr_aot_entry_coroutine_frame_initialize(void *opaque) {\n"
                       "    if (!opaque) return;\n"
                       "    XrAotEntryCoroutineFrame zero = {0};\n"
                       "    *(XrAotEntryCoroutineFrame *)opaque = zero;\n"
                       "}\n\n",
                       ir->entry_function))
        return false;
    if (!append_text(buffer, "void xr_aot_entry_coroutine_frame_dispose(void *opaque) {\n"
                             "    if (!opaque) return;\n") ||
        (arena && !append_text(buffer, "    xr_aot_context_destroy("
                                       "&((XrAotEntryCoroutineFrame *)opaque)->context);\n")) ||
        !append_text(buffer,
                     "    ((XrAotEntryCoroutineFrame *)opaque)->function.state = UINT32_MAX;\n"
                     "}\n\n") ||
        !append_format(buffer,
                       "XrBackendNativeOutcome xr_aot_entry_coroutine_step(void *opaque) {\n"
                       "    XrBackendNativeOutcome invalid = {UINT32_C(3), 0, 0, 0};\n"
                       "    if (!opaque) return invalid;\n"
                       "    XrAotEntryCoroutineFrame *frame = "
                       "(XrAotEntryCoroutineFrame *)opaque;\n"
                       "    XrAotOutcome native = xr_aot_fn_%u_step(&frame->context, "
                       "&frame->function);\n"
                       "    if (native.kind == UINT32_C(0)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(0), %s, "
                       "frame->function.state, 0};\n"
                       "        return result;\n"
                       "    }\n"
                       "    if (native.kind == UINT32_C(5)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(1), 0, "
                       "frame->function.state, native.value_kind};\n"
                       "        return result;\n"
                       "    }\n"
                       "    if (native.kind == UINT32_C(1)) {\n"
                       "        XrBackendNativeOutcome result = {UINT32_C(2), 0, "
                       "frame->function.state, native.trap};\n"
                       "        return result;\n"
                       "    }\n"
                       "    return invalid;\n"
                       "}\n\n",
                       ir->entry_function,
                       entry->result_type_id == XR_CORE_TYPE_I64 ? "native.i64" : "0"))
        return false;
    if (!append_text(buffer, "const XrBackendNativeDescriptor "
                             "xr_aot_entry_coroutine_descriptor = {\n"
                             "    UINT32_C(1),\n"
                             "    UINT32_C(0),\n"
                             "    {{"))
        return false;
    for (size_t index = 0u; index < sizeof(ir->execution_id.bytes); ++index) {
        if (!append_format(buffer, "%sUINT8_C(%u)", index == 0u ? "" : ", ",
                           (unsigned) ir->execution_id.bytes[index]))
            return false;
    }
    return append_text(buffer, "}},\n"
                               "    sizeof(XrAotEntryCoroutineFrame),\n"
                               "    xr_aot_entry_coroutine_frame_initialize,\n"
                               "    xr_aot_entry_coroutine_step,\n"
                               "    xr_aot_entry_coroutine_frame_dispose,\n"
                               "};\n\n");
}

static bool emit_main(CBuffer *buffer, const XrBackendIR *ir) {
    const XrBackendFunction *entry = &ir->functions[ir->entry_function];
    bool checked = false;
    bool wrapping = false;
    bool arena = false;
    bool output = false;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    XrAotHostedClockBindings clock = hosted_clock_bindings(ir);
    XrAotHostedPipeBindings pipe = hosted_pipe_bindings(ir);
    if (entry->parameter_count != 0u)
        return false;
    if (!append_text(buffer, "int main(void) {\n    XrAotContext xr_ctx = {0};\n"))
        return false;
    if (output &&
        !append_text(buffer,
                     "    xr_ctx.provider_output_write = xr_aot_host_output_write;\n"))
        return false;
    if ((clock.realtime || clock.monotonic || clock.process_cpu) &&
        !append_text(buffer,
                     "    xr_ctx.provider_call_i64_nullary = "
                     "xr_aot_host_clock_nullary;\n"))
        return false;
    if (clock.utc_offset &&
        !append_text(buffer,
                     "    xr_ctx.provider_call_i64_unary = xr_aot_host_clock_unary;\n"))
        return false;
    if (pipe.open && !append_text(buffer, "    xr_ctx.provider_call_optional_i64_pair_nullary = "
                                          "xr_aot_host_pipe_open;\n"))
        return false;
    if (pipe.close &&
        !append_text(buffer,
                     "    xr_ctx.provider_call_bool_i64_unary = xr_aot_host_pipe_close;\n"))
        return false;
    if (entry->error_type_id != XR_CORE_TYPE_VOID) {
        char storage[32];
        const char *type = type_c_name(entry->error_type_id, storage);
        const char *initializer =
            entry->error_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ? "{0}" : "0";
        if (!type || !append_format(buffer, "    %s entry_error = %s;\n", type, initializer))
            return false;
    }
    if (entry->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, "    uint32_t entry_panic = 0;\n"))
        return false;
    if (entry->coroutine_safepoint_count != 0u) {
        if (entry->error_type_id != XR_CORE_TYPE_VOID ||
            entry->panic_type_id != XR_CORE_TYPE_VOID ||
            !append_format(buffer,
                           "    XrAotCoroutineFrame%u frame = {0};\n"
                           "    XrAotOutcome result;\n"
                           "    do { result = xr_aot_fn_%u_step(&xr_ctx, &frame); } "
                           "while (result.kind == UINT32_C(5));\n",
                           ir->entry_function, ir->entry_function))
            return false;
    } else if (!append_format(buffer, "    XrAotOutcome result = xr_aot_fn_%u(&xr_ctx",
                              ir->entry_function)) {
        return false;
    }
    if (entry->coroutine_safepoint_count == 0u && entry->error_type_id != XR_CORE_TYPE_VOID) {
        if (!append_text(buffer, ", &entry_error"))
            return false;
    }
    if (entry->coroutine_safepoint_count == 0u && entry->panic_type_id != XR_CORE_TYPE_VOID &&
        !append_text(buffer, ", &entry_panic"))
        return false;
    if (entry->coroutine_safepoint_count == 0u && !append_text(buffer, ");\n"))
        return false;
    if (!append_text(buffer, "    int exit_code = (int)(200u + result.kind * 10u + result.trap);\n"
                             "    if (result.kind == 0) {\n"))
        return false;
    switch (entry->result_type_id) {
        case XR_CORE_TYPE_VOID:
            if (!append_text(buffer, "        exit_code = 0;\n"))
                return false;
            break;
        case XR_CORE_TYPE_BOOL:
            if (!append_text(buffer, "        exit_code = result.boolean ? 1 : 0;\n"))
                return false;
            break;
        case XR_CORE_TYPE_I64:
            if (!append_text(buffer,
                             "        exit_code = (int)((uint64_t)result.i64 & UINT64_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U32:
            if (!append_text(buffer, "        exit_code = (int)(result.u32 & UINT32_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_U16:
        case XR_CORE_TYPE_TARGET_OS:
        case XR_CORE_TYPE_TARGET_ARCH:
        case XR_CORE_TYPE_TARGET_ABI:
        case XR_CORE_TYPE_TARGET_ENDIAN:
            if (!append_text(buffer, "        exit_code = (int)(result.u16 & UINT16_C(255));\n"))
                return false;
            break;
        case XR_CORE_TYPE_ERROR:
            if (!append_text(buffer, "        exit_code = (int)(result.error & UINT32_C(255));\n"))
                return false;
            break;
        default:
            return false;
    }
    if (!append_text(buffer, "    }\n"))
        return false;
    if (arena && !append_text(buffer, "    xr_aot_context_destroy(&xr_ctx);\n"))
        return false;
    return append_text(buffer, "    return exit_code;\n}\n");
}

XrBackendStatus xr_backend_ir_emit_c(const XrBackendIR *ir, bool standalone_main,
                                     XrGeneratedC *generated_out,
                                     XrBackendDiagnostic *diagnostic_out) {
    if (generated_out)
        memset(generated_out, 0, sizeof(*generated_out));
    xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_OK, 0u, 0u, 0u, 0u);
    if (!ir || !ir->verified || !generated_out || !xr_backend_ir_verify(ir, diagnostic_out) ||
        !xr_backend_ir_translation_validate(ir, diagnostic_out)) {
        if (!diagnostic_out || diagnostic_out->status == XR_BACKEND_OK)
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_INVALID_INPUT, 0u, 0u, 0u, 0u);
        return diagnostic_out ? diagnostic_out->status : XR_BACKEND_INVALID_INPUT;
    }
    CBuffer buffer = {0};
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
    if (standalone_main && machine &&
        machine->runtime_profile != XR_TARGET_RUNTIME_PROFILE_HOSTED) {
        bool checked = false;
        bool wrapping = false;
        bool arena = false;
        bool output = false;
        scan_helpers(ir, &checked, &wrapping, &arena, &output);
        if (output) {
            xr_backend_set_diagnostic(diagnostic_out, XR_BACKEND_EMISSION_REJECTED, 0u, 0u, 0u,
                                      0u);
            return XR_BACKEND_EMISSION_REJECTED;
        }
    }
    bool emitted = emit_prelude(&buffer, ir, standalone_main);
    emitted = emitted && emit_coroutine_frames(&buffer, ir);
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function_signature(&buffer, &ir->functions[function], ir, function, true);
    emitted = emitted && append_text(&buffer, "\n");
    for (uint32_t function = 0; emitted && function < ir->function_count; ++function)
        emitted = emit_function(&buffer, ir, function);
    emitted = emitted && emit_coroutine_entry_adapter(&buffer, ir);
    if (emitted && standalone_main)
        emitted = emit_main(&buffer, ir);
    XiCgenVerifyResult verify = {0};
    if (!emitted || buffer.failed || !xi_cgen_verify_output(buffer.bytes, buffer.size, &verify)) {
        xr_free(buffer.bytes);
        xr_backend_set_diagnostic(
            diagnostic_out, buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED,
            0u, 0u, 0u, 0u);
        return buffer.failed ? XR_BACKEND_OUT_OF_MEMORY : XR_BACKEND_EMISSION_REJECTED;
    }
    generated_out->bytes = buffer.bytes;
    generated_out->size = buffer.size;
    generated_out->execution_id = ir->execution_id;
    generated_out->backend_id = ir->backend_id;
    generated_out->optimization_policy_id = ir->optimization_policy_id;
    generated_out->target_profile_id = xr_target_profile_fingerprint(ir->profile);
    xr_semantic_fingerprint((const uint8_t *) buffer.bytes, buffer.size,
                            &generated_out->source_digest);
    return XR_BACKEND_OK;
}

void xr_generated_c_free(XrGeneratedC *generated) {
    if (!generated)
        return;
    xr_free(generated->bytes);
    memset(generated, 0, sizeof(*generated));
}
