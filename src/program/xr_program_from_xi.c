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
#include "xr_program_internal.h"
#include "xr_program_xi_projection_gen.h"
#include "../ir/xi_op_name.h"

#include "../base/xmalloc.h"
#include "../analysis/xglobal_summary.h"
#include "../core/xr_core_spec_gen.h"
#include "../frontend/analyzer/xanalyzer.h"
#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xenum_layout.h"
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
    XrCoreIrValueCategory category;
    XrCoreIrOwnershipDisposition ownership;
    uint8_t implicit_invoke_kind;
} XrXiBlockArgumentStorage;

enum {
    XR_XI_INVOKE_ARGUMENT_NONE = 0,
    XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT = 1,
    XR_XI_INVOKE_ARGUMENT_ERROR = 2,
};

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
    XrParamMode *parameter_modes;
    XrCoreIrBlockInput *blocks;
    XrXiBlockStorage *block_storage;
    uint32_t local_effect_mask;
} XrXiFunctionStorage;

typedef struct XrXiModuleStorage {
    const XiFunc *root;
    const XrProgramSemanticModuleInput *source_authority;
    const XiFunc **xi_functions;
    uint32_t function_count;
    XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    XrCoreIrFunctionInput *functions;
    XrXiFunctionStorage *function_storage;
} XrXiModuleStorage;

typedef struct XrXiTypeStorage {
    XrCoreIrTypeInput input;
    uint16_t *field_types;
    XrCoreIrVariantInput *variants;
    uint16_t **variant_payload_types;
    XrCoreIrCallableSignatureInput *callable_signature;
    uint16_t *callable_parameter_types;
    XrParamMode *callable_parameter_modes;
} XrXiTypeStorage;

typedef struct XrXiBuildContext {
    const XrProgramFromXiInput *source;
    XrCoreIrModuleInput *modules;
    XrXiModuleStorage *storage;
    XrXiTypeStorage *type_storage;
    XrCoreIrTypeInput *types;
    uint32_t type_count;
    uint32_t type_capacity;
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

static bool map_builtin_type(const XrType *type, uint16_t *type_id) {
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
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            if (type->instance.class_name && strcmp(type->instance.class_name, "PanicInfo") == 0) {
                *type_id = XR_CORE_TYPE_PANIC_INFO;
                return true;
            }
            return false;
        default:
            return false;
    }
}

static const XrXiTypeStorage *find_dynamic_type_by_id(const XrXiBuildContext *context,
                                                      uint16_t type_id) {
    for (uint32_t index = 0; context && index < context->type_count; ++index) {
        if (context->type_storage[index].input.local_id == type_id)
            return &context->type_storage[index];
    }
    return NULL;
}

static bool type_stack_contains(const XrType *const *stack, uint32_t depth, const XrType *type) {
    for (uint32_t index = 0; index < depth; ++index) {
        if (stack[index] == type)
            return true;
    }
    return false;
}

static bool map_type_recursive(XrXiBuildContext *context, const XrType *type, uint16_t *type_id,
                               const XrType *const *stack, uint32_t depth);
static XrCoreIrOwnershipDisposition logical_ownership_for_type(const XrXiBuildContext *context,
                                                               uint16_t type_id);

static const XiClassData *find_aggregate_schema(const XrXiBuildContext *context, const XrType *type,
                                                const XiModule **owner_module) {
    const XiClassData *found = NULL;
    const XiModule *found_module = NULL;
    if (!context || !type || (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS) ||
        !type->instance.class_ref || !type->instance.class_ref->struct_layout)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        const XiModule *module = root ? root->module : NULL;
        for (uint16_t index = 0; module && index < module->nclasses; ++index) {
            const XiClassData *candidate = module->classes ? module->classes[index] : NULL;
            if (!candidate || candidate->class_info != type->instance.class_ref ||
                candidate->needs_runtime_type || candidate->is_generic_skeleton ||
                !candidate->struct_layout ||
                candidate->struct_layout->kind == XR_AGG_LAYOUT_UNION ||
                candidate->instance_field_count == 0u || !candidate->instance_field_names ||
                !candidate->instance_field_types ||
                candidate->instance_field_count != candidate->struct_layout->field_count)
                continue;
            if (found)
                return NULL;
            found = candidate;
            found_module = module;
        }
    }
    if (owner_module)
        *owner_module = found_module;
    return found;
}

static bool variant_schema_matches_type(const XiEnumData *schema, const XrType *type) {
    if (!schema || !type || type->kind != XR_KIND_ENUM || !type->enum_type.enum_name ||
        !schema->name || strcmp(schema->name, type->enum_type.enum_name) != 0)
        return false;
    uint32_t layout_id =
        type->enum_type.layout ? type->enum_type.layout->layout_id : type->enum_type.layout_id;
    if (layout_id == 0u || schema->layout_id != layout_id || !schema->is_adt || !schema->members ||
        schema->member_count == 0u)
        return false;
    for (uint32_t variant = 0; variant < schema->member_count; variant++) {
        const XiEnumMemberData *member = &schema->members[variant];
        if (!member->name || !member->name[0] || member->ordinal != variant ||
            member->payload_count < 0 || member->payload_count > UINT16_MAX ||
            !xr_enum_payload_names_are_exact(member->payload_names,
                                             (uint16_t) member->payload_count) ||
            (member->payload_count > 0 && !member->payload_types))
            return false;
        for (uint32_t prior = 0; prior < variant; prior++) {
            if (strcmp(member->name, schema->members[prior].name) == 0)
                return false;
        }
        for (int field = 0; field < member->payload_count; field++) {
            if (!member->payload_types[field])
                return false;
        }
    }
    return true;
}

static const XiEnumData *find_variant_schema(const XrXiBuildContext *context, const XrType *type) {
    const XiEnumData *found = NULL;
    for (uint32_t module_index = 0; context && module_index < context->source->module_count;
         ++module_index) {
        const XiFunc *root = context->source->module_roots[module_index];
        const XiModule *module = root ? root->module : NULL;
        for (uint16_t slot = 0; module && module->slot_enums && slot < module->nslots; ++slot) {
            const XiEnumData *candidate = module->slot_enums[slot];
            if (!variant_schema_matches_type(candidate, type))
                continue;
            if (found && found != candidate)
                return NULL;
            found = candidate;
        }
        for (uint16_t function = 0; module && function < module->nfuncs; ++function) {
            const XiFunc *owner = module->functions[function];
            for (uint32_t block = 0; owner && block < owner->nblocks; ++block) {
                const XiBlock *row = owner->blocks[block];
                for (uint32_t value = 0; row && value < row->nvalues; ++value) {
                    const XiValue *candidate_value = row->values[value];
                    const XiEnumData *candidate =
                        candidate_value && candidate_value->op == XI_CONST &&
                                candidate_value->aux_kind == XI_AUX_KIND_ENUM_NAMESPACE
                            ? (const XiEnumData *) candidate_value->aux
                            : NULL;
                    if (!variant_schema_matches_type(candidate, type))
                        continue;
                    if (found && found != candidate)
                        return NULL;
                    found = candidate;
                }
            }
        }
    }
    return found;
}

static const XrType *substitute_variant_payload_type(const XiEnumData *schema,
                                                     const XrType *enum_type,
                                                     const XrType *payload_type) {
    if (!schema || !enum_type || !payload_type || !schema->type_param_names ||
        schema->type_param_count != enum_type->enum_type.type_arg_count ||
        !enum_type->enum_type.type_args)
        return payload_type;
    XrType *substituted =
        xr_type_substitute(NULL, (XrType *) payload_type, schema->type_param_names,
                           enum_type->enum_type.type_args, schema->type_param_count);
    return substituted ? substituted : payload_type;
}

static void put_dynamic_type_reference(const XrXiBuildContext *context,
                                       uint8_t row[1u + XR_CORE_IR_KEY_SIZE], uint16_t type_id) {
    const XrXiTypeStorage *dynamic = find_dynamic_type_by_id(context, type_id);
    memset(row, 0, 1u + XR_CORE_IR_KEY_SIZE);
    if (dynamic) {
        row[0] = UINT8_C(1);
        memcpy(row + 1u, dynamic->input.key.bytes, XR_CORE_IR_KEY_SIZE);
    } else {
        row[1] = (uint8_t) (type_id >> 8u);
        row[2] = (uint8_t) type_id;
    }
}

static XrXiTypeStorage *append_type_storage(XrXiBuildContext *context) {
    if (!context || context->type_count >= UINT16_MAX - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u)
        return NULL;
    if (context->type_count == context->type_capacity) {
        uint32_t capacity = context->type_capacity ? context->type_capacity * 2u : 8u;
        if (capacity < context->type_count ||
            (size_t) capacity > SIZE_MAX / sizeof(*context->type_storage))
            return NULL;
        XrXiTypeStorage *grown =
            xr_realloc(context->type_storage, (size_t) capacity * sizeof(*context->type_storage));
        if (!grown)
            return NULL;
        context->type_storage = grown;
        context->type_capacity = capacity;
    }
    XrXiTypeStorage *storage = &context->type_storage[context->type_count];
    memset(storage, 0, sizeof(*storage));
    storage->input.local_id = (uint16_t) (XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + context->type_count);
    return storage;
}

