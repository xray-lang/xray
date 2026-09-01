/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_from_xi.c - Verified Xi to canonical XrProgram producer
 */

#include "xr_program_from_xi.h"
#include "xr_program_xi_projection_gen.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include "../runtime/value/xtype.h"
#include "xr_program_verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrXiBlockArgumentStorage {
    const XiValue *source;
    const XiPhi *phi;
    XrCoreIrKey key;
    uint16_t type_id;
} XrXiBlockArgumentStorage;

typedef struct XrXiBlockStorage {
    const XiBlock *xi;
    XrXiBlockArgumentStorage *argument_storage;
    uint32_t argument_count;
    uint32_t argument_capacity;
    XrCoreIrValueInput *arguments;
    XrCoreIrInstructionInput *instructions;
} XrXiBlockStorage;

typedef struct XrXiFunctionStorage {
    const XiFunc *xi;
    XrCoreIrKey key;
    uint16_t *parameter_types;
    XrCoreIrBlockInput *blocks;
    XrXiBlockStorage *block_storage;
    uint32_t local_effect_mask;
} XrXiFunctionStorage;

typedef struct XrXiModuleStorage {
    const XiFunc *root;
    const XrProgramSemanticModuleInput *source_authority;
    XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    XrCoreIrFunctionInput *functions;
    XrXiFunctionStorage *function_storage;
} XrXiModuleStorage;

typedef struct XrXiBuildContext {
    const XrProgramFromXiInput *source;
    XrCoreIrModuleInput *modules;
    XrXiModuleStorage *storage;
} XrXiBuildContext;

static XrProgramBuildStatus fail(char *diagnostic, size_t diagnostic_size,
                                 XrProgramBuildStatus status, const char *format, ...) {
    if (diagnostic && diagnostic_size != 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(diagnostic, diagnostic_size, format, arguments);
        va_end(arguments);
    }
    return status;
}

static void put_u32_be(uint8_t output[4], uint32_t value) {
    output[0] = (uint8_t) (value >> 24u);
    output[1] = (uint8_t) (value >> 16u);
    output[2] = (uint8_t) (value >> 8u);
    output[3] = (uint8_t) value;
}

static void put_u64_be(uint8_t output[8], uint64_t value) {
    for (uint32_t index = 0; index < 8u; ++index)
        output[index] = (uint8_t) (value >> (56u - index * 8u));
}

