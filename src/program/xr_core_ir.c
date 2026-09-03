/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_core_ir.c - Canonical compiler-private CoreIR ownership and closure
 */

#include "xr_program_internal.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../core/xr_core_spec_gen.h"
#include "xr_program_schema_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int key_compare_value(XrCoreIrKey left, XrCoreIrKey right) {
    return memcmp(left.bytes, right.bytes, XR_CORE_IR_KEY_SIZE);
}

static int module_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrModule *) left)->key,
                             ((const XrCoreIrModule *) right)->key);
}

static int function_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrFunction *) left)->key,
                             ((const XrCoreIrFunction *) right)->key);
}

static int block_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrBlock *) left)->key,
                             ((const XrCoreIrBlock *) right)->key);
}

static int type_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrType *) left)->key,
                             ((const XrCoreIrType *) right)->key);
}

static int interface_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrInterface *) left)->key,
                             ((const XrCoreIrInterface *) right)->key);
}

static int conformance_compare(const void *left, const void *right) {
    return key_compare_value(((const XrCoreIrConformance *) left)->key,
                             ((const XrCoreIrConformance *) right)->key);
}

static int view_origin_compare(const void *left, const void *right) {
    const XrViewOrigin *a = left;
    const XrViewOrigin *b = right;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->param_ordinal != b->param_ordinal)
        return a->param_ordinal < b->param_ordinal ? -1 : 1;
    return 0;
}

XrCoreIrKey xr_core_ir_key(const void *semantic_bytes, size_t semantic_size) {
    static const uint8_t domain[] = "xray-core-ir-semantic-key-v1";
    XrCoreIrKey key = {{0}};
    XrSHA256Context context;
    if (!semantic_bytes && semantic_size != 0)
        return key;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain));
    xr_sha256_update(&context, (const uint8_t *) semantic_bytes, semantic_size);
    xr_sha256_final(&context, key.bytes);
    return key;
}

bool xr_core_ir_key_equal(XrCoreIrKey left, XrCoreIrKey right) {
    return key_compare_value(left, right) == 0;
}

bool xr_core_ir_key_is_zero(XrCoreIrKey key) {
    uint8_t combined = 0;
    for (size_t index = 0; index < XR_CORE_IR_KEY_SIZE; ++index)
        combined |= key.bytes[index];
    return combined == 0;
}

static bool type_id_is_builtin(uint16_t type_id) {
    return type_id <= XR_CORE_TYPE_PANIC_INFO;
}