static bool map_callable_type_recursive(XrXiBuildContext *context, const XrType *type,
                                        uint16_t *type_id, const XrType *const *stack,
                                        uint32_t depth) {
    if (!context || !type || !type_id || type->kind != XR_KIND_FUNCTION || type->is_nullable ||
        type->function.param_count < 0 || type->function.param_count > UINT16_MAX ||
        (type->function.param_count != 0 && !type->function.params) ||
        !type->function.return_type || type->function.is_variadic || type->function.is_c_abi ||
        type->function.type_param_count != 0 ||
        type->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
        type->function.view_origin_count != 0 || depth >= 64u ||
        type_stack_contains(stack, depth, type))
        return false;

    uint32_t parameter_count = (uint32_t) type->function.param_count;
    uint16_t *parameter_types =
        parameter_count ? xr_calloc(parameter_count, sizeof(*parameter_types)) : NULL;
    XrParamMode *parameter_modes =
        parameter_count ? xr_calloc(parameter_count, sizeof(*parameter_modes)) : NULL;
    if (parameter_count && (!parameter_types || !parameter_modes)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }

    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    bool valid = true;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        XrFunctionParam *source_parameter = &type->function.params[parameter];
        if (!source_parameter->type || !xr_param_mode_is_valid(source_parameter->mode) ||
            !map_type_recursive(context, source_parameter->type, &parameter_types[parameter],
                                nested_stack, depth + 1u) ||
            parameter_types[parameter] == XR_CORE_TYPE_VOID) {
            valid = false;
            break;
        }
        parameter_modes[parameter] = source_parameter->mode;
    }
    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!valid || !map_type_recursive(context, type->function.return_type, &result_type,
                                      nested_stack, depth + 1u)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }

    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    if (parameter_count > (SIZE_MAX - 6u - type_reference_size) / (type_reference_size + 1u)) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    size_t material_size =
        5u + (size_t) parameter_count * (type_reference_size + 1u) + type_reference_size + 1u;
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x4c);
    put_u32_be(material + cursor, parameter_count);
    cursor += 4u;
    for (uint32_t parameter = 0; parameter < parameter_count; ++parameter) {
        put_dynamic_type_reference(context, material + cursor, parameter_types[parameter]);
        cursor += type_reference_size;
        material[cursor++] = (uint8_t) parameter_modes[parameter];
    }
    put_dynamic_type_reference(context, material + cursor, result_type);
    cursor += type_reference_size;
    material[cursor++] = (uint8_t) logical_ownership_for_type(context, result_type);
    if (cursor != material_size) {
        xr_free(material);
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(parameter_types);
            xr_free(parameter_modes);
            return true;
        }
    }

    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    storage->callable_signature = xr_calloc(1u, sizeof(*storage->callable_signature));
    if (!storage->callable_signature) {
        xr_free(parameter_types);
        xr_free(parameter_modes);
        return false;
    }
    storage->callable_parameter_types = parameter_types;
    storage->callable_parameter_modes = parameter_modes;
    storage->callable_signature->parameter_types = parameter_types;
    storage->callable_signature->parameter_modes = parameter_modes;
    storage->callable_signature->parameter_count = parameter_count;
    storage->callable_signature->receiver_mode = XR_PARAM_READ;
    storage->callable_signature->result_type_id = result_type;
    storage->callable_signature->result_ownership =
        logical_ownership_for_type(context, result_type);
    storage->callable_signature->error_type_id = XR_CORE_TYPE_VOID;
    storage->callable_signature->panic_type_id = XR_CORE_TYPE_VOID;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_CALLABLE;
    storage->input.ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    storage->input.copy_contract = XR_CORE_IR_COPY_EXPLICIT;
    storage->input.callable_signature = storage->callable_signature;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_variant_type_recursive(XrXiBuildContext *context, const XrType *type,
                                       uint16_t *type_id, const XrType *const *stack,
                                       uint32_t depth) {
    const XiEnumData *schema = find_variant_schema(context, type);
    if (!schema || !type->enum_type.layout || depth >= 64u ||
        type_stack_contains(stack, depth, type))
        return false;

    XrCoreIrVariantInput *variants = xr_calloc(schema->member_count, sizeof(*variants));
    uint16_t **payload_storage = xr_calloc(schema->member_count, sizeof(*payload_storage));
    if (!variants || !payload_storage) {
        xr_free(variants);
        xr_free(payload_storage);
        return false;
    }
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    uint64_t payload_total = 0u;
    bool valid = true;
    for (uint32_t variant = 0; valid && variant < schema->member_count; ++variant) {
        const XiEnumMemberData *member = &schema->members[variant];
        if (!member->name || !member->name[0] || member->ordinal != variant ||
            member->payload_count < 0 || member->payload_count > UINT16_MAX ||
            !xr_enum_payload_names_are_exact(member->payload_names,
                                             (uint16_t) member->payload_count) ||
            (member->payload_count > 0 && !member->payload_types)) {
            valid = false;
            break;
        }
        variants[variant].payload_count = (uint32_t) member->payload_count;
        payload_total += variants[variant].payload_count;
        if (payload_total > UINT32_MAX) {
            valid = false;
            break;
        }
        if (member->payload_count == 0)
            continue;
        payload_storage[variant] =
            xr_calloc((size_t) member->payload_count, sizeof(*payload_storage[variant]));
        if (!payload_storage[variant]) {
            valid = false;
            break;
        }
        variants[variant].payload_types = payload_storage[variant];
        for (int field = 0; field < member->payload_count; ++field) {
            const XrType *payload_type =
                substitute_variant_payload_type(schema, type, member->payload_types[field]);
            if (!map_type_recursive(context, payload_type, &payload_storage[variant][field],
                                    nested_stack, depth + 1u)) {
                valid = false;
                break;
            }
        }
    }
    if (!valid)
        goto fail;

    const char *owner = type->enum_type.layout->nominal_owner;
    const char *name = type->enum_type.enum_name;
    size_t owner_length = owner ? strlen(owner) : 0u;
    size_t name_length = name ? strlen(name) : 0u;
    if (owner_length > UINT32_MAX || name_length > UINT32_MAX || owner_length > SIZE_MAX - 13u ||
        name_length > SIZE_MAX - 13u - owner_length)
        goto fail;
    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    size_t material_size = 13u + owner_length + name_length;
    for (uint32_t variant = 0; variant < schema->member_count; ++variant) {
        size_t variant_name_length =
            schema->members[variant].name ? strlen(schema->members[variant].name) : 0u;
        if (variant_name_length > UINT32_MAX)
            goto fail;
        size_t row_size = 8u + variant_name_length;
        for (uint32_t field = 0; field < variants[variant].payload_count; field++) {
            size_t field_name_length = strlen(schema->members[variant].payload_names[field]);
            if (field_name_length > UINT32_MAX ||
                field_name_length > SIZE_MAX - 4u - type_reference_size ||
                row_size > SIZE_MAX - 4u - type_reference_size - field_name_length)
                goto fail;
            row_size += 4u + field_name_length + type_reference_size;
        }
        if (row_size < variant_name_length || material_size > SIZE_MAX - row_size)
            goto fail;
        material_size += row_size;
    }
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material)
        goto fail;
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x57);
    put_u32_be(material + cursor, (uint32_t) owner_length);
    cursor += 4u;
    memcpy(material + cursor, owner, owner_length);
    cursor += owner_length;
    put_u32_be(material + cursor, (uint32_t) name_length);
    cursor += 4u;
    memcpy(material + cursor, name, name_length);
    cursor += name_length;
    put_u32_be(material + cursor, schema->member_count);
    cursor += 4u;
    for (uint32_t variant = 0; variant < schema->member_count; ++variant) {
        const char *variant_name = schema->members[variant].name;
        size_t variant_name_length = variant_name ? strlen(variant_name) : 0u;
        put_u32_be(material + cursor, (uint32_t) variant_name_length);
        cursor += 4u;
        memcpy(material + cursor, variant_name, variant_name_length);
        cursor += variant_name_length;
        put_u32_be(material + cursor, variants[variant].payload_count);
        cursor += 4u;
        for (uint32_t field = 0; field < variants[variant].payload_count; ++field) {
            const char *field_name = schema->members[variant].payload_names[field];
            size_t field_name_length = strlen(field_name);
            put_u32_be(material + cursor, (uint32_t) field_name_length);
            cursor += 4u;
            memcpy(material + cursor, field_name, field_name_length);
            cursor += field_name_length;
            put_dynamic_type_reference(context, material + cursor,
                                       variants[variant].payload_types[field]);
            cursor += type_reference_size;
        }
    }
    if (cursor != material_size) {
        xr_free(material);
        goto fail;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            goto reuse;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage)
        goto fail;
    storage->variants = variants;
    storage->variant_payload_types = payload_storage;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_VARIANT;
    storage->input.variants = variants;
    storage->input.variant_count = schema->member_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;

reuse:
    for (uint32_t variant = 0; variant < schema->member_count; ++variant)
        xr_free(payload_storage[variant]);
    xr_free(payload_storage);
    xr_free(variants);
    return true;

fail:
    for (uint32_t variant = 0; variant < schema->member_count; ++variant)
        xr_free(payload_storage[variant]);
    xr_free(payload_storage);
    xr_free(variants);
    return false;
}

