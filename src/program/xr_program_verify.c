/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_verify.c - Bounded semantic admission for canonical XrProgram
 */

#include "xr_program_verify.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "xr_validated_program_internal.h"

#include <string.h>

typedef struct VerifyReader {
    const uint8_t *bytes;
    size_t size;
    size_t offset;
    bool valid;
} VerifyReader;

typedef struct VerifyContext {
    XrProgramVerifyBudget budget;
    uint64_t work;
    XrProgramDiagnostic diagnostic;
    XrValidatedProgram *program;
} VerifyContext;

static XrProgramSemanticLocation no_location(void) {
    XrProgramSemanticLocation location = {
        .section_id = 0,
        .function_id = XR_PROGRAM_LOCATION_NONE,
        .block_id = XR_PROGRAM_LOCATION_NONE,
        .instruction_id = XR_PROGRAM_LOCATION_NONE,
        .value_id = XR_PROGRAM_LOCATION_NONE,
    };
    return location;
}

static void reject(VerifyContext *context, XrProgramDiagnosticKind kind,
                   XrProgramSemanticLocation location) {
    if (context->diagnostic.kind != XR_PROGRAM_DIAGNOSTIC_NONE)
        return;
    context->diagnostic.kind = kind;
    context->diagnostic.location = location;
}

static bool spend(VerifyContext *context, uint64_t amount, XrProgramSemanticLocation location) {
    if (amount > context->budget.max_work || context->work > context->budget.max_work - amount) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->work += amount;
    return true;
}

static uint64_t take_uvar(VerifyReader *reader) {
    uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned count = 0; count < 10u; ++count) {
        if (!reader->valid || reader->offset >= reader->size) {
            reader->valid = false;
            return 0;
        }
        uint8_t byte = reader->bytes[reader->offset++];
        value |= (uint64_t) (byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0)
            return value;
        shift += 7u;
    }
    reader->valid = false;
    return 0;
}

static bool take_bytes(VerifyReader *reader, void *output, size_t size) {
    if (!reader->valid || reader->offset > reader->size || size > reader->size - reader->offset) {
        reader->valid = false;
        return false;
    }
    if (output)
        memcpy(output, reader->bytes + reader->offset, size);
    reader->offset += size;
    return true;
}

static int64_t take_svar(VerifyReader *reader) {
    uint64_t value = take_uvar(reader);
    if ((value & 1u) == 0u)
        return (int64_t) (value >> 1u);
    return -(int64_t) (value >> 1u) - 1;
}

static VerifyReader section_reader(const XrProgramView *view, uint32_t section_index) {
    XrProgramSectionView section = view->sections[section_index];
    VerifyReader reader = {
        .bytes = view->artifact + section.offset,
        .size = (size_t) section.size,
        .valid = true,
    };
    return reader;
}

static bool reader_done(const VerifyReader *reader) {
    return reader->valid && reader->offset == reader->size;
}

static bool type_is_runtime(const XrValidatedProgram *program, uint64_t type_id) {
    return type_id <= XR_CORE_TYPE_PANIC_INFO ||
           (program && type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE < program->type_count);
}

static bool type_is_view(const XrValidatedProgram *program, uint16_t type_id) {
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type && type->kind == XR_CORE_IR_TYPE_VIEW;
}

static bool type_is_existential_ref(const XrValidatedProgram *program, uint16_t type_id) {
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type && type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
           type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF;
}

static bool existential_receiver_mode_supported(const XrValidatedType *type, XrParamMode mode) {
    if (!type || type->kind != XR_CORE_IR_TYPE_EXISTENTIAL)
        return false;
    switch (type->interface_use_kind) {
        case XR_CORE_IR_INTERFACE_EXISTENTIAL_READ:
            return mode == XR_PARAM_READ;
        case XR_CORE_IR_INTERFACE_EXISTENTIAL_REF:
            return mode == XR_PARAM_READ || mode == XR_PARAM_REF;
        case XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE:
            return mode == XR_PARAM_READ || mode == XR_PARAM_MOVE;
        case XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE:
            return true;
        default:
            return false;
    }
}

static const XrValidatedConformance *find_conformance(const XrValidatedProgram *program,
                                                      uint16_t implementor_type_id,
                                                      uint32_t interface_id) {
    if (!program)
        return NULL;
    for (uint32_t index = 0; index < program->conformance_count; ++index) {
        const XrValidatedConformance *row = &program->conformances[index];
        if (row->implementor_type_id == implementor_type_id && row->interface_id == interface_id)
            return row;
    }
    return NULL;
}

static bool type_is_nominal(const XrValidatedProgram *program, uint16_t type_id) {
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type &&
           (type->kind == XR_CORE_IR_TYPE_AGGREGATE || type->kind == XR_CORE_IR_TYPE_VARIANT) &&
           type->nominal_kind != XR_CORE_IR_NOMINAL_NONE;
}

static bool parse_current_core_spec_fingerprint(uint8_t output[XR_PROGRAM_DIGEST_SIZE]) {
    const char *hex = XR_CORE_SPEC_SEMANTIC_SHA256;
    for (size_t index = 0; index < XR_PROGRAM_DIGEST_SIZE; ++index) {
        unsigned high = (unsigned) (hex[index * 2u] >= 'a' ? hex[index * 2u] - 'a' + 10
                                                           : hex[index * 2u] - '0');
        unsigned low = (unsigned) (hex[index * 2u + 1u] >= 'a' ? hex[index * 2u + 1u] - 'a' + 10
                                                               : hex[index * 2u + 1u] - '0');
        if (high > 15u || low > 15u)
            return false;
        output[index] = (uint8_t) ((high << 4u) | low);
    }
    return hex[XR_PROGRAM_DIGEST_SIZE * 2u] == '\0';
}

static void free_instruction(XrValidatedInstruction *instruction) {
    xr_free(instruction->operands);
    xr_free(instruction->successors);
}

static void free_type(XrValidatedType *type) {
    if (!type)
        return;
    for (uint32_t variant = 0; variant < type->variant_count; ++variant)
        xr_free(type->variants[variant].payload_types);
    xr_free(type->variants);
    xr_free(type->field_types);
}

static void free_function(XrValidatedFunction *function) {
    if (!function)
        return;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction)
            free_instruction(&row->instructions[instruction]);
        xr_free(row->instructions);
        xr_free(row->argument_ids);
        xr_free(row->argument_types);
        xr_free(row->argument_categories);
        xr_free(row->argument_ownerships);
    }
    xr_free(function->blocks);
    for (uint32_t value = 0; value < function->value_count; ++value)
        xr_free(function->value_root_sets ? function->value_root_sets[value].root_ids : NULL);
    xr_free(function->value_root_sets);
    xr_free(function->roots);
    xr_free(function->value_types);
    xr_free(function->value_categories);
    xr_free(function->value_ownerships);
    xr_free(function->value_blocks);
    xr_free(function->value_positions);
}

void xr_validated_program_free(XrValidatedProgram *program) {
    if (!program)
        return;
    if (atomic_fetch_sub_explicit(&program->references, 1u, memory_order_acq_rel) != 1u)
        return;
    for (uint32_t function = 0; function < program->function_count; ++function)
        free_function(&program->functions[function]);
    for (uint32_t type = 0; type < program->type_count; ++type)
        free_type(&program->types[type]);
    for (uint32_t signature = 0; signature < program->signature_count; ++signature) {
        xr_free(program->signatures[signature].parameter_types);
        xr_free(program->signatures[signature].parameter_modes);
        xr_free(program->signatures[signature].result_borrow_origins);
    }
    for (uint32_t interface = 0; interface < program->interface_count; ++interface)
        xr_free(program->interfaces[interface].slot_signature_ids);
    for (uint32_t conformance = 0; conformance < program->conformance_count; ++conformance)
        xr_free(program->conformances[conformance].slot_function_ids);
    xr_free(program->types);
    xr_free(program->signatures);
    xr_free(program->interfaces);
    xr_free(program->conformances);
    xr_free(program->functions);
    xr_free(program->constants);
    xr_free(program->bytes);
    xr_free(program);
}

XrValidatedProgram *xr_validated_program_retain(const XrValidatedProgram *program) {
    XrValidatedProgram *retained = (XrValidatedProgram *) program;
    if (retained)
        atomic_fetch_add_explicit(&retained->references, 1u, memory_order_relaxed);
    return retained;
}

static bool validated_type_graph_visit(const XrValidatedProgram *program, uint32_t index,
                                       uint8_t *state) {
    if (state[index] == 1u)
        return false;
    if (state[index] == 2u)
        return true;
    state[index] = 1u;
    const XrValidatedType *type = &program->types[index];
    if (type->kind == XR_CORE_IR_TYPE_VIEW || type->kind == XR_CORE_IR_TYPE_CALLABLE ||
        type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        state[index] = 2u;
        return true;
    }
    for (uint32_t field = 0; field < type->field_count; ++field) {
        uint16_t child = type->field_types[field];
        if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            !validated_type_graph_visit(program, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
            return false;
    }
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        const XrValidatedVariant *row = &type->variants[variant];
        for (uint32_t field = 0; field < row->payload_count; ++field) {
            uint16_t child = row->payload_types[field];
            if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                !validated_type_graph_visit(program, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE,
                                            state))
                return false;
        }
    }
    state[index] = 2u;
    return true;
}