static bool type_id_supported(const XrCoreIrProgram *program, uint16_t type_id) {
    if (type_id_is_builtin(type_id))
        return true;
    if (!program || type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return false;
    return (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE < program->type_count;
}

static bool value_category_is_valid(XrCoreIrValueCategory category) {
    return category == XR_CORE_IR_VALUE || category == XR_CORE_IR_PLACE;
}

static bool ownership_disposition_is_valid(XrCoreIrOwnershipDisposition ownership) {
    return ownership == XR_CORE_IR_NON_OWNER || ownership == XR_CORE_IR_OWNER;
}

static XrCoreIrTypeOwnership type_ownership(const XrCoreIrProgram *program, uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    if (!program || type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
        return XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
    return program->types[type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE].ownership;
}

static bool value_contract_is_valid(const XrCoreIrProgram *program, uint16_t type_id,
                                    XrCoreIrValueCategory category,
                                    XrCoreIrOwnershipDisposition ownership) {
    if (!value_category_is_valid(category) || !ownership_disposition_is_valid(ownership))
        return false;
    if (category == XR_CORE_IR_PLACE)
        return ownership == XR_CORE_IR_NON_OWNER;
    return ownership != XR_CORE_IR_OWNER ||
           type_ownership(program, type_id) == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
}

static bool copy_bytes(void **destination, const void *source, size_t count, size_t item_size) {
    *destination = NULL;
    if (count == 0)
        return source == NULL;
    if (!source || item_size > SIZE_MAX / count)
        return false;
    void *copy = xr_malloc(count * item_size);
    if (!copy)
        return false;
    memcpy(copy, source, count * item_size);
    *destination = copy;
    return true;
}

static void free_instruction(XrCoreIrInstruction *instruction) {
    if (!instruction)
        return;
    xr_free(instruction->operands);
    xr_free(instruction->successors);
}

static void free_signature(XrCoreIrCallableSignature *signature) {
    if (!signature)
        return;
    xr_free(signature->parameter_types);
    xr_free(signature->parameter_modes);
    xr_free(signature->result_borrow_origins);
}

static void free_type(XrCoreIrType *type) {
    if (!type)
        return;
    for (uint32_t variant = 0; variant < type->variant_count; ++variant)
        xr_free(type->variants[variant].payload_types);
    xr_free(type->variants);
    xr_free(type->field_types);
    free_signature(&type->callable_signature);
}

static void free_block(XrCoreIrBlock *block) {
    if (!block)
        return;
    for (uint32_t index = 0; index < block->instruction_count; ++index)
        free_instruction(&block->instructions[index]);
    xr_free(block->instructions);
    xr_free(block->arguments);
}

static void free_function(XrCoreIrFunction *function) {
    if (!function)
        return;
    for (uint32_t index = 0; index < function->block_count; ++index)
        free_block(&function->blocks[index]);
    xr_free(function->blocks);
    xr_free(function->parameter_types);
    xr_free(function->parameter_modes);
    xr_free(function->result_borrow_origins);
    for (uint32_t index = 0; index < function->value_root_set_count; ++index)
        xr_free(function->value_root_sets[index].roots);
    xr_free(function->value_root_sets);
    xr_free(function->roots);
}

void xr_core_ir_program_free(XrCoreIrProgram *program) {
    if (!program)
        return;
    for (uint32_t module_index = 0; program->modules && module_index < program->module_count;
         ++module_index) {
        XrCoreIrModule *module = &program->modules[module_index];
        for (uint32_t function_index = 0; function_index < module->function_count; ++function_index)
            free_function(&module->functions[function_index]);
        xr_free(module->functions);
        xr_free(module->constants);
    }
    for (uint32_t type = 0; program->types && type < program->type_count; ++type)
        free_type(&program->types[type]);
    for (uint32_t interface = 0; program->interfaces && interface < program->interface_count;
         ++interface) {
        for (uint32_t slot = 0; slot < program->interfaces[interface].slot_count; ++slot)
            free_signature(&program->interfaces[interface].slots[slot]);
        xr_free(program->interfaces[interface].slots);
    }
    for (uint32_t conformance = 0;
         program->conformances && conformance < program->conformance_count; ++conformance)
        xr_free(program->conformances[conformance].slot_functions);
    xr_free(program->interfaces);
    xr_free(program->conformances);
    xr_free(program->types);
    xr_free(program->modules);
    xr_free(program->required_features);
    xr_free(program);
}

static XrProgramBuildStatus copy_signature(const XrCoreIrCallableSignatureInput *input,
                                           XrCoreIrCallableSignature *output) {
    memset(output, 0, sizeof(*output));
    if (!input)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    output->parameter_count = input->parameter_count;
    output->has_receiver = input->has_receiver;
    output->receiver_mode = input->receiver_mode;
    output->result_type_id = input->result_type_id;
    output->result_ownership = input->result_ownership;
    output->result_borrow_origin_count = input->result_borrow_origin_count;
    output->error_type_id = input->error_type_id;
    output->panic_type_id = input->panic_type_id;
    output->effect_mask = input->effect_mask;
    output->capability_mask = input->capability_mask;
    if (!copy_bytes((void **) &output->parameter_types, input->parameter_types,
                    input->parameter_count, sizeof(uint16_t)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (input->parameter_count != 0u) {
        output->parameter_modes = xr_calloc(input->parameter_count, sizeof(XrParamMode));
        if (!output->parameter_modes) {
            free_signature(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t parameter = 0; parameter < input->parameter_count; ++parameter)
            output->parameter_modes[parameter] =
                input->parameter_modes ? input->parameter_modes[parameter] : XR_PARAM_READ;
    } else if (input->parameter_modes) {
        free_signature(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (!copy_bytes((void **) &output->result_borrow_origins, input->result_borrow_origins,
                    input->result_borrow_origin_count, sizeof(XrViewOrigin))) {
        free_signature(output);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (output->result_borrow_origin_count != 0u) {
        qsort(output->result_borrow_origins, output->result_borrow_origin_count,
              sizeof(XrViewOrigin), view_origin_compare);
        uint32_t unique = 0;
        for (uint32_t index = 0; index < output->result_borrow_origin_count; ++index) {
            if (unique == 0u || view_origin_compare(&output->result_borrow_origins[unique - 1u],
                                                    &output->result_borrow_origins[index]) != 0)
                output->result_borrow_origins[unique++] = output->result_borrow_origins[index];
        }
        output->result_borrow_origin_count = unique;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_instruction(const XrCoreIrInstructionInput *input,
                                             XrCoreIrInstruction *output) {
    memset(output, 0, sizeof(*output));
    output->operation_id = input->operation_id;
    output->result = input->result;
    output->result_type_id = input->result_type_id;
    output->result_category = input->result_category;
    output->result_ownership = input->result_ownership;
    output->operand_count = input->operand_count;
    output->immediate_kind = input->immediate_kind;
    output->successor_count = input->successor_count;
    switch (input->immediate_kind) {
        case XR_CORE_IR_IMMEDIATE_NONE:
            break;
        case XR_CORE_IR_IMMEDIATE_I64:
            output->immediate.i64 = input->immediate.i64;
            break;
        case XR_CORE_IR_IMMEDIATE_U32:
            output->immediate.u32 = input->immediate.u32;
            break;
        case XR_CORE_IR_IMMEDIATE_BOOL:
            output->immediate.boolean = input->immediate.boolean;
            break;
        case XR_CORE_IR_IMMEDIATE_CONSTANT:
        case XR_CORE_IR_IMMEDIATE_FUNCTION:
            output->immediate.key = input->immediate.key;
            break;
        case XR_CORE_IR_IMMEDIATE_FIELD:
            output->immediate.field_ordinal = input->immediate.field_ordinal;
            break;
        case XR_CORE_IR_IMMEDIATE_VARIANT:
            output->immediate.variant_ordinal = input->immediate.variant_ordinal;
            break;
        case XR_CORE_IR_IMMEDIATE_VARIANT_FIELD:
            output->immediate.variant_field.variant_ordinal =
                input->immediate.variant_field.variant_ordinal;
            output->immediate.variant_field.field_ordinal =
                input->immediate.variant_field.field_ordinal;
            break;
        case XR_CORE_IR_IMMEDIATE_TYPE:
            output->immediate.type_id = input->immediate.type_id;
            break;
        default:
            return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (!copy_bytes((void **) &output->operands, input->operands, input->operand_count,
                    sizeof(XrCoreIrKey)) ||
        !copy_bytes((void **) &output->successors, input->successors, input->successor_count,
                    sizeof(XrCoreIrKey))) {
        free_instruction(output);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_block(const XrCoreIrBlockInput *input, XrCoreIrBlock *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->argument_count = input->argument_count;
    output->instruction_count = input->instruction_count;
    if (!copy_bytes((void **) &output->arguments, input->arguments, input->argument_count,
                    sizeof(XrCoreIrValueInput)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (input->instruction_count != 0) {
        if (!input->instructions) {
            free_block(output);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        output->instructions = xr_calloc(input->instruction_count, sizeof(XrCoreIrInstruction));
        if (!output->instructions) {
            free_block(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
    } else if (input->instructions) {
        free_block(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    for (uint32_t index = 0; index < input->instruction_count; ++index) {
        XrProgramBuildStatus status =
            copy_instruction(&input->instructions[index], &output->instructions[index]);
        if (status != XR_PROGRAM_BUILD_OK) {
            free_block(output);
            return status;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_function(const XrCoreIrFunctionInput *input,
                                          XrCoreIrFunction *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->parameter_count = input->parameter_count;
    output->has_receiver = input->has_receiver;
    output->receiver_mode = input->receiver_mode;
    output->result_type_id = input->result_type_id;
    output->result_ownership = input->result_ownership;
    output->result_borrow_origin_count = input->result_borrow_origin_count;
    output->error_type_id = input->error_type_id;
    output->panic_type_id = input->panic_type_id;
    output->effect_mask = input->effect_mask;
    output->capability_mask = input->capability_mask;
    output->entry_block = input->entry_block;
    output->block_count = input->block_count;
    output->root_count = input->root_count;
    output->value_root_set_count = input->value_root_set_count;
    output->flags = input->flags;
    if (!copy_bytes((void **) &output->parameter_types, input->parameter_types,
                    input->parameter_count, sizeof(uint16_t)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (input->parameter_count != 0u) {
        output->parameter_modes = xr_calloc(input->parameter_count, sizeof(XrParamMode));
        if (!output->parameter_modes) {
            free_function(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t parameter = 0; parameter < input->parameter_count; ++parameter) {
            output->parameter_modes[parameter] =
                input->parameter_modes ? input->parameter_modes[parameter] : XR_PARAM_READ;
        }
    } else if (input->parameter_modes) {
        free_function(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (!copy_bytes((void **) &output->result_borrow_origins, input->result_borrow_origins,
                    input->result_borrow_origin_count, sizeof(XrViewOrigin))) {
        free_function(output);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (output->result_borrow_origin_count != 0u) {
        qsort(output->result_borrow_origins, output->result_borrow_origin_count,
              sizeof(XrViewOrigin), view_origin_compare);
        uint32_t unique = 0;
        for (uint32_t index = 0; index < output->result_borrow_origin_count; ++index) {
            if (unique == 0u || view_origin_compare(&output->result_borrow_origins[unique - 1u],
                                                    &output->result_borrow_origins[index]) != 0)
                output->result_borrow_origins[unique++] = output->result_borrow_origins[index];
        }
        output->result_borrow_origin_count = unique;
    }
    if (!copy_bytes((void **) &output->roots, input->roots, input->root_count,
                    sizeof(XrCoreIrRoot))) {
        free_function(output);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    if (input->value_root_set_count != 0u) {
        if (!input->value_root_sets) {
            free_function(output);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        output->value_root_sets =
            xr_calloc(input->value_root_set_count, sizeof(XrCoreIrValueRootSet));
        if (!output->value_root_sets) {
            free_function(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t index = 0; index < input->value_root_set_count; ++index) {
            const XrCoreIrValueRootSetInput *source = &input->value_root_sets[index];
            XrCoreIrValueRootSet *target = &output->value_root_sets[index];
            target->value = source->value;
            target->root_count = source->root_count;
            if (!copy_bytes((void **) &target->roots, source->roots, source->root_count,
                            sizeof(XrCoreIrKey))) {
                free_function(output);
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            }
        }
    } else if (input->value_root_sets) {
        free_function(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (input->block_count == 0 || !input->blocks) {
        free_function(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    output->blocks = xr_calloc(input->block_count, sizeof(XrCoreIrBlock));
    if (!output->blocks) {
        free_function(output);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    for (uint32_t index = 0; index < input->block_count; ++index) {
        XrProgramBuildStatus status = copy_block(&input->blocks[index], &output->blocks[index]);
        if (status != XR_PROGRAM_BUILD_OK) {
            free_function(output);
            return status;
        }
    }
    qsort(output->blocks, output->block_count, sizeof(XrCoreIrBlock), block_compare);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_module(const XrCoreIrModuleInput *input, XrCoreIrModule *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->constant_count = input->constant_count;
    output->function_count = input->function_count;
    if (!copy_bytes((void **) &output->constants, input->constants, input->constant_count,
                    sizeof(XrCoreIrConstantInput)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (input->function_count != 0) {
        if (!input->functions)
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        output->functions = xr_calloc(input->function_count, sizeof(XrCoreIrFunction));
        if (!output->functions)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    } else if (input->functions) {
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    for (uint32_t index = 0; index < input->function_count; ++index) {
        XrProgramBuildStatus status =
            copy_function(&input->functions[index], &output->functions[index]);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    qsort(output->functions, output->function_count, sizeof(XrCoreIrFunction), function_compare);
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_type(const XrCoreIrTypeInput *input, XrCoreIrType *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->type_id = input->local_id;
    output->kind = input->kind;
    output->nominal_kind = input->nominal_kind;
    output->ownership = input->ownership;
    output->copy_contract = input->copy_contract;
    output->field_count = input->field_count;
    output->variant_count = input->variant_count;
    output->view_element_type = input->view_element_type;
    output->view_capability = input->view_capability;
    output->existential_interface = input->existential_interface;
    output->interface_use_kind = input->interface_use_kind;
    if (!copy_bytes((void **) &output->field_types, input->field_types, input->field_count,
                    sizeof(uint16_t)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (input->variant_count != 0u) {
        if (!input->variants) {
            free_type(output);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        output->variants = xr_calloc(input->variant_count, sizeof(XrCoreIrVariant));
        if (!output->variants) {
            free_type(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
    } else if (input->variants) {
        free_type(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    for (uint32_t variant = 0; variant < input->variant_count; ++variant) {
        output->variants[variant].payload_count = input->variants[variant].payload_count;
        if (!copy_bytes((void **) &output->variants[variant].payload_types,
                        input->variants[variant].payload_types,
                        input->variants[variant].payload_count, sizeof(uint16_t))) {
            free_type(output);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
    }
    if (input->kind == XR_CORE_IR_TYPE_CALLABLE) {
        XrProgramBuildStatus status =
            copy_signature(input->callable_signature, &output->callable_signature);
        if (status != XR_PROGRAM_BUILD_OK) {
            free_type(output);
            return status;
        }
    } else if (input->callable_signature) {
        free_type(output);
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_interface(const XrCoreIrInterfaceInput *input,
                                           XrCoreIrInterface *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->slot_count = input->slot_count;
    if (input->slot_count == 0u || !input->slots)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    output->slots = xr_calloc(input->slot_count, sizeof(XrCoreIrCallableSignature));
    if (!output->slots)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t slot = 0; slot < input->slot_count; ++slot) {
        XrProgramBuildStatus status = copy_signature(&input->slots[slot], &output->slots[slot]);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus copy_conformance(const XrCoreIrConformanceInput *input,
                                             XrCoreIrConformance *output) {
    memset(output, 0, sizeof(*output));
    output->key = input->key;
    output->implementor_type_id = input->implementor_type_id;
    output->implementor_kind = input->implementor_kind;
    output->interface_key = input->interface_key;
    output->slot_count = input->slot_count;
    if (!copy_bytes((void **) &output->slot_functions, input->slot_functions, input->slot_count,
                    sizeof(XrCoreIrKey)))
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    return XR_PROGRAM_BUILD_OK;
}

static bool remap_type_id(const XrCoreIrType *types, uint32_t count, uint16_t old_id,
                          uint16_t *new_id) {
    if (type_id_is_builtin(old_id)) {
        *new_id = old_id;
        return true;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (types[index].type_id == old_id) {
            *new_id = (uint16_t) (XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + index);
            return true;
        }
    }
    return false;
}

static bool remap_program_types(XrCoreIrProgram *program) {
    for (uint32_t index = 0; index < program->type_count; ++index) {
        XrCoreIrType *type = &program->types[index];
        for (uint32_t field = 0; field < type->field_count; ++field) {
            if (!remap_type_id(program->types, program->type_count, type->field_types[field],
                               &type->field_types[field]))
                return false;
        }
        for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
            XrCoreIrVariant *row = &type->variants[variant];
            for (uint32_t field = 0; field < row->payload_count; ++field) {
                if (!remap_type_id(program->types, program->type_count, row->payload_types[field],
                                   &row->payload_types[field]))
                    return false;
            }
        }
        if (type->kind == XR_CORE_IR_TYPE_VIEW &&
            !remap_type_id(program->types, program->type_count, type->view_element_type,
                           &type->view_element_type))
            return false;
        if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
            XrCoreIrCallableSignature *signature = &type->callable_signature;
            for (uint32_t parameter = 0; parameter < signature->parameter_count; ++parameter)
                if (!remap_type_id(program->types, program->type_count,
                                   signature->parameter_types[parameter],
                                   &signature->parameter_types[parameter]))
                    return false;
            if (!remap_type_id(program->types, program->type_count, signature->result_type_id,
                               &signature->result_type_id) ||
                !remap_type_id(program->types, program->type_count, signature->error_type_id,
                               &signature->error_type_id) ||
                !remap_type_id(program->types, program->type_count, signature->panic_type_id,
                               &signature->panic_type_id))
                return false;
        }
    }
    for (uint32_t interface = 0; interface < program->interface_count; ++interface) {
        XrCoreIrInterface *row = &program->interfaces[interface];
        for (uint32_t slot = 0; slot < row->slot_count; ++slot) {
            XrCoreIrCallableSignature *signature = &row->slots[slot];
            for (uint32_t parameter = 0; parameter < signature->parameter_count; ++parameter)
                if (!remap_type_id(program->types, program->type_count,
                                   signature->parameter_types[parameter],
                                   &signature->parameter_types[parameter]))
                    return false;
            if (!remap_type_id(program->types, program->type_count, signature->result_type_id,
                               &signature->result_type_id) ||
                !remap_type_id(program->types, program->type_count, signature->error_type_id,
                               &signature->error_type_id) ||
                !remap_type_id(program->types, program->type_count, signature->panic_type_id,
                               &signature->panic_type_id))
                return false;
        }
    }
    for (uint32_t conformance = 0; conformance < program->conformance_count; ++conformance)
        if (!remap_type_id(program->types, program->type_count,
                           program->conformances[conformance].implementor_type_id,
                           &program->conformances[conformance].implementor_type_id))
            return false;
    for (uint32_t module = 0; module < program->module_count; ++module) {
        XrCoreIrModule *module_row = &program->modules[module];
        for (uint32_t constant = 0; constant < module_row->constant_count; ++constant) {
            if (!remap_type_id(program->types, program->type_count,
                               module_row->constants[constant].type_id,
                               &module_row->constants[constant].type_id))
                return false;
        }
        for (uint32_t function = 0; function < module_row->function_count; ++function) {
            XrCoreIrFunction *function_row = &module_row->functions[function];
            if (!remap_type_id(program->types, program->type_count, function_row->result_type_id,
                               &function_row->result_type_id))
                return false;
            if (!remap_type_id(program->types, program->type_count, function_row->error_type_id,
                               &function_row->error_type_id))
                return false;
            if (!remap_type_id(program->types, program->type_count, function_row->panic_type_id,
                               &function_row->panic_type_id))
                return false;
            for (uint32_t parameter = 0; parameter < function_row->parameter_count; ++parameter) {
                if (!remap_type_id(program->types, program->type_count,
                                   function_row->parameter_types[parameter],
                                   &function_row->parameter_types[parameter]))
                    return false;
            }
            for (uint32_t block = 0; block < function_row->block_count; ++block) {
                XrCoreIrBlock *block_row = &function_row->blocks[block];
                for (uint32_t argument = 0; argument < block_row->argument_count; ++argument) {
                    if (!remap_type_id(program->types, program->type_count,
                                       block_row->arguments[argument].type_id,
                                       &block_row->arguments[argument].type_id))
                        return false;
                }
                for (uint32_t instruction = 0; instruction < block_row->instruction_count;
                     ++instruction) {
                    XrCoreIrInstruction *row = &block_row->instructions[instruction];
                    if (!remap_type_id(program->types, program->type_count, row->result_type_id,
                                       &row->result_type_id))
                        return false;
                    if (row->immediate_kind == XR_CORE_IR_IMMEDIATE_TYPE &&
                        !remap_type_id(program->types, program->type_count, row->immediate.type_id,
                                       &row->immediate.type_id))
                        return false;
                }
            }
        }
    }
    for (uint32_t index = 0; index < program->type_count; ++index)
        program->types[index].type_id = (uint16_t) (XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + index);
    return true;
}

static const XrCoreIrFunction *find_function(const XrCoreIrProgram *program, XrCoreIrKey key) {
    for (uint32_t module_index = 0; module_index < program->module_count; ++module_index) {
        const XrCoreIrModule *module = &program->modules[module_index];
        for (uint32_t index = 0; index < module->function_count; ++index) {
            if (xr_core_ir_key_equal(module->functions[index].key, key))
                return &module->functions[index];
        }
    }
    return NULL;
}

static const XrCoreIrConstantInput *find_constant(const XrCoreIrProgram *program, XrCoreIrKey key) {
    for (uint32_t module_index = 0; module_index < program->module_count; ++module_index) {
        const XrCoreIrModule *module = &program->modules[module_index];
        for (uint32_t index = 0; index < module->constant_count; ++index) {
            if (xr_core_ir_key_equal(module->constants[index].key, key))
                return &module->constants[index];
        }
    }
    return NULL;
}

static const XrCoreIrInterface *find_interface(const XrCoreIrProgram *program, XrCoreIrKey key) {
    for (uint32_t index = 0; index < program->interface_count; ++index)
        if (xr_core_ir_key_equal(program->interfaces[index].key, key))
            return &program->interfaces[index];
    return NULL;
}

static const XrCoreIrBlock *find_block(const XrCoreIrFunction *function, XrCoreIrKey key) {
    for (uint32_t index = 0; index < function->block_count; ++index) {
        if (xr_core_ir_key_equal(function->blocks[index].key, key))
            return &function->blocks[index];
    }
    return NULL;
}

static bool function_has_value(const XrCoreIrFunction *function, XrCoreIrKey key) {
    size_t occurrences = 0;
    for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
        const XrCoreIrBlock *block = &function->blocks[block_index];
        for (uint32_t index = 0; index < block->argument_count; ++index) {
            if (xr_core_ir_key_equal(block->arguments[index].key, key))
                ++occurrences;
        }
        for (uint32_t index = 0; index < block->instruction_count; ++index) {
            if (!xr_core_ir_key_is_zero(block->instructions[index].result) &&
                xr_core_ir_key_equal(block->instructions[index].result, key))
                ++occurrences;
        }
    }
    return occurrences == 1u;
}

static bool function_value_contract(const XrCoreIrFunction *function, XrCoreIrKey key,
                                    uint16_t *type_id, XrCoreIrValueCategory *category,
                                    XrCoreIrOwnershipDisposition *ownership) {
    for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
        const XrCoreIrBlock *block = &function->blocks[block_index];
        for (uint32_t index = 0; index < block->argument_count; ++index) {
            if (!xr_core_ir_key_equal(block->arguments[index].key, key))
                continue;
            *type_id = block->arguments[index].type_id;
            *category = block->arguments[index].category;
            *ownership = block->arguments[index].ownership;
            return true;
        }
        for (uint32_t index = 0; index < block->instruction_count; ++index) {
            const XrCoreIrInstruction *instruction = &block->instructions[index];
            if (!xr_core_ir_key_is_zero(instruction->result) &&
                xr_core_ir_key_equal(instruction->result, key)) {
                *type_id = instruction->result_type_id;
                *category = instruction->result_category;
                *ownership = instruction->result_ownership;
                return true;
            }
        }
    }
    return false;
}

static const XrCoreIrRoot *function_root(const XrCoreIrFunction *function, XrCoreIrKey key) {
    const XrCoreIrRoot *match = NULL;
    for (uint32_t index = 0; index < function->root_count; ++index) {
        if (!xr_core_ir_key_equal(function->roots[index].key, key))
            continue;
        if (match)
            return NULL;
        match = &function->roots[index];
    }
    return match;
}

static const XrCoreIrValueRootSet *function_value_root_set(const XrCoreIrFunction *function,
                                                           XrCoreIrKey value) {
    const XrCoreIrValueRootSet *match = NULL;
    for (uint32_t index = 0; index < function->value_root_set_count; ++index) {
        if (!xr_core_ir_key_equal(function->value_root_sets[index].value, value))
            continue;
        if (match)
            return NULL;
        match = &function->value_root_sets[index];
    }
    return match;
}

static bool type_is_view(const XrCoreIrProgram *program, uint16_t type_id) {
    return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
           (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE < program->type_count &&
           program->types[type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE].kind == XR_CORE_IR_TYPE_VIEW;
}

static bool type_is_existential_ref(const XrCoreIrProgram *program, uint16_t type_id) {
    if (type_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ||
        (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE >= program->type_count)
        return false;
    const XrCoreIrType *type = &program->types[type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE];
    return type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
           type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF;
}

static XrCoreIrCallableSignature function_signature(const XrCoreIrFunction *function) {
    XrCoreIrCallableSignature signature = {
        .parameter_types = function->parameter_types,
        .parameter_modes = function->parameter_modes,
        .parameter_count = function->parameter_count,
        .has_receiver = function->has_receiver,
        .receiver_mode = function->receiver_mode,
        .result_type_id = function->result_type_id,
        .result_ownership = function->result_ownership,
        .result_borrow_origins = function->result_borrow_origins,
        .result_borrow_origin_count = function->result_borrow_origin_count,
        .error_type_id = function->error_type_id,
        .panic_type_id = function->panic_type_id,
        .effect_mask = function->effect_mask,
        .capability_mask = function->capability_mask,
    };
    return signature;
}

static bool signature_is_valid(const XrCoreIrProgram *program,
                               const XrCoreIrCallableSignature *signature) {
    if (!signature || signature->parameter_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
        (signature->parameter_count != 0u &&
         (!signature->parameter_types || !signature->parameter_modes)) ||
        !type_id_supported(program, signature->result_type_id) ||
        !type_id_supported(program, signature->error_type_id) ||
        !type_id_supported(program, signature->panic_type_id) ||
        type_is_existential_ref(program, signature->result_type_id) ||
        type_is_existential_ref(program, signature->error_type_id) ||
        (signature->panic_type_id != XR_CORE_TYPE_VOID &&
         signature->panic_type_id != XR_CORE_TYPE_PANIC_INFO) ||
        !ownership_disposition_is_valid(signature->result_ownership) ||
        (signature->result_type_id == XR_CORE_TYPE_VOID &&
         signature->result_ownership != XR_CORE_IR_NON_OWNER) ||
        (signature->result_type_id != XR_CORE_TYPE_VOID &&
         signature->result_ownership !=
             (type_ownership(program, signature->result_type_id) == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                  ? XR_CORE_IR_OWNER
                  : XR_CORE_IR_NON_OWNER)) ||
        !xr_param_mode_is_valid(signature->receiver_mode) ||
        (signature->has_receiver && (signature->parameter_count == 0u ||
                                     signature->parameter_modes[0] != signature->receiver_mode)) ||
        (!signature->has_receiver && signature->receiver_mode != XR_PARAM_READ) ||
        signature->result_borrow_origin_count > XR_PROGRAM_LIMIT_ROOTS_PER_VALUE ||
        (signature->result_borrow_origin_count != 0u && !signature->result_borrow_origins))
        return false;
    for (uint32_t parameter = 0; parameter < signature->parameter_count; ++parameter)
        if (!type_id_supported(program, signature->parameter_types[parameter]) ||
            signature->parameter_types[parameter] == XR_CORE_TYPE_VOID ||
            !xr_param_mode_is_valid(signature->parameter_modes[parameter]))
            return false;
    uint32_t explicit_parameter_count =
        signature->parameter_count - (signature->has_receiver ? 1u : 0u);
    for (uint32_t index = 0; index < signature->result_borrow_origin_count; ++index) {
        const XrViewOrigin *origin = &signature->result_borrow_origins[index];
        if ((index != 0u &&
             view_origin_compare(&signature->result_borrow_origins[index - 1u], origin) >= 0) ||
            origin->kind > XR_VIEW_ORIGIN_STATIC)
            return false;
        if (origin->kind == XR_VIEW_ORIGIN_PARAM) {
            if (origin->param_ordinal < 0 ||
                (uint32_t) origin->param_ordinal >= explicit_parameter_count ||
                signature->parameter_modes[(uint32_t) origin->param_ordinal +
                                           (signature->has_receiver ? 1u : 0u)] != XR_PARAM_READ)
                return false;
        } else if (origin->param_ordinal != -1 ||
                   (origin->kind == XR_VIEW_ORIGIN_RECEIVER &&
                    (!signature->has_receiver || signature->receiver_mode != XR_PARAM_READ))) {
            return false;
        }
    }
    bool readonly_view_result =
        type_is_view(program, signature->result_type_id) &&
        program->types[signature->result_type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE]
                .view_capability == XR_CORE_IR_VIEW_READ;
    return readonly_view_result == (signature->result_borrow_origin_count != 0u);
}

static bool signature_satisfies_interface(const XrCoreIrProgram *program,
                                          const XrCoreIrCallableSignature *implementation,
                                          const XrCoreIrCallableSignature *requirement,
                                          uint16_t implementor_type, XrCoreIrKey interface_key) {
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
            if (implementation->parameter_types[index] != implementor_type ||
                requirement->parameter_types[index] < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE)
                return false;
            const XrCoreIrType *receiver = &program->types[requirement->parameter_types[index] -
                                                           XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE];
            if (receiver->kind != XR_CORE_IR_TYPE_EXISTENTIAL ||
                !xr_core_ir_key_equal(receiver->existential_interface, interface_key))
                return false;
        } else if (implementation->parameter_types[index] != requirement->parameter_types[index]) {
            return false;
        }
    }
    for (uint32_t index = 0; index < requirement->result_borrow_origin_count; ++index)
        if (view_origin_compare(&implementation->result_borrow_origins[index],
                                &requirement->result_borrow_origins[index]) != 0)
            return false;
    return true;
}

static bool roots_have_same_identity(const XrCoreIrRoot *left, const XrCoreIrRoot *right) {
    if (left->kind != right->kind)
        return false;
    if (left->kind == XR_CORE_IR_ROOT_PARAMETER)
        return left->parameter_ordinal == right->parameter_ordinal;
    if (left->kind == XR_CORE_IR_ROOT_LOCAL)
        return xr_core_ir_key_equal(left->source_value, right->source_value);
    return true;
}

static bool validate_function_roots(const XrCoreIrProgram *program,
                                    const XrCoreIrFunction *function) {
    uint32_t explicit_parameter_count =
        function->parameter_count - (function->has_receiver ? 1u : 0u);
    for (uint32_t index = 0; index < function->result_borrow_origin_count; ++index) {
        const XrViewOrigin *origin = &function->result_borrow_origins[index];
        if ((index != 0u &&
             view_origin_compare(&function->result_borrow_origins[index - 1u], origin) >= 0) ||
            origin->kind > XR_VIEW_ORIGIN_STATIC)
            return false;
        if (origin->kind == XR_VIEW_ORIGIN_PARAM) {
            if (origin->param_ordinal < 0 ||
                (uint32_t) origin->param_ordinal >= explicit_parameter_count ||
                function->parameter_modes[(uint32_t) origin->param_ordinal +
                                          (function->has_receiver ? 1u : 0u)] != XR_PARAM_READ)
                return false;
        } else if (origin->param_ordinal != -1 ||
                   (origin->kind == XR_VIEW_ORIGIN_RECEIVER &&
                    (!function->has_receiver || function->receiver_mode != XR_PARAM_READ))) {
            return false;
        }
    }
    bool readonly_view_result =
        type_is_view(program, function->result_type_id) &&
        program->types[function->result_type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE]
                .view_capability == XR_CORE_IR_VIEW_READ;
    if (readonly_view_result != (function->result_borrow_origin_count != 0u))
        return false;

    for (uint32_t index = 0; index < function->root_count; ++index) {
        const XrCoreIrRoot *root = &function->roots[index];
        if (xr_core_ir_key_is_zero(root->key) || root->kind > XR_CORE_IR_ROOT_LOCAL)
            return false;
        for (uint32_t prior = 0; prior < index; ++prior) {
            if (xr_core_ir_key_equal(root->key, function->roots[prior].key) ||
                roots_have_same_identity(root, &function->roots[prior]))
                return false;
        }
        if (root->kind == XR_CORE_IR_ROOT_PARAMETER) {
            if (root->parameter_ordinal < 0 ||
                (uint32_t) root->parameter_ordinal >= explicit_parameter_count ||
                !xr_core_ir_key_is_zero(root->source_value))
                return false;
        } else if (root->kind == XR_CORE_IR_ROOT_RECEIVER) {
            if (!function->has_receiver || root->parameter_ordinal != -1 ||
                !xr_core_ir_key_is_zero(root->source_value))
                return false;
        } else if (root->kind == XR_CORE_IR_ROOT_STATIC) {
            if (root->parameter_ordinal != -1 || !xr_core_ir_key_is_zero(root->source_value))
                return false;
        } else {
            uint16_t source_type = XR_CORE_TYPE_VOID;
            XrCoreIrValueCategory source_category = XR_CORE_IR_VALUE;
            XrCoreIrOwnershipDisposition source_ownership = XR_CORE_IR_NON_OWNER;
            if (root->parameter_ordinal != -1 || xr_core_ir_key_is_zero(root->source_value) ||
                !function_value_contract(function, root->source_value, &source_type,
                                         &source_category, &source_ownership) ||
                source_ownership != XR_CORE_IR_OWNER || type_is_view(program, source_type))
                return false;
        }
    }

    for (uint32_t index = 0; index < function->value_root_set_count; ++index) {
        const XrCoreIrValueRootSet *set = &function->value_root_sets[index];
        uint16_t value_type = XR_CORE_TYPE_VOID;
        XrCoreIrValueCategory category = XR_CORE_IR_VALUE;
        XrCoreIrOwnershipDisposition ownership = XR_CORE_IR_NON_OWNER;
        if (xr_core_ir_key_is_zero(set->value) || set->root_count == 0u || !set->roots ||
            !function_value_contract(function, set->value, &value_type, &category, &ownership) ||
            !type_is_view(program, value_type) ||
            function_value_root_set(function, set->value) != set)
            return false;
        for (uint32_t root = 0; root < set->root_count; ++root) {
            if (!function_root(function, set->roots[root]))
                return false;
            for (uint32_t prior = 0; prior < root; ++prior)
                if (xr_core_ir_key_equal(set->roots[root], set->roots[prior]))
                    return false;
        }
    }
    for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
        const XrCoreIrBlock *block = &function->blocks[block_index];
        for (uint32_t argument = 0; argument < block->argument_count; ++argument)
            if (type_is_view(program, block->arguments[argument].type_id) &&
                !function_value_root_set(function, block->arguments[argument].key))
                return false;
        for (uint32_t instruction = 0; instruction < block->instruction_count; ++instruction) {
            const XrCoreIrInstruction *row = &block->instructions[instruction];
            if (!xr_core_ir_key_is_zero(row->result) &&
                type_is_view(program, row->result_type_id) &&
                !function_value_root_set(function, row->result))
                return false;
        }
    }
    return true;
}

static bool type_graph_visit(const XrCoreIrProgram *program, uint32_t index, uint8_t *state) {
    if (state[index] == 1u)
        return false;
    if (state[index] == 2u)
        return true;
    state[index] = 1u;
    const XrCoreIrType *type = &program->types[index];
    if (type->kind == XR_CORE_IR_TYPE_VIEW || type->kind == XR_CORE_IR_TYPE_CALLABLE ||
        type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        state[index] = 2u;
        return true;
    }
    for (uint32_t field = 0; field < type->field_count; ++field) {
        uint16_t child = type->field_types[field];
        if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            !type_graph_visit(program, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
            return false;
    }
    for (uint32_t variant = 0; variant < type->variant_count; ++variant) {
        const XrCoreIrVariant *row = &type->variants[variant];
        for (uint32_t field = 0; field < row->payload_count; ++field) {
            uint16_t child = row->payload_types[field];
            if (child >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                !type_graph_visit(program, child - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE, state))
                return false;
        }
    }
    state[index] = 2u;
    return true;
}

static bool validate_types(const XrCoreIrProgram *program) {
    uint8_t *state = xr_calloc(program->type_count ? program->type_count : 1u, sizeof(uint8_t));
    if (!state)
        return false;
    bool valid = true;
    for (uint32_t index = 0; valid && index < program->type_count; ++index) {
        const XrCoreIrType *type = &program->types[index];
        valid = !xr_core_ir_key_is_zero(type->key) &&
                type->type_id == XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + index &&
                (index == 0 || !xr_core_ir_key_equal(program->types[index - 1u].key, type->key)) &&
                type->ownership <= XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                type->copy_contract <= XR_CORE_IR_COPY_FORBIDDEN &&
                ((type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL &&
                  type->copy_contract == XR_CORE_IR_COPY_TRIVIAL) ||
                 (type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                  type->copy_contract != XR_CORE_IR_COPY_TRIVIAL));
        if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
            valid = valid && type->field_count != 0u && type->field_types &&
                    type->variant_count == 0u && !type->variants &&
                    type->nominal_kind <= XR_CORE_IR_NOMINAL_ENUM;
            for (uint32_t field = 0; valid && field < type->field_count; ++field)
                valid = type_id_supported(program, type->field_types[field]) &&
                        type->field_types[field] != XR_CORE_TYPE_VOID;
        } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
            valid = valid && type->field_count == 0u && !type->field_types &&
                    type->variant_count != 0u && type->variants &&
                    type->nominal_kind <= XR_CORE_IR_NOMINAL_ENUM;
            for (uint32_t variant = 0; valid && variant < type->variant_count; ++variant) {
                const XrCoreIrVariant *row = &type->variants[variant];
                valid = row->payload_count == 0u || row->payload_types;
                for (uint32_t field = 0; valid && field < row->payload_count; ++field)
                    valid = type_id_supported(program, row->payload_types[field]) &&
                            row->payload_types[field] != XR_CORE_TYPE_VOID;
            }
        } else if (type->kind == XR_CORE_IR_TYPE_VIEW) {
            valid = valid && type->field_count == 0u && !type->field_types &&
                    type->variant_count == 0u && !type->variants &&
                    type->nominal_kind == XR_CORE_IR_NOMINAL_NONE &&
                    type_id_supported(program, type->view_element_type) &&
                    type->view_element_type != XR_CORE_TYPE_VOID &&
                    (type->view_capability == XR_CORE_IR_VIEW_READ ||
                     type->view_capability == XR_CORE_IR_VIEW_WRITE_EXCLUSIVE) &&
                    type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL &&
                    type->copy_contract == XR_CORE_IR_COPY_TRIVIAL;
        } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
            valid = valid && type->field_count == 0u && !type->field_types &&
                    type->variant_count == 0u && !type->variants &&
                    type->nominal_kind == XR_CORE_IR_NOMINAL_NONE &&
                    type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL &&
                    type->copy_contract == XR_CORE_IR_COPY_TRIVIAL &&
                    signature_is_valid(program, &type->callable_signature);
        } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
            bool trivial = type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_READ;
            bool affine =
                type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF ||
                type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE;
            valid = valid && type->field_count == 0u && !type->field_types &&
                    type->variant_count == 0u && !type->variants &&
                    type->nominal_kind == XR_CORE_IR_NOMINAL_NONE &&
                    !xr_core_ir_key_is_zero(type->existential_interface) &&
                    find_interface(program, type->existential_interface) != NULL &&
                    (trivial || affine) &&
                    (trivial ? type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL &&
                                   type->copy_contract == XR_CORE_IR_COPY_TRIVIAL
                             : type->ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE &&
                                   type->copy_contract == XR_CORE_IR_COPY_FORBIDDEN);
        } else {
            valid = false;
        }
        if (valid)
            valid = type_graph_visit(program, index, state);
    }
    for (uint32_t index = 0; valid && index < program->type_count; ++index) {
        const XrCoreIrType *type = &program->types[index];
        for (uint32_t field = 0; valid && field < type->field_count; ++field)
            valid = !type_is_existential_ref(program, type->field_types[field]) &&
                    (type->ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                     type_ownership(program, type->field_types[field]) ==
                         XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL);
        for (uint32_t variant = 0; valid && variant < type->variant_count; ++variant) {
            const XrCoreIrVariant *row = &type->variants[variant];
            for (uint32_t field = 0; valid && field < row->payload_count; ++field)
                valid = !type_is_existential_ref(program, row->payload_types[field]) &&
                        (type->ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                         type_ownership(program, row->payload_types[field]) ==
                             XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL);
        }
    }
    xr_free(state);
    return valid;
}

static bool validate_program_tables(const XrCoreIrProgram *program) {
    for (uint32_t interface = 0; interface < program->interface_count; ++interface) {
        const XrCoreIrInterface *row = &program->interfaces[interface];
        if (xr_core_ir_key_is_zero(row->key) || row->slot_count == 0u || !row->slots ||
            row->slot_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            (interface != 0u &&
             key_compare_value(program->interfaces[interface - 1u].key, row->key) >= 0))
            return false;
        for (uint32_t slot = 0; slot < row->slot_count; ++slot)
            if (!signature_is_valid(program, &row->slots[slot]))
                return false;
    }
    for (uint32_t conformance = 0; conformance < program->conformance_count; ++conformance) {
        const XrCoreIrConformance *row = &program->conformances[conformance];
        const XrCoreIrInterface *interface = find_interface(program, row->interface_key);
        const XrCoreIrType *implementor =
            row->implementor_type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
                    (uint32_t) row->implementor_type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE <
                        program->type_count
                ? &program->types[row->implementor_type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE]
                : NULL;
        if (xr_core_ir_key_is_zero(row->key) || !interface || !implementor ||
            row->implementor_kind == XR_CORE_IR_NOMINAL_NONE ||
            row->implementor_kind > XR_CORE_IR_NOMINAL_ENUM ||
            implementor->nominal_kind != row->implementor_kind ||
            (implementor->kind != XR_CORE_IR_TYPE_AGGREGATE &&
             implementor->kind != XR_CORE_IR_TYPE_VARIANT) ||
            row->slot_count != interface->slot_count ||
            (row->slot_count != 0u && !row->slot_functions) ||
            (conformance != 0u &&
             key_compare_value(program->conformances[conformance - 1u].key, row->key) >= 0))
            return false;
        for (uint32_t prior = 0; prior < conformance; ++prior)
            if (program->conformances[prior].implementor_type_id == row->implementor_type_id &&
                xr_core_ir_key_equal(program->conformances[prior].interface_key,
                                     row->interface_key))
                return false;
        for (uint32_t slot = 0; slot < row->slot_count; ++slot) {
            const XrCoreIrFunction *function = find_function(program, row->slot_functions[slot]);
            if (!function)
                return false;
            XrCoreIrCallableSignature signature = function_signature(function);
            if (!signature_satisfies_interface(program, &signature, &interface->slots[slot],
                                               row->implementor_type_id, row->interface_key))
                return false;
        }
    }
    return true;
}

static XrProgramBuildStatus validate_program(const XrCoreIrProgram *program, char *diagnostic,
                                             size_t diagnostic_size) {
    uint64_t constant_count = 0;
    uint64_t function_count = 0;
    bool has_base_feature = false;
    for (uint32_t index = 0; index < program->required_feature_count; ++index) {
        uint16_t feature = program->required_features[index];
        if (!xr_core_spec_feature_active(feature)) {
            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                      "required CoreSpec feature %u is unsupported", feature);
            return XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE;
        }
        if (index != 0 && program->required_features[index - 1] >= feature) {
            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                      "required features are not unique canonical IDs");
            return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
        }
        has_base_feature |= feature == XR_CORE_FEATURE_CORE_BASE;
    }
    if (!has_base_feature) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "walking skeleton requires core.base");
        return XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE;
    }
    if (!validate_types(program)) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "dynamic type graph is malformed or recursive by value");
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    if (!validate_program_tables(program)) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "interface or conformance table is invalid");
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    for (uint32_t left = 0; left < program->module_count; ++left) {
        const XrCoreIrModule *module = &program->modules[left];
        if (xr_core_ir_key_is_zero(module->key) ||
            (left != 0 && xr_core_ir_key_equal(program->modules[left - 1].key, module->key))) {
            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                      "module semantic identity is empty or duplicated");
            return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
        }
        constant_count += module->constant_count;
        function_count += module->function_count;
        for (uint32_t index = 0; index < module->constant_count; ++index) {
            const XrCoreIrConstantInput *constant = &module->constants[index];
            if (xr_core_ir_key_is_zero(constant->key) ||
                (constant->kind == XR_CORE_IR_CONSTANT_I64 &&
                 constant->type_id != XR_CORE_TYPE_I64) ||
                (constant->kind == XR_CORE_IR_CONSTANT_BOOL &&
                 constant->type_id != XR_CORE_TYPE_BOOL) ||
                (constant->kind != XR_CORE_IR_CONSTANT_I64 &&
                 constant->kind != XR_CORE_IR_CONSTANT_BOOL)) {
                xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                          "constant %u is not a canonical typed constant", index);
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            const XrCoreIrConstantInput *found = find_constant(program, constant->key);
            if (found != constant) {
                xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                          "constant semantic identity is duplicated");
                return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
            }
        }
    }
    if (constant_count > XR_PROGRAM_LIMIT_CONSTANTS || function_count == 0 ||
        function_count > XR_PROGRAM_LIMIT_FUNCTIONS) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "program constant/function count exceeds W2 limits");
        return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
    }

    for (uint32_t module_index = 0; module_index < program->module_count; ++module_index) {
        const XrCoreIrModule *module = &program->modules[module_index];
        for (uint32_t function_index = 0; function_index < module->function_count;
             ++function_index) {
            const XrCoreIrFunction *function = &module->functions[function_index];
            XrCoreIrCallableSignature signature = function_signature(function);
            if (xr_core_ir_key_is_zero(function->key) ||
                find_function(program, function->key) != function ||
                !signature_is_valid(program, &signature) ||
                !type_id_supported(program, function->result_type_id) ||
                !type_id_supported(program, function->error_type_id) ||
                !type_id_supported(program, function->panic_type_id) ||
                (function->panic_type_id != XR_CORE_TYPE_VOID &&
                 function->panic_type_id != XR_CORE_TYPE_PANIC_INFO) ||
                !ownership_disposition_is_valid(function->result_ownership) ||
                (function->result_type_id == XR_CORE_TYPE_VOID &&
                 function->result_ownership != XR_CORE_IR_NON_OWNER) ||
                (function->result_type_id != XR_CORE_TYPE_VOID &&
                 function->result_ownership != (type_ownership(program, function->result_type_id) ==
                                                        XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                                                    ? XR_CORE_IR_OWNER
                                                    : XR_CORE_IR_NON_OWNER)) ||
                !xr_param_mode_is_valid(function->receiver_mode) ||
                (function->has_receiver &&
                 (function->parameter_count == 0u ||
                  function->parameter_modes[0] != function->receiver_mode)) ||
                (!function->has_receiver && function->receiver_mode != XR_PARAM_READ) ||
                function->block_count == 0 ||
                function->block_count > XR_PROGRAM_LIMIT_BLOCKS_PER_FUNCTION ||
                function->root_count > XR_PROGRAM_LIMIT_ROOTS_PER_FUNCTION ||
                function->value_root_set_count > XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION ||
                !find_block(function, function->entry_block) ||
                !validate_function_roots(program, function)) {
                xr_program_set_diagnostic(
                    diagnostic, diagnostic_size,
                    "function identity, signature, or entry block is invalid");
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            for (uint32_t index = 0; index < function->parameter_count; ++index) {
                if (!type_id_supported(program, function->parameter_types[index]) ||
                    function->parameter_types[index] == XR_CORE_TYPE_VOID ||
                    !xr_param_mode_is_valid(function->parameter_modes[index])) {
                    xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                              "function parameter contract is unsupported");
                    return XR_PROGRAM_BUILD_INVALID_INPUT;
                }
            }
            const XrCoreIrBlock *entry = find_block(function, function->entry_block);
            if (!entry || entry->argument_count != function->parameter_count) {
                xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                          "entry block does not match function signature");
                return XR_PROGRAM_BUILD_INVALID_INPUT;
            }
            for (uint32_t index = 0; index < function->parameter_count; ++index) {
                XrCoreIrValueCategory expected = function->parameter_modes[index] == XR_PARAM_REF
                                                     ? XR_CORE_IR_PLACE
                                                     : XR_CORE_IR_VALUE;
                XrCoreIrOwnershipDisposition expected_ownership =
                    function->parameter_modes[index] == XR_PARAM_MOVE &&
                            type_ownership(program, function->parameter_types[index]) ==
                                XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                        ? XR_CORE_IR_OWNER
                        : XR_CORE_IR_NON_OWNER;
                if (entry->arguments[index].type_id != function->parameter_types[index] ||
                    entry->arguments[index].category != expected ||
                    entry->arguments[index].ownership != expected_ownership) {
                    xr_program_set_diagnostic(
                        diagnostic, diagnostic_size,
                        "entry block value/place contract does not match function signature");
                    return XR_PROGRAM_BUILD_INVALID_INPUT;
                }
            }
            uint64_t value_count = 0;
            for (uint32_t block_index = 0; block_index < function->block_count; ++block_index) {
                const XrCoreIrBlock *block = &function->blocks[block_index];
                if (xr_core_ir_key_is_zero(block->key) ||
                    find_block(function, block->key) != block) {
                    xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                              "block identity is empty or duplicated");
                    return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
                }
                value_count += block->argument_count;
                for (uint32_t index = 0; index < block->argument_count; ++index) {
                    if (xr_core_ir_key_is_zero(block->arguments[index].key) ||
                        !type_id_supported(program, block->arguments[index].type_id) ||
                        block->arguments[index].type_id == XR_CORE_TYPE_VOID ||
                        !value_contract_is_valid(program, block->arguments[index].type_id,
                                                 block->arguments[index].category,
                                                 block->arguments[index].ownership) ||
                        !function_has_value(function, block->arguments[index].key)) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "block argument is invalid");
                        return XR_PROGRAM_BUILD_INVALID_INPUT;
                    }
                }
                for (uint32_t index = 0; index < block->instruction_count; ++index) {
                    const XrCoreIrInstruction *instruction = &block->instructions[index];
                    if (!xr_core_spec_operation_by_id(instruction->operation_id) ||
                        instruction->operand_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                        instruction->successor_count > XR_PROGRAM_LIMIT_SUCCESSORS_PER_OPERATION) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "instruction operation or arity is unsupported");
                        return XR_PROGRAM_BUILD_INVALID_INPUT;
                    }
                    if (!xr_core_ir_key_is_zero(instruction->result)) {
                        if (!type_id_supported(program, instruction->result_type_id) ||
                            instruction->result_type_id == XR_CORE_TYPE_VOID ||
                            !value_contract_is_valid(program, instruction->result_type_id,
                                                     instruction->result_category,
                                                     instruction->result_ownership)) {
                            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                      "instruction result type is unsupported");
                            return XR_PROGRAM_BUILD_INVALID_INPUT;
                        }
                        if (!function_has_value(function, instruction->result)) {
                            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                      "instruction result identity is duplicated");
                            return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
                        }
                        value_count++;
                    } else if (instruction->result_type_id != XR_CORE_TYPE_VOID ||
                               instruction->result_category != XR_CORE_IR_VALUE ||
                               instruction->result_ownership != XR_CORE_IR_NON_OWNER) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "result-less instruction must use void type");
                        return XR_PROGRAM_BUILD_INVALID_INPUT;
                    }
                    for (uint32_t operand = 0; operand < instruction->operand_count; ++operand) {
                        if (!function_has_value(function, instruction->operands[operand])) {
                            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                      "instruction has unresolved value operand");
                            return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
                        }
                    }
                    for (uint32_t successor = 0; successor < instruction->successor_count;
                         ++successor) {
                        if (!find_block(function, instruction->successors[successor])) {
                            xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                      "instruction has unresolved successor");
                            return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
                        }
                    }
                    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_CONSTANT &&
                        !find_constant(program, instruction->immediate.key)) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "instruction has unresolved constant");
                        return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
                    }
                    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION &&
                        !find_function(program, instruction->immediate.key)) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "instruction has unresolved function");
                        return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
                    }
                    if (instruction->immediate_kind == XR_CORE_IR_IMMEDIATE_TYPE &&
                        (!type_id_supported(program, instruction->immediate.type_id) ||
                         instruction->immediate.type_id == XR_CORE_TYPE_VOID)) {
                        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                                  "instruction has unresolved type immediate");
                        return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
                    }
                }
            }
            if (value_count > XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION) {
                xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                          "function value count exceeds W2 limit");
                return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

XrProgramBuildStatus xr_core_ir_program_build(const XrCoreIrProgramInput *input,
                                              XrCoreIrProgram **program_out, char *diagnostic,
                                              size_t diagnostic_size) {
    if (program_out)
        *program_out = NULL;
    if (diagnostic && diagnostic_size != 0)
        diagnostic[0] = '\0';
    if (!input || !program_out || !input->semantic_profile_fingerprint ||
        !input->required_features || input->required_feature_count == 0 ||
        input->required_feature_count > XR_PROGRAM_LIMIT_FEATURES || !input->modules ||
        input->module_count == 0 || input->module_count > XR_PROGRAM_LIMIT_FUNCTIONS ||
        input->type_count > XR_PROGRAM_LIMIT_TYPES - 6u ||
        input->type_count > UINT16_MAX - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u ||
        input->interface_count > XR_PROGRAM_LIMIT_TYPES ||
        input->conformance_count > XR_PROGRAM_LIMIT_TYPES ||
        (input->type_count == 0u) != (input->types == NULL) ||
        (input->interface_count == 0u) != (input->interfaces == NULL) ||
        (input->conformance_count == 0u) != (input->conformances == NULL)) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "CoreIR program input is incomplete");
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    }
    XrCoreIrProgram *program = xr_calloc(1, sizeof(XrCoreIrProgram));
    if (!program)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    memcpy(program->semantic_profile_fingerprint, input->semantic_profile_fingerprint,
           XR_PROGRAM_DIGEST_SIZE);
    program->required_feature_count = input->required_feature_count;
    program->type_count = input->type_count;
    program->interface_count = input->interface_count;
    program->conformance_count = input->conformance_count;
    program->module_count = input->module_count;
    if (!copy_bytes((void **) &program->required_features, input->required_features,
                    input->required_feature_count, sizeof(uint16_t))) {
        xr_core_ir_program_free(program);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    /* Insertion sort is bounded by the small feature ceiling and deterministic. */
    for (uint32_t index = 1; index < program->required_feature_count; ++index) {
        uint16_t value = program->required_features[index];
        uint32_t cursor = index;
        while (cursor != 0 && program->required_features[cursor - 1] > value) {
            program->required_features[cursor] = program->required_features[cursor - 1];
            --cursor;
        }
        program->required_features[cursor] = value;
    }
    if (input->type_count != 0u) {
        program->types = xr_calloc(input->type_count, sizeof(XrCoreIrType));
        if (!program->types) {
            xr_core_ir_program_free(program);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
    }
    for (uint32_t index = 0; index < input->type_count; ++index) {
        if (input->types[index].local_id < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE) {
            xr_core_ir_program_free(program);
            return XR_PROGRAM_BUILD_INVALID_INPUT;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (input->types[previous].local_id == input->types[index].local_id) {
                xr_core_ir_program_free(program);
                return XR_PROGRAM_BUILD_DUPLICATE_IDENTITY;
            }
        }
        XrProgramBuildStatus status = copy_type(&input->types[index], &program->types[index]);
        if (status != XR_PROGRAM_BUILD_OK) {
            xr_core_ir_program_free(program);
            return status;
        }
    }
    if (program->type_count != 0u)
        qsort(program->types, program->type_count, sizeof(XrCoreIrType), type_compare);
    if (input->interface_count != 0u) {
        program->interfaces = xr_calloc(input->interface_count, sizeof(XrCoreIrInterface));
        if (!program->interfaces) {
            xr_core_ir_program_free(program);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t index = 0; index < input->interface_count; ++index) {
            XrProgramBuildStatus status =
                copy_interface(&input->interfaces[index], &program->interfaces[index]);
            if (status != XR_PROGRAM_BUILD_OK) {
                xr_core_ir_program_free(program);
                return status;
            }
        }
        qsort(program->interfaces, program->interface_count, sizeof(XrCoreIrInterface),
              interface_compare);
    }
    if (input->conformance_count != 0u) {
        program->conformances = xr_calloc(input->conformance_count, sizeof(XrCoreIrConformance));
        if (!program->conformances) {
            xr_core_ir_program_free(program);
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        }
        for (uint32_t index = 0; index < input->conformance_count; ++index) {
            XrProgramBuildStatus status =
                copy_conformance(&input->conformances[index], &program->conformances[index]);
            if (status != XR_PROGRAM_BUILD_OK) {
                xr_core_ir_program_free(program);
                return status;
            }
        }
        qsort(program->conformances, program->conformance_count, sizeof(XrCoreIrConformance),
              conformance_compare);
    }
    program->modules = xr_calloc(input->module_count, sizeof(XrCoreIrModule));
    if (!program->modules) {
        xr_core_ir_program_free(program);
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    }
    for (uint32_t index = 0; index < input->module_count; ++index) {
        XrProgramBuildStatus status = copy_module(&input->modules[index], &program->modules[index]);
        if (status != XR_PROGRAM_BUILD_OK) {
            xr_core_ir_program_free(program);
            return status;
        }
    }
    qsort(program->modules, program->module_count, sizeof(XrCoreIrModule), module_compare);
    if (!remap_program_types(program)) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "CoreIR references an unresolved dynamic type label");
        xr_core_ir_program_free(program);
        return XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE;
    }
    XrProgramBuildStatus status = validate_program(program, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK) {
        xr_core_ir_program_free(program);
        return status;
    }
    *program_out = program;
    return XR_PROGRAM_BUILD_OK;
}

const char *xr_program_build_status_name(XrProgramBuildStatus status) {
    switch (status) {
        case XR_PROGRAM_BUILD_OK:
            return "ok";
        case XR_PROGRAM_BUILD_INVALID_INPUT:
            return "invalid-input";
        case XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE:
            return "unsupported-feature";
        case XR_PROGRAM_BUILD_DUPLICATE_IDENTITY:
            return "duplicate-identity";
        case XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE:
            return "unresolved-reference";
        case XR_PROGRAM_BUILD_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_BUILD_OUT_OF_MEMORY:
            return "out-of-memory";
        default:
            return "unknown";
    }
}