static bool map_aggregate_type_recursive(XrXiBuildContext *context, const XrType *type,
                                         uint16_t *type_id, const XrType *const *stack,
                                         uint32_t depth) {
    const XiModule *owner_module = NULL;
    const XiClassData *schema = find_aggregate_schema(context, type, &owner_module);
    if (!schema || !owner_module || !owner_module->identity || !schema->class_name ||
        depth >= 64u || type_stack_contains(stack, depth, type))
        return false;

    uint32_t field_count = schema->instance_field_count;
    uint16_t *field_types = xr_calloc(field_count, sizeof(*field_types));
    if (!field_types)
        return false;
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    for (uint32_t field = 0; field < field_count; ++field) {
        const char *field_name = schema->instance_field_names[field];
        const XrType *field_type = schema->instance_field_types[field];
        if (!field_name || !field_name[0] || !field_type || !schema->struct_layout->field_names ||
            !schema->struct_layout->field_names[field] ||
            strcmp(field_name, schema->struct_layout->field_names[field]) != 0 ||
            !map_type_recursive(context, field_type, &field_types[field], nested_stack,
                                depth + 1u)) {
            xr_free(field_types);
            return false;
        }
    }

    size_t module_length = strlen(owner_module->identity);
    size_t name_length = strlen(schema->class_name);
    if (module_length > UINT32_MAX || name_length > UINT32_MAX || module_length > SIZE_MAX - 13u ||
        name_length > SIZE_MAX - 13u - module_length) {
        xr_free(field_types);
        return false;
    }
    const size_t type_reference_size = 1u + XR_CORE_IR_KEY_SIZE;
    size_t material_size = 13u + module_length + name_length;
    for (uint32_t field = 0; field < field_count; ++field) {
        size_t field_name_length = strlen(schema->instance_field_names[field]);
        if (field_name_length > UINT32_MAX ||
            field_name_length > SIZE_MAX - 4u - type_reference_size ||
            material_size > SIZE_MAX - 4u - type_reference_size - field_name_length) {
            xr_free(field_types);
            return false;
        }
        material_size += 4u + field_name_length + type_reference_size;
    }
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(field_types);
        return false;
    }
    size_t cursor = 0u;
    material[cursor++] = UINT8_C(0x53);
    put_u32_be(material + cursor, (uint32_t) module_length);
    cursor += 4u;
    memcpy(material + cursor, owner_module->identity, module_length);
    cursor += module_length;
    put_u32_be(material + cursor, (uint32_t) name_length);
    cursor += 4u;
    memcpy(material + cursor, schema->class_name, name_length);
    cursor += name_length;
    put_u32_be(material + cursor, field_count);
    cursor += 4u;
    for (uint32_t field = 0; field < field_count; ++field) {
        const char *field_name = schema->instance_field_names[field];
        size_t field_name_length = strlen(field_name);
        put_u32_be(material + cursor, (uint32_t) field_name_length);
        cursor += 4u;
        memcpy(material + cursor, field_name, field_name_length);
        cursor += field_name_length;
        put_dynamic_type_reference(context, material + cursor, field_types[field]);
        cursor += type_reference_size;
    }
    if (cursor != material_size) {
        xr_free(material);
        xr_free(field_types);
        return false;
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(field_types);
            return true;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(field_types);
        return false;
    }
    storage->field_types = field_types;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_AGGREGATE;
    storage->input.field_types = field_types;
    storage->input.field_count = field_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_type_recursive(XrXiBuildContext *context, const XrType *type, uint16_t *type_id,
                               const XrType *const *stack, uint32_t depth) {
    if (map_builtin_type(type, type_id))
        return true;
    if (context && type && type_id && type->kind == XR_KIND_FUNCTION)
        return map_callable_type_recursive(context, type, type_id, stack, depth);
    if (context && type && type_id && !type->is_nullable && type->kind == XR_KIND_ENUM)
        return map_variant_type_recursive(context, type, type_id, stack, depth);
    if (context && type && type_id && !type->is_nullable &&
        (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
        type->instance.class_ref && type->instance.class_ref->struct_layout)
        return map_aggregate_type_recursive(context, type, type_id, stack, depth);
    if (!context || !type || !type_id || type->is_nullable || type->kind != XR_KIND_TUPLE ||
        type->tuple.element_count <= 0 || !type->tuple.element_types || depth >= 64u ||
        type_stack_contains(stack, depth, type))
        return false;

    uint32_t field_count = (uint32_t) type->tuple.element_count;
    if (field_count > UINT16_MAX)
        return false;
    uint16_t *field_types = xr_calloc(field_count, sizeof(*field_types));
    if (!field_types)
        return false;
    const XrType *nested_stack[64];
    if (depth != 0u)
        memcpy(nested_stack, stack, depth * sizeof(*nested_stack));
    nested_stack[depth] = type;
    for (uint32_t field = 0; field < field_count; ++field) {
        if (!map_type_recursive(context, type->tuple.element_types[field], &field_types[field],
                                nested_stack, depth + 1u)) {
            xr_free(field_types);
            return false;
        }
    }

    const size_t row_size = 1u + XR_CORE_IR_KEY_SIZE;
    if (field_count > (SIZE_MAX - 5u) / row_size) {
        xr_free(field_types);
        return false;
    }
    size_t material_size = 5u + (size_t) field_count * row_size;
    uint8_t *material = xr_calloc(material_size, 1u);
    if (!material) {
        xr_free(field_types);
        return false;
    }
    material[0] = UINT8_C(0x54);
    put_u32_be(material + 1u, field_count);
    for (uint32_t field = 0; field < field_count; ++field) {
        uint8_t *row = material + 5u + (size_t) field * row_size;
        put_dynamic_type_reference(context, row, field_types[field]);
    }
    XrCoreIrKey semantic_key = xr_core_ir_key(material, material_size);
    xr_free(material);
    for (uint32_t index = 0; index < context->type_count; ++index) {
        if (xr_core_ir_key_equal(context->type_storage[index].input.key, semantic_key)) {
            *type_id = context->type_storage[index].input.local_id;
            xr_free(field_types);
            return true;
        }
    }
    XrXiTypeStorage *storage = append_type_storage(context);
    if (!storage) {
        xr_free(field_types);
        return false;
    }
    storage->field_types = field_types;
    storage->input.key = semantic_key;
    storage->input.kind = XR_CORE_IR_TYPE_AGGREGATE;
    storage->input.field_types = storage->field_types;
    storage->input.field_count = field_count;
    *type_id = storage->input.local_id;
    ++context->type_count;
    return true;
}

static bool map_type(XrXiBuildContext *context, const XrType *type, uint16_t *type_id) {
    return map_type_recursive(context, type, type_id, NULL, 0u);
}

static const XiFunc *resolved_direct_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call);

static XrProgramBuildStatus function_has_uncaught_panic(XrXiBuildContext *context,
                                                        const XiFunc *function,
                                                        const XiFunc **stack, uint32_t depth,
                                                        uint32_t capacity, bool *has_panic,
                                                        char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !stack || !has_panic || depth >= capacity)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi panic call graph is not finitely representable");
    for (uint32_t index = 0u; index < depth; ++index) {
        if (stack[index] == function) {
            *has_panic = false;
            return XR_PROGRAM_BUILD_OK;
        }
    }
    stack[depth] = function;
    bool found = false;
    for (uint32_t block_index = 0u; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0u; block && value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value)
                continue;
            if (value->op == XI_TRY || value->op == XI_CATCH || value->op == XI_END_TRY)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %s still uses implicit panic-handler state",
                            function->name ? function->name : "<anonymous>");
            if (value->op == XI_THROW) {
                uint16_t payload_type = XR_CORE_TYPE_VOID;
                if (value->nargs != 1u || !value->args || !value->args[0] ||
                    !map_type(context, value->args[0]->type, &payload_type) ||
                    payload_type != XR_CORE_TYPE_PANIC_INFO)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                                "Xi throw v%u does not publish typed PanicInfo", value->id);
                found = true;
            }
            if (value->op == XI_CALL) {
                const XiFunc *callee = resolved_direct_callee(context, function, value);
                if (!callee)
                    continue;
                bool callee_panic = false;
                XrProgramBuildStatus status =
                    function_has_uncaught_panic(context, callee, stack, depth + 1u, capacity,
                                                &callee_panic, diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
                found = found || callee_panic;
            }
        }
    }
    *has_panic = found;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus map_function_panic_type(XrXiBuildContext *context,
                                                    const XiFunc *function, uint16_t *panic_type_id,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !panic_type_id)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    uint32_t capacity = 1u;
    for (uint32_t module = 0u; module < context->source->module_count; ++module) {
        const XiFunc *root = context->source->module_roots[module];
        if (root && root->module)
            capacity += root->module->nfuncs;
    }
    const XiFunc **stack = xr_calloc(capacity, sizeof(*stack));
    if (!stack)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    bool has_panic = false;
    XrProgramBuildStatus status = function_has_uncaught_panic(
        context, function, stack, 0u, capacity, &has_panic, diagnostic, diagnostic_size);
    xr_free(stack);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
    *panic_type_id = has_panic ? XR_CORE_TYPE_PANIC_INFO : XR_CORE_TYPE_VOID;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus map_function_error_type(XrXiBuildContext *context,
                                                    const XiFunc *function, uint16_t *error_type_id,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!context || !function || !error_type_id)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    *error_type_id = XR_CORE_TYPE_VOID;
    if (function->error_effect_nothrow)
        return XR_PROGRAM_BUILD_OK;
    const XaAnalyzer *analyzer = function->analyzer;
    const XaEffectSummary *effect =
        analyzer && function->analyzer_effect_id != XA_EFFECT_NONE
            ? xa_effect_db_get(analyzer->effect_db, function->analyzer_effect_id)
            : NULL;
    /* Error continuation construction depends only on the error-channel
     * component of the effect product.  Uncertainty in allocation,
     * suspension, or task-spawn semantics must not poison a complete typed
     * escaping-error set. */
    if (!effect || effect->error_set_completeness != XA_EFFECT_COMPLETE ||
        effect->error_unknown_reasons != XA_UNKNOWN_NONE || effect->escaping.count != 1u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s does not have one complete typed escaping-error set",
                    function->name ? function->name : "<anonymous>");
    const XaErrorTypeSet *error = &effect->escaping.types[0];
    XrType *type = xa_effect_db_error_type_handle(analyzer->effect_db, error->type_id);
    if (!type || !map_type(context, type, error_type_id) || *error_type_id == XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s escaping error type is not active in CoreSpec",
                    function->name ? function->name : "<anonymous>");
    const XrXiTypeStorage *mapped = find_dynamic_type_by_id(context, *error_type_id);
    if (!mapped || mapped->input.kind != XR_CORE_IR_TYPE_VARIANT)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %s escaping error is not a closed record enum",
                    function->name ? function->name : "<anonymous>");
    return XR_PROGRAM_BUILD_OK;
}