static bool parse_types(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_TYPES - 1u);
    uint64_t total_count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_TYPES;
    if (total_count < 6u || total_count > XR_PROGRAM_LIMIT_TYPES ||
        total_count - 6u > UINT16_MAX - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u ||
        !spend(context, total_count, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    for (uint32_t id = 0; id < 6u; ++id) {
        uint64_t type_id = take_uvar(&reader);
        uint64_t kind = take_uvar(&reader);
        uint64_t ownership = take_uvar(&reader);
        uint64_t copy_contract = take_uvar(&reader);
        XrCoreIrTypeOwnership expected_ownership = id == XR_CORE_TYPE_PANIC_INFO
                                                       ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                                                       : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        XrCoreIrCopyContract expected_copy =
            id == XR_CORE_TYPE_VOID || id == XR_CORE_TYPE_PANIC_INFO ? XR_CORE_IR_COPY_FORBIDDEN
                                                                     : XR_CORE_IR_COPY_TRIVIAL;
        if (type_id != id || kind != id || ownership != expected_ownership ||
            copy_contract != expected_copy) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
    }
    context->program->type_count = (uint32_t) (total_count - 6u);
    if (context->program->type_count != 0u) {
        context->program->types = xr_calloc(context->program->type_count, sizeof(XrValidatedType));
        if (!context->program->types) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    uint8_t previous_key[XR_CORE_IR_KEY_SIZE] = {0};
    for (uint32_t index = 0; index < context->program->type_count; ++index) {
        XrValidatedType *type = &context->program->types[index];
        uint64_t type_id = take_uvar(&reader);
        uint64_t kind = take_uvar(&reader);
        uint64_t ownership = take_uvar(&reader);
        uint64_t copy_contract = take_uvar(&reader);
        take_bytes(&reader, type->key.bytes, sizeof(type->key.bytes));
        uint64_t shape_head = take_uvar(&reader);
        location.value_id = type_id <= UINT32_MAX ? (uint32_t) type_id : XR_PROGRAM_LOCATION_NONE;
        if (type_id != XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + index ||
            (kind != XR_PROGRAM_TYPE_KIND_AGGREGATE && kind != XR_PROGRAM_TYPE_KIND_VARIANT &&
             kind != XR_PROGRAM_TYPE_KIND_VIEW && kind != XR_PROGRAM_TYPE_KIND_CALLABLE &&
             kind != XR_PROGRAM_TYPE_KIND_EXISTENTIAL) ||
            ownership > XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
            copy_contract > XR_CORE_IR_COPY_FORBIDDEN ||
            ((ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL) !=
             (copy_contract == XR_CORE_IR_COPY_TRIVIAL)) ||
            (index != 0u && memcmp(previous_key, type->key.bytes, sizeof(previous_key)) >= 0) ||
            !spend(context, 1u, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        memcpy(previous_key, type->key.bytes, sizeof(previous_key));
        type->type_id = (uint16_t) type_id;
        type->kind = kind == XR_PROGRAM_TYPE_KIND_AGGREGATE  ? XR_CORE_IR_TYPE_AGGREGATE
                     : kind == XR_PROGRAM_TYPE_KIND_VARIANT  ? XR_CORE_IR_TYPE_VARIANT
                     : kind == XR_PROGRAM_TYPE_KIND_VIEW     ? XR_CORE_IR_TYPE_VIEW
                     : kind == XR_PROGRAM_TYPE_KIND_CALLABLE ? XR_CORE_IR_TYPE_CALLABLE
                                                             : XR_CORE_IR_TYPE_EXISTENTIAL;
        type->ownership = (XrCoreIrTypeOwnership) ownership;
        type->copy_contract = (XrCoreIrCopyContract) copy_contract;
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            uint64_t field_count = take_uvar(&reader);
            if (shape_head > XR_CORE_IR_NOMINAL_ENUM || field_count == 0u ||
                field_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                !spend(context, field_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
                return false;
            }
            type->nominal_kind = (XrCoreIrNominalKind) shape_head;
            type->field_count = (uint32_t) field_count;
            type->field_types = xr_calloc(type->field_count, sizeof(uint16_t));
            if (!type->field_types) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
            for (uint32_t field = 0; field < type->field_count; ++field) {
                uint64_t field_type = take_uvar(&reader);
                if (!type_is_runtime(context->program, field_type) ||
                    field_type == XR_CORE_TYPE_VOID) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                    return false;
                }
                type->field_types[field] = (uint16_t) field_type;
            }
        } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
            uint64_t variant_count = take_uvar(&reader);
            if (shape_head > XR_CORE_IR_NOMINAL_ENUM || variant_count == 0u ||
                variant_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                !spend(context, variant_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
                return false;
            }
            type->nominal_kind = (XrCoreIrNominalKind) shape_head;
            type->variant_count = (uint32_t) variant_count;
            type->variants = xr_calloc(type->variant_count, sizeof(XrValidatedVariant));
            if (!type->variants) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
            for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
                uint64_t payload_count = take_uvar(&reader);
                if (payload_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                    !spend(context, payload_count, location)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
                    return false;
                }
                XrValidatedVariant *row = &type->variants[variant];
                row->payload_count = (uint32_t) payload_count;
                if (row->payload_count != 0u) {
                    row->payload_types = xr_calloc(row->payload_count, sizeof(uint16_t));
                    if (!row->payload_types) {
                        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                        return false;
                    }
                }
                for (uint32_t field = 0; field < row->payload_count; ++field) {
                    uint64_t field_type = take_uvar(&reader);
                    if (!type_is_runtime(context->program, field_type) ||
                        field_type == XR_CORE_TYPE_VOID) {
                        reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                        return false;
                    }
                    row->payload_types[field] = (uint16_t) field_type;
                }
            }
        } else if (type->kind == XR_CORE_IR_TYPE_VIEW) {
            uint64_t capability = take_uvar(&reader);
            if (!type_is_runtime(context->program, shape_head) || shape_head == XR_CORE_TYPE_VOID ||
                ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
                copy_contract != XR_CORE_IR_COPY_TRIVIAL ||
                (capability != XR_CORE_IR_VIEW_READ &&
                 capability != XR_CORE_IR_VIEW_WRITE_EXCLUSIVE)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            type->view_element_type = (uint16_t) shape_head;
            type->view_capability = (XrCoreIrViewCapability) capability;
        } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
            if (ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                copy_contract != XR_CORE_IR_COPY_EXPLICIT || shape_head > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            type->signature_id = (uint32_t) shape_head;
        } else {
            uint64_t use_kind = take_uvar(&reader);
            bool trivial = use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_READ;
            bool affine = use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF ||
                          use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                          use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE;
            if (shape_head > UINT32_MAX || (!trivial && !affine) ||
                (trivial && (ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
                             copy_contract != XR_CORE_IR_COPY_TRIVIAL)) ||
                (affine && (ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                            copy_contract != XR_CORE_IR_COPY_FORBIDDEN))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            type->interface_id = (uint32_t) shape_head;
            type->interface_use_kind = (XrCoreIrInterfaceUseKind) use_kind;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    uint8_t *state = xr_calloc(context->program->type_count ? context->program->type_count : 1u,
                               sizeof(uint8_t));
    if (!state) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    bool acyclic = true;
    for (uint32_t index = 0; acyclic && index < context->program->type_count; ++index)
        acyclic = validated_type_graph_visit(context->program, index, state);
    xr_free(state);
    if (!acyclic) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
        return false;
    }
    for (uint32_t index = 0; index < context->program->type_count; ++index) {
        const XrValidatedType *type = &context->program->types[index];
        for (uint32_t field = 0; field < type->field_count; ++field) {
            if (type_is_existential_ref(context->program, type->field_types[field]) ||
                (type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                 xr_validated_program_type_ownership(context->program, type->field_types[field]) !=
                     XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
        }
        for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            for (uint32_t field = 0; field < row->payload_count; ++field) {
                if (type_is_existential_ref(context->program, row->payload_types[field]) ||
                    (type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                     xr_validated_program_type_ownership(context->program,
                                                         row->payload_types[field]) !=
                         XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                    return false;
                }
            }
        }
    }
    return true;
}

static bool parse_constants(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_CONSTANTS - 1u);
    uint64_t count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_CONSTANTS;
    if (count > XR_PROGRAM_LIMIT_CONSTANTS || count > SIZE_MAX / sizeof(XrValidatedConstant) ||
        !spend(context, count + 1u, location))
        return false;
    context->program->constant_count = (uint32_t) count;
    if (count != 0) {
        context->program->constants = xr_calloc((size_t) count, sizeof(XrValidatedConstant));
        if (!context->program->constants) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    for (uint32_t index = 0; index < (uint32_t) count; ++index) {
        uint64_t id = take_uvar(&reader);
        uint64_t type_id = take_uvar(&reader);
        uint64_t kind = take_uvar(&reader);
        XrValidatedConstant *constant = &context->program->constants[index];
        location.value_id = index;
        if (id != index || !type_is_runtime(context->program, type_id) ||
            (kind != XR_CORE_IR_CONSTANT_I64 && kind != XR_CORE_IR_CONSTANT_BOOL)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        constant->type_id = (uint16_t) type_id;
        constant->kind = (XrCoreIrConstantKind) kind;
        if (kind == XR_CORE_IR_CONSTANT_I64) {
            constant->value.i64 = take_svar(&reader);
            if (type_id != XR_CORE_TYPE_I64) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
        } else {
            uint64_t value = take_uvar(&reader);
            if (type_id != XR_CORE_TYPE_BOOL || value > 1u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
            constant->value.boolean = value != 0u;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static int compare_u32(uint32_t left, uint32_t right) {
    return left == right ? 0 : (left < right ? -1 : 1);
}

static int validated_signature_compare(const XrValidatedSignature *left,
                                       const XrValidatedSignature *right) {
    int order = compare_u32(left->parameter_count, right->parameter_count);
    if (order != 0)
        return order;
    for (uint32_t index = 0; index < left->parameter_count; ++index) {
        order = compare_u32(left->parameter_types[index], right->parameter_types[index]);
        if (order != 0)
            return order;
        order = compare_u32(left->parameter_modes[index], right->parameter_modes[index]);
        if (order != 0)
            return order;
    }
#define COMPARE_SIGNATURE_FIELD(field)                                                             \
    do {                                                                                           \
        order = compare_u32((uint32_t) left->field, (uint32_t) right->field);                      \
        if (order != 0)                                                                            \
            return order;                                                                          \
    } while (0)
    COMPARE_SIGNATURE_FIELD(has_receiver);
    COMPARE_SIGNATURE_FIELD(receiver_mode);
    COMPARE_SIGNATURE_FIELD(result_type_id);
    COMPARE_SIGNATURE_FIELD(result_ownership);
    COMPARE_SIGNATURE_FIELD(result_borrow_origin_count);
    for (uint32_t index = 0; index < left->result_borrow_origin_count; ++index) {
        order = compare_u32(left->result_borrow_origins[index].kind,
                            right->result_borrow_origins[index].kind);
        if (order != 0)
            return order;
        if (left->result_borrow_origins[index].param_ordinal !=
            right->result_borrow_origins[index].param_ordinal)
            return left->result_borrow_origins[index].param_ordinal <
                           right->result_borrow_origins[index].param_ordinal
                       ? -1
                       : 1;
    }
    COMPARE_SIGNATURE_FIELD(error_type_id);
    COMPARE_SIGNATURE_FIELD(panic_type_id);
    COMPARE_SIGNATURE_FIELD(effect_mask);
    COMPARE_SIGNATURE_FIELD(capability_mask);
#undef COMPARE_SIGNATURE_FIELD
    return 0;
}

static bool parse_signature(VerifyContext *context, VerifyReader *reader,
                            XrValidatedSignature *signature, uint32_t id,
                            XrProgramSemanticLocation location) {
    uint64_t encoded_id = take_uvar(reader);
    uint64_t parameter_count = take_uvar(reader);
    location.value_id = id;
    if (encoded_id != id || parameter_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
        parameter_count > SIZE_MAX / sizeof(uint16_t) ||
        !spend(context, parameter_count, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    signature->parameter_count = (uint32_t) parameter_count;
    if (parameter_count != 0u) {
        signature->parameter_types = xr_calloc((size_t) parameter_count, sizeof(uint16_t));
        signature->parameter_modes = xr_calloc((size_t) parameter_count, sizeof(XrParamMode));
        if (!signature->parameter_types || !signature->parameter_modes) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    for (uint32_t parameter = 0; parameter < signature->parameter_count; ++parameter) {
        uint64_t type_id = take_uvar(reader);
        uint64_t mode = take_uvar(reader);
        if (!type_is_runtime(context->program, type_id) || type_id == XR_CORE_TYPE_VOID ||
            mode > XR_PARAM_MOVE) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        signature->parameter_types[parameter] = (uint16_t) type_id;
        signature->parameter_modes[parameter] = (XrParamMode) mode;
    }
    uint64_t has_receiver = take_uvar(reader);
    uint64_t receiver_mode = take_uvar(reader);
    uint64_t result_type = take_uvar(reader);
    uint64_t result_ownership = take_uvar(reader);
    uint64_t origin_count = take_uvar(reader);
    if (has_receiver > 1u || receiver_mode > XR_PARAM_MOVE ||
        (has_receiver != 0u &&
         (parameter_count == 0u || signature->parameter_modes[0] != receiver_mode)) ||
        (has_receiver == 0u && receiver_mode != XR_PARAM_READ) ||
        origin_count > XR_PROGRAM_LIMIT_ROOTS_PER_VALUE ||
        origin_count > SIZE_MAX / sizeof(XrViewOrigin) || !spend(context, origin_count, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    signature->has_receiver = has_receiver != 0u;
    signature->receiver_mode = (XrParamMode) receiver_mode;
    signature->result_borrow_origin_count = (uint32_t) origin_count;
    if (origin_count != 0u) {
        signature->result_borrow_origins = xr_calloc((size_t) origin_count, sizeof(XrViewOrigin));
        if (!signature->result_borrow_origins) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    XrViewOrigin prior_origin = {.kind = XR_VIEW_ORIGIN_PARAM, .param_ordinal = -1};
    for (uint32_t origin = 0; origin < signature->result_borrow_origin_count; ++origin) {
        uint64_t kind = take_uvar(reader);
        uint64_t parameter = kind == XR_VIEW_ORIGIN_PARAM ? take_uvar(reader) : UINT64_MAX;
        if (kind > XR_VIEW_ORIGIN_STATIC ||
            (kind == XR_VIEW_ORIGIN_PARAM && parameter > INT16_MAX) ||
            (kind == XR_VIEW_ORIGIN_PARAM &&
             (parameter >= parameter_count - (signature->has_receiver ? 1u : 0u) ||
              signature->parameter_modes[(uint32_t) parameter +
                                         (signature->has_receiver ? 1u : 0u)] != XR_PARAM_READ)) ||
            (kind == XR_VIEW_ORIGIN_RECEIVER &&
             (!signature->has_receiver || signature->receiver_mode != XR_PARAM_READ))) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        XrViewOrigin row = {
            .kind = (XrViewOriginKind) kind,
            .param_ordinal = kind == XR_VIEW_ORIGIN_PARAM ? (int16_t) parameter : -1,
        };
        if (origin != 0u &&
            (row.kind < prior_origin.kind ||
             (row.kind == prior_origin.kind && row.param_ordinal <= prior_origin.param_ordinal))) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        signature->result_borrow_origins[origin] = row;
        prior_origin = row;
    }
    uint64_t error_type = take_uvar(reader);
    uint64_t panic_type = take_uvar(reader);
    uint64_t effect_mask = take_uvar(reader);
    uint64_t capability_mask = take_uvar(reader);
    const XrValidatedType *result_view =
        result_type <= UINT16_MAX
            ? xr_validated_program_type(context->program, (uint16_t) result_type)
            : NULL;
    bool readonly_view = result_view && result_view->kind == XR_CORE_IR_TYPE_VIEW &&
                         result_view->view_capability == XR_CORE_IR_VIEW_READ;
    if (!type_is_runtime(context->program, result_type) ||
        !type_is_runtime(context->program, error_type) ||
        !type_is_runtime(context->program, panic_type) ||
        (result_type <= UINT16_MAX &&
         type_is_existential_ref(context->program, (uint16_t) result_type)) ||
        (error_type <= UINT16_MAX &&
         type_is_existential_ref(context->program, (uint16_t) error_type)) ||
        (panic_type != XR_CORE_TYPE_VOID && panic_type != XR_CORE_TYPE_PANIC_INFO) ||
        result_ownership > XR_CORE_IR_OWNER || effect_mask > UINT32_MAX ||
        capability_mask > UINT32_MAX ||
        readonly_view != (signature->result_borrow_origin_count != 0u) ||
        (type_is_view(context->program, (uint16_t) result_type) && !readonly_view)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    signature->result_type_id = (uint16_t) result_type;
    signature->result_ownership = (XrCoreIrOwnershipDisposition) result_ownership;
    signature->error_type_id = (uint16_t) error_type;
    signature->panic_type_id = (uint16_t) panic_type;
    signature->effect_mask = (uint32_t) effect_mask;
    signature->capability_mask = (uint32_t) capability_mask;
    XrCoreIrOwnershipDisposition expected_result =
        signature->result_type_id != XR_CORE_TYPE_VOID &&
                xr_validated_program_type_ownership(context->program, signature->result_type_id) ==
                    XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
            ? XR_CORE_IR_OWNER
            : XR_CORE_IR_NON_OWNER;
    if ((signature->effect_mask & ~UINT32_C(0x1f)) != 0u ||
        (signature->capability_mask & ~UINT32_C(0x01)) != 0u ||
        ((signature->error_type_id == XR_CORE_TYPE_VOID) !=
         ((signature->effect_mask & XR_CORE_EFFECT_ERROR) == 0u)) ||
        signature->error_type_id == XR_CORE_TYPE_PANIC_INFO ||
        ((signature->panic_type_id == XR_CORE_TYPE_VOID) !=
         ((signature->effect_mask & XR_CORE_EFFECT_PANIC) == 0u)) ||
        signature->result_ownership != expected_result) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    return true;
}

static bool parse_functions(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_FUNCTIONS - 1u);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_FUNCTIONS;
    uint64_t signature_count = take_uvar(&reader);
    if (signature_count == 0u || signature_count > XR_PROGRAM_LIMIT_FUNCTIONS ||
        signature_count > SIZE_MAX / sizeof(XrValidatedSignature) ||
        !spend(context, signature_count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->program->signature_count = (uint32_t) signature_count;
    context->program->signatures =
        xr_calloc((size_t) signature_count, sizeof(XrValidatedSignature));
    if (!context->program->signatures) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    for (uint32_t id = 0; id < context->program->signature_count; ++id) {
        if (!parse_signature(context, &reader, &context->program->signatures[id], id, location))
            return false;
        if (id != 0u && validated_signature_compare(&context->program->signatures[id - 1u],
                                                    &context->program->signatures[id]) >= 0) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
    }
    for (uint32_t type = 0; type < context->program->type_count; ++type)
        if (context->program->types[type].kind == XR_CORE_IR_TYPE_CALLABLE &&
            (context->program->types[type].signature_id >= context->program->signature_count ||
             context->program->signatures[context->program->types[type].signature_id]
                 .has_receiver)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }

    uint64_t count = take_uvar(&reader);
    if (count == 0u || count > context->budget.max_functions ||
        count > SIZE_MAX / sizeof(XrValidatedFunction) || !spend(context, count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->program->function_count = (uint32_t) count;
    context->program->functions = xr_calloc((size_t) count, sizeof(XrValidatedFunction));
    if (!context->program->functions) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    for (uint32_t id = 0; id < context->program->function_count; ++id) {
        XrValidatedFunction *function = &context->program->functions[id];
        uint64_t encoded_id = take_uvar(&reader);
        uint64_t signature_id = take_uvar(&reader);
        uint64_t entry_block = take_uvar(&reader);
        uint64_t block_count = take_uvar(&reader);
        uint64_t value_count = take_uvar(&reader);
        uint64_t flags = take_uvar(&reader);
        location.function_id = id;
        if (encoded_id != id || signature_id >= context->program->signature_count ||
            block_count == 0u || block_count > context->budget.max_blocks_per_function ||
            entry_block >= block_count || value_count > context->budget.max_values_per_function ||
            flags > UINT32_MAX || block_count > SIZE_MAX / sizeof(XrValidatedBlock) ||
            value_count > SIZE_MAX / sizeof(uint16_t) ||
            !spend(context, block_count + value_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        const XrValidatedSignature *signature = &context->program->signatures[signature_id];
        function->signature_id = (uint32_t) signature_id;
        function->parameter_types = signature->parameter_types;
        function->parameter_modes = signature->parameter_modes;
        function->parameter_count = signature->parameter_count;
        function->has_receiver = signature->has_receiver;
        function->receiver_mode = signature->receiver_mode;
        function->result_type_id = signature->result_type_id;
        function->result_ownership = signature->result_ownership;
        function->result_borrow_origins = signature->result_borrow_origins;
        function->result_borrow_origin_count = signature->result_borrow_origin_count;
        function->error_type_id = signature->error_type_id;
        function->panic_type_id = signature->panic_type_id;
        function->effect_mask = signature->effect_mask;
        function->capability_mask = signature->capability_mask;
        function->entry_block = (uint32_t) entry_block;
        function->block_count = (uint32_t) block_count;
        function->value_count = (uint32_t) value_count;
        function->flags = (uint32_t) flags;
        function->blocks = xr_calloc((size_t) block_count, sizeof(XrValidatedBlock));
        function->value_types =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint16_t));
        function->value_categories =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(XrCoreIrValueCategory));
        function->value_ownerships = xr_calloc((size_t) (value_count ? value_count : 1u),
                                               sizeof(XrCoreIrOwnershipDisposition));
        function->value_blocks =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint32_t));
        function->value_positions =
            xr_calloc((size_t) (value_count ? value_count : 1u), sizeof(uint32_t));
        if (!function->blocks || !function->value_types || !function->value_categories ||
            !function->value_ownerships || !function->value_blocks || !function->value_positions) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool allocate_u32(uint64_t count, uint32_t **output) {
    *output = NULL;
    if (count == 0)
        return true;
    if (count > SIZE_MAX / sizeof(uint32_t))
        return false;
    *output = xr_calloc((size_t) count, sizeof(uint32_t));
    return *output != NULL;
}

static bool parse_instruction(VerifyContext *context, VerifyReader *reader,
                              XrValidatedInstruction *instruction,
                              XrProgramSemanticLocation location) {
    uint64_t operation_id = take_uvar(reader);
    uint64_t result_plus_one = take_uvar(reader);
    uint64_t result_type = take_uvar(reader);
    uint64_t result_category = take_uvar(reader);
    uint64_t result_ownership = take_uvar(reader);
    uint64_t operand_count = take_uvar(reader);
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    if (operation_id > UINT16_MAX || !xr_core_spec_operation_by_id((uint16_t) operation_id)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    if (result_plus_one > UINT32_MAX || !type_is_runtime(context->program, result_type) ||
        result_category > XR_CORE_IR_PLACE || result_ownership > XR_CORE_IR_OWNER ||
        (result_category == XR_CORE_IR_PLACE && result_ownership != XR_CORE_IR_NON_OWNER)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    if (operand_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    if (!spend(context, operand_count + 1u, location))
        return false;
    if (!allocate_u32(operand_count, &instruction->operands)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    instruction->operation_id = (uint16_t) operation_id;
    instruction->result_id =
        result_plus_one == 0 ? XR_PROGRAM_LOCATION_NONE : (uint32_t) result_plus_one - 1u;
    instruction->result_type_id = (uint16_t) result_type;
    instruction->result_category = (XrCoreIrValueCategory) result_category;
    instruction->result_ownership = (XrCoreIrOwnershipDisposition) result_ownership;
    instruction->operand_count = (uint32_t) operand_count;
    for (uint32_t operand = 0; operand < (uint32_t) operand_count; ++operand) {
        uint64_t value = take_uvar(reader);
        if (value > UINT32_MAX) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
        instruction->operands[operand] = (uint32_t) value;
    }
    uint64_t immediate_kind = take_uvar(reader);
    if (immediate_kind > XR_CORE_IR_IMMEDIATE_TYPE) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    instruction->immediate_kind = (XrCoreIrImmediateKind) immediate_kind;
    switch (instruction->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            break;
        case XR_CORE_IR_IMMEDIATE_I64:
            instruction->immediate.i64 = take_svar(reader);
            break;
        case XR_CORE_IR_IMMEDIATE_U32: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.u32 = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_BOOL: {
            uint64_t value = take_uvar(reader);
            if (value > 1u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.boolean = value != 0u;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_CONSTANT: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.constant_id = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_FUNCTION: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.function_id = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_FIELD: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.field_ordinal = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_VARIANT: {
            uint64_t value = take_uvar(reader);
            if (value > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.variant_ordinal = (uint32_t) value;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_VARIANT_FIELD: {
            uint64_t variant = take_uvar(reader);
            uint64_t field = take_uvar(reader);
            if (variant > UINT32_MAX || field > UINT32_MAX) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.variant_field.variant_ordinal = (uint32_t) variant;
            instruction->immediate.variant_field.field_ordinal = (uint32_t) field;
            break;
        }
        case XR_CORE_IR_IMMEDIATE_TYPE: {
            uint64_t value = take_uvar(reader);
            if (!type_is_runtime(context->program, value) || value == XR_CORE_TYPE_VOID) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            instruction->immediate.type_id = (uint16_t) value;
            break;
        }
        default:
            return false;
    }
    uint64_t successor_count = take_uvar(reader);
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    if (successor_count > XR_PROGRAM_LIMIT_SUCCESSORS_PER_OPERATION) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    if (!spend(context, successor_count, location))
        return false;
    if (!allocate_u32(successor_count, &instruction->successors)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    instruction->successor_count = (uint32_t) successor_count;
    for (uint32_t successor = 0; successor < (uint32_t) successor_count; ++successor) {
        uint64_t value = take_uvar(reader);
        if (value > UINT32_MAX) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            return false;
        }
        instruction->successors[successor] = (uint32_t) value;
    }
    if (!reader->valid) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool parse_code(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_CODE - 1u);
    uint64_t function_count = take_uvar(&reader);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_CODE;
    if (function_count != context->program->function_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    uint64_t operation_count = 0;
    for (uint32_t function_id = 0; function_id < context->program->function_count; ++function_id) {
        XrValidatedFunction *function = &context->program->functions[function_id];
        uint64_t encoded_function = take_uvar(&reader);
        uint64_t block_count = take_uvar(&reader);
        location.function_id = function_id;
        if (encoded_function != function_id || block_count != function->block_count) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
            return false;
        }
        uint32_t next_value = 0;
        for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
            XrValidatedBlock *block = &function->blocks[block_id];
            uint64_t encoded_block = take_uvar(&reader);
            uint64_t argument_count = take_uvar(&reader);
            location.block_id = block_id;
            if (encoded_block != block_id || argument_count > function->value_count ||
                argument_count > SIZE_MAX / sizeof(uint16_t)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                return false;
            }
            if (!allocate_u32(argument_count, &block->argument_ids)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
            block->argument_count = (uint32_t) argument_count;
            if (argument_count != 0) {
                block->argument_types = xr_calloc((size_t) argument_count, sizeof(uint16_t));
                block->argument_categories =
                    xr_calloc((size_t) argument_count, sizeof(XrCoreIrValueCategory));
                block->argument_ownerships =
                    xr_calloc((size_t) argument_count, sizeof(XrCoreIrOwnershipDisposition));
                if (!block->argument_types || !block->argument_categories ||
                    !block->argument_ownerships) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                    return false;
                }
            }
            for (uint32_t argument = 0; argument < (uint32_t) argument_count; ++argument) {
                uint64_t value_id = take_uvar(&reader);
                uint64_t type_id = take_uvar(&reader);
                uint64_t category = take_uvar(&reader);
                uint64_t ownership = take_uvar(&reader);
                location.value_id = (uint32_t) value_id;
                if (value_id != next_value || value_id >= function->value_count ||
                    !type_is_runtime(context->program, type_id) || type_id == XR_CORE_TYPE_VOID ||
                    category > XR_CORE_IR_PLACE || ownership > XR_CORE_IR_OWNER ||
                    (category == XR_CORE_IR_PLACE && ownership != XR_CORE_IR_NON_OWNER)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                    return false;
                }
                block->argument_ids[argument] = next_value;
                block->argument_types[argument] = (uint16_t) type_id;
                block->argument_categories[argument] = (XrCoreIrValueCategory) category;
                block->argument_ownerships[argument] = (XrCoreIrOwnershipDisposition) ownership;
                function->value_types[next_value] = (uint16_t) type_id;
                function->value_categories[next_value] = (XrCoreIrValueCategory) category;
                function->value_ownerships[next_value] = (XrCoreIrOwnershipDisposition) ownership;
                function->value_blocks[next_value] = block_id;
                function->value_positions[next_value] = 0u;
                ++next_value;
            }
            uint64_t instruction_count = take_uvar(&reader);
            if (instruction_count > context->budget.max_operations ||
                instruction_count > SIZE_MAX / sizeof(XrValidatedInstruction) ||
                operation_count > context->budget.max_operations - instruction_count ||
                !spend(context, instruction_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
                return false;
            }
            operation_count += instruction_count;
            block->instruction_count = (uint32_t) instruction_count;
            if (instruction_count != 0) {
                block->instructions =
                    xr_calloc((size_t) instruction_count, sizeof(XrValidatedInstruction));
                if (!block->instructions) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                    return false;
                }
            }
            for (uint32_t instruction_id = 0; instruction_id < (uint32_t) instruction_count;
                 ++instruction_id) {
                location.instruction_id = instruction_id;
                if (!parse_instruction(context, &reader, &block->instructions[instruction_id],
                                       location))
                    return false;
                XrValidatedInstruction *instruction = &block->instructions[instruction_id];
                if (instruction->result_id != XR_PROGRAM_LOCATION_NONE) {
                    location.value_id = instruction->result_id;
                    if (instruction->result_id != next_value ||
                        instruction->result_id >= function->value_count ||
                        instruction->result_type_id == XR_CORE_TYPE_VOID) {
                        reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                        return false;
                    }
                    function->value_types[next_value] = instruction->result_type_id;
                    function->value_categories[next_value] = instruction->result_category;
                    function->value_ownerships[next_value] = instruction->result_ownership;
                    function->value_blocks[next_value] = block_id;
                    function->value_positions[next_value] = instruction_id + 1u;
                    ++next_value;
                } else if (instruction->result_type_id != XR_CORE_TYPE_VOID ||
                           instruction->result_category != XR_CORE_IR_VALUE ||
                           instruction->result_ownership != XR_CORE_IR_NON_OWNER) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
                    return false;
                }
            }
        }
        if (next_value != function->value_count) {
            location.value_id = next_value;
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION, location);
            return false;
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static uint32_t root_for_origin(const XrValidatedFunction *function, const XrViewOrigin *origin) {
    for (uint32_t root = 0; root < function->root_count; ++root) {
        const XrValidatedRoot *row = &function->roots[root];
        if ((origin->kind == XR_VIEW_ORIGIN_PARAM && row->kind == XR_CORE_IR_ROOT_PARAMETER &&
             row->parameter_ordinal == (uint32_t) origin->param_ordinal) ||
            (origin->kind == XR_VIEW_ORIGIN_RECEIVER && row->kind == XR_CORE_IR_ROOT_RECEIVER) ||
            (origin->kind == XR_VIEW_ORIGIN_STATIC && row->kind == XR_CORE_IR_ROOT_STATIC))
            return root;
    }
    return XR_PROGRAM_LOCATION_NONE;
}

static bool key_is_zero(XrCoreIrKey key) {
    uint8_t combined = 0u;
    for (size_t index = 0; index < sizeof(key.bytes); ++index)
        combined |= key.bytes[index];
    return combined == 0u;
}

static bool validated_signature_satisfies_interface(const XrValidatedProgram *program,
                                                    const XrValidatedSignature *implementation,
                                                    const XrValidatedSignature *requirement,
                                                    uint16_t implementor_type,
                                                    uint32_t interface_id) {
    if (implementation->parameter_count != requirement->parameter_count ||
        implementation->has_receiver != requirement->has_receiver ||
        implementation->receiver_mode != requirement->receiver_mode ||
        implementation->result_type_id != requirement->result_type_id ||
        implementation->result_ownership != requirement->result_ownership ||
        implementation->result_borrow_origin_count != requirement->result_borrow_origin_count ||
        implementation->error_type_id != requirement->error_type_id ||
        implementation->panic_type_id != requirement->panic_type_id ||
        implementation->effect_mask != requirement->effect_mask ||
        implementation->capability_mask != requirement->capability_mask)
        return false;
    for (uint32_t index = 0; index < requirement->parameter_count; ++index) {
        if (implementation->parameter_modes[index] != requirement->parameter_modes[index])
            return false;
        if (requirement->has_receiver && index == 0u) {
            const XrValidatedType *receiver =
                xr_validated_program_type(program, requirement->parameter_types[index]);
            if (implementation->parameter_types[index] != implementor_type || !receiver ||
                receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                receiver->interface_id != interface_id)
                return false;
        } else if (implementation->parameter_types[index] != requirement->parameter_types[index]) {
            return false;
        }
    }
    for (uint32_t index = 0; index < requirement->result_borrow_origin_count; ++index)
        if (implementation->result_borrow_origins[index].kind !=
                requirement->result_borrow_origins[index].kind ||
            implementation->result_borrow_origins[index].param_ordinal !=
                requirement->result_borrow_origins[index].param_ordinal)
            return false;
    return true;
}

static bool parse_semantic_metadata(VerifyContext *context, const XrProgramView *view) {
    VerifyReader reader = section_reader(view, XR_PROGRAM_SECTION_SEMANTIC_METADATA - 1u);
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_SEMANTIC_METADATA;
    uint64_t interface_count = take_uvar(&reader);
    if (interface_count > XR_PROGRAM_LIMIT_TYPES ||
        interface_count > SIZE_MAX / sizeof(XrValidatedInterface) ||
        !spend(context, interface_count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->program->interface_count = (uint32_t) interface_count;
    if (interface_count != 0u) {
        context->program->interfaces =
            xr_calloc((size_t) interface_count, sizeof(XrValidatedInterface));
        if (!context->program->interfaces) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    for (uint32_t interface = 0; interface < context->program->interface_count; ++interface) {
        XrValidatedInterface *row = &context->program->interfaces[interface];
        uint64_t id = take_uvar(&reader);
        take_bytes(&reader, row->key.bytes, sizeof(row->key.bytes));
        uint64_t slot_count = take_uvar(&reader);
        if (id != interface || key_is_zero(row->key) ||
            (interface != 0u && memcmp(context->program->interfaces[interface - 1u].key.bytes,
                                       row->key.bytes, sizeof(row->key.bytes)) >= 0) ||
            slot_count == 0u || slot_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            slot_count > SIZE_MAX / sizeof(uint32_t) || !spend(context, slot_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        row->slot_count = (uint32_t) slot_count;
        row->slot_signature_ids = xr_calloc((size_t) slot_count, sizeof(uint32_t));
        if (!row->slot_signature_ids) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
        for (uint32_t slot = 0; slot < row->slot_count; ++slot) {
            uint64_t signature_id = take_uvar(&reader);
            const XrValidatedSignature *signature =
                signature_id < context->program->signature_count
                    ? &context->program->signatures[signature_id]
                    : NULL;
            const XrValidatedType *receiver =
                signature && signature->has_receiver && signature->parameter_count != 0u
                    ? xr_validated_program_type(context->program, signature->parameter_types[0])
                    : NULL;
            if (!signature || !receiver || receiver->interface_id != interface ||
                !existential_receiver_mode_supported(receiver, signature->receiver_mode)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
                return false;
            }
            row->slot_signature_ids[slot] = (uint32_t) signature_id;
        }
    }
    for (uint32_t type = 0; type < context->program->type_count; ++type)
        if (context->program->types[type].kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
            context->program->types[type].interface_id >= context->program->interface_count) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }

    uint64_t conformance_count = take_uvar(&reader);
    if (conformance_count > XR_PROGRAM_LIMIT_TYPES ||
        conformance_count > SIZE_MAX / sizeof(XrValidatedConformance) ||
        !spend(context, conformance_count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, location);
        return false;
    }
    context->program->conformance_count = (uint32_t) conformance_count;
    if (conformance_count != 0u) {
        context->program->conformances =
            xr_calloc((size_t) conformance_count, sizeof(XrValidatedConformance));
        if (!context->program->conformances) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
    }
    for (uint32_t conformance = 0; conformance < context->program->conformance_count;
         ++conformance) {
        XrValidatedConformance *row = &context->program->conformances[conformance];
        uint64_t id = take_uvar(&reader);
        take_bytes(&reader, row->key.bytes, sizeof(row->key.bytes));
        uint64_t implementor_type = take_uvar(&reader);
        uint64_t implementor_kind = take_uvar(&reader);
        uint64_t interface_id = take_uvar(&reader);
        uint64_t slot_count = take_uvar(&reader);
        const XrValidatedType *implementor =
            implementor_type <= UINT16_MAX
                ? xr_validated_program_type(context->program, (uint16_t) implementor_type)
                : NULL;
        if (id != conformance || key_is_zero(row->key) ||
            (conformance != 0u && memcmp(context->program->conformances[conformance - 1u].key.bytes,
                                         row->key.bytes, sizeof(row->key.bytes)) >= 0) ||
            !implementor || implementor_kind == XR_CORE_IR_NOMINAL_NONE ||
            implementor_kind > XR_CORE_IR_NOMINAL_ENUM ||
            implementor->nominal_kind != implementor_kind ||
            (implementor->kind != XR_CORE_IR_TYPE_AGGREGATE &&
             implementor->kind != XR_CORE_IR_TYPE_VARIANT) ||
            interface_id >= context->program->interface_count ||
            slot_count != context->program->interfaces[interface_id].slot_count ||
            slot_count > SIZE_MAX / sizeof(uint32_t) || !spend(context, slot_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
        for (uint32_t prior = 0; prior < conformance; ++prior)
            if (context->program->conformances[prior].implementor_type_id == implementor_type &&
                context->program->conformances[prior].interface_id == interface_id) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
                return false;
            }
        row->implementor_type_id = (uint16_t) implementor_type;
        row->implementor_kind = (XrCoreIrNominalKind) implementor_kind;
        row->interface_id = (uint32_t) interface_id;
        row->slot_count = (uint32_t) slot_count;
        row->slot_function_ids = xr_calloc((size_t) slot_count, sizeof(uint32_t));
        if (!row->slot_function_ids) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
        for (uint32_t slot = 0; slot < row->slot_count; ++slot) {
            uint64_t function_id = take_uvar(&reader);
            uint32_t requirement_id =
                context->program->interfaces[row->interface_id].slot_signature_ids[slot];
            if (function_id >= context->program->function_count ||
                !validated_signature_satisfies_interface(
                    context->program,
                    &context->program
                         ->signatures[context->program->functions[function_id].signature_id],
                    &context->program->signatures[requirement_id], row->implementor_type_id,
                    row->interface_id)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
                return false;
            }
            row->slot_function_ids[slot] = (uint32_t) function_id;
        }
    }

    uint64_t function_count = take_uvar(&reader);
    if (function_count != context->program->function_count ||
        !spend(context, function_count + 1u, location)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
        return false;
    }
    for (uint32_t function_id = 0; function_id < context->program->function_count; ++function_id) {
        XrValidatedFunction *function = &context->program->functions[function_id];
        uint64_t encoded_function = take_uvar(&reader);
        uint64_t root_count = take_uvar(&reader);
        location.function_id = function_id;
        if (encoded_function != function_id || root_count > XR_PROGRAM_LIMIT_ROOTS_PER_FUNCTION ||
            root_count > SIZE_MAX / sizeof(XrValidatedRoot) ||
            !spend(context, root_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
            return false;
        }
        function->root_count = (uint32_t) root_count;
        if (root_count != 0u) {
            function->roots = xr_calloc((size_t) root_count, sizeof(XrValidatedRoot));
            if (!function->roots) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
        }
        XrCoreIrRootKind prior_kind = XR_CORE_IR_ROOT_PARAMETER;
        uint32_t prior_payload = 0u;
        bool have_prior = false;
        for (uint32_t root_id = 0; root_id < (uint32_t) root_count; ++root_id) {
            uint64_t encoded_root = take_uvar(&reader);
            uint64_t kind = take_uvar(&reader);
            uint64_t payload = kind == XR_CORE_IR_ROOT_PARAMETER || kind == XR_CORE_IR_ROOT_LOCAL
                                   ? take_uvar(&reader)
                                   : 0u;
            location.value_id = root_id;
            if (encoded_root != root_id || kind > XR_CORE_IR_ROOT_LOCAL || payload > UINT32_MAX ||
                (have_prior &&
                 (kind < prior_kind || (kind == prior_kind && payload <= prior_payload)))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            XrValidatedRoot *root = &function->roots[root_id];
            root->kind = (XrCoreIrRootKind) kind;
            root->parameter_ordinal = XR_PROGRAM_LOCATION_NONE;
            root->source_value_id = XR_PROGRAM_LOCATION_NONE;
            if (root->kind == XR_CORE_IR_ROOT_PARAMETER) {
                uint32_t explicit_count =
                    function->parameter_count - (function->has_receiver ? 1u : 0u);
                if (payload >= explicit_count) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                    return false;
                }
                root->parameter_ordinal = (uint32_t) payload;
            } else if (root->kind == XR_CORE_IR_ROOT_RECEIVER) {
                if (!function->has_receiver) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                    return false;
                }
            } else if (root->kind == XR_CORE_IR_ROOT_LOCAL) {
                if (payload >= function->value_count ||
                    function->value_ownerships[payload] != XR_CORE_IR_OWNER ||
                    type_is_view(context->program, function->value_types[payload])) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                    return false;
                }
                root->source_value_id = (uint32_t) payload;
            }
            prior_kind = root->kind;
            prior_payload = (uint32_t) payload;
            have_prior = true;
        }
        uint64_t row_count = take_uvar(&reader);
        if (row_count > function->value_count || !spend(context, row_count, location)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
            return false;
        }
        function->value_root_sets = xr_calloc(function->value_count ? function->value_count : 1u,
                                              sizeof(XrValidatedValueRootSet));
        if (!function->value_root_sets) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
            return false;
        }
        uint32_t prior_value = 0u;
        bool have_value = false;
        for (uint32_t row = 0; row < (uint32_t) row_count; ++row) {
            uint64_t value_id = take_uvar(&reader);
            uint64_t value_root_count = take_uvar(&reader);
            location.value_id =
                value_id <= UINT32_MAX ? (uint32_t) value_id : XR_PROGRAM_LOCATION_NONE;
            if (value_id >= function->value_count || (have_value && value_id <= prior_value) ||
                !type_is_view(context->program, function->value_types[value_id]) ||
                value_root_count == 0u || value_root_count > XR_PROGRAM_LIMIT_ROOTS_PER_VALUE ||
                value_root_count > SIZE_MAX / sizeof(uint32_t) ||
                !spend(context, value_root_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            XrValidatedValueRootSet *set = &function->value_root_sets[value_id];
            set->root_count = (uint32_t) value_root_count;
            set->root_ids = xr_calloc((size_t) value_root_count, sizeof(uint32_t));
            if (!set->root_ids) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
                return false;
            }
            uint32_t prior_root = 0u;
            for (uint32_t index = 0; index < (uint32_t) value_root_count; ++index) {
                uint64_t root_id = take_uvar(&reader);
                if (root_id >= function->root_count || (index != 0u && root_id <= prior_root)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                    return false;
                }
                set->root_ids[index] = (uint32_t) root_id;
                prior_root = (uint32_t) root_id;
            }
            prior_value = (uint32_t) value_id;
            have_value = true;
        }
        for (uint32_t value_id = 0; value_id < function->value_count; ++value_id) {
            bool view_value = type_is_view(context->program, function->value_types[value_id]);
            if (view_value != (function->value_root_sets[value_id].root_count != 0u)) {
                location.value_id = value_id;
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
        }
        for (uint32_t origin = 0; origin < function->result_borrow_origin_count; ++origin) {
            if (root_for_origin(function, &function->result_borrow_origins[origin]) ==
                XR_PROGRAM_LOCATION_NONE) {
                location.value_id = origin;
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
        }
        const XrValidatedBlock *entry = &function->blocks[function->entry_block];
        for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            uint32_t value_id = entry->argument_ids[parameter];
            if (!type_is_view(context->program, function->value_types[value_id]))
                continue;
            XrViewOrigin origin = {
                .kind = function->has_receiver && parameter == 0u ? XR_VIEW_ORIGIN_RECEIVER
                                                                  : XR_VIEW_ORIGIN_PARAM,
                .param_ordinal = function->has_receiver && parameter != 0u
                                     ? (int16_t) (parameter - 1u)
                                     : (int16_t) parameter,
            };
            if (origin.kind == XR_VIEW_ORIGIN_RECEIVER)
                origin.param_ordinal = -1;
            uint32_t root_id = root_for_origin(function, &origin);
            const XrValidatedValueRootSet *set = &function->value_root_sets[value_id];
            if (root_id == XR_PROGRAM_LOCATION_NONE || set->root_count != 1u ||
                set->root_ids[0] != root_id) {
                location.value_id = value_id;
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
        }
    }
    if (!reader_done(&reader)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_STRUCTURAL, location);
        return false;
    }
    return true;
}

static bool value_is_available(const XrValidatedFunction *function, uint32_t block_id,
                               uint32_t instruction_id, uint32_t value_id) {
    if (value_id >= function->value_count)
        return false;
    return function->value_blocks[value_id] == block_id &&
           function->value_positions[value_id] <= instruction_id;
}

static bool instruction_is_terminator(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_BRANCH ||
           operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH ||
           operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
           operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
           operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE ||
           operation_id == XR_CORE_OP_CORE_RETURN || operation_id == XR_CORE_OP_CORE_TRAP ||
           operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ||
           operation_id == XR_CORE_OP_CORE_PANIC_PUBLISH;
}

static bool expect_shape(VerifyContext *context, const XrValidatedInstruction *instruction,
                         XrProgramSemanticLocation location, uint32_t operands, uint32_t successors,
                         XrCoreIrImmediateKind immediate_kind, uint16_t result_type,
                         bool has_result) {
    if (instruction->operand_count != operands || instruction->successor_count != successors) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    if (instruction->immediate_kind != immediate_kind) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    if ((instruction->result_id != XR_PROGRAM_LOCATION_NONE) != has_result ||
        instruction->result_type_id != result_type) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    return true;
}

static bool operand_type_is(const XrValidatedFunction *function,
                            const XrValidatedInstruction *instruction, uint32_t operand,
                            uint16_t type_id) {
    return operand < instruction->operand_count &&
           function->value_types[instruction->operands[operand]] == type_id;
}

static bool operand_category_is(const XrValidatedFunction *function,
                                const XrValidatedInstruction *instruction, uint32_t operand,
                                XrCoreIrValueCategory category) {
    return operand < instruction->operand_count &&
           function->value_categories[instruction->operands[operand]] == category;
}

static bool operand_ownership_is(const XrValidatedFunction *function,
                                 const XrValidatedInstruction *instruction, uint32_t operand,
                                 XrCoreIrOwnershipDisposition ownership) {
    return operand < instruction->operand_count &&
           function->value_ownerships[instruction->operands[operand]] == ownership;
}

static bool call_operand_ownership_is(const XrValidatedProgram *program,
                                      const XrValidatedFunction *caller,
                                      const XrValidatedInstruction *instruction, uint32_t operand,
                                      XrParamMode mode, uint16_t type_id) {
    if (operand >= instruction->operand_count)
        return false;
    XrCoreIrOwnershipDisposition actual = caller->value_ownerships[instruction->operands[operand]];
    if (mode == XR_PARAM_MOVE)
        return actual == (xr_validated_program_type_ownership(program, type_id) ==
                                  XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                              ? XR_CORE_IR_OWNER
                              : XR_CORE_IR_NON_OWNER);
    if (mode == XR_PARAM_READ &&
        xr_validated_program_type_ownership(program, type_id) == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE)
        return actual == XR_CORE_IR_OWNER || actual == XR_CORE_IR_NON_OWNER;
    return actual == XR_CORE_IR_NON_OWNER;
}

static XrCoreIrOwnershipDisposition ownership_for_type(const XrValidatedProgram *program,
                                                       uint16_t type_id) {
    return xr_validated_program_type_ownership(program, type_id) == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
               ? XR_CORE_IR_OWNER
               : XR_CORE_IR_NON_OWNER;
}

static bool value_ownership_contract_is_valid(const XrValidatedProgram *program, uint16_t type_id,
                                              XrCoreIrValueCategory category,
                                              XrCoreIrOwnershipDisposition ownership) {
    if (ownership > XR_CORE_IR_OWNER)
        return false;
    if (category == XR_CORE_IR_PLACE)
        return ownership == XR_CORE_IR_NON_OWNER;
    return ownership != XR_CORE_IR_OWNER || xr_validated_program_type_ownership(program, type_id) ==
                                                XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
}

static bool root_set_contains(const XrValidatedValueRootSet *set, uint32_t root_id) {
    if (!set)
        return false;
    for (uint32_t index = 0; index < set->root_count; ++index)
        if (set->root_ids[index] == root_id)
            return true;
    return false;
}

static bool root_set_is_subset(const XrValidatedValueRootSet *source,
                               const XrValidatedValueRootSet *target) {
    if (!source || !target)
        return false;
    for (uint32_t index = 0; index < source->root_count; ++index)
        if (!root_set_contains(target, source->root_ids[index]))
            return false;
    return true;
}

static uint32_t root_for_source_value(const XrValidatedFunction *function, uint32_t value_id) {
    const XrValidatedBlock *entry = &function->blocks[function->entry_block];
    for (uint32_t parameter = 0; parameter < entry->argument_count; ++parameter) {
        if (entry->argument_ids[parameter] != value_id)
            continue;
        XrViewOrigin origin = {
            .kind = function->has_receiver && parameter == 0u ? XR_VIEW_ORIGIN_RECEIVER
                                                              : XR_VIEW_ORIGIN_PARAM,
            .param_ordinal = function->has_receiver && parameter != 0u ? (int16_t) (parameter - 1u)
                                                                       : (int16_t) parameter,
        };
        if (origin.kind == XR_VIEW_ORIGIN_RECEIVER)
            origin.param_ordinal = -1;
        return root_for_origin(function, &origin);
    }
    for (uint32_t root = 0; root < function->root_count; ++root)
        if (function->roots[root].kind == XR_CORE_IR_ROOT_LOCAL &&
            function->roots[root].source_value_id == value_id)
            return root;
    return XR_PROGRAM_LOCATION_NONE;
}

static uint32_t call_operand_prefix(uint16_t operation_id) {
    return operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
                   operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE
               ? 1u
               : 0u;
}

static bool mapped_call_result_roots_match(const XrValidatedProgram *program,
                                           const XrValidatedFunction *caller,
                                           const XrValidatedSignature *callee,
                                           const XrValidatedInstruction *instruction,
                                           uint32_t result_value_id) {
    if (!type_is_view(program, callee->result_type_id))
        return true;
    if (result_value_id >= caller->value_count || !caller->value_root_sets)
        return false;
    const XrValidatedValueRootSet *actual = &caller->value_root_sets[result_value_id];
    bool *expected = xr_calloc(caller->root_count ? caller->root_count : 1u, sizeof(bool));
    if (!expected)
        return false;
    bool valid = true;
    uint32_t expected_count = 0u;
    for (uint32_t index = 0; valid && index < callee->result_borrow_origin_count; ++index) {
        const XrViewOrigin *origin = &callee->result_borrow_origins[index];
        if (origin->kind == XR_VIEW_ORIGIN_STATIC) {
            uint32_t root = root_for_origin(caller, origin);
            if (root == XR_PROGRAM_LOCATION_NONE)
                valid = false;
            else if (!expected[root]) {
                expected[root] = true;
                ++expected_count;
            }
            continue;
        }
        uint32_t operand =
            call_operand_prefix(instruction->operation_id) +
            (origin->kind == XR_VIEW_ORIGIN_RECEIVER
                 ? 0u
                 : (uint32_t) origin->param_ordinal + (callee->has_receiver ? 1u : 0u));
        if (operand >= instruction->operand_count) {
            valid = false;
            break;
        }
        uint32_t value_id = instruction->operands[operand];
        if (type_is_view(program, caller->value_types[value_id])) {
            const XrValidatedValueRootSet *source = &caller->value_root_sets[value_id];
            if (source->root_count == 0u) {
                valid = false;
                break;
            }
            for (uint32_t root_index = 0; root_index < source->root_count; ++root_index) {
                uint32_t root = source->root_ids[root_index];
                if (!expected[root]) {
                    expected[root] = true;
                    ++expected_count;
                }
            }
        } else {
            uint32_t root = root_for_source_value(caller, value_id);
            if (root == XR_PROGRAM_LOCATION_NONE)
                valid = false;
            else if (!expected[root]) {
                expected[root] = true;
                ++expected_count;
            }
        }
    }
    valid = valid && actual->root_count == expected_count;
    for (uint32_t index = 0; valid && index < actual->root_count; ++index)
        valid = actual->root_ids[index] < caller->root_count && expected[actual->root_ids[index]];
    xr_free(expected);
    return valid;
}

static const XrValidatedSignature *
witness_call_signature(const XrValidatedProgram *program, const XrValidatedFunction *caller,
                       const XrValidatedInstruction *instruction) {
    if (!program || !caller || !instruction || instruction->operand_count == 0u ||
        instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_U32)
        return NULL;
    uint32_t receiver_value = instruction->operands[0];
    if (receiver_value >= caller->value_count)
        return NULL;
    uint16_t receiver_type_id = caller->value_types[receiver_value];
    const XrValidatedType *receiver = xr_validated_program_type(program, receiver_type_id);
    if (!receiver || receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
        receiver->interface_id >= program->interface_count)
        return NULL;
    const XrValidatedInterface *interface = &program->interfaces[receiver->interface_id];
    if (instruction->immediate.u32 >= interface->slot_count)
        return NULL;
    uint32_t signature_id = interface->slot_signature_ids[instruction->immediate.u32];
    if (signature_id >= program->signature_count)
        return NULL;
    const XrValidatedSignature *signature = &program->signatures[signature_id];
    if (!signature->has_receiver || signature->parameter_count == 0u ||
        signature->parameter_types[0] != receiver_type_id)
        return NULL;
    return signature;
}

static const XrValidatedSignature *
callable_call_signature(const XrValidatedProgram *program, const XrValidatedFunction *caller,
                        const XrValidatedInstruction *instruction) {
    if (!program || !caller || !instruction || instruction->operand_count == 0u ||
        instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE)
        return NULL;
    uint32_t callable_value = instruction->operands[0];
    if (callable_value >= caller->value_count)
        return NULL;
    const XrValidatedType *callable =
        xr_validated_program_type(program, caller->value_types[callable_value]);
    if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= program->signature_count)
        return NULL;
    const XrValidatedSignature *signature = &program->signatures[callable->signature_id];
    return signature->has_receiver ? NULL : signature;
}

static bool borrow_origins_equal(const XrValidatedSignature *visible,
                                 const XrValidatedSignature *target) {
    if (visible->result_borrow_origin_count != target->result_borrow_origin_count)
        return false;
    for (uint32_t index = 0; index < visible->result_borrow_origin_count; ++index)
        if (visible->result_borrow_origins[index].kind !=
                target->result_borrow_origins[index].kind ||
            visible->result_borrow_origins[index].param_ordinal !=
                target->result_borrow_origins[index].param_ordinal)
            return false;
    return true;
}

static bool callable_target_matches(const XrValidatedProgram *program,
                                    const XrValidatedType *callable,
                                    const XrValidatedFunction *target, uint16_t capture_type) {
    if (!program || !callable || !target || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
        callable->signature_id >= program->signature_count ||
        target->signature_id >= program->signature_count)
        return false;
    const XrValidatedSignature *visible = &program->signatures[callable->signature_id];
    const XrValidatedSignature *implementation = &program->signatures[target->signature_id];
    uint32_t receiver_count = capture_type == XR_CORE_TYPE_VOID ? 0u : 1u;
    if (visible->has_receiver || implementation->has_receiver != (receiver_count != 0u) ||
        implementation->parameter_count != visible->parameter_count + receiver_count ||
        (receiver_count != 0u && (implementation->receiver_mode != XR_PARAM_READ ||
                                  implementation->parameter_types[0] != capture_type ||
                                  implementation->parameter_modes[0] != XR_PARAM_READ)) ||
        implementation->result_type_id != visible->result_type_id ||
        implementation->result_ownership != visible->result_ownership ||
        implementation->error_type_id != visible->error_type_id ||
        implementation->panic_type_id != visible->panic_type_id ||
        implementation->effect_mask != visible->effect_mask ||
        implementation->capability_mask != visible->capability_mask ||
        !borrow_origins_equal(visible, implementation))
        return false;
    for (uint32_t parameter = 0; parameter < visible->parameter_count; ++parameter)
        if (implementation->parameter_types[parameter + receiver_count] !=
                visible->parameter_types[parameter] ||
            implementation->parameter_modes[parameter + receiver_count] !=
                visible->parameter_modes[parameter])
            return false;
    for (uint32_t origin = 0; origin < implementation->result_borrow_origin_count; ++origin)
        if (implementation->result_borrow_origins[origin].kind == XR_VIEW_ORIGIN_RECEIVER)
            return false;
    return true;
}

static const XrValidatedSignature *
operation_call_signature(const XrValidatedProgram *program, const XrValidatedFunction *caller,
                         const XrValidatedInstruction *instruction) {
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE)
        return callable_call_signature(program, caller, instruction);
    if (instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE)
        return witness_call_signature(program, caller, instruction);
    if ((instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_DIRECT &&
         instruction->operation_id != XR_CORE_OP_CORE_CALL_SEALED_INVOKE) ||
        instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
        instruction->immediate.function_id >= program->function_count)
        return NULL;
    const XrValidatedFunction *callee = &program->functions[instruction->immediate.function_id];
    return callee->signature_id < program->signature_count
               ? &program->signatures[callee->signature_id]
               : NULL;
}

static bool verify_successor_arguments(VerifyContext *context, const XrValidatedFunction *function,
                                       const XrValidatedInstruction *instruction,
                                       uint32_t successor_index, uint32_t operand_start,
                                       XrProgramSemanticLocation location) {
    if (successor_index >= instruction->successor_count ||
        instruction->successors[successor_index] >= function->block_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
        return false;
    }
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor_index]];
    if (operand_start > instruction->operand_count ||
        target->argument_count > instruction->operand_count - operand_start) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    for (uint32_t index = 0; index < target->argument_count; ++index) {
        if (!operand_type_is(function, instruction, operand_start + index,
                             target->argument_types[index]) ||
            !operand_category_is(function, instruction, operand_start + index,
                                 target->argument_categories[index]) ||
            !operand_ownership_is(function, instruction, operand_start + index,
                                  target->argument_ownerships[index])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
        uint32_t source_value = instruction->operands[operand_start + index];
        uint32_t target_value = target->argument_ids[index];
        if (type_is_view(context->program, target->argument_types[index]) &&
            !root_set_is_subset(&function->value_root_sets[source_value],
                                &function->value_root_sets[target_value])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
            return false;
        }
    }
    return true;
}

static bool verify_successor_argument_suffix(VerifyContext *context,
                                             const XrValidatedFunction *function,
                                             const XrValidatedInstruction *instruction,
                                             uint32_t successor_index, uint32_t target_start,
                                             uint32_t operand_start,
                                             XrProgramSemanticLocation location) {
    if (successor_index >= instruction->successor_count ||
        instruction->successors[successor_index] >= function->block_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
        return false;
    }
    const XrValidatedBlock *target = &function->blocks[instruction->successors[successor_index]];
    if (target_start > target->argument_count || operand_start > instruction->operand_count ||
        target->argument_count - target_start > instruction->operand_count - operand_start) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    for (uint32_t index = target_start; index < target->argument_count; ++index) {
        uint32_t operand = operand_start + index - target_start;
        if (!operand_type_is(function, instruction, operand, target->argument_types[index]) ||
            !operand_category_is(function, instruction, operand,
                                 target->argument_categories[index]) ||
            !operand_ownership_is(function, instruction, operand,
                                  target->argument_ownerships[index])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
        uint32_t source_value = instruction->operands[operand];
        uint32_t target_value = target->argument_ids[index];
        if (type_is_view(context->program, target->argument_types[index]) &&
            !root_set_is_subset(&function->value_root_sets[source_value],
                                &function->value_root_sets[target_value])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
            return false;
        }
    }
    return true;
}

static bool witness_arguments_match(const XrValidatedProgram *program,
                                    const XrValidatedFunction *caller,
                                    const XrValidatedInstruction *instruction,
                                    const XrValidatedSignature *signature) {
    if (!signature || instruction->operand_count < signature->parameter_count)
        return false;
    for (uint32_t argument = 0; argument < signature->parameter_count; ++argument) {
        XrCoreIrValueCategory expected_category =
            argument == 0u
                ? XR_CORE_IR_VALUE
                : (signature->parameter_modes[argument] == XR_PARAM_REF ? XR_CORE_IR_PLACE
                                                                        : XR_CORE_IR_VALUE);
        bool ownership_matches =
            argument == 0u
                ? call_operand_ownership_is(program, caller, instruction, argument,
                                            signature->parameter_modes[argument] == XR_PARAM_MOVE
                                                ? XR_PARAM_MOVE
                                                : XR_PARAM_READ,
                                            signature->parameter_types[argument])
                : call_operand_ownership_is(program, caller, instruction, argument,
                                            signature->parameter_modes[argument],
                                            signature->parameter_types[argument]);
        if (!operand_type_is(caller, instruction, argument, signature->parameter_types[argument]) ||
            !operand_category_is(caller, instruction, argument, expected_category) ||
            !ownership_matches)
            return false;
    }
    return true;
}

static bool callable_arguments_match(const XrValidatedProgram *program,
                                     const XrValidatedFunction *caller,
                                     const XrValidatedInstruction *instruction,
                                     const XrValidatedSignature *signature) {
    if (!signature || instruction->operand_count < signature->parameter_count + 1u ||
        !operand_category_is(caller, instruction, 0u, XR_CORE_IR_VALUE) ||
        !operand_ownership_is(caller, instruction, 0u, XR_CORE_IR_OWNER))
        return false;
    for (uint32_t argument = 0; argument < signature->parameter_count; ++argument) {
        uint32_t operand = argument + 1u;
        if (!operand_type_is(caller, instruction, operand, signature->parameter_types[argument]) ||
            !operand_category_is(caller, instruction, operand,
                                 signature->parameter_modes[argument] == XR_PARAM_REF
                                     ? XR_CORE_IR_PLACE
                                     : XR_CORE_IR_VALUE) ||
            !call_operand_ownership_is(program, caller, instruction, operand,
                                       signature->parameter_modes[argument],
                                       signature->parameter_types[argument]))
            return false;
    }
    return true;
}

static bool verify_witness_invoke(VerifyContext *context, XrValidatedFunction *function,
                                  XrValidatedInstruction *instruction,
                                  XrProgramSemanticLocation location, uint32_t *local_effects) {
    const XrValidatedSignature *callee =
        witness_call_signature(context->program, function, instruction);
    if (!callee || instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
        instruction->result_type_id != XR_CORE_TYPE_VOID) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    uint32_t expected_successors = 1u + (has_error ? 1u : 0u) + (has_panic ? 1u : 0u);
    if ((!has_error && !has_panic) || instruction->successor_count != expected_successors ||
        !witness_arguments_match(context->program, function, instruction, callee)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    for (uint32_t successor = 0u; successor < expected_successors; ++successor) {
        if (instruction->successors[successor] >= function->block_count) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    if (normal->argument_count < normal_implicit ||
        (normal_implicit != 0u && (normal->argument_types[0] != callee->result_type_id ||
                                   normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                                   normal->argument_ownerships[0] != callee->result_ownership))) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    uint32_t error_successor = has_error ? 1u : UINT32_MAX;
    uint32_t panic_successor = has_panic ? 1u + (has_error ? 1u : 0u) : UINT32_MAX;
    if (has_error) {
        const XrValidatedBlock *error = &function->blocks[instruction->successors[error_successor]];
        if (error->argument_count == 0u || error->argument_types[0] != callee->error_type_id ||
            error->argument_categories[0] != XR_CORE_IR_VALUE ||
            error->argument_ownerships[0] !=
                ownership_for_type(context->program, callee->error_type_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    if (has_panic) {
        const XrValidatedBlock *panic = &function->blocks[instruction->successors[panic_successor]];
        if (panic->argument_count == 0u || panic->argument_types[0] != callee->panic_type_id ||
            panic->argument_categories[0] != XR_CORE_IR_VALUE ||
            panic->argument_ownerships[0] !=
                ownership_for_type(context->program, callee->panic_type_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    if (normal_implicit != 0u &&
        !mapped_call_result_roots_match(context->program, function, callee, instruction,
                                        normal->argument_ids[0])) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
        return false;
    }
    uint32_t operand = callee->parameter_count;
    if (!verify_successor_argument_suffix(context, function, instruction, 0u, normal_implicit,
                                          operand, location))
        return false;
    operand += normal->argument_count - normal_implicit;
    if (has_error) {
        const XrValidatedBlock *error = &function->blocks[instruction->successors[error_successor]];
        if (!verify_successor_argument_suffix(context, function, instruction, error_successor, 1u,
                                              operand, location))
            return false;
        operand += error->argument_count - 1u;
    }
    if (has_panic) {
        const XrValidatedBlock *panic = &function->blocks[instruction->successors[panic_successor]];
        if (!verify_successor_argument_suffix(context, function, instruction, panic_successor, 1u,
                                              operand, location))
            return false;
        operand += panic->argument_count - 1u;
    }
    if (instruction->operand_count != operand) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    uint32_t escaping_effects = callee->effect_mask;
    if (has_error)
        escaping_effects &= ~XR_CORE_EFFECT_ERROR;
    if (has_panic)
        escaping_effects &= ~XR_CORE_EFFECT_PANIC;
    *local_effects |= escaping_effects;
    if ((function->effect_mask & escaping_effects) != escaping_effects) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
        return false;
    }
    if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
        return false;
    }
    return true;
}

static bool verify_callable_invoke(VerifyContext *context, XrValidatedFunction *function,
                                   XrValidatedInstruction *instruction,
                                   XrProgramSemanticLocation location, uint32_t *local_effects) {
    const XrValidatedSignature *callee =
        callable_call_signature(context->program, function, instruction);
    if (!callee || instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
        instruction->result_type_id != XR_CORE_TYPE_VOID) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
        return false;
    }
    bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
    bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
    uint32_t expected_successors = 1u + (has_error ? 1u : 0u) + (has_panic ? 1u : 0u);
    if ((!has_error && !has_panic) || instruction->successor_count != expected_successors ||
        !callable_arguments_match(context->program, function, instruction, callee)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    for (uint32_t successor = 0u; successor < expected_successors; ++successor) {
        if (instruction->successors[successor] >= function->block_count) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
    uint32_t normal_implicit = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
    if (normal->argument_count < normal_implicit ||
        (normal_implicit != 0u && (normal->argument_types[0] != callee->result_type_id ||
                                   normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                                   normal->argument_ownerships[0] != callee->result_ownership))) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    uint32_t error_successor = has_error ? 1u : UINT32_MAX;
    uint32_t panic_successor = has_panic ? 1u + (has_error ? 1u : 0u) : UINT32_MAX;
    if (has_error) {
        const XrValidatedBlock *error = &function->blocks[instruction->successors[error_successor]];
        if (error->argument_count == 0u || error->argument_types[0] != callee->error_type_id ||
            error->argument_categories[0] != XR_CORE_IR_VALUE ||
            error->argument_ownerships[0] !=
                ownership_for_type(context->program, callee->error_type_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    if (has_panic) {
        const XrValidatedBlock *panic = &function->blocks[instruction->successors[panic_successor]];
        if (panic->argument_count == 0u || panic->argument_types[0] != callee->panic_type_id ||
            panic->argument_categories[0] != XR_CORE_IR_VALUE ||
            panic->argument_ownerships[0] !=
                ownership_for_type(context->program, callee->panic_type_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    if (normal_implicit != 0u &&
        !mapped_call_result_roots_match(context->program, function, callee, instruction,
                                        normal->argument_ids[0])) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
        return false;
    }
    uint32_t operand = callee->parameter_count + 1u;
    if (!verify_successor_argument_suffix(context, function, instruction, 0u, normal_implicit,
                                          operand, location))
        return false;
    operand += normal->argument_count - normal_implicit;
    if (has_error) {
        const XrValidatedBlock *error = &function->blocks[instruction->successors[error_successor]];
        if (!verify_successor_argument_suffix(context, function, instruction, error_successor, 1u,
                                              operand, location))
            return false;
        operand += error->argument_count - 1u;
    }
    if (has_panic) {
        const XrValidatedBlock *panic = &function->blocks[instruction->successors[panic_successor]];
        if (!verify_successor_argument_suffix(context, function, instruction, panic_successor, 1u,
                                              operand, location))
            return false;
        operand += panic->argument_count - 1u;
    }
    if (instruction->operand_count != operand) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    uint32_t escaping_effects = callee->effect_mask;
    if (has_error)
        escaping_effects &= ~XR_CORE_EFFECT_ERROR;
    if (has_panic)
        escaping_effects &= ~XR_CORE_EFFECT_PANIC;
    *local_effects |= escaping_effects;
    if ((function->effect_mask & escaping_effects) != escaping_effects) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
        return false;
    }
    if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
        return false;
    }
    return true;
}

static bool operation_consumes_operand(const VerifyContext *context,
                                       const XrValidatedFunction *function,
                                       const XrValidatedInstruction *instruction,
                                       uint32_t operand_index) {
    if ((instruction->operation_id == XR_CORE_OP_CORE_OWNER_MOVE ||
         instruction->operation_id == XR_CORE_OP_CORE_OWNER_DROP ||
         instruction->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL) &&
        operand_index == 0u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_PLACE_STORE && operand_index == 1u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_RETURN && operand_index == 0u)
        return true;
    if ((instruction->operation_id == XR_CORE_OP_CORE_ERROR_PUBLISH ||
         instruction->operation_id == XR_CORE_OP_CORE_PANIC_PUBLISH) &&
        operand_index == 0u)
        return true;
    if (instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK && operand_index == 0u) {
        const XrValidatedType *result =
            xr_validated_program_type(context->program, instruction->result_type_id);
        return result && result->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
               (result->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                result->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE) &&
               function->value_ownerships[instruction->operands[0]] == XR_CORE_IR_OWNER;
    }
    if (instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PROJECT && operand_index == 0u)
        return function->value_ownerships[instruction->operands[0]] == XR_CORE_IR_OWNER;
    if (instruction->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK && operand_index == 0u)
        return true;
    const XrValidatedSignature *callee =
        operation_call_signature(context->program, function, instruction);
    if (!callee)
        return false;
    uint32_t prefix = call_operand_prefix(instruction->operation_id);
    return operand_index >= prefix && operand_index - prefix < callee->parameter_count &&
           callee->parameter_modes[operand_index - prefix] == XR_PARAM_MOVE;
}

static uint32_t owner_occurrences_on_successor_edge(const VerifyContext *context,
                                                    const XrValidatedFunction *function,
                                                    const XrValidatedInstruction *terminator,
                                                    uint32_t successor_index, uint32_t value_id) {
    uint32_t start = 0u;
    if (terminator->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH) {
        start = 1u;
        if (successor_index != 0u)
            start += function->blocks[terminator->successors[0]].argument_count;
    } else if (terminator->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
               terminator->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
               terminator->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
        const XrValidatedSignature *callee =
            operation_call_signature(context->program, function, terminator);
        if (!callee)
            return 0u;
        start = call_operand_prefix(terminator->operation_id) + callee->parameter_count;
        for (uint32_t prior = 0u; prior < successor_index; ++prior) {
            uint32_t implicit =
                prior == 0u ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) : 1u;
            const XrValidatedBlock *target = &function->blocks[terminator->successors[prior]];
            if (target->argument_count < implicit)
                return 0u;
            start += target->argument_count - implicit;
        }
        uint32_t implicit =
            successor_index == 0u ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) : 1u;
        const XrValidatedBlock *target = &function->blocks[terminator->successors[successor_index]];
        if (target->argument_count < implicit)
            return 0u;
        uint32_t count = target->argument_count - implicit;
        uint32_t occurrences = 0u;
        for (uint32_t index = 0; index < count; ++index)
            occurrences += terminator->operands[start + index] == value_id;
        return occurrences;
    }
    uint32_t count = function->blocks[terminator->successors[successor_index]].argument_count;
    uint32_t occurrences = 0u;
    for (uint32_t index = 0; index < count; ++index)
        occurrences += terminator->operands[start + index] == value_id;
    return occurrences;
}

static bool verify_owner_block_closure(VerifyContext *context, uint32_t function_id,
                                       uint32_t block_id, const bool *consumed) {
    XrValidatedFunction *function = &context->program->functions[function_id];
    const XrValidatedBlock *block = &function->blocks[block_id];
    const XrValidatedInstruction *terminator = &block->instructions[block->instruction_count - 1u];
    bool transfers = terminator->operation_id == XR_CORE_OP_CORE_BRANCH ||
                     terminator->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH ||
                     terminator->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
                     terminator->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
                     terminator->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE;
    for (uint32_t value = 0; value < function->value_count; ++value) {
        if (function->value_blocks[value] != block_id ||
            function->value_ownerships[value] != XR_CORE_IR_OWNER || consumed[value])
            continue;
        XrProgramSemanticLocation location = {
            .section_id = XR_PROGRAM_SECTION_CODE,
            .function_id = function_id,
            .block_id = block_id,
            .instruction_id = block->instruction_count - 1u,
            .value_id = value,
        };
        if (!transfers) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
        for (uint32_t successor = 0; successor < terminator->successor_count; ++successor) {
            if (owner_occurrences_on_successor_edge(context, function, terminator, successor,
                                                    value) != 1u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
                return false;
            }
        }
    }
    return true;
}

static bool verify_operation(VerifyContext *context, uint32_t function_id, uint32_t block_id,
                             uint32_t instruction_id, uint32_t *local_effects,
                             uint32_t *local_capabilities, bool *consumed) {
    XrValidatedFunction *function = &context->program->functions[function_id];
    XrValidatedInstruction *instruction = &function->blocks[block_id].instructions[instruction_id];
    XrProgramSemanticLocation location = {
        .section_id = XR_PROGRAM_SECTION_CODE,
        .function_id = function_id,
        .block_id = block_id,
        .instruction_id = instruction_id,
        .value_id = instruction->result_id,
    };
    const XrCoreOperationSpec *spec = xr_core_spec_operation_by_id(instruction->operation_id);
    if (!spec) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
        return false;
    }
    *local_effects |= spec->effect_mask;
    *local_capabilities |= spec->capability_mask;
    XrCoreIrValueCategory expected_result_category =
        instruction->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL ? XR_CORE_IR_PLACE
                                                                 : XR_CORE_IR_VALUE;
    if (instruction->result_category != expected_result_category) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    bool ownership_result_operation =
        instruction->operation_id == XR_CORE_OP_CORE_OWNER_COPY ||
        instruction->operation_id == XR_CORE_OP_CORE_OWNER_MOVE ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
        instruction->operation_id == XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT ||
        instruction->operation_id == XR_CORE_OP_CORE_VARIANT_CONSTRUCT ||
        instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
        instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PROJECT ||
        instruction->operation_id == XR_CORE_OP_CORE_CALLABLE_PACK;
    if (instruction->result_ownership == XR_CORE_IR_OWNER && !ownership_result_operation) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
        return false;
    }
    bool category_polymorphic = instruction->operation_id == XR_CORE_OP_CORE_BLOCK_ARGUMENT ||
                                instruction->operation_id == XR_CORE_OP_CORE_BRANCH ||
                                instruction->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_DIRECT ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_DIRECT ||
                                instruction->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE ||
                                instruction->operation_id == XR_CORE_OP_CORE_PLACE_LOCAL ||
                                instruction->operation_id == XR_CORE_OP_CORE_PLACE_LOAD ||
                                instruction->operation_id == XR_CORE_OP_CORE_PLACE_STORE ||
                                instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PACK ||
                                instruction->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_PROJECT;
    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
        location.value_id = instruction->operands[operand];
        if (!value_is_available(function, block_id, instruction_id,
                                instruction->operands[operand])) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
        if (consumed[instruction->operands[operand]]) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
            return false;
        }
        if (operation_consumes_operand(context, function, instruction, operand))
            consumed[instruction->operands[operand]] = true;
        if (!category_polymorphic &&
            !operand_category_is(function, instruction, operand, XR_CORE_IR_VALUE)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
            return false;
        }
    }
    location.value_id = instruction->result_id;
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_CONSTANT_I64:
        case XR_CORE_OP_CORE_CONSTANT_BOOL: {
            uint16_t expected = instruction->operation_id == XR_CORE_OP_CORE_CONSTANT_I64
                                    ? XR_CORE_TYPE_I64
                                    : XR_CORE_TYPE_BOOL;
            if (!expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_CONSTANT,
                              expected, true) ||
                instruction->immediate.constant_id >= context->program->constant_count ||
                context->program->constants[instruction->immediate.constant_id].type_id !=
                    expected) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_ADD_I64:
        case XR_CORE_OP_CORE_SUB_I64:
        case XR_CORE_OP_CORE_MUL_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_I64, true) ||
                instruction->immediate.u32 > 1u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_DIV_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_I64, true) ||
                instruction->immediate.u32 != 0u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_COMPARE_I64:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_BOOL, true) ||
                instruction->immediate.u32 > 5u ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_I64) ||
                !operand_type_is(function, instruction, 1, XR_CORE_TYPE_I64)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_BLOCK_ARGUMENT: {
            const XrValidatedBlock *block = &function->blocks[block_id];
            if (!expect_shape(context, instruction, location, block->argument_count, 0,
                              XR_CORE_IR_IMMEDIATE_NONE, XR_CORE_TYPE_VOID, false))
                return false;
            for (uint32_t index = 0; index < block->argument_count; ++index) {
                if (instruction->operands[index] != block->argument_ids[index]) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_VALUE_USE, location);
                    return false;
                }
            }
            if (instruction_id != 0u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_BRANCH: {
            if (instruction->successor_count != 1u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_VOID ||
                instruction->successors[0] >= function->block_count ||
                instruction->operand_count !=
                    function->blocks[instruction->successors[0]].argument_count ||
                !verify_successor_arguments(context, function, instruction, 0, 0, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CONDITIONAL_BRANCH: {
            if (instruction->successor_count != 2u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_VOID ||
                instruction->successors[0] >= function->block_count ||
                instruction->successors[1] >= function->block_count ||
                instruction->operand_count !=
                    1u + function->blocks[instruction->successors[0]].argument_count +
                        function->blocks[instruction->successors[1]].argument_count ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_BOOL) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_VALUE) ||
                !verify_successor_arguments(context, function, instruction, 0, 1u, location) ||
                !verify_successor_arguments(
                    context, function, instruction, 1,
                    1u + function->blocks[instruction->successors[0]].argument_count, location)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_RETURN: {
            uint32_t expected = function->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
            if (!expect_shape(context, instruction, location, expected, 0,
                              XR_CORE_IR_IMMEDIATE_NONE, XR_CORE_TYPE_VOID, false) ||
                (expected == 1u &&
                 (!operand_type_is(function, instruction, 0, function->result_type_id) ||
                  !operand_ownership_is(function, instruction, 0, function->result_ownership)))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            if (expected == 1u && type_is_view(context->program, function->result_type_id)) {
                const XrValidatedValueRootSet *set =
                    &function->value_root_sets[instruction->operands[0]];
                for (uint32_t root = 0; root < set->root_count; ++root) {
                    bool allowed = false;
                    for (uint32_t origin = 0; origin < function->result_borrow_origin_count;
                         ++origin)
                        allowed |=
                            root_for_origin(function, &function->result_borrow_origins[origin]) ==
                            set->root_ids[root];
                    if (!allowed) {
                        reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                        return false;
                    }
                }
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_SEALED_DIRECT: {
            if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
                instruction->successor_count != 0u ||
                instruction->immediate.function_id >= context->program->function_count) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            const XrValidatedFunction *callee =
                &context->program->functions[instruction->immediate.function_id];
            if (callee->error_type_id != XR_CORE_TYPE_VOID ||
                callee->panic_type_id != XR_CORE_TYPE_VOID ||
                instruction->operand_count != callee->parameter_count ||
                (callee->result_type_id == XR_CORE_TYPE_VOID) !=
                    (instruction->result_id == XR_PROGRAM_LOCATION_NONE) ||
                instruction->result_type_id != callee->result_type_id ||
                instruction->result_ownership != callee->result_ownership) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            for (uint32_t argument = 0; argument < callee->parameter_count; ++argument) {
                if (!operand_type_is(function, instruction, argument,
                                     callee->parameter_types[argument]) ||
                    !operand_category_is(function, instruction, argument,
                                         callee->parameter_modes[argument] == XR_PARAM_REF
                                             ? XR_CORE_IR_PLACE
                                             : XR_CORE_IR_VALUE) ||
                    !call_operand_ownership_is(context->program, function, instruction, argument,
                                               callee->parameter_modes[argument],
                                               callee->parameter_types[argument])) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            if (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
                !mapped_call_result_roots_match(context->program, function,
                                                &context->program->signatures[callee->signature_id],
                                                instruction, instruction->result_id)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            if ((function->effect_mask & callee->effect_mask) != callee->effect_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
                return false;
            }
            if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT: {
            const XrValidatedSignature *callee =
                callable_call_signature(context->program, function, instruction);
            if (!callee || instruction->successor_count != 0u ||
                callee->error_type_id != XR_CORE_TYPE_VOID ||
                callee->panic_type_id != XR_CORE_TYPE_VOID ||
                instruction->operand_count != callee->parameter_count + 1u ||
                (callee->result_type_id == XR_CORE_TYPE_VOID) !=
                    (instruction->result_id == XR_PROGRAM_LOCATION_NONE) ||
                instruction->result_type_id != callee->result_type_id ||
                instruction->result_ownership != callee->result_ownership ||
                !callable_arguments_match(context->program, function, instruction, callee)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            if (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
                !mapped_call_result_roots_match(context->program, function, callee, instruction,
                                                instruction->result_id)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            if ((function->effect_mask & callee->effect_mask) != callee->effect_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
                return false;
            }
            if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_WITNESS_DIRECT: {
            const XrValidatedSignature *callee =
                witness_call_signature(context->program, function, instruction);
            if (!callee || instruction->successor_count != 0u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            if (callee->error_type_id != XR_CORE_TYPE_VOID ||
                callee->panic_type_id != XR_CORE_TYPE_VOID ||
                instruction->operand_count != callee->parameter_count ||
                (callee->result_type_id == XR_CORE_TYPE_VOID) !=
                    (instruction->result_id == XR_PROGRAM_LOCATION_NONE) ||
                instruction->result_type_id != callee->result_type_id ||
                instruction->result_ownership != callee->result_ownership ||
                !witness_arguments_match(context->program, function, instruction, callee)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            if (instruction->result_id != XR_PROGRAM_LOCATION_NONE &&
                !mapped_call_result_roots_match(context->program, function, callee, instruction,
                                                instruction->result_id)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            if ((function->effect_mask & callee->effect_mask) != callee->effect_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
                return false;
            }
            if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_SEALED_INVOKE: {
            if (instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
                instruction->immediate.function_id >= context->program->function_count ||
                instruction->result_id != XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_VOID) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            const XrValidatedFunction *callee =
                &context->program->functions[instruction->immediate.function_id];
            bool has_error = callee->error_type_id != XR_CORE_TYPE_VOID;
            bool has_panic = callee->panic_type_id != XR_CORE_TYPE_VOID;
            uint32_t expected_successors = 1u + (has_error ? 1u : 0u) + (has_panic ? 1u : 0u);
            if ((!has_error && !has_panic) || instruction->successor_count != expected_successors) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            for (uint32_t successor = 0u; successor < expected_successors; ++successor) {
                if (instruction->successors[successor] >= function->block_count) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            const XrValidatedBlock *normal = &function->blocks[instruction->successors[0]];
            uint32_t normal_implicit = callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u;
            if (normal->argument_count < normal_implicit ||
                (normal_implicit != 0u &&
                 (normal->argument_types[0] != callee->result_type_id ||
                  normal->argument_categories[0] != XR_CORE_IR_VALUE ||
                  normal->argument_ownerships[0] != callee->result_ownership))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            uint32_t error_successor = has_error ? 1u : UINT32_MAX;
            uint32_t panic_successor = has_panic ? 1u + (has_error ? 1u : 0u) : UINT32_MAX;
            if (has_error) {
                const XrValidatedBlock *error =
                    &function->blocks[instruction->successors[error_successor]];
                if (error->argument_count == 0u ||
                    error->argument_types[0] != callee->error_type_id ||
                    error->argument_categories[0] != XR_CORE_IR_VALUE ||
                    error->argument_ownerships[0] !=
                        ownership_for_type(context->program, callee->error_type_id)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            if (has_panic) {
                const XrValidatedBlock *panic =
                    &function->blocks[instruction->successors[panic_successor]];
                if (panic->argument_count == 0u ||
                    panic->argument_types[0] != callee->panic_type_id ||
                    panic->argument_categories[0] != XR_CORE_IR_VALUE ||
                    panic->argument_ownerships[0] !=
                        ownership_for_type(context->program, callee->panic_type_id)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            if (normal_implicit != 0u &&
                !mapped_call_result_roots_match(context->program, function,
                                                &context->program->signatures[callee->signature_id],
                                                instruction, normal->argument_ids[0])) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_ROOT, location);
                return false;
            }
            for (uint32_t argument = 0; argument < callee->parameter_count; ++argument) {
                if (!operand_type_is(function, instruction, argument,
                                     callee->parameter_types[argument]) ||
                    !operand_category_is(function, instruction, argument,
                                         callee->parameter_modes[argument] == XR_PARAM_REF
                                             ? XR_CORE_IR_PLACE
                                             : XR_CORE_IR_VALUE) ||
                    !call_operand_ownership_is(context->program, function, instruction, argument,
                                               callee->parameter_modes[argument],
                                               callee->parameter_types[argument])) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            uint32_t operand = callee->parameter_count;
            if (!verify_successor_argument_suffix(context, function, instruction, 0u,
                                                  normal_implicit, operand, location))
                return false;
            operand += normal->argument_count - normal_implicit;
            if (has_error) {
                const XrValidatedBlock *error =
                    &function->blocks[instruction->successors[error_successor]];
                if (!verify_successor_argument_suffix(context, function, instruction,
                                                      error_successor, 1u, operand, location))
                    return false;
                operand += error->argument_count - 1u;
            }
            if (has_panic) {
                const XrValidatedBlock *panic =
                    &function->blocks[instruction->successors[panic_successor]];
                if (!verify_successor_argument_suffix(context, function, instruction,
                                                      panic_successor, 1u, operand, location))
                    return false;
                operand += panic->argument_count - 1u;
            }
            if (instruction->operand_count != operand) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            uint32_t escaping_effects = callee->effect_mask;
            if (has_error)
                escaping_effects &= ~XR_CORE_EFFECT_ERROR;
            if (has_panic)
                escaping_effects &= ~XR_CORE_EFFECT_PANIC;
            *local_effects |= escaping_effects;
            if ((function->effect_mask & escaping_effects) != escaping_effects) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
                return false;
            }
            if ((function->capability_mask & callee->capability_mask) != callee->capability_mask) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_CALL_WITNESS_INVOKE:
            return verify_witness_invoke(context, function, instruction, location, local_effects);
        case XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE:
            return verify_callable_invoke(context, function, instruction, location, local_effects);
        case XR_CORE_OP_CORE_TRAP:
            if (!expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_U32,
                              XR_CORE_TYPE_VOID, false) ||
                instruction->immediate.u32 != 4u) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_ERROR_PUBLISH:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              XR_CORE_TYPE_VOID, false) ||
                function->error_type_id == XR_CORE_TYPE_VOID ||
                !operand_type_is(function, instruction, 0, function->error_type_id) ||
                !operand_ownership_is(
                    function, instruction, 0,
                    ownership_for_type(context->program, function->error_type_id))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_PANIC_PUBLISH:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              XR_CORE_TYPE_VOID, false) ||
                function->panic_type_id != XR_CORE_TYPE_PANIC_INFO ||
                !operand_type_is(function, instruction, 0, XR_CORE_TYPE_PANIC_INFO) ||
                !operand_ownership_is(function, instruction, 0, XR_CORE_IR_OWNER)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_TARGET_POINTER_WIDTH:
            return expect_shape(context, instruction, location, 0, 0, XR_CORE_IR_IMMEDIATE_NONE,
                                XR_CORE_TYPE_U32, true);
        case XR_CORE_OP_CORE_CALLABLE_PACK: {
            const XrValidatedType *callable =
                xr_validated_program_type(context->program, instruction->result_type_id);
            if (!callable || callable->kind != XR_CORE_IR_TYPE_CALLABLE ||
                instruction->operand_count > 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FUNCTION ||
                instruction->immediate.function_id >= context->program->function_count ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                instruction->result_ownership != XR_CORE_IR_OWNER) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            uint16_t capture_type = XR_CORE_TYPE_VOID;
            if (instruction->operand_count != 0u) {
                uint32_t capture_value = instruction->operands[0];
                capture_type = function->value_types[capture_value];
                const XrValidatedType *capture =
                    xr_validated_program_type(context->program, capture_type);
                if (!capture || capture->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                    capture->ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                    capture->copy_contract != XR_CORE_IR_COPY_EXPLICIT ||
                    !operand_category_is(function, instruction, 0u, XR_CORE_IR_VALUE) ||
                    !operand_ownership_is(function, instruction, 0u, XR_CORE_IR_OWNER)) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            if (!callable_target_matches(
                    context->program, callable,
                    &context->program->functions[instruction->immediate.function_id],
                    capture_type)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_OWNER_COPY: {
            XrCoreIrOwnershipDisposition expected =
                ownership_for_type(context->program, instruction->result_type_id);
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              instruction->result_type_id, true) ||
                !operand_type_is(function, instruction, 0, instruction->result_type_id) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_VALUE) ||
                instruction->result_ownership != expected ||
                xr_validated_program_copy_contract(context->program, instruction->result_type_id) ==
                    XR_CORE_IR_COPY_FORBIDDEN) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        }
        case XR_CORE_OP_CORE_OWNER_MOVE:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              instruction->result_type_id, true) ||
                !operand_type_is(function, instruction, 0, instruction->result_type_id) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_VALUE) ||
                instruction->result_ownership !=
                    function->value_ownerships[instruction->operands[0]] ||
                (xr_validated_program_type_ownership(context->program,
                                                     instruction->result_type_id) ==
                     XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                 instruction->result_ownership != XR_CORE_IR_OWNER)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_OWNER_DROP:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              XR_CORE_TYPE_VOID, false) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_VALUE) ||
                (xr_validated_program_type_ownership(
                     context->program, function->value_types[instruction->operands[0]]) ==
                     XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                 !operand_ownership_is(function, instruction, 0, XR_CORE_IR_OWNER))) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_PLACE_LOCAL:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              instruction->result_type_id, true) ||
                instruction->result_category != XR_CORE_IR_PLACE ||
                !operand_type_is(function, instruction, 0, instruction->result_type_id) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_VALUE) ||
                !operand_ownership_is(function, instruction, 0, XR_CORE_IR_NON_OWNER) ||
                xr_validated_program_type_ownership(context->program,
                                                    instruction->result_type_id) !=
                    XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_PLACE_LOAD:
            if (!expect_shape(context, instruction, location, 1, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              instruction->result_type_id, true) ||
                !operand_type_is(function, instruction, 0, instruction->result_type_id) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_PLACE) ||
                instruction->result_ownership != XR_CORE_IR_NON_OWNER ||
                xr_validated_program_type_ownership(context->program,
                                                    instruction->result_type_id) !=
                    XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_PLACE_STORE:
            if (!expect_shape(context, instruction, location, 2, 0, XR_CORE_IR_IMMEDIATE_NONE,
                              XR_CORE_TYPE_VOID, false) ||
                !operand_category_is(function, instruction, 0, XR_CORE_IR_PLACE) ||
                !operand_category_is(function, instruction, 1, XR_CORE_IR_VALUE) ||
                !operand_ownership_is(function, instruction, 1, XR_CORE_IR_NON_OWNER) ||
                function->value_types[instruction->operands[0]] !=
                    function->value_types[instruction->operands[1]] ||
                xr_validated_program_type_ownership(
                    context->program, function->value_types[instruction->operands[1]]) !=
                    XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            return true;
        case XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT: {
            const XrValidatedType *type =
                xr_validated_program_type(context->program, instruction->result_type_id);
            if (!type || type->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                instruction->operand_count != type->field_count ||
                instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                instruction->result_ownership !=
                    ownership_for_type(context->program, instruction->result_type_id)) {
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                return false;
            }
            for (uint32_t field = 0; field < type->field_count; ++field) {
                if (!operand_type_is(function, instruction, field, type->field_types[field])) {
                    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                    return false;
                }
            }
            return true;
        }
        case XR_CORE_OP_CORE_AGGREGATE_PROJECT: {
            if (instruction->operand_count != 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE)
                goto aggregate_type_reject;
            uint16_t aggregate_type_id = function->value_types[instruction->operands[0]];
            const XrValidatedType *type =
                xr_validated_program_type(context->program, aggregate_type_id);
            if (!type || type->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                instruction->immediate.field_ordinal >= type->field_count ||
                instruction->result_type_id !=
                    type->field_types[instruction->immediate.field_ordinal])
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_AGGREGATE_UPDATE: {
            if (instruction->operand_count != 2u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_FIELD ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE)
                goto aggregate_type_reject;
            uint16_t aggregate_type_id = function->value_types[instruction->operands[0]];
            const XrValidatedType *type =
                xr_validated_program_type(context->program, aggregate_type_id);
            if (!type || type->kind != XR_CORE_IR_TYPE_AGGREGATE ||
                instruction->immediate.field_ordinal >= type->field_count ||
                instruction->result_type_id != aggregate_type_id ||
                type->ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
                !operand_type_is(function, instruction, 1u,
                                 type->field_types[instruction->immediate.field_ordinal]))
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_VARIANT_CONSTRUCT: {
            const XrValidatedType *type =
                xr_validated_program_type(context->program, instruction->result_type_id);
            uint32_t variant = instruction->immediate.variant_ordinal;
            if (!type || type->kind != XR_CORE_IR_TYPE_VARIANT ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_VARIANT ||
                instruction->successor_count != 0u ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                variant >= type->variant_count ||
                instruction->operand_count != type->variants[variant].payload_count ||
                instruction->result_ownership !=
                    ownership_for_type(context->program, instruction->result_type_id))
                goto aggregate_type_reject;
            for (uint32_t field = 0; field < type->variants[variant].payload_count; ++field) {
                if (!operand_type_is(function, instruction, field,
                                     type->variants[variant].payload_types[field]))
                    goto aggregate_type_reject;
            }
            return true;
        }
        case XR_CORE_OP_CORE_VARIANT_TEST: {
            if (instruction->operand_count != 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_VARIANT ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != XR_CORE_TYPE_BOOL)
                goto aggregate_type_reject;
            const XrValidatedType *type = xr_validated_program_type(
                context->program, function->value_types[instruction->operands[0]]);
            if (!type || type->kind != XR_CORE_IR_TYPE_VARIANT ||
                instruction->immediate.variant_ordinal >= type->variant_count)
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_VARIANT_PROJECT: {
            if (instruction->operand_count != 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_VARIANT_FIELD ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE)
                goto aggregate_type_reject;
            const XrValidatedType *type = xr_validated_program_type(
                context->program, function->value_types[instruction->operands[0]]);
            uint32_t variant = instruction->immediate.variant_field.variant_ordinal;
            uint32_t field = instruction->immediate.variant_field.field_ordinal;
            if (!type || type->kind != XR_CORE_IR_TYPE_VARIANT || variant >= type->variant_count ||
                field >= type->variants[variant].payload_count ||
                instruction->result_type_id != type->variants[variant].payload_types[field])
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_EXISTENTIAL_PACK: {
            const XrValidatedType *existential =
                xr_validated_program_type(context->program, instruction->result_type_id);
            if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                instruction->operand_count != 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_NONE ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                instruction->result_category != XR_CORE_IR_VALUE)
                goto aggregate_type_reject;
            uint32_t operand = instruction->operands[0];
            uint16_t concrete_type = function->value_types[operand];
            if (!type_is_nominal(context->program, concrete_type) ||
                !find_conformance(context->program, concrete_type, existential->interface_id) ||
                instruction->result_ownership !=
                    ownership_for_type(context->program, instruction->result_type_id))
                goto aggregate_type_reject;
            XrCoreIrValueCategory expected_category =
                existential->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF
                    ? XR_CORE_IR_PLACE
                    : XR_CORE_IR_VALUE;
            XrCoreIrOwnershipDisposition expected_operand_ownership =
                (existential->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                 existential->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE)
                    ? ownership_for_type(context->program, concrete_type)
                    : XR_CORE_IR_NON_OWNER;
            if (function->value_categories[operand] != expected_category ||
                function->value_ownerships[operand] != expected_operand_ownership)
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_EXISTENTIAL_TEST: {
            if (!expect_shape(context, instruction, location, 1u, 0u, XR_CORE_IR_IMMEDIATE_TYPE,
                              XR_CORE_TYPE_BOOL, true) ||
                !operand_category_is(function, instruction, 0u, XR_CORE_IR_VALUE) ||
                !type_is_nominal(context->program, instruction->immediate.type_id))
                goto aggregate_type_reject;
            const XrValidatedType *existential = xr_validated_program_type(
                context->program, function->value_types[instruction->operands[0]]);
            if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                !find_conformance(context->program, instruction->immediate.type_id,
                                  existential->interface_id))
                goto aggregate_type_reject;
            return true;
        }
        case XR_CORE_OP_CORE_EXISTENTIAL_PROJECT: {
            if (instruction->operand_count != 1u || instruction->successor_count != 0u ||
                instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_TYPE ||
                instruction->result_id == XR_PROGRAM_LOCATION_NONE ||
                instruction->result_type_id != instruction->immediate.type_id ||
                !type_is_nominal(context->program, instruction->immediate.type_id))
                goto aggregate_type_reject;
            const XrValidatedType *existential = xr_validated_program_type(
                context->program, function->value_types[instruction->operands[0]]);
            if (!existential || existential->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                !find_conformance(context->program, instruction->immediate.type_id,
                                  existential->interface_id))
                goto aggregate_type_reject;
            XrCoreIrValueCategory expected_category =
                existential->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF
                    ? XR_CORE_IR_PLACE
                    : XR_CORE_IR_VALUE;
            if (instruction->result_category != expected_category ||
                instruction->result_ownership !=
                    (expected_category == XR_CORE_IR_PLACE
                         ? XR_CORE_IR_NON_OWNER
                         : ((existential->interface_use_kind ==
                                 XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                             existential->interface_use_kind ==
                                 XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE)
                                ? ownership_for_type(context->program, instruction->result_type_id)
                                : XR_CORE_IR_NON_OWNER)))
                goto aggregate_type_reject;
            return true;
        }
        default:
            reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY, location);
            return false;
    }

aggregate_type_reject:
    reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
    return false;
}

static const XrValidatedInstruction *value_instruction(const XrValidatedFunction *function,
                                                       uint32_t value_id) {
    if (value_id >= function->value_count || function->value_positions[value_id] == 0u)
        return NULL;
    uint32_t block_id = function->value_blocks[value_id];
    uint32_t instruction_id = function->value_positions[value_id] - 1u;
    if (block_id >= function->block_count ||
        instruction_id >= function->blocks[block_id].instruction_count)
        return NULL;
    const XrValidatedInstruction *instruction =
        &function->blocks[block_id].instructions[instruction_id];
    return instruction->result_id == value_id ? instruction : NULL;
}

static uint32_t existential_identity_source(const XrValidatedFunction *function,
                                            uint32_t value_id) {
    for (uint32_t depth = 0; depth < function->value_count; ++depth) {
        const XrValidatedInstruction *instruction = value_instruction(function, value_id);
        if (!instruction || instruction->operand_count != 1u ||
            (instruction->operation_id != XR_CORE_OP_CORE_OWNER_COPY &&
             instruction->operation_id != XR_CORE_OP_CORE_OWNER_MOVE))
            return value_id;
        value_id = instruction->operands[0];
    }
    return XR_PROGRAM_LOCATION_NONE;
}

static bool edge_argument_source(const XrValidatedProgram *program,
                                 const XrValidatedFunction *function,
                                 const XrValidatedInstruction *terminator, uint32_t successor_index,
                                 uint32_t argument_index, uint32_t *source_out) {
    if (!source_out || successor_index >= terminator->successor_count ||
        terminator->successors[successor_index] >= function->block_count)
        return false;
    uint32_t operand = argument_index;
    if (terminator->operation_id == XR_CORE_OP_CORE_BRANCH) {
        operand = argument_index;
    } else if (terminator->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH) {
        operand = 1u + argument_index;
        if (successor_index != 0u)
            operand += function->blocks[terminator->successors[0]].argument_count;
    } else if (terminator->operation_id == XR_CORE_OP_CORE_CALL_SEALED_INVOKE ||
               terminator->operation_id == XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE ||
               terminator->operation_id == XR_CORE_OP_CORE_CALL_WITNESS_INVOKE) {
        const XrValidatedSignature *callee =
            operation_call_signature(program, function, terminator);
        if (!callee)
            return false;
        operand = call_operand_prefix(terminator->operation_id) + callee->parameter_count;
        for (uint32_t prior = 0; prior < successor_index; ++prior) {
            const XrValidatedBlock *target = &function->blocks[terminator->successors[prior]];
            uint32_t implicit =
                prior == 0u ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) : 1u;
            if (target->argument_count < implicit)
                return false;
            operand += target->argument_count - implicit;
        }
        uint32_t implicit =
            successor_index == 0u ? (callee->result_type_id == XR_CORE_TYPE_VOID ? 0u : 1u) : 1u;
        if (argument_index < implicit)
            return false;
        operand += argument_index - implicit;
    } else {
        return false;
    }
    if (operand >= terminator->operand_count)
        return false;
    *source_out = terminator->operands[operand];
    return true;
}

static uint32_t meet_exact_type(uint32_t left, uint32_t right) {
    const uint32_t top = UINT32_MAX;
    const uint32_t none = UINT32_MAX - 1u;
    if (left == top)
        return right;
    if (right == top)
        return left;
    return left == right ? left : none;
}

static bool verify_existential_projection_guards(VerifyContext *context, uint32_t function_id) {
    const uint32_t top = UINT32_MAX;
    const uint32_t none = UINT32_MAX - 1u;
    const XrValidatedFunction *function = &context->program->functions[function_id];
    uint32_t *facts =
        xr_malloc((size_t) (function->value_count ? function->value_count : 1u) * sizeof(uint32_t));
    if (!facts) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, no_location());
        return false;
    }
    for (uint32_t value = 0; value < function->value_count; ++value)
        facts[value] = none;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        if (block == function->entry_block)
            continue;
        for (uint32_t argument = 0; argument < function->blocks[block].argument_count; ++argument)
            facts[function->blocks[block].argument_ids[argument]] = top;
    }

    bool changed = true;
    uint32_t iterations = 0u;
    while (changed) {
        if (++iterations > function->value_count + function->block_count + 1u) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT, no_location());
            xr_free(facts);
            return false;
        }
        if (!spend(context, function->block_count, no_location())) {
            xr_free(facts);
            return false;
        }
        changed = false;
        for (uint32_t target_id = 0; target_id < function->block_count; ++target_id) {
            if (target_id == function->entry_block)
                continue;
            const XrValidatedBlock *target = &function->blocks[target_id];
            for (uint32_t argument = 0; argument < target->argument_count; ++argument) {
                uint32_t merged = top;
                bool has_predecessor = false;
                for (uint32_t source_id = 0; source_id < function->block_count; ++source_id) {
                    const XrValidatedBlock *source = &function->blocks[source_id];
                    const XrValidatedInstruction *terminator =
                        &source->instructions[source->instruction_count - 1u];
                    if (!spend(context, 1u + terminator->successor_count, no_location())) {
                        xr_free(facts);
                        return false;
                    }
                    for (uint32_t successor = 0; successor < terminator->successor_count;
                         ++successor) {
                        if (terminator->successors[successor] != target_id)
                            continue;
                        has_predecessor = true;
                        uint32_t source_value = 0u;
                        if (!edge_argument_source(context->program, function, terminator, successor,
                                                  argument, &source_value)) {
                            merged = meet_exact_type(merged, none);
                            continue;
                        }
                        uint32_t edge_fact = facts[source_value];
                        if (terminator->operation_id == XR_CORE_OP_CORE_CONDITIONAL_BRANCH &&
                            successor == 0u) {
                            const XrValidatedInstruction *test =
                                value_instruction(function, terminator->operands[0]);
                            if (test && test->operation_id == XR_CORE_OP_CORE_EXISTENTIAL_TEST &&
                                existential_identity_source(function, test->operands[0]) ==
                                    existential_identity_source(function, source_value))
                                edge_fact = test->immediate.type_id;
                        }
                        merged = meet_exact_type(merged, edge_fact);
                    }
                }
                if (!has_predecessor)
                    merged = none;
                uint32_t value_id = target->argument_ids[argument];
                if (facts[value_id] != merged) {
                    facts[value_id] = merged;
                    changed = true;
                }
            }
        }
        for (uint32_t block = 0; block < function->block_count; ++block) {
            const XrValidatedBlock *row = &function->blocks[block];
            for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
                const XrValidatedInstruction *op = &row->instructions[instruction];
                if (op->result_id == XR_PROGRAM_LOCATION_NONE || op->operand_count != 1u ||
                    (op->operation_id != XR_CORE_OP_CORE_OWNER_COPY &&
                     op->operation_id != XR_CORE_OP_CORE_OWNER_MOVE))
                    continue;
                const XrValidatedType *type =
                    xr_validated_program_type(context->program, op->result_type_id);
                if (!type || type->kind != XR_CORE_IR_TYPE_EXISTENTIAL)
                    continue;
                uint32_t inherited = facts[op->operands[0]];
                if (facts[op->result_id] != inherited) {
                    facts[op->result_id] = inherited;
                    changed = true;
                }
            }
        }
    }

    bool valid = true;
    for (uint32_t block = 0; valid && block < function->block_count; ++block) {
        const XrValidatedBlock *row = &function->blocks[block];
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
            const XrValidatedInstruction *op = &row->instructions[instruction];
            if (op->operation_id != XR_CORE_OP_CORE_EXISTENTIAL_PROJECT)
                continue;
            if (facts[op->operands[0]] != op->immediate.type_id) {
                XrProgramSemanticLocation location = {
                    .section_id = XR_PROGRAM_SECTION_CODE,
                    .function_id = function_id,
                    .block_id = block,
                    .instruction_id = instruction,
                    .value_id = op->operands[0],
                };
                reject(context, XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE, location);
                valid = false;
                break;
            }
        }
    }
    xr_free(facts);
    return valid;
}

static bool verify_function(VerifyContext *context, uint32_t function_id) {
    XrValidatedFunction *function = &context->program->functions[function_id];
    XrProgramSemanticLocation location = no_location();
    location.section_id = XR_PROGRAM_SECTION_FUNCTIONS;
    location.function_id = function_id;
    if ((function->flags & ~XR_PROGRAM_FUNCTION_ENTRY) != 0u ||
        (function->effect_mask & ~UINT32_C(0x1f)) != 0u ||
        (function->capability_mask & ~UINT32_C(0x01)) != 0u ||
        ((function->error_type_id == XR_CORE_TYPE_VOID) !=
         ((function->effect_mask & XR_CORE_EFFECT_ERROR) == 0u)) ||
        function->error_type_id == XR_CORE_TYPE_PANIC_INFO ||
        ((function->panic_type_id == XR_CORE_TYPE_VOID) !=
         ((function->effect_mask & XR_CORE_EFFECT_PANIC) == 0u)) ||
        (function->panic_type_id != XR_CORE_TYPE_VOID &&
         function->panic_type_id != XR_CORE_TYPE_PANIC_INFO) ||
        function->result_ownership !=
            (function->result_type_id != XR_CORE_TYPE_VOID
                 ? ownership_for_type(context->program, function->result_type_id)
                 : XR_CORE_IR_NON_OWNER)) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    XrValidatedBlock *entry = &function->blocks[function->entry_block];
    if (entry->argument_count != function->parameter_count) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_FUNCTION, location);
        return false;
    }
    for (uint32_t parameter = 0; parameter < function->parameter_count; ++parameter) {
        XrCoreIrValueCategory expected = function->parameter_modes[parameter] == XR_PARAM_REF
                                             ? XR_CORE_IR_PLACE
                                             : XR_CORE_IR_VALUE;
        XrCoreIrOwnershipDisposition expected_ownership =
            function->parameter_modes[parameter] == XR_PARAM_MOVE
                ? ownership_for_type(context->program, function->parameter_types[parameter])
                : XR_CORE_IR_NON_OWNER;
        if (entry->argument_types[parameter] != function->parameter_types[parameter] ||
            entry->argument_categories[parameter] != expected ||
            entry->argument_ownerships[parameter] != expected_ownership) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
    }
    for (uint32_t value = 0; value < function->value_count; ++value) {
        if (!value_ownership_contract_is_valid(context->program, function->value_types[value],
                                               function->value_categories[value],
                                               function->value_ownerships[value])) {
            location.value_id = value;
            reject(context, XR_PROGRAM_DIAGNOSTIC_TYPE, location);
            return false;
        }
    }
    uint32_t local_effects = 0;
    uint32_t local_capabilities = 0;
    bool *consumed = xr_calloc(function->value_count ? function->value_count : 1u, sizeof(bool));
    if (!consumed) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        memset(consumed, 0, (size_t) function->value_count * sizeof(bool));
        XrValidatedBlock *block = &function->blocks[block_id];
        location.block_id = block_id;
        if (block->instruction_count == 0 ||
            !instruction_is_terminator(
                block->instructions[block->instruction_count - 1u].operation_id)) {
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            xr_free(consumed);
            return false;
        }
        for (uint32_t instruction_id = 0; instruction_id < block->instruction_count;
             ++instruction_id) {
            if (instruction_id + 1u != block->instruction_count &&
                instruction_is_terminator(block->instructions[instruction_id].operation_id)) {
                location.instruction_id = instruction_id;
                reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
                xr_free(consumed);
                return false;
            }
            if (!verify_operation(context, function_id, block_id, instruction_id, &local_effects,
                                  &local_capabilities, consumed)) {
                xr_free(consumed);
                return false;
            }
        }
        if (!verify_owner_block_closure(context, function_id, block_id, consumed)) {
            xr_free(consumed);
            return false;
        }
    }
    xr_free(consumed);
    if ((function->effect_mask & local_effects) != local_effects) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_EFFECT, location);
        return false;
    }
    if ((function->capability_mask & local_capabilities) != local_capabilities) {
        reject(context, XR_PROGRAM_DIAGNOSTIC_CAPABILITY, location);
        return false;
    }

    bool *reachable = xr_calloc(function->block_count, sizeof(bool));
    uint32_t *queue = xr_calloc(function->block_count, sizeof(uint32_t));
    if (!reachable || !queue) {
        xr_free(queue);
        xr_free(reachable);
        reject(context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, location);
        return false;
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    reachable[function->entry_block] = true;
    queue[tail++] = function->entry_block;
    while (head != tail) {
        uint32_t block_id = queue[head++];
        const XrValidatedBlock *block = &function->blocks[block_id];
        const XrValidatedInstruction *terminator =
            &block->instructions[block->instruction_count - 1u];
        for (uint32_t successor = 0; successor < terminator->successor_count; ++successor) {
            uint32_t target = terminator->successors[successor];
            if (!reachable[target]) {
                reachable[target] = true;
                queue[tail++] = target;
            }
        }
    }
    for (uint32_t block_id = 0; block_id < function->block_count; ++block_id) {
        if (!reachable[block_id]) {
            location.block_id = block_id;
            reject(context, XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW, location);
            xr_free(queue);
            xr_free(reachable);
            return false;
        }
    }
    xr_free(queue);
    xr_free(reachable);
    return verify_existential_projection_guards(context, function_id);
}

XrProgramVerifyBudget xr_program_verify_default_budget(void) {
    XrProgramVerifyBudget budget = {
        .decode =
            {
                .max_bytes = XR_PROGRAM_LIMIT_ARTIFACT_BYTES,
                .max_records = XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION,
                .max_operations = XR_PROGRAM_LIMIT_OPERATIONS,
            },
        .max_work = UINT64_C(100000000),
        .max_functions = XR_PROGRAM_LIMIT_FUNCTIONS,
        .max_blocks_per_function = XR_PROGRAM_LIMIT_BLOCKS_PER_FUNCTION,
        .max_values_per_function = XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION,
        .max_operations = XR_PROGRAM_LIMIT_OPERATIONS,
    };
    return budget;
}

XrProgramVerifyStatus xr_program_validate(const uint8_t *bytes, size_t size,
                                          const XrProgramVerifyBudget *budget,
                                          XrValidatedProgram **program_out,
                                          XrProgramDiagnostic *diagnostic_out) {
    XrProgramVerifyBudget selected = budget ? *budget : xr_program_verify_default_budget();
    VerifyContext context = {.budget = selected, .diagnostic = {.location = no_location()}};
    XrProgramView view;
    uint8_t expected_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    if (program_out)
        *program_out = NULL;
    if (diagnostic_out)
        memset(diagnostic_out, 0, sizeof(*diagnostic_out));
    if (!bytes || !program_out || selected.max_work == 0 || selected.max_functions == 0 ||
        selected.max_blocks_per_function == 0 || selected.max_values_per_function == 0 ||
        selected.max_operations == 0) {
        return XR_PROGRAM_VERIFY_INVALID_INPUT;
    }
    XrProgramDecodeStatus decode =
        xr_program_decode_structure(bytes, size, &selected.decode, &view, NULL, 0);
    if (decode != XR_PROGRAM_DECODE_OK) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_STRUCTURAL;
        context.diagnostic.decode_status = decode;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return decode == XR_PROGRAM_DECODE_RESOURCE_LIMIT ? XR_PROGRAM_VERIFY_RESOURCE_LIMIT
                                                          : XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED;
    }
    if (!parse_current_core_spec_fingerprint(expected_fingerprint) ||
        memcmp(view.core_spec_fingerprint, expected_fingerprint, sizeof(expected_fingerprint)) !=
            0) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return XR_PROGRAM_VERIFY_SEMANTIC_REJECTED;
    }
    context.program = xr_calloc(1u, sizeof(XrValidatedProgram));
    if (!context.program) {
        context.diagnostic.kind = XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY;
        if (diagnostic_out)
            *diagnostic_out = context.diagnostic;
        return XR_PROGRAM_VERIFY_OUT_OF_MEMORY;
    }
    atomic_init(&context.program->references, 1u);
    context.program->bytes = xr_malloc(size);
    if (!context.program->bytes) {
        reject(&context, XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY, no_location());
        goto rejected;
    }
    memcpy(context.program->bytes, bytes, size);
    context.program->size = size;
    context.program->id = view.id;
    memcpy(context.program->semantic_profile_fingerprint, view.semantic_profile_fingerprint,
           XR_PROGRAM_DIGEST_SIZE);
    if (!parse_types(&context, &view) || !parse_constants(&context, &view) ||
        !parse_functions(&context, &view) || !parse_code(&context, &view) ||
        !parse_semantic_metadata(&context, &view))
        goto rejected;

    uint32_t entry_count = 0;
    for (uint32_t function = 0; function < context.program->function_count; ++function) {
        if (!verify_function(&context, function))
            goto rejected;
        if ((context.program->functions[function].flags & XR_PROGRAM_FUNCTION_ENTRY) != 0u) {
            context.program->entry_function = function;
            ++entry_count;
        }
    }
    if (entry_count != 1u) {
        reject(&context, XR_PROGRAM_DIAGNOSTIC_ENTRY_POINT, no_location());
        goto rejected;
    }
    context.program->verifier_work = context.work;
    *program_out = context.program;
    if (diagnostic_out)
        *diagnostic_out = context.diagnostic;
    return XR_PROGRAM_VERIFY_OK;

rejected:
    if (diagnostic_out)
        *diagnostic_out = context.diagnostic;
    XrProgramVerifyStatus status = XR_PROGRAM_VERIFY_SEMANTIC_REJECTED;
    if (context.diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT)
        status = XR_PROGRAM_VERIFY_RESOURCE_LIMIT;
    else if (context.diagnostic.kind == XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY)
        status = XR_PROGRAM_VERIFY_OUT_OF_MEMORY;
    xr_validated_program_free(context.program);
    return status;
}

XrProgramId xr_validated_program_id(const XrValidatedProgram *program) {
    XrProgramId id = {{0}};
    return program ? program->id : id;
}

uint32_t xr_validated_program_function_count(const XrValidatedProgram *program) {
    return program ? program->function_count : 0u;
}

uint32_t xr_validated_program_entry_function(const XrValidatedProgram *program) {
    return program ? program->entry_function : XR_PROGRAM_LOCATION_NONE;
}

uint64_t xr_validated_program_verifier_work(const XrValidatedProgram *program) {
    return program ? program->verifier_work : 0u;
}

const uint8_t *xr_validated_program_bytes(const XrValidatedProgram *program, size_t *size_out) {
    if (size_out)
        *size_out = program ? program->size : 0u;
    return program ? program->bytes : NULL;
}

const char *xr_program_verify_status_name(XrProgramVerifyStatus status) {
    switch (status) {
        case XR_PROGRAM_VERIFY_OK:
            return "ok";
        case XR_PROGRAM_VERIFY_INVALID_INPUT:
            return "invalid-input";
        case XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED:
            return "structural-rejected";
        case XR_PROGRAM_VERIFY_SEMANTIC_REJECTED:
            return "semantic-rejected";
        case XR_PROGRAM_VERIFY_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_VERIFY_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}

const char *xr_program_diagnostic_kind_name(XrProgramDiagnosticKind kind) {
    switch (kind) {
        case XR_PROGRAM_DIAGNOSTIC_NONE:
            return "none";
        case XR_PROGRAM_DIAGNOSTIC_STRUCTURAL:
            return "structural";
        case XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY:
            return "core-spec-identity";
        case XR_PROGRAM_DIAGNOSTIC_TYPE:
            return "type";
        case XR_PROGRAM_DIAGNOSTIC_FUNCTION:
            return "function";
        case XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION:
            return "value-definition";
        case XR_PROGRAM_DIAGNOSTIC_VALUE_USE:
            return "value-use";
        case XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW:
            return "control-flow";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY:
            return "operation-arity";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE:
            return "operation-type";
        case XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE:
            return "operation-immediate";
        case XR_PROGRAM_DIAGNOSTIC_EFFECT:
            return "effect";
        case XR_PROGRAM_DIAGNOSTIC_CAPABILITY:
            return "capability";
        case XR_PROGRAM_DIAGNOSTIC_ROOT:
            return "root";
        case XR_PROGRAM_DIAGNOSTIC_ENTRY_POINT:
            return "entry-point";
        default:
            return "unknown";
    }
}