static XrCoreIrKey key_from_stable_id(uint8_t domain, XrStableId id) {
    uint8_t material[1u + XR_STABLE_ID_BYTES];
    material[0] = domain;
    memcpy(material + 1u, id.bytes, sizeof(id.bytes));
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey key_from_stable_id_and_u32(uint8_t domain, XrStableId id, uint32_t value) {
    uint8_t material[1u + XR_STABLE_ID_BYTES + 4u];
    material[0] = domain;
    memcpy(material + 1u, id.bytes, sizeof(id.bytes));
    put_u32_be(material + 1u + sizeof(id.bytes), value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey constant_key(XrStableId module_id, uint16_t type_id, int64_t value) {
    uint8_t material[1u + XR_STABLE_ID_BYTES + 2u + 8u];
    material[0] = UINT8_C(0x43);
    memcpy(material + 1u, module_id.bytes, sizeof(module_id.bytes));
    material[1u + sizeof(module_id.bytes)] = (uint8_t) (type_id >> 8u);
    material[2u + sizeof(module_id.bytes)] = (uint8_t) type_id;
    put_u64_be(material + 3u + sizeof(module_id.bytes), (uint64_t) value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey key_from_key_and_u32(uint8_t domain, XrCoreIrKey key, uint32_t value) {
    uint8_t material[1u + XR_CORE_IR_KEY_SIZE + 4u];
    material[0] = domain;
    memcpy(material + 1u, key.bytes, sizeof(key.bytes));
    put_u32_be(material + 1u + sizeof(key.bytes), value);
    return xr_core_ir_key(material, sizeof(material));
}

static XrCoreIrKey function_key(XrStableId module_id, uint32_t function_index) {
    return key_from_stable_id_and_u32(UINT8_C(0x46), module_id, function_index);
}

static XrCoreIrKey block_key(const XrXiFunctionStorage *function, const XiBlock *block) {
    return key_from_key_and_u32(UINT8_C(0x42), function->key, block->id);
}

static XrCoreIrKey value_key(const XrXiFunctionStorage *function, const XiValue *value) {
    return key_from_key_and_u32(UINT8_C(0x56), function->key, value->id);
}

static bool map_type(const XrType *type, uint16_t *type_id) {
    if (!type || !type_id || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_UNIT:
            *type_id = XR_CORE_TYPE_VOID;
            return true;
        case XR_KIND_BOOL:
            *type_id = XR_CORE_TYPE_BOOL;
            return true;
        case XR_KIND_INT:
            if (type->scalar_rep == XR_NATIVE_I64) {
                *type_id = XR_CORE_TYPE_I64;
                return true;
            }
            if (type->scalar_rep == XR_NATIVE_U32) {
                *type_id = XR_CORE_TYPE_U32;
                return true;
            }
            return false;
        default:
            return false;
    }
}

static const XiModule *function_module(const XiFunc *function) {
    while (function && function->parent_func)
        function = function->parent_func;
    return function ? function->module : NULL;
}

static const XrXiFunctionStorage *find_xi_function(const XrXiBuildContext *context,
                                                   const XiFunc *needle, uint32_t *module_index_out,
                                                   uint32_t *function_index_out) {
    if (!context || !needle)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *storage = &context->storage[module_index];
        const XiModule *module = storage->root ? storage->root->module : NULL;
        if (!module || !storage->function_storage)
            continue;
        for (uint32_t function_index = 0; function_index < module->nfuncs; ++function_index) {
            const XiFunc *function = module->functions[function_index];
            if (function == needle) {
                if (module_index_out)
                    *module_index_out = module_index;
                if (function_index_out)
                    *function_index_out = function_index;
                return &storage->function_storage[function_index];
            }
        }
    }
    return NULL;
}

static bool get_shared_is_only_call_callee(const XiFunc *function, const XiValue *shared) {
    bool found = false;
    for (uint32_t block_index = 0; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (block->control == shared)
            return false;
        for (uint32_t value_index = 0; value_index < block->nvalues; ++value_index) {
            const XiValue *consumer = block->values[value_index];
            for (uint16_t argument = 0; argument < consumer->nargs; ++argument) {
                if (consumer->args[argument] != shared)
                    continue;
                if (consumer->op != XI_CALL || argument != 0u)
                    return false;
                found = true;
            }
        }
    }
    return found;
}

static const XiFunc *resolved_direct_callee(const XiFunc *caller, const XiValue *call) {
    if (!caller || !call || call->op != XI_CALL || call->nargs == 0u)
        return NULL;
    const XiValue *callee_value = xi_value_trace_identity(call->args[0]);
    const XiModule *owner = function_module(caller);
    if (!callee_value || callee_value->op != XI_GET_SHARED || !owner || !owner->slot_funcs ||
        callee_value->aux_int < 0 || (uint64_t) callee_value->aux_int >= owner->nslots ||
        !get_shared_is_only_call_callee(caller, callee_value))
        return NULL;
    return owner->slot_funcs[callee_value->aux_int];
}

static XrCoreIrKey imported_value_key(const XrXiFunctionStorage *function, const XiBlock *block,
                                      const XiValue *value) {
    uint8_t material[1u + XR_CORE_IR_KEY_SIZE + 8u];
    material[0] = UINT8_C(0x41);
    memcpy(material + 1u, function->key.bytes, sizeof(function->key.bytes));
    put_u32_be(material + 1u + sizeof(function->key.bytes), block->id);
    put_u32_be(material + 5u + sizeof(function->key.bytes), value->id);
    return xr_core_ir_key(material, sizeof(material));
}

static XrXiBlockStorage *find_block_storage(const XrXiFunctionStorage *function,
                                            const XiBlock *block) {
    if (!function || !block)
        return NULL;
    for (uint32_t index = 0; index < function->xi->nblocks; ++index) {
        if (function->block_storage[index].xi == block)
            return &function->block_storage[index];
    }
    return NULL;
}

static XrXiBlockArgumentStorage *find_block_argument(XrXiBlockStorage *block,
                                                     const XiValue *source) {
    for (uint32_t index = 0; block && index < block->argument_count; ++index) {
        if (block->argument_storage[index].source == source)
            return &block->argument_storage[index];
    }
    return NULL;
}

static XrProgramBuildStatus add_block_argument(XrXiFunctionStorage *function,
                                               XrXiBlockStorage *block, const XiValue *source,
                                               const XiPhi *phi, bool *changed, char *diagnostic,
                                               size_t diagnostic_size) {
    source = xi_value_trace_identity(source);
    if (!source || !source->type)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi block argument source is incomplete");
    if (find_block_argument(block, source))
        return XR_PROGRAM_BUILD_OK;
    if (block->argument_count == block->argument_capacity) {
        uint32_t capacity = block->argument_capacity ? block->argument_capacity * 2u : 4u;
        size_t allocation_size = (size_t) capacity * sizeof(*block->argument_storage);
        if (capacity < block->argument_count ||
            allocation_size / sizeof(*block->argument_storage) != capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrXiBlockArgumentStorage *arguments = xr_realloc(block->argument_storage, allocation_size);
        if (!arguments)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block->argument_storage = arguments;
        block->argument_capacity = capacity;
    }
    uint16_t type_id = XR_CORE_TYPE_VOID;
    if (!map_type(source->type, &type_id) || type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi live-in v%u has no active CoreSpec value type", source->id);
    XrXiBlockArgumentStorage *argument = &block->argument_storage[block->argument_count++];
    *argument = (XrXiBlockArgumentStorage) {
        .source = source,
        .phi = phi,
        .key = phi || (source->op == XI_PARAM && block->xi == function->xi->entry)
                   ? value_key(function, source)
                   : imported_value_key(function, block->xi, source),
        .type_id = type_id,
    };
    if (changed)
        *changed = true;
    return XR_PROGRAM_BUILD_OK;
}

static bool value_operand_key(const XrXiFunctionStorage *function, const XrXiBlockStorage *block,
                              const XiValue *value, XrCoreIrKey *key_out) {
    value = xi_value_trace_identity(value);
    if (!value || !key_out)
        return false;
    if (value->block != block->xi) {
        XrXiBlockArgumentStorage *argument = find_block_argument((XrXiBlockStorage *) block, value);
        if (!argument)
            return false;
        *key_out = argument->key;
        return true;
    }
    if (!xr_program_xi_value_is_materialized(value->op))
        return false;
    *key_out = value_key(function, value);
    return true;
}

static XrProgramBuildStatus add_constant(XrXiModuleStorage *module, const XiValue *value,
                                         XrCoreIrKey *constant_key_out, char *diagnostic,
                                         size_t diagnostic_size) {
    uint16_t type_id = XR_CORE_TYPE_VOID;
    if (!map_type(value->type, &type_id) ||
        (type_id != XR_CORE_TYPE_I64 && type_id != XR_CORE_TYPE_BOOL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi constant v%u has no active CoreSpec type", value->id);
    XrCoreIrKey key =
        constant_key(module->source_authority->module_identity, type_id, value->aux_int);
    for (uint32_t index = 0; index < module->constant_count; ++index) {
        if (xr_core_ir_key_equal(module->constants[index].key, key)) {
            *constant_key_out = key;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    if (module->constant_count == module->constant_capacity) {
        uint32_t capacity = module->constant_capacity ? module->constant_capacity * 2u : 8u;
        size_t allocation_size = (size_t) capacity * sizeof(XrCoreIrConstantInput);
        if (capacity < module->constant_count ||
            allocation_size / sizeof(XrCoreIrConstantInput) != capacity)
            return XR_PROGRAM_BUILD_RESOURCE_LIMIT;
        XrCoreIrConstantInput *constants = xr_realloc(module->constants, allocation_size);
        if (!constants)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        module->constants = constants;
        module->constant_capacity = capacity;
    }
    XrCoreIrConstantInput *constant = &module->constants[module->constant_count++];
    memset(constant, 0, sizeof(*constant));
    constant->key = key;
    constant->type_id = type_id;
    if (type_id == XR_CORE_TYPE_I64) {
        constant->kind = XR_CORE_IR_CONSTANT_I64;
        constant->value.i64 = value->aux_int;
    } else {
        constant->kind = XR_CORE_IR_CONSTANT_BOOL;
        constant->value.boolean = value->aux_int != 0;
    }
    *constant_key_out = key;
    return XR_PROGRAM_BUILD_OK;
}

static void free_instruction_input(XrCoreIrInstructionInput *instruction) {
    xr_free((void *) instruction->operands);
    xr_free((void *) instruction->successors);
}

static void free_context(XrXiBuildContext *context) {
    if (!context)
        return;
    for (uint32_t module_index = 0;
         context->storage && module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        uint32_t function_count =
            module->root && module->root->module ? module->root->module->nfuncs : 0u;
        for (uint32_t function_index = 0;
             module->function_storage && function_index < function_count; ++function_index) {
            XrXiFunctionStorage *function = &module->function_storage[function_index];
            for (uint32_t block_index = 0;
                 function->block_storage && function->xi && block_index < function->xi->nblocks;
                 ++block_index) {
                XrXiBlockStorage *block = &function->block_storage[block_index];
                uint32_t instruction_count =
                    function->blocks ? function->blocks[block_index].instruction_count : 0u;
                for (uint32_t instruction = 0;
                     block->instructions && instruction < instruction_count; ++instruction)
                    free_instruction_input(&block->instructions[instruction]);
                xr_free(block->instructions);
                xr_free(block->arguments);
                xr_free(block->argument_storage);
            }
            xr_free(function->block_storage);
            xr_free(function->blocks);
            xr_free(function->parameter_types);
        }
        xr_free(module->function_storage);
        xr_free(module->functions);
        xr_free(module->constants);
    }
    xr_free(context->storage);
    xr_free(context->modules);
}

static XrProgramBuildStatus set_operands(XrCoreIrInstructionInput *instruction,
                                         const XrXiFunctionStorage *function,
                                         const XrXiBlockStorage *block, XiValue *const *values,
                                         uint32_t value_count, char *diagnostic,
                                         size_t diagnostic_size) {
    if (value_count == 0)
        return XR_PROGRAM_BUILD_OK;
    XrCoreIrKey *operands = xr_calloc(value_count, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t index = 0; index < value_count; ++index) {
        if (!value_operand_key(function, block, values[index], &operands[index])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi operand v%u cannot be represented by active CoreSpec",
                        values[index] ? values[index]->id : 0u);
        }
    }
    instruction->operands = operands;
    instruction->operand_count = value_count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
translate_call(const XrXiBuildContext *context, const XrXiModuleStorage *module,
               XrXiFunctionStorage *function, const XiValue *value, const XrXiBlockStorage *block,
               const XrProgramXiProjection *projection, XrCoreIrInstructionInput *instruction,
               char *diagnostic, size_t diagnostic_size) {
    (void) module;
    const XiFunc *callee = resolved_direct_callee(function->xi, value);
    const XrXiFunctionStorage *callee_storage = find_xi_function(context, callee, NULL, NULL);
    if (!callee || !callee_storage)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u is not an exact resolved sealed direct call", value->id);

    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!map_type(value->type, &result_type))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u result type is not active in CoreSpec", value->id);
    instruction->operation_id = projection->core_operation_id;
    instruction->result_type_id = result_type;
    if (result_type != XR_CORE_TYPE_VOID)
        instruction->result = value_key(function, value);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = callee_storage->key;
    function->local_effect_mask |= UINT32_C(1) << 2u;
    return set_operands(instruction, function, block, value->args + 1u, value->nargs - 1u,
                        diagnostic, diagnostic_size);
}

static XrProgramBuildStatus
translate_value(const XrXiBuildContext *context, XrXiModuleStorage *module,
                XrXiFunctionStorage *function, const XiValue *value, const XrXiBlockStorage *block,
                XrCoreIrInstructionInput *instruction, char *diagnostic, size_t diagnostic_size) {
    memset(instruction, 0, sizeof(*instruction));
    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!map_type(value->type, &result_type))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi value v%u result type is not active in CoreSpec", value->id);
    XrProgramXiProjection projection;
    if (!xr_program_xi_projection(value->op, result_type, &projection))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi operation %u at v%u has no active CoreSpec projection", value->op,
                    value->id);

    switch (projection.kind) {
        case XR_PROGRAM_XI_PROJECTION_CONSTANT: {
            XrCoreIrKey constant;
            XrProgramBuildStatus status =
                add_constant(module, value, &constant, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
            instruction->immediate.key = constant;
            return XR_PROGRAM_BUILD_OK;
        }
        case XR_PROGRAM_XI_PROJECTION_BINARY_ARITHMETIC:
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_I64)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi arithmetic v%u is not exact i64 binary arithmetic", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_I64;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            instruction->immediate.u32 = projection.immediate_u32;
            function->local_effect_mask |= UINT32_C(1);
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        case XR_PROGRAM_XI_PROJECTION_COMPARE: {
            uint16_t left_type = XR_CORE_TYPE_VOID;
            uint16_t right_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(value->args[0]->type, &left_type) ||
                !map_type(value->args[1]->type, &right_type) || left_type != XR_CORE_TYPE_I64 ||
                right_type != XR_CORE_TYPE_I64)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi comparison v%u is not an exact i64 comparison", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            instruction->immediate.u32 = projection.immediate_u32;
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_SEALED_DIRECT_CALL:
            return translate_call(context, module, function, value, block, &projection, instruction,
                                  diagnostic, diagnostic_size);
        default:
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi operation %u at v%u has an invalid CoreSpec projection", value->op,
                        value->id);
    }
}

static bool value_is_skipped(const XiFunc *function, const XiValue *value) {
    if (value->op == XI_PARAM || xi_copy_is_identity_alias(value))
        return true;
    return value->op == XI_GET_SHARED && get_shared_is_only_call_callee(function, value);
}

static XrProgramBuildStatus require_value_available(XrXiFunctionStorage *function,
                                                    XrXiBlockStorage *block, const XiValue *value,
                                                    bool *changed, char *diagnostic,
                                                    size_t diagnostic_size) {
    value = xi_value_trace_identity(value);
    if (!value || !value->block)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi operand has no defining block");
    if (value->block == block->xi)
        return XR_PROGRAM_BUILD_OK;
    if (block->xi == function->xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block depends on non-parameter v%u", value->id);
    return add_block_argument(function, block, value, NULL, changed, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus collect_value_live_ins(XrXiFunctionStorage *function,
                                                   XrXiBlockStorage *block, const XiValue *value,
                                                   bool *changed, char *diagnostic,
                                                   size_t diagnostic_size) {
    uint16_t begin = value->op == XI_CALL ? 1u : 0u;
    for (uint16_t argument = begin; argument < value->nargs; ++argument) {
        XrProgramBuildStatus status = require_value_available(
            function, block, value->args[argument], changed, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static int block_argument_compare(const void *left, const void *right) {
    const XrXiBlockArgumentStorage *a = left;
    const XrXiBlockArgumentStorage *b = right;
    return memcmp(a->key.bytes, b->key.bytes, sizeof(a->key.bytes));
}

static const XiValue *edge_argument_value(const XrXiBlockArgumentStorage *argument,
                                          const XiBlock *predecessor, const XiBlock *successor) {
    if (!argument->phi)
        return argument->source;
    for (uint16_t index = 0; index < successor->npreds; ++index) {
        if (successor->preds[index] == predecessor && index < argument->phi->value.nargs)
            return argument->phi->value.args[index];
    }
    return NULL;
}

static XrProgramBuildStatus close_block_arguments(XrXiFunctionStorage *function, char *diagnostic,
                                                  size_t diagnostic_size) {
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        block->xi = function->xi->blocks[block_index];
    }
    XrXiBlockStorage *entry = find_block_storage(function, function->xi->entry);
    if (!entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function entry block is absent");
    for (uint16_t parameter = 0; parameter < function->xi->nparams; ++parameter) {
        XrProgramBuildStatus status =
            add_block_argument(function, entry, function->xi->params[parameter], NULL, NULL,
                               diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        for (const XiPhi *phi = block->xi->phis; phi; phi = phi->next) {
            XrProgramBuildStatus status = add_block_argument(function, block, &phi->value, phi,
                                                             NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        for (uint32_t value_index = 0; value_index < block->xi->nvalues; ++value_index) {
            const XiValue *value = block->xi->values[value_index];
            if (value_is_skipped(function->xi, value))
                continue;
            XrProgramBuildStatus status =
                collect_value_live_ins(function, block, value, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        if (block->xi->control) {
            XrProgramBuildStatus status = require_value_available(
                function, block, block->xi->control, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
    }

    uint64_t limit = (uint64_t) function->xi->nblocks *
                     ((uint64_t) function->xi->next_value_id + function->xi->nparams + 1u);
    for (uint64_t iteration = 0; iteration <= limit; ++iteration) {
        bool changed = false;
        for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
            XrXiBlockStorage *successor = &function->block_storage[block_index];
            uint32_t argument_count = successor->argument_count;
            for (uint16_t predecessor_index = 0; predecessor_index < successor->xi->npreds;
                 ++predecessor_index) {
                const XiBlock *predecessor_xi = successor->xi->preds[predecessor_index];
                XrXiBlockStorage *predecessor = find_block_storage(function, predecessor_xi);
                if (!predecessor)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi CFG predecessor is absent");
                for (uint32_t argument_index = 0; argument_index < argument_count;
                     ++argument_index) {
                    XrXiBlockArgumentStorage argument = successor->argument_storage[argument_index];
                    const XiValue *incoming =
                        edge_argument_value(&argument, predecessor_xi, successor->xi);
                    XrProgramBuildStatus status = require_value_available(
                        function, predecessor, incoming, &changed, diagnostic, diagnostic_size);
                    if (status != XR_PROGRAM_BUILD_OK)
                        return status;
                }
            }
        }
        if (!changed)
            break;
        if (iteration == limit)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block-parameter closure did not converge");
    }
    if (entry->argument_count != function->xi->nparams)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block acquired non-parameter live-ins");

    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        if (block->xi != function->xi->entry && block->argument_count > 1u)
            qsort(block->argument_storage, block->argument_count, sizeof(*block->argument_storage),
                  block_argument_compare);
        if (block->argument_count == 0u)
            continue;
        block->arguments = xr_calloc(block->argument_count, sizeof(*block->arguments));
        if (!block->arguments)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        for (uint32_t argument = 0; argument < block->argument_count; ++argument) {
            block->arguments[argument].key = block->argument_storage[argument].key;
            block->arguments[argument].type_id = block->argument_storage[argument].type_id;
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus
set_edge_operands(XrCoreIrInstructionInput *instruction, const XrXiFunctionStorage *function,
                  const XrXiBlockStorage *predecessor, const XrXiBlockStorage *first,
                  const XrXiBlockStorage *second, const XiValue *control, char *diagnostic,
                  size_t diagnostic_size) {
    uint32_t count =
        (control ? 1u : 0u) + first->argument_count + (second ? second->argument_count : 0u);
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0;
    if (control) {
        if (!value_operand_key(function, predecessor, control, &operands[cursor++])) {
            xr_free(operands);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi branch condition is unavailable");
        }
    }
    const XrXiBlockStorage *successors[2] = {first, second};
    for (uint32_t successor_index = 0; successor_index < (second ? 2u : 1u); ++successor_index) {
        const XrXiBlockStorage *successor = successors[successor_index];
        for (uint32_t argument = 0; argument < successor->argument_count; ++argument) {
            const XiValue *incoming = edge_argument_value(&successor->argument_storage[argument],
                                                          predecessor->xi, successor->xi);
            if (!value_operand_key(function, predecessor, incoming, &operands[cursor++])) {
                xr_free(operands);
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi edge argument is unavailable in predecessor b%u",
                            predecessor->xi->id);
            }
        }
    }
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus build_function(const XrXiBuildContext *context,
                                           XrXiModuleStorage *module, uint32_t function_index,
                                           char *diagnostic, size_t diagnostic_size) {
    const XiFunc *xi = module->root->module->functions[function_index];
    XrCoreIrFunctionInput *output = &module->functions[function_index];
    XrXiFunctionStorage *storage = &module->function_storage[function_index];
    XrCoreIrKey key = storage->key;
    memset(output, 0, sizeof(*output));
    memset(storage, 0, sizeof(*storage));
    storage->xi = xi;
    storage->key = key;
    if (xi->stage != XI_STAGE_OPTIMIZED || xi->semantic_plan || xi->nblocks == 0u ||
        xi->nchildren != 0u || !xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %u is not a leaf Optimized program input", function_index);
    output->key = storage->key;
    output->parameter_count = xi->nparams;
    if (!map_type(xi->return_type, &output->result_type_id))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %u result type is not active in CoreSpec", function_index);
    if (xi->nparams != 0) {
        storage->parameter_types = xr_calloc(xi->nparams, sizeof(*storage->parameter_types));
        if (!storage->parameter_types)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        for (uint16_t parameter = 0; parameter < xi->nparams; ++parameter) {
            if (!xi->params || !xi->params[parameter] || xi->params[parameter]->op != XI_PARAM ||
                xi->params[parameter]->aux_int != parameter ||
                !map_type(xi->params[parameter]->type, &storage->parameter_types[parameter]) ||
                storage->parameter_types[parameter] == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %u parameter %u is not a CoreSpec value", function_index,
                            parameter);
        }
        output->parameter_types = storage->parameter_types;
    }
    output->flags = xi == context->source->entry_function ? XR_PROGRAM_FUNCTION_ENTRY : 0u;

    storage->blocks = xr_calloc(xi->nblocks, sizeof(*storage->blocks));
    storage->block_storage = xr_calloc(xi->nblocks, sizeof(*storage->block_storage));
    if (!storage->blocks || !storage->block_storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    output->blocks = storage->blocks;
    output->block_count = xi->nblocks;
    output->entry_block = block_key(storage, xi->entry);
    XrProgramBuildStatus status = close_block_arguments(storage, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;

    for (uint32_t block_index = 0; block_index < xi->nblocks; ++block_index) {
        const XiBlock *xi_block = xi->blocks[block_index];
        XrCoreIrBlockInput *block_output = &storage->blocks[block_index];
        XrXiBlockStorage *block_storage = &storage->block_storage[block_index];
        block_output->key = block_key(storage, xi_block);
        block_output->arguments = block_storage->arguments;
        block_output->argument_count = block_storage->argument_count;

        uint32_t emitted = block_storage->argument_count != 0u ? 1u : 0u;
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            if (!value_is_skipped(xi, xi_block->values[value_index]))
                ++emitted;
        }
        ++emitted;
        block_storage->instructions = xr_calloc(emitted, sizeof(*block_storage->instructions));
        if (!block_storage->instructions)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        block_output->instructions = block_storage->instructions;
        block_output->instruction_count = emitted;

        uint32_t instruction_index = 0;
        if (block_storage->argument_count != 0u) {
            XrCoreIrInstructionInput *arguments = &block_storage->instructions[instruction_index++];
            arguments->operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT;
            arguments->result_type_id = XR_CORE_TYPE_VOID;
            arguments->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            XrCoreIrKey *operands = xr_calloc(block_storage->argument_count, sizeof(*operands));
            if (!operands)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            for (uint32_t argument = 0; argument < block_storage->argument_count; ++argument)
                operands[argument] = block_storage->arguments[argument].key;
            arguments->operands = operands;
            arguments->operand_count = block_storage->argument_count;
        }
        for (uint32_t value_index = 0; value_index < xi_block->nvalues; ++value_index) {
            const XiValue *value = xi_block->values[value_index];
            if (value_is_skipped(xi, value))
                continue;
            status = translate_value(context, module, storage, value, block_storage,
                                     &block_storage->instructions[instruction_index++], diagnostic,
                                     diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }

        XrCoreIrInstructionInput *terminator = &block_storage->instructions[instruction_index++];
        terminator->result_type_id = XR_CORE_TYPE_VOID;
        terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        if (xi_block->kind == XI_BLOCK_RETURN) {
            terminator->operation_id = XR_CORE_OP_CORE_RETURN;
            if (xi_block->control) {
                XiValue *returned[] = {xi_block->control};
                status = set_operands(terminator, storage, block_storage, returned, 1u, diagnostic,
                                      diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            } else if (output->result_type_id != XR_CORE_TYPE_VOID) {
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u has a value-less non-void return", function_index);
            }
        } else if (xi_block->kind == XI_BLOCK_PLAIN) {
            XrXiBlockStorage *successor = find_block_storage(storage, xi_block->succs[0]);
            if (!successor || xi_block->succs[1])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi plain block b%u has an invalid successor", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_BRANCH;
            XrCoreIrKey *successors = xr_calloc(1u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, successor->xi);
            terminator->successors = successors;
            terminator->successor_count = 1u;
            status = set_edge_operands(terminator, storage, block_storage, successor, NULL, NULL,
                                       diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_IF) {
            XrXiBlockStorage *true_block = find_block_storage(storage, xi_block->succs[0]);
            XrXiBlockStorage *false_block = find_block_storage(storage, xi_block->succs[1]);
            uint16_t condition_type = XR_CORE_TYPE_VOID;
            if (!true_block || !false_block || !xi_block->control ||
                !map_type(xi_block->control->type, &condition_type) ||
                condition_type != XR_CORE_TYPE_BOOL)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi conditional block b%u is incomplete", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH;
            XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, true_block->xi);
            successors[1] = block_key(storage, false_block->xi);
            terminator->successors = successors;
            terminator->successor_count = 2u;
            status = set_edge_operands(terminator, storage, block_storage, true_block, false_block,
                                       xi_block->control, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else {
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi unreachable block b%u requires an explicit CoreSpec terminal",
                        xi_block->id);
        }
        if (instruction_index != emitted)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block b%u instruction accounting is inconsistent", xi_block->id);
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus close_effects(XrXiBuildContext *context, char *diagnostic,
                                          size_t diagnostic_size) {
    uint32_t total_functions = 0;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *module = &context->storage[module_index];
        total_functions += module->root->module->nfuncs;
        for (uint32_t function = 0; function < module->root->module->nfuncs; ++function)
            module->functions[function].effect_mask =
                module->function_storage[function].local_effect_mask;
    }
    for (uint32_t iteration = 0; iteration < total_functions; ++iteration) {
        bool changed = false;
        for (uint32_t module_index = 0; module_index < context->source->module_count;
             ++module_index) {
            XrXiModuleStorage *module = &context->storage[module_index];
            for (uint32_t function_index = 0; function_index < module->root->module->nfuncs;
                 ++function_index) {
                XrXiFunctionStorage *function = &module->function_storage[function_index];
                uint32_t effects = module->functions[function_index].effect_mask;
                for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
                    const XiBlock *block = function->xi->blocks[block_index];
                    for (uint32_t value_index = 0; value_index < block->nvalues; ++value_index) {
                        const XiValue *value = block->values[value_index];
                        if (value->op != XI_CALL)
                            continue;
                        const XiFunc *callee = resolved_direct_callee(function->xi, value);
                        uint32_t callee_module = 0;
                        uint32_t callee_function = 0;
                        if (!callee ||
                            !find_xi_function(context, callee, &callee_module, &callee_function))
                            return fail(diagnostic, diagnostic_size,
                                        XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                        "Xi effect closure has an unresolved sealed call");
                        effects |=
                            context->storage[callee_module].functions[callee_function].effect_mask;
                    }
                }
                if (effects != module->functions[function_index].effect_mask) {
                    module->functions[function_index].effect_mask = effects;
                    changed = true;
                }
            }
        }
        if (!changed)
            return XR_PROGRAM_BUILD_OK;
    }
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi call-effect closure did not converge");
}

static uint32_t input_value_occurrences(const XrCoreIrFunctionInput *function, XrCoreIrKey key) {
    uint32_t occurrences = 0;
    for (uint32_t block = 0; block < function->block_count; ++block) {
        const XrCoreIrBlockInput *row = &function->blocks[block];
        for (uint32_t argument = 0; argument < row->argument_count; ++argument)
            occurrences += xr_core_ir_key_equal(row->arguments[argument].key, key);
        for (uint32_t instruction = 0; instruction < row->instruction_count; ++instruction) {
            XrCoreIrKey result = row->instructions[instruction].result;
            occurrences += !xr_core_ir_key_is_zero(result) && xr_core_ir_key_equal(result, key);
        }
    }
    return occurrences;
}

static XrProgramBuildStatus validate_input_value_identities(const XrXiBuildContext *context,
                                                            char *diagnostic,
                                                            size_t diagnostic_size) {
    for (uint32_t module = 0; module < context->source->module_count; ++module) {
        const XrCoreIrModuleInput *module_row = &context->modules[module];
        for (uint32_t function = 0; function < module_row->function_count; ++function) {
            const XrCoreIrFunctionInput *function_row = &module_row->functions[function];
            for (uint32_t block = 0; block < function_row->block_count; ++block) {
                const XrCoreIrBlockInput *block_row = &function_row->blocks[block];
                for (uint32_t argument = 0; argument < block_row->argument_count; ++argument) {
                    XrCoreIrKey key = block_row->arguments[argument].key;
                    uint32_t occurrences = input_value_occurrences(function_row, key);
                    if (occurrences != 1u)
                        return fail(diagnostic, diagnostic_size,
                                    XR_PROGRAM_BUILD_DUPLICATE_IDENTITY,
                                    "Xi function %u block %u argument %u has %u definitions",
                                    function, block, argument, occurrences);
                }
            }
        }
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus build_context(XrXiBuildContext *context, char *diagnostic,
                                          size_t diagnostic_size) {
    context->modules = xr_calloc(context->source->module_count, sizeof(*context->modules));
    context->storage = xr_calloc(context->source->module_count, sizeof(*context->storage));
    if (!context->modules || !context->storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;

    /* Publish every module/function identity before translating a body. This
     * makes forward and cross-module calls independent of input order. */
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        XrXiModuleStorage *storage = &context->storage[module_index];
        XrCoreIrModuleInput *output = &context->modules[module_index];
        storage->root = root;
        if (!root || root->stage != XI_STAGE_OPTIMIZED || root->semantic_plan || !root->module ||
            root->module->init != root || !root->module->source_semantic_module_present)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module %u is not verified canonical-program input", module_index);
        storage->source_authority = &root->module->source_semantic_module;
        if (root->module->nfuncs == 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module %u has no source functions", module_index);
        output->key = key_from_stable_id(UINT8_C(0x4d), storage->source_authority->module_identity);
        storage->functions = xr_calloc(root->module->nfuncs, sizeof(*storage->functions));
        storage->function_storage =
            xr_calloc(root->module->nfuncs, sizeof(*storage->function_storage));
        if (!storage->functions || !storage->function_storage)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        output->functions = storage->functions;
        output->function_count = root->module->nfuncs;
        for (uint32_t function = 0; function < root->module->nfuncs; ++function) {
            storage->function_storage[function].xi = root->module->functions[function];
            storage->function_storage[function].key =
                function_key(storage->source_authority->module_identity, function);
        }
    }

    if (!find_xi_function(context, context->source->entry_function, NULL, NULL))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "canonical-program entry is not a source function in the input graph");

    uint32_t entry_count = 0;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        XrXiModuleStorage *storage = &context->storage[module_index];
        XrCoreIrModuleInput *output = &context->modules[module_index];
        for (uint32_t function = 0; function < storage->root->module->nfuncs; ++function) {
            XrProgramBuildStatus status =
                build_function(context, storage, function, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            entry_count += (storage->functions[function].flags & XR_PROGRAM_FUNCTION_ENTRY) != 0u;
        }
        output->constants = storage->constants;
        output->constant_count = storage->constant_count;
    }
    if (entry_count != 1u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi input must identify exactly one program entry");
    XrProgramBuildStatus status = close_effects(context, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        context->modules[module_index].constants = context->storage[module_index].constants;
        context->modules[module_index].constant_count =
            context->storage[module_index].constant_count;
    }
    return validate_input_value_identities(context, diagnostic, diagnostic_size);
}

XrProgramBuildStatus xr_program_write_from_xi(const XrProgramFromXiInput *input,
                                              XrProgramArtifact *artifact_out, char *diagnostic,
                                              size_t diagnostic_size) {
    if (artifact_out)
        memset(artifact_out, 0, sizeof(*artifact_out));
    if (diagnostic && diagnostic_size != 0)
        diagnostic[0] = '\0';
    if (!input || !artifact_out || !input->module_roots || input->module_count == 0u ||
        !input->entry_function || !input->semantic_profile_fingerprint)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi program producer input is incomplete");
    XrXiBuildContext context = {.source = input};
    XrProgramBuildStatus status = build_context(&context, diagnostic, diagnostic_size);
    XrCoreIrProgram *program = NULL;
    if (status == XR_PROGRAM_BUILD_OK) {
        uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
        XrCoreIrProgramInput core_input = {
            .semantic_profile_fingerprint = input->semantic_profile_fingerprint,
            .required_features = &feature,
            .required_feature_count = 1u,
            .modules = context.modules,
            .module_count = input->module_count,
        };
        status = xr_core_ir_program_build(&core_input, &program, diagnostic, diagnostic_size);
    }
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact_out, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK) {
        XrValidatedProgram *validated = NULL;
        XrProgramDiagnostic verify_diagnostic;
        XrProgramVerifyStatus verify = xr_program_validate(artifact_out->bytes, artifact_out->size,
                                                           NULL, &validated, &verify_diagnostic);
        xr_validated_program_free(validated);
        if (verify != XR_PROGRAM_VERIFY_OK) {
            xr_program_artifact_free(artifact_out);
            status = fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                          "Xi-produced XrProgram failed semantic verification: %s/%s",
                          xr_program_verify_status_name(verify),
                          xr_program_diagnostic_kind_name(verify_diagnostic.kind));
        }
    }
    xr_core_ir_program_free(program);
    free_context(&context);
    return status;
}