static XrCoreIrOwnershipDisposition logical_ownership_for_type(const XrXiBuildContext *context,
                                                               uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_OWNER;
    const XrXiTypeStorage *type = find_dynamic_type_by_id(context, type_id);
    return type && type->input.ownership == XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ? XR_CORE_IR_OWNER
                                                                             : XR_CORE_IR_NON_OWNER;
}

static XrCoreIrCopyContract logical_copy_contract_for_type(const XrXiBuildContext *context,
                                                           uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_VOID || type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_COPY_FORBIDDEN;
    const XrXiTypeStorage *type = find_dynamic_type_by_id(context, type_id);
    return type ? type->input.copy_contract : XR_CORE_IR_COPY_TRIVIAL;
}

static const XrXiFunctionStorage *find_xi_function(const XrXiBuildContext *context,
                                                   const XiFunc *needle, uint32_t *module_index_out,
                                                   uint32_t *function_index_out) {
    if (!context || !needle)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *storage = &context->storage[module_index];
        if (!storage->root || !storage->function_storage || !storage->xi_functions)
            continue;
        for (uint32_t function_index = 0; function_index < storage->function_count;
             ++function_index) {
            const XiFunc *function = storage->xi_functions[function_index];
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

static bool value_is_only_elided_operand(const XiFunc *function, const XiValue *value) {
    bool found = false;
    for (uint32_t block_index = 0; function && block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (block->control == value)
            return false;
        for (uint32_t value_index = 0; block && value_index < block->nvalues; ++value_index) {
            const XiValue *consumer = block->values[value_index];
            for (uint16_t argument = 0; consumer && argument < consumer->nargs; ++argument) {
                if (consumer->args[argument] != value)
                    continue;
                bool elided = argument == 0u &&
                              (consumer->op == XI_CALL || consumer->op == XI_VARIANT_CONSTRUCT);
                if (!elided)
                    return false;
                found = true;
            }
        }
    }
    return found;
}

static const XgCallsiteSummary *resolved_callsite(const XrXiBuildContext *context,
                                                  const XiFunc *caller, const XiValue *call) {
    if (!context || !context->source || !context->source->global_evidence || !caller || !call ||
        call->xg_callsite_id == XG_NO_ID || caller->xg_body_func_id == XG_NO_ID)
        return NULL;
    const XgCallsiteSummary *row = xg_global_evidence_find_callsite(
        context->source->global_evidence, (XgCallsiteId) call->xg_callsite_id);
    if (!row || row->owner_func_id != (XgFuncId) caller->xg_body_func_id)
        return NULL;
    return row;
}

static const XiFunc *find_xi_function_by_xg_id(const XrXiBuildContext *context,
                                               XgFuncId function_id) {
    if (!context || function_id == XG_NO_ID)
        return NULL;
    for (uint32_t module_index = 0; module_index < context->source->module_count; ++module_index) {
        const XrXiModuleStorage *module = &context->storage[module_index];
        for (uint32_t function_index = 0;
             module->xi_functions && function_index < module->function_count; ++function_index) {
            const XiFunc *function = module->xi_functions[function_index];
            if (function && function->xg_body_func_id == function_id)
                return function;
        }
    }
    return NULL;
}

static const XiFunc *resolved_direct_callee(const XrXiBuildContext *context, const XiFunc *caller,
                                            const XiValue *call) {
    if (!call || call->op != XI_CALL || call->nargs == 0u)
        return NULL;
    const XgCallsiteSummary *row = resolved_callsite(context, caller, call);
    if (!row || row->kind != XG_CALL_DIRECT_FUNC || row->static_target_func_id == XG_NO_ID)
        return NULL;
    return find_xi_function_by_xg_id(context, row->static_target_func_id);
}

static const XiFunc *resolved_callable_target(const XiFunc *caller, const XiValue *value) {
    if (!caller || !value)
        return NULL;
    if (value->op == XI_CLOSURE_NEW && value->aux)
        return (const XiFunc *) value->aux;
    return NULL;
}

static const XiValue *block_sealed_invoke_call(const XrXiBuildContext *context,
                                               const XiFunc *function, const XiBlock *block) {
    if (!function || !block || block->kind != XI_BLOCK_IF || !block->control ||
        block->control->op != XI_ERR_CHECK || !block->succs[0] || !block->succs[1])
        return NULL;
    const XiValue *previous = NULL;
    bool reached_check = false;
    for (uint32_t index = 0; index < block->nvalues; ++index) {
        const XiValue *value = block->values[index];
        if (value == block->control) {
            reached_check = true;
            break;
        }
        previous = value;
    }
    return reached_check && previous && previous->op == XI_CALL &&
                   resolved_direct_callee(context, function, previous)
               ? previous
               : NULL;
}

static const XiValue *block_error_catch(const XiBlock *block) {
    const XiValue *found = NULL;
    for (uint32_t index = 0; block && index < block->nvalues; ++index) {
        const XiValue *value = block->values[index];
        if (!value || value->op != XI_ERR_CATCH)
            continue;
        if (found)
            return NULL;
        found = value;
    }
    return found;
}

static bool value_is_invoke_scaffold(const XrXiBuildContext *context, const XiFunc *function,
                                     const XiValue *value) {
    if (!value)
        return false;
    if (value->op == XI_ERR_RETURN)
        return true;
    if (!function || !value->block)
        return false;
    const XiValue *call = block_sealed_invoke_call(context, function, value->block);
    if (value == call || value == value->block->control)
        return call != NULL;
    if (value->op == XI_ERR_CATCH)
        return block_error_catch(value->block) == value;
    return false;
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

static bool xi_type_has_logical_value_identity(const XrType *type) {
    return type && (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_ENUM ||
                    ((type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS) &&
                     type->instance.class_ref && type->instance.class_ref->struct_layout));
}

static const XiValue *logical_value_identity(const XiValue *value) {
    while (xi_copy_is_identity_alias(value) && value->nargs == 1u && value->args && value->args[0])
        value = value->args[0];
    while (
        value && value->nargs == 1u && value->args && value->args[0] && value->type &&
        ((value->op == XI_RETAIN && xi_type_has_logical_value_identity(value->type)) ||
         (xi_copy_is_value_clone(value) &&
          (value->type->kind == XR_KIND_TUPLE ||
           ((value->type->kind == XR_KIND_INSTANCE || value->type->kind == XR_KIND_CLASS) &&
            value->type->instance.class_ref && value->type->instance.class_ref->struct_layout))))) {
        value = value->args[0];
        while (xi_copy_is_identity_alias(value) && value->nargs == 1u && value->args &&
               value->args[0])
            value = value->args[0];
    }
    return value;
}

static XrCoreIrValueCategory logical_value_category(const XiValue *value) {
    value = logical_value_identity(value);
    return value && ((value->op == XI_PARAM && value->param_mode == XR_PARAM_REF) ||
                     value->op == XI_LOCAL_ADDR)
               ? XR_CORE_IR_PLACE
               : XR_CORE_IR_VALUE;
}

static XrProgramBuildStatus add_block_argument(XrXiBuildContext *context,
                                               XrXiFunctionStorage *function,
                                               XrXiBlockStorage *block, const XiValue *source,
                                               const XiPhi *phi, uint16_t type_override,
                                               uint8_t implicit_invoke_kind, bool *changed,
                                               char *diagnostic, size_t diagnostic_size) {
    source = logical_value_identity(source);
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
    uint16_t type_id = type_override;
    if ((type_id == XR_CORE_TYPE_VOID && !map_type(context, source->type, &type_id)) ||
        type_id == XR_CORE_TYPE_VOID)
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
        .category = logical_value_category(source),
        .ownership = implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE
                         ? logical_ownership_for_type(context, type_id)
                     : source->op == XI_PARAM && source->param_mode == XR_PARAM_MOVE
                         ? logical_ownership_for_type(context, type_id)
                         : XR_CORE_IR_NON_OWNER,
        .implicit_invoke_kind = implicit_invoke_kind,
    };
    if (changed)
        *changed = true;
    return XR_PROGRAM_BUILD_OK;
}

static bool value_operand_key(const XrXiFunctionStorage *function, const XrXiBlockStorage *block,
                              const XiValue *value, XrCoreIrKey *key_out) {
    value = logical_value_identity(value);
    if (!value || !key_out)
        return false;
    XrXiBlockArgumentStorage *argument = find_block_argument((XrXiBlockStorage *) block, value);
    if (argument) {
        *key_out = argument->key;
        return true;
    }
    if (value->block != block->xi)
        return false;
    if (!xr_program_xi_value_is_materialized(value->op))
        return false;
    *key_out = value_key(function, value);
    return true;
}

static XrProgramBuildStatus add_constant(XrXiBuildContext *context, XrXiModuleStorage *module,
                                         const XiValue *value, XrCoreIrKey *constant_key_out,
                                         char *diagnostic, size_t diagnostic_size) {
    uint16_t type_id = XR_CORE_TYPE_VOID;
    if (!map_type(context, value->type, &type_id) ||
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
        uint32_t function_count = module->function_count;
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
            xr_free(function->parameter_modes);
        }
        xr_free(module->function_storage);
        xr_free(module->functions);
        xr_free(module->xi_functions);
        xr_free(module->constants);
    }
    xr_free(context->storage);
    xr_free(context->modules);
    for (uint32_t type = 0; type < context->type_count; ++type) {
        for (uint32_t variant = 0; variant < context->type_storage[type].input.variant_count;
             ++variant)
            xr_free(context->type_storage[type].variant_payload_types[variant]);
        xr_free(context->type_storage[type].variant_payload_types);
        xr_free(context->type_storage[type].variants);
        xr_free(context->type_storage[type].field_types);
        xr_free(context->type_storage[type].callable_parameter_types);
        xr_free(context->type_storage[type].callable_parameter_modes);
        xr_free(context->type_storage[type].callable_signature);
    }
    xr_free(context->type_storage);
    xr_free(context->types);
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
translate_call(XrXiBuildContext *context, const XrXiModuleStorage *module,
               XrXiFunctionStorage *function, const XiValue *value, const XrXiBlockStorage *block,
               const XrProgramXiProjection *projection, XrCoreIrInstructionInput *instruction,
               char *diagnostic, size_t diagnostic_size) {
    (void) module;
    const XgCallsiteSummary *callsite = resolved_callsite(context, function->xi, value);
    if (!callsite)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks matching global callsite evidence", value->id);
    if (callsite->kind != XG_CALL_DIRECT_FUNC && callsite->kind != XG_CALL_CLOSURE)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u uses unsupported global callsite kind %u", value->id,
                    callsite->kind);
    if ((callsite->flags & XG_CALL_ERROR_EFFECT_VERIFIED) == 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi call v%u lacks verified callsite error evidence", value->id);
    const XiFunc *callee = resolved_direct_callee(context, function->xi, value);
    const XrXiFunctionStorage *callee_storage = find_xi_function(context, callee, NULL, NULL);
    if (!callee) {
        if ((callsite->flags & XG_CALL_MAY_ERROR) != 0u)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi indirect call v%u requires an explicit error continuation", value->id);
        const XiValue *callee_value = value->nargs ? logical_value_identity(value->args[0]) : NULL;
        uint16_t callable_type_id = XR_CORE_TYPE_VOID;
        if (!callee_value || !map_type(context, callee_value->type, &callable_type_id))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u has no typed callable target", value->id);
        const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
        if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
            !callable->callable_signature ||
            callable->callable_signature->error_type_id != XR_CORE_TYPE_VOID ||
            callable->callable_signature->panic_type_id != XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u requires an unsupported callable continuation", value->id);
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, value->type, &result_type) ||
            result_type != callable->callable_signature->result_type_id)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi call v%u result disagrees with its callable signature", value->id);
        instruction->operation_id = XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT;
        instruction->result_type_id = result_type;
        instruction->result_ownership = logical_ownership_for_type(context, result_type);
        if (result_type != XR_CORE_TYPE_VOID)
            instruction->result = value_key(function, value);
        instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                            diagnostic_size);
    }
    if (!callee_storage)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                    "Xi call v%u names a function outside the canonical graph", value->id);
    uint16_t error_type = XR_CORE_TYPE_VOID;
    XrProgramBuildStatus error_status =
        map_function_error_type(context, callee, &error_type, diagnostic, diagnostic_size);
    if (error_status != XR_PROGRAM_BUILD_OK)
        return error_status;
    if (error_type != XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "fallible Xi call v%u lacks an explicit invoke continuation", value->id);
    uint16_t panic_type = XR_CORE_TYPE_VOID;
    XrProgramBuildStatus panic_status =
        map_function_panic_type(context, callee, &panic_type, diagnostic, diagnostic_size);
    if (panic_status != XR_PROGRAM_BUILD_OK)
        return panic_status;
    if (panic_type != XR_CORE_TYPE_VOID)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "panic-capable Xi call v%u lacks an explicit panic continuation", value->id);

    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!map_type(context, value->type, &result_type))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi call v%u result type is not active in CoreSpec", value->id);
    instruction->operation_id = projection->core_operation_id;
    instruction->result_type_id = result_type;
    instruction->result_ownership = logical_ownership_for_type(context, result_type);
    if (result_type != XR_CORE_TYPE_VOID)
        instruction->result = value_key(function, value);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = callee_storage->key;
    return set_operands(instruction, function, block, value->args + 1u, value->nargs - 1u,
                        diagnostic, diagnostic_size);
}

static XrProgramBuildStatus translate_callable_pack(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    const XiValue *value,
                                                    XrCoreIrInstructionInput *instruction,
                                                    char *diagnostic, size_t diagnostic_size) {
    uint16_t callable_type_id = XR_CORE_TYPE_VOID;
    const XiFunc *target = resolved_callable_target(function->xi, value);
    const XrXiFunctionStorage *target_storage = find_xi_function(context, target, NULL, NULL);
    if (!target || !target_storage || !map_type(context, value->type, &callable_type_id))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi callable pack v%u has no exact target or signature", value->id);
    const XrXiTypeStorage *callable = find_dynamic_type_by_id(context, callable_type_id);
    if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
        !callable->callable_signature || value->nargs != 0u)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi callable pack v%u is not captureless", value->id);
    instruction->operation_id = XR_CORE_OP_CORE_CALLABLE_PACK;
    instruction->result = value_key(function, value);
    instruction->result_type_id = callable_type_id;
    instruction->result_ownership = XR_CORE_IR_OWNER;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
    instruction->immediate.key = target_storage->key;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus translate_value(XrXiBuildContext *context, XrXiModuleStorage *module,
                                            XrXiFunctionStorage *function, const XiValue *value,
                                            const XrXiBlockStorage *block,
                                            XrCoreIrInstructionInput *instruction, char *diagnostic,
                                            size_t diagnostic_size) {
    memset(instruction, 0, sizeof(*instruction));
    uint16_t result_type = XR_CORE_TYPE_VOID;
    if (!map_type(context, value->type, &result_type))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi operation %u at v%u result type kind %u is not active in CoreSpec",
                    value->op, value->id, value->type ? (unsigned) value->type->kind : UINT32_MAX);
    XrProgramXiProjection projection;
    if (!xr_program_xi_projection(value->op, result_type, &projection))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi operation %s (%u) at v%u has no active CoreSpec projection",
                    xi_op_name(value->op), value->op, value->id);

    switch (projection.kind) {
        case XR_PROGRAM_XI_PROJECTION_CONSTANT: {
            XrCoreIrKey constant;
            XrProgramBuildStatus status =
                add_constant(context, module, value, &constant, diagnostic, diagnostic_size);
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
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        case XR_PROGRAM_XI_PROJECTION_COMPARE: {
            uint16_t left_type = XR_CORE_TYPE_VOID;
            uint16_t right_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(context, value->args[0]->type, &left_type) ||
                !map_type(context, value->args[1]->type, &right_type) ||
                left_type != XR_CORE_TYPE_I64 || right_type != XR_CORE_TYPE_I64)
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
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_CONSTRUCT: {
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                value->nargs != type->input.field_count || value->aux_int < 0 ||
                (uint64_t) (value->aux_int & XI_TUPLE_AUX_ARITY_MASK) != type->input.field_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple construction v%u has no exact logical aggregate shape",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_PROJECT: {
            uint16_t aggregate_type_id = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || value->aux_int < 0 ||
                !map_type(context, value->args[0]->type, &aggregate_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple projection v%u has no exact logical aggregate source",
                            value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, aggregate_type_id);
            uint32_t ordinal = (uint32_t) value->aux_int;
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                ordinal >= type->input.field_count ||
                type->input.field_types[ordinal] != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi tuple projection v%u has an invalid declaration ordinal",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
            instruction->immediate.field_ordinal = ordinal;
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_AGGREGATE_UPDATE: {
            uint16_t source_type_id = XR_CORE_TYPE_VOID;
            uint16_t replacement_type_id = XR_CORE_TYPE_VOID;
            uint32_t ordinal = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (value->nargs != 2u || !map_type(context, value->args[0]->type, &source_type_id) ||
                !map_type(context, value->args[1]->type, &replacement_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi aggregate update v%u has no exact logical operands", value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_AGGREGATE ||
                source_type_id != result_type || ordinal >= type->input.field_count ||
                type->input.field_types[ordinal] != replacement_type_id)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi aggregate update v%u has an invalid declaration ordinal or type",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
            instruction->immediate.field_ordinal = ordinal;
            return set_operands(instruction, function, block, value->args, value->nargs, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_CONSTRUCT: {
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, result_type);
            uint32_t variant = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT || value->nargs < 1u ||
                variant >= type->input.variant_count ||
                value->nargs - 1u != type->input.variants[variant].payload_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant construction v%u has no exact logical variant shape",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
            instruction->immediate.variant_ordinal = variant;
            return set_operands(instruction, function, block, value->args + 1u, value->nargs - 1u,
                                diagnostic, diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_TEST: {
            uint16_t variant_type_id = XR_CORE_TYPE_VOID;
            uint32_t variant = value->aux_int >= 0 ? (uint32_t) value->aux_int : UINT32_MAX;
            if (value->nargs != 1u || result_type != XR_CORE_TYPE_BOOL ||
                !map_type(context, value->args[0]->type, &variant_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant test v%u has no exact logical variant source", value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, variant_type_id);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT ||
                variant >= type->input.variant_count)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant test v%u has an invalid declaration ordinal", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = XR_CORE_TYPE_BOOL;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT;
            instruction->immediate.variant_ordinal = variant;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_VARIANT_PROJECT: {
            uint16_t variant_type_id = XR_CORE_TYPE_VOID;
            uint32_t variant = xi_variant_projection_variant(value);
            uint32_t field = xi_variant_projection_field(value);
            if (value->nargs != 1u || !map_type(context, value->args[0]->type, &variant_type_id))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant projection v%u has no exact logical variant source",
                            value->id);
            const XrXiTypeStorage *type = find_dynamic_type_by_id(context, variant_type_id);
            if (!type || type->input.kind != XR_CORE_IR_TYPE_VARIANT ||
                variant >= type->input.variant_count ||
                field >= type->input.variants[variant].payload_count ||
                type->input.variants[variant].payload_types[field] != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi variant projection v%u has an invalid declaration ordinal",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT_FIELD;
            instruction->immediate.variant_field.variant_ordinal = variant;
            instruction->immediate.variant_field.field_ordinal = field;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_OWNER_COPY: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->op != XI_CALL_BUILTIN || value->nargs != 1u || !value->aux ||
                value->aux_kind != XI_AUX_KIND_NONE ||
                strcmp((const char *) value->aux, "copy") != 0 ||
                result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &operand_type) ||
                operand_type != result_type ||
                logical_copy_contract_for_type(context, result_type) == XR_CORE_IR_COPY_FORBIDDEN)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi copy v%u has no exact logical copy contract", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_OWNER_MOVE: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &operand_type) ||
                operand_type != result_type)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi owner transfer v%u has no exact logical value type", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_ownership = logical_ownership_for_type(context, result_type);
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_LOCAL: {
            uint16_t operand_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &operand_type) ||
                operand_type != result_type || !xi_local_addr_names_operand_storage(value->aux_int))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi local address v%u is not an exact call-bound local place",
                            value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->result_category = XR_CORE_IR_PLACE;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_LOAD: {
            uint16_t place_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 1u || result_type == XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &place_type) ||
                place_type != result_type ||
                logical_value_category(value->args[0]) != XR_CORE_IR_PLACE)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi place load v%u has no exact pointee type", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result = value_key(function, value);
            instruction->result_type_id = result_type;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, 1u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_PLACE_STORE: {
            uint16_t place_type = XR_CORE_TYPE_VOID;
            uint16_t value_type = XR_CORE_TYPE_VOID;
            if (value->nargs != 2u || result_type != XR_CORE_TYPE_VOID ||
                !map_type(context, value->args[0]->type, &place_type) ||
                !map_type(context, value->args[1]->type, &value_type) || place_type != value_type ||
                logical_value_category(value->args[0]) != XR_CORE_IR_PLACE)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi place store v%u has no exact pointee/value contract", value->id);
            instruction->operation_id = projection.core_operation_id;
            instruction->result_type_id = XR_CORE_TYPE_VOID;
            instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
            return set_operands(instruction, function, block, value->args, 2u, diagnostic,
                                diagnostic_size);
        }
        case XR_PROGRAM_XI_PROJECTION_CALLABLE_PACK:
            return translate_callable_pack(context, function, value, instruction, diagnostic,
                                           diagnostic_size);
        default:
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi operation %u at v%u has an invalid CoreSpec projection", value->op,
                        value->id);
    }
}

static bool value_is_skipped(const XrXiBuildContext *context, const XiFunc *function,
                             const XiValue *value) {
    if (value_is_invoke_scaffold(context, function, value))
        return true;
    if (value->op == XI_PARAM || value->op == XI_THROW || xi_copy_is_identity_alias(value) ||
        (xi_copy_is_value_clone(value) && logical_value_identity(value) != value))
        return true;
    /* Physical RC is executor-private representation.  Canonical ownership is
     * reconstructed from typed owner creation and semantic uses, never from
     * retain/release counts. */
    if (value->op == XI_RETAIN || value->op == XI_RELEASE)
        return true;
    return (value->op == XI_GET_SHARED || value->op == XI_GET_BUILTIN) &&
           value_is_only_elided_operand(function, value);
}

/* Captureless closure creation establishes one affine callable owner.  When
 * its sole semantic use is one indirect call in the same block, its logical
 * lifetime ends immediately after that call.  Physical ARC users are ignored:
 * they can be rewritten by an executor without changing this conclusion. */
static const XiValue *logical_callable_owner_ending_after(const XiFunc *function,
                                                          const XiValue *consumer) {
    if (!function || !consumer || consumer->op != XI_CALL || consumer->nargs == 0u ||
        !consumer->args)
        return NULL;
    const XiValue *owner = logical_value_identity(consumer->args[0]);
    if (!owner || owner->op != XI_CLOSURE_NEW || owner->nargs != 0u ||
        owner->block != consumer->block)
        return NULL;
    uint32_t semantic_uses = 0u;
    for (uint32_t block_index = 0; block_index < function->nblocks; ++block_index) {
        const XiBlock *block = function->blocks[block_index];
        if (block && block->control && logical_value_identity(block->control) == owner)
            return NULL;
        for (uint32_t value_index = 0; block && value_index < block->nvalues; ++value_index) {
            const XiValue *value = block->values[value_index];
            if (!value)
                continue;
            bool physical_arc = value->op == XI_RETAIN || value->op == XI_RELEASE;
            for (uint16_t argument = 0; argument < value->nargs; ++argument) {
                if (logical_value_identity(value->args[argument]) != owner || physical_arc)
                    continue;
                semantic_uses++;
                if (value != consumer || argument != 0u)
                    return NULL;
            }
        }
    }
    return semantic_uses == 1u ? owner : NULL;
}

static XrProgramBuildStatus emit_logical_owner_drop(const XrXiFunctionStorage *function,
                                                    const XrXiBlockStorage *block,
                                                    const XiValue *owner,
                                                    XrCoreIrInstructionInput *instruction,
                                                    char *diagnostic, size_t diagnostic_size) {
    if (!function || !block || !owner || !instruction)
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    XrCoreIrKey *operands = xr_calloc(1u, sizeof(*operands));
    if (!operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    if (!value_operand_key(function, block, owner, operands)) {
        xr_free(operands);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "logical callable owner v%u is unavailable at lifetime end", owner->id);
    }
    memset(instruction, 0, sizeof(*instruction));
    instruction->operation_id = XR_CORE_OP_CORE_OWNER_DROP;
    instruction->result_type_id = XR_CORE_TYPE_VOID;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
    instruction->operands = operands;
    instruction->operand_count = 1u;
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus require_value_available(XrXiBuildContext *context,
                                                    XrXiFunctionStorage *function,
                                                    XrXiBlockStorage *block, const XiValue *value,
                                                    bool *changed, char *diagnostic,
                                                    size_t diagnostic_size) {
    value = logical_value_identity(value);
    if (!value || !value->block)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi operand has no defining block");
    if (value->block == block->xi)
        return XR_PROGRAM_BUILD_OK;
    if (block->xi == function->xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi entry block depends on non-parameter v%u", value->id);
    return add_block_argument(context, function, block, value, NULL, XR_CORE_TYPE_VOID,
                              XR_XI_INVOKE_ARGUMENT_NONE, changed, diagnostic, diagnostic_size);
}

static XrProgramBuildStatus collect_value_live_ins(XrXiBuildContext *context,
                                                   XrXiFunctionStorage *function,
                                                   XrXiBlockStorage *block, const XiValue *value,
                                                   bool *changed, char *diagnostic,
                                                   size_t diagnostic_size) {
    uint16_t begin = value->op == XI_VARIANT_CONSTRUCT ||
                             (value->op == XI_CALL &&
                              resolved_direct_callee(context, function->xi, value) != NULL)
                         ? 1u
                         : 0u;
    for (uint16_t argument = begin; argument < value->nargs; ++argument) {
        XrProgramBuildStatus status = require_value_available(
            context, function, block, value->args[argument], changed, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static int block_argument_compare(const void *left, const void *right) {
    const XrXiBlockArgumentStorage *a = left;
    const XrXiBlockArgumentStorage *b = right;
    if ((a->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE) !=
        (b->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE))
        return a->implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE ? -1 : 1;
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

static XrProgramBuildStatus prepare_invoke_arguments(XrXiBuildContext *context,
                                                     XrXiFunctionStorage *function,
                                                     char *diagnostic, size_t diagnostic_size) {
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        const XiBlock *source = function->xi->blocks[block_index];
        const XiValue *call = block_sealed_invoke_call(context, function->xi, source);
        if (!call)
            continue;
        const XiFunc *callee = resolved_direct_callee(context, function->xi, call);
        uint16_t error_type = XR_CORE_TYPE_VOID;
        XrProgramBuildStatus status =
            map_function_error_type(context, callee, &error_type, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        if (error_type == XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi error check in b%u guards an infallible sealed call", source->id);
        uint16_t panic_type = XR_CORE_TYPE_VOID;
        status = map_function_panic_type(context, callee, &panic_type, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
        if (panic_type != XR_CORE_TYPE_VOID)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke b%u has no explicit panic continuation", source->id);
        XrXiBlockStorage *normal = find_block_storage(function, source->succs[1]);
        XrXiBlockStorage *error = find_block_storage(function, source->succs[0]);
        const XiValue *caught = block_error_catch(source->succs[0]);
        if (!normal || !error || !caught)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke b%u does not expose direct typed normal/error continuations",
                        source->id);
        uint16_t result_type = XR_CORE_TYPE_VOID;
        if (!map_type(context, call->type, &result_type))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                        "Xi invoke call v%u result type is not active in CoreSpec", call->id);
        if (result_type != XR_CORE_TYPE_VOID) {
            status = add_block_argument(context, function, normal, call, NULL, result_type,
                                        XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT, NULL, diagnostic,
                                        diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        status = add_block_argument(context, function, error, caught, NULL, error_type,
                                    XR_XI_INVOKE_ARGUMENT_ERROR, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    return XR_PROGRAM_BUILD_OK;
}

static XrProgramBuildStatus close_block_arguments(XrXiBuildContext *context,
                                                  XrXiFunctionStorage *function, char *diagnostic,
                                                  size_t diagnostic_size) {
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        block->xi = function->xi->blocks[block_index];
    }
    XrProgramBuildStatus invoke_status =
        prepare_invoke_arguments(context, function, diagnostic, diagnostic_size);
    if (invoke_status != XR_PROGRAM_BUILD_OK)
        return invoke_status;
    XrXiBlockStorage *entry = find_block_storage(function, function->xi->entry);
    if (!entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi function entry block is absent");
    for (uint16_t parameter = 0; parameter < function->xi->nparams; ++parameter) {
        XrProgramBuildStatus status = add_block_argument(
            context, function, entry, function->xi->params[parameter], NULL, XR_CORE_TYPE_VOID,
            XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
        if (status != XR_PROGRAM_BUILD_OK)
            return status;
    }
    for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
        XrXiBlockStorage *block = &function->block_storage[block_index];
        for (const XiPhi *phi = block->xi->phis; phi; phi = phi->next) {
            XrProgramBuildStatus status =
                add_block_argument(context, function, block, &phi->value, phi, XR_CORE_TYPE_VOID,
                                   XR_XI_INVOKE_ARGUMENT_NONE, NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        for (uint32_t value_index = 0; value_index < block->xi->nvalues; ++value_index) {
            const XiValue *value = block->xi->values[value_index];
            if (value_is_skipped(context, function->xi, value))
                continue;
            XrProgramBuildStatus status = collect_value_live_ins(context, function, block, value,
                                                                 NULL, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        }
        if (block->xi->control) {
            XrProgramBuildStatus status = require_value_available(
                context, function, block, block->xi->control, NULL, diagnostic, diagnostic_size);
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
                    if (argument.implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NONE)
                        continue;
                    const XiValue *incoming =
                        edge_argument_value(&argument, predecessor_xi, successor->xi);
                    XrProgramBuildStatus status =
                        require_value_available(context, function, predecessor, incoming, &changed,
                                                diagnostic, diagnostic_size);
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
            block->arguments[argument].category = block->argument_storage[argument].category;
            block->arguments[argument].ownership = block->argument_storage[argument].ownership;
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

static XrProgramBuildStatus set_invoke_operands(XrCoreIrInstructionInput *instruction,
                                                const XrXiFunctionStorage *function,
                                                const XrXiBlockStorage *predecessor,
                                                const XiValue *call, const XrXiBlockStorage *normal,
                                                const XrXiBlockStorage *error, char *diagnostic,
                                                size_t diagnostic_size) {
    uint32_t normal_implicit = call->type && call->type->kind != XR_KIND_UNIT ? 1u : 0u;
    if (normal->argument_count < normal_implicit || error->argument_count == 0u ||
        (normal_implicit != 0u &&
         normal->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_NORMAL_RESULT) ||
        error->argument_storage[0].implicit_invoke_kind != XR_XI_INVOKE_ARGUMENT_ERROR)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi invoke continuations lack ordered implicit result/error arguments");
    uint32_t parameter_count = call->nargs - 1u;
    uint32_t count =
        parameter_count + normal->argument_count - normal_implicit + error->argument_count - 1u;
    XrCoreIrKey *operands = count ? xr_calloc(count, sizeof(*operands)) : NULL;
    if (count && !operands)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    for (uint16_t parameter = 1u; parameter < call->nargs; ++parameter) {
        if (!value_operand_key(function, predecessor, call->args[parameter], &operands[cursor++]))
            goto unavailable;
    }
    const XrXiBlockStorage *successors[2] = {normal, error};
    const uint32_t starts[2] = {normal_implicit, 1u};
    for (uint32_t edge = 0u; edge < 2u; ++edge) {
        const XrXiBlockStorage *successor = successors[edge];
        for (uint32_t argument = starts[edge]; argument < successor->argument_count; ++argument) {
            const XiValue *incoming = edge_argument_value(&successor->argument_storage[argument],
                                                          predecessor->xi, successor->xi);
            if (!value_operand_key(function, predecessor, incoming, &operands[cursor++]))
                goto unavailable;
        }
    }
    instruction->operands = operands;
    instruction->operand_count = count;
    return XR_PROGRAM_BUILD_OK;

unavailable:
    xr_free(operands);
    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi invoke edge operand is unavailable in predecessor b%u", predecessor->xi->id);
}

static XrProgramBuildStatus build_function(XrXiBuildContext *context, XrXiModuleStorage *module,
                                           uint32_t function_index, char *diagnostic,
                                           size_t diagnostic_size) {
    const XiFunc *xi =
        function_index < module->function_count ? module->xi_functions[function_index] : NULL;
    XrCoreIrFunctionInput *output = &module->functions[function_index];
    XrXiFunctionStorage *storage = &module->function_storage[function_index];
    XrCoreIrKey key = storage->key;
    memset(output, 0, sizeof(*output));
    memset(storage, 0, sizeof(*storage));
    storage->xi = xi;
    storage->key = key;
    if (!xi || xi->stage != XI_STAGE_OPTIMIZED || xi->semantic_plan || xi->nblocks == 0u ||
        !xi->entry)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %u is not an Optimized program input", function_index);
    output->key = storage->key;
    output->parameter_count = xi->nparams;
    if (!map_type(context, xi->return_type, &output->result_type_id))
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                    "Xi function %u result type is not active in CoreSpec", function_index);
    XrProgramBuildStatus signature_status =
        map_function_error_type(context, xi, &output->error_type_id, diagnostic, diagnostic_size);
    if (signature_status != XR_PROGRAM_BUILD_OK)
        return signature_status;
    signature_status =
        map_function_panic_type(context, xi, &output->panic_type_id, diagnostic, diagnostic_size);
    if (signature_status != XR_PROGRAM_BUILD_OK)
        return signature_status;
    output->result_ownership = logical_ownership_for_type(context, output->result_type_id);
    output->has_receiver = xi->has_receiver;
    output->receiver_mode = xi->has_receiver ? xi->receiver_mode : XR_PARAM_READ;
    if (xi->nparams != 0) {
        storage->parameter_types = xr_calloc(xi->nparams, sizeof(*storage->parameter_types));
        storage->parameter_modes = xr_calloc(xi->nparams, sizeof(*storage->parameter_modes));
        if (!storage->parameter_types || !storage->parameter_modes)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        for (uint16_t parameter = 0; parameter < xi->nparams; ++parameter) {
            if (!xi->params || !xi->params[parameter] || xi->params[parameter]->op != XI_PARAM ||
                xi->params[parameter]->aux_int != parameter ||
                !map_type(context, xi->params[parameter]->type,
                          &storage->parameter_types[parameter]) ||
                storage->parameter_types[parameter] == XR_CORE_TYPE_VOID)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi function %u parameter %u is not a CoreSpec value", function_index,
                            parameter);
            if (!xr_param_mode_is_valid((XrParamMode) xi->params[parameter]->param_mode))
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi function %u parameter %u has an invalid mode", function_index,
                            parameter);
            storage->parameter_modes[parameter] = (XrParamMode) xi->params[parameter]->param_mode;
        }
        output->parameter_types = storage->parameter_types;
        output->parameter_modes = storage->parameter_modes;
    }
    output->flags = xi == context->source->entry_function ? XR_PROGRAM_FUNCTION_ENTRY : 0u;

    storage->blocks = xr_calloc(xi->nblocks, sizeof(*storage->blocks));
    storage->block_storage = xr_calloc(xi->nblocks, sizeof(*storage->block_storage));
    if (!storage->blocks || !storage->block_storage)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    output->blocks = storage->blocks;
    output->block_count = xi->nblocks;
    output->entry_block = block_key(storage, xi->entry);
    XrProgramBuildStatus status =
        close_block_arguments(context, storage, diagnostic, diagnostic_size);
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
            const XiValue *value = xi_block->values[value_index];
            if (!value_is_skipped(context, xi, value))
                ++emitted;
            if (logical_callable_owner_ending_after(xi, value))
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
            if (value_is_skipped(context, xi, value))
                continue;
            XrCoreIrInstructionInput *instruction =
                &block_storage->instructions[instruction_index++];
            status = translate_value(context, module, storage, value, block_storage, instruction,
                                     diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
            const XrCoreOperationSpec *operation =
                xr_core_spec_operation_by_id(instruction->operation_id);
            if (!operation)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "translated Xi v%u has no CoreSpec operation", value->id);
            storage->local_effect_mask |= operation->effect_mask;
            const XiValue *drop_owner = logical_callable_owner_ending_after(xi, value);
            if (drop_owner) {
                XrCoreIrInstructionInput *drop = &block_storage->instructions[instruction_index++];
                status = emit_logical_owner_drop(storage, block_storage, drop_owner, drop,
                                                 diagnostic, diagnostic_size);
                if (status != XR_PROGRAM_BUILD_OK)
                    return status;
            }
        }

        XrCoreIrInstructionInput *terminator = &block_storage->instructions[instruction_index++];
        terminator->result_type_id = XR_CORE_TYPE_VOID;
        terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_NONE;
        const XiValue *invoke_call = block_sealed_invoke_call(context, xi, xi_block);
        if (invoke_call) {
            const XiFunc *callee = resolved_direct_callee(context, xi, invoke_call);
            const XrXiFunctionStorage *callee_storage =
                find_xi_function(context, callee, NULL, NULL);
            XrXiBlockStorage *normal = find_block_storage(storage, xi_block->succs[1]);
            XrXiBlockStorage *error = find_block_storage(storage, xi_block->succs[0]);
            if (!callee_storage || !normal || !error)
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi invoke block b%u has an unresolved continuation", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_CALL_SEALED_INVOKE;
            terminator->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
            terminator->immediate.key = callee_storage->key;
            XrCoreIrKey *successors = xr_calloc(2u, sizeof(*successors));
            if (!successors)
                return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
            successors[0] = block_key(storage, normal->xi);
            successors[1] = block_key(storage, error->xi);
            terminator->successors = successors;
            terminator->successor_count = 2u;
            status = set_invoke_operands(terminator, storage, block_storage, invoke_call, normal,
                                         error, diagnostic, diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_UNREACHABLE) {
            const XiValue *throw_value = NULL;
            for (uint32_t value_index = 0u; value_index < xi_block->nvalues; ++value_index) {
                const XiValue *candidate = xi_block->values[value_index];
                if (!candidate || candidate->op != XI_THROW)
                    continue;
                if (throw_value)
                    return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                                "Xi panic block b%u has multiple throw terminals", xi_block->id);
                throw_value = candidate;
            }
            if (!throw_value || throw_value->nargs != 1u || !throw_value->args ||
                !throw_value->args[0])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
                            "Xi unreachable block b%u has no typed panic terminal", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_PANIC_PUBLISH;
            XiValue *published[] = {throw_value->args[0]};
            status = set_operands(terminator, storage, block_storage, published, 1u, diagnostic,
                                  diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_RETURN && xi_block->control &&
                   xi_block->control->op == XI_ERR_RETURN) {
            if (xi_block->control->nargs != 1u || !xi_block->control->args[0])
                return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                            "Xi error publication in b%u has no typed payload", xi_block->id);
            terminator->operation_id = XR_CORE_OP_CORE_ERROR_PUBLISH;
            XiValue *published[] = {xi_block->control->args[0]};
            status = set_operands(terminator, storage, block_storage, published, 1u, diagnostic,
                                  diagnostic_size);
            if (status != XR_PROGRAM_BUILD_OK)
                return status;
        } else if (xi_block->kind == XI_BLOCK_RETURN) {
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
                !map_type(context, xi_block->control->type, &condition_type) ||
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
        const XrCoreOperationSpec *terminal_operation =
            xr_core_spec_operation_by_id(terminator->operation_id);
        if (!terminal_operation)
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi block b%u has no CoreSpec terminal operation", xi_block->id);
        storage->local_effect_mask |= terminal_operation->effect_mask;
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
        total_functions += module->function_count;
        for (uint32_t function = 0; function < module->function_count; ++function)
            module->functions[function].effect_mask =
                module->function_storage[function].local_effect_mask;
    }
    for (uint32_t iteration = 0; iteration < total_functions; ++iteration) {
        bool changed = false;
        for (uint32_t module_index = 0; module_index < context->source->module_count;
             ++module_index) {
            XrXiModuleStorage *module = &context->storage[module_index];
            for (uint32_t function_index = 0; function_index < module->function_count;
                 ++function_index) {
                XrXiFunctionStorage *function = &module->function_storage[function_index];
                uint32_t effects = module->functions[function_index].effect_mask;
                for (uint32_t block_index = 0; block_index < function->xi->nblocks; ++block_index) {
                    const XiBlock *block = function->xi->blocks[block_index];
                    for (uint32_t value_index = 0; value_index < block->nvalues; ++value_index) {
                        const XiValue *value = block->values[value_index];
                        if (value->op != XI_CALL)
                            continue;
                        const XiFunc *callee = resolved_direct_callee(context, function->xi, value);
                        uint32_t callee_module = 0;
                        uint32_t callee_function = 0;
                        uint32_t callee_effects = 0u;
                        if (callee) {
                            if (!find_xi_function(context, callee, &callee_module,
                                                  &callee_function))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi effect closure has an unresolved sealed call");
                            callee_effects = context->storage[callee_module]
                                                 .functions[callee_function]
                                                 .effect_mask;
                            if (block_sealed_invoke_call(context, function->xi, block) == value)
                                callee_effects &= ~XR_CORE_EFFECT_ERROR;
                        } else {
                            uint16_t callable_type_id = XR_CORE_TYPE_VOID;
                            const XiValue *callee_value =
                                value->nargs ? logical_value_identity(value->args[0]) : NULL;
                            if (!callee_value ||
                                !map_type(context, callee_value->type, &callable_type_id))
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi effect closure has an untyped indirect call");
                            const XrXiTypeStorage *callable =
                                find_dynamic_type_by_id(context, callable_type_id);
                            if (!callable || callable->input.kind != XR_CORE_IR_TYPE_CALLABLE ||
                                !callable->callable_signature)
                                return fail(diagnostic, diagnostic_size,
                                            XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
                                            "Xi effect closure has a non-callable indirect target");
                            callee_effects = callable->callable_signature->effect_mask;
                        }
                        effects |= callee_effects;
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

static XrProgramBuildStatus finalize_type_inputs(XrXiBuildContext *context) {
    if (context->type_count == 0u)
        return XR_PROGRAM_BUILD_OK;
    context->types = xr_calloc(context->type_count, sizeof(*context->types));
    if (!context->types)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    for (uint32_t type = 0; type < context->type_count; ++type)
        context->types[type] = context->type_storage[type].input;
    return XR_PROGRAM_BUILD_OK;
}

static bool count_nested_functions(const XiFunc *function, uint32_t *count) {
    if (!function || !count)
        return false;
    for (uint16_t child = 0; child < function->nchildren; ++child) {
        if (*count == UINT32_MAX)
            return false;
        ++*count;
        if (!count_nested_functions(function->children[child], count))
            return false;
    }
    return true;
}

static bool append_nested_functions(const XiFunc *function, const XiFunc **functions,
                                    uint32_t capacity, uint32_t *cursor) {
    if (!function || !functions || !cursor)
        return false;
    for (uint16_t child = 0; child < function->nchildren; ++child) {
        if (*cursor >= capacity || !function->children[child])
            return false;
        functions[(*cursor)++] = function->children[child];
        if (!append_nested_functions(function->children[child], functions, capacity, cursor))
            return false;
    }
    return true;
}

static XrProgramBuildStatus collect_module_functions(XrXiModuleStorage *storage, char *diagnostic,
                                                     size_t diagnostic_size) {
    const XiModule *module = storage && storage->root ? storage->root->module : NULL;
    if (!module || module->nfuncs == 0u || !module->functions)
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi module has no source functions");
    uint32_t count = module->nfuncs;
    for (uint16_t function = 0; function < module->nfuncs; ++function)
        if (!module->functions[function] ||
            !count_nested_functions(module->functions[function], &count))
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree is malformed or too large");
    const XiFunc **functions = xr_calloc(count, sizeof(*functions));
    if (!functions)
        return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
    uint32_t cursor = 0u;
    for (uint16_t function = 0; function < module->nfuncs; ++function)
        functions[cursor++] = module->functions[function];
    for (uint16_t function = 0; function < module->nfuncs; ++function) {
        if (!append_nested_functions(module->functions[function], functions, count, &cursor)) {
            xr_free(functions);
            return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                        "Xi module function tree cannot be flattened");
        }
    }
    if (cursor != count) {
        xr_free(functions);
        return fail(diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                    "Xi module function count is inconsistent");
    }
    storage->xi_functions = functions;
    storage->function_count = count;
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
        XrProgramBuildStatus collect_status =
            collect_module_functions(storage, diagnostic, diagnostic_size);
        if (collect_status != XR_PROGRAM_BUILD_OK)
            return collect_status;
        output->key = key_from_stable_id(UINT8_C(0x4d), storage->source_authority->module_identity);
        storage->functions = xr_calloc(storage->function_count, sizeof(*storage->functions));
        storage->function_storage =
            xr_calloc(storage->function_count, sizeof(*storage->function_storage));
        if (!storage->functions || !storage->function_storage)
            return XR_PROGRAM_BUILD_OUT_OF_MEMORY;
        output->functions = storage->functions;
        output->function_count = storage->function_count;
        for (uint32_t function = 0; function < storage->function_count; ++function) {
            storage->function_storage[function].xi = storage->xi_functions[function];
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
        for (uint32_t function = 0; function < storage->function_count; ++function) {
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
    status = finalize_type_inputs(context);
    if (status != XR_PROGRAM_BUILD_OK)
        return status;
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
        !input->entry_function || !input->global_evidence || !input->semantic_profile_fingerprint)
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
            .types = context.types,
            .type_count = context.type_count,
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
            const XrCoreIrInstruction *failed_instruction = NULL;
            uint32_t flat_function = 0u;
            for (uint32_t module = 0u;
                 program && module < program->module_count && !failed_instruction; ++module) {
                const XrCoreIrModule *module_row = &program->modules[module];
                for (uint32_t function = 0u; function < module_row->function_count;
                     ++function, ++flat_function) {
                    if (flat_function != verify_diagnostic.location.function_id)
                        continue;
                    const XrCoreIrFunction *function_row = &module_row->functions[function];
                    if (verify_diagnostic.location.block_id < function_row->block_count) {
                        const XrCoreIrBlock *block =
                            &function_row->blocks[verify_diagnostic.location.block_id];
                        if (verify_diagnostic.location.instruction_id < block->instruction_count)
                            failed_instruction =
                                &block->instructions[verify_diagnostic.location.instruction_id];
                    }
                    break;
                }
            }
            xr_program_artifact_free(artifact_out);
            status = fail(
                diagnostic, diagnostic_size, XR_PROGRAM_BUILD_INVALID_INPUT,
                "Xi-produced XrProgram failed semantic verification: %s/%s "
                "at function=%u block=%u instruction=%u value=%u operation=%u "
                "result_type=%u category=%u ownership=%u result_zero=%u",
                xr_program_verify_status_name(verify),
                xr_program_diagnostic_kind_name(verify_diagnostic.kind),
                verify_diagnostic.location.function_id, verify_diagnostic.location.block_id,
                verify_diagnostic.location.instruction_id, verify_diagnostic.location.value_id,
                failed_instruction ? failed_instruction->operation_id : 0u,
                failed_instruction ? failed_instruction->result_type_id : 0u,
                failed_instruction ? failed_instruction->result_category : 0u,
                failed_instruction ? failed_instruction->result_ownership : 0u,
                failed_instruction ? (unsigned) xr_core_ir_key_is_zero(failed_instruction->result)
                                   : 1u);
        }
    }
    xr_core_ir_program_free(program);
    free_context(&context);
    return status;
}
