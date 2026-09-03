/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_decode.c - Bounded canonical XrProgram structural decoder
 */

#include "xr_program_decode.h"

#include "../base/xmalloc.h"
#include "../core/xr_core_spec_gen.h"
#include "xr_program_internal.h"

#include <string.h>

typedef struct Reader {
    const uint8_t *bytes;
    size_t size;
    size_t offset;
    uint64_t records;
    uint64_t operations;
    XrProgramDecodeBudget budget;
    XrProgramDecodeStatus status;
} Reader;

static bool take_bytes(Reader *reader, void *output, size_t size) {
    if (reader->status != XR_PROGRAM_DECODE_OK)
        return false;
    if (reader->offset > reader->size || size > reader->size - reader->offset) {
        reader->status = XR_PROGRAM_DECODE_TRUNCATED;
        return false;
    }
    if (output)
        memcpy(output, reader->bytes + reader->offset, size);
    reader->offset += size;
    return true;
}

static uint16_t take_u16(Reader *reader) {
    uint8_t bytes[2] = {0};
    take_bytes(reader, bytes, sizeof(bytes));
    return (uint16_t) bytes[0] | (uint16_t) ((uint16_t) bytes[1] << 8u);
}

static uint64_t take_uvar(Reader *reader) {
    uint64_t value = 0;
    unsigned shift = 0;
    unsigned count = 0;
    uint8_t byte = 0;
    do {
        if (count == 10u) {
            reader->status = XR_PROGRAM_DECODE_NONCANONICAL;
            return 0;
        }
        if (!take_bytes(reader, &byte, 1u))
            return 0;
        uint8_t payload = byte & UINT8_C(0x7f);
        if (count == 9u && payload > 1u) {
            reader->status = XR_PROGRAM_DECODE_NONCANONICAL;
            return 0;
        }
        value |= (uint64_t) payload << shift;
        shift += 7u;
        ++count;
    } while ((byte & UINT8_C(0x80)) != 0);
    if (count > 1u && (byte & UINT8_C(0x7f)) == 0u) {
        reader->status = XR_PROGRAM_DECODE_NONCANONICAL;
        return 0;
    }
    return value;
}

static bool count_records(Reader *reader, uint64_t count) {
    if (reader->status != XR_PROGRAM_DECODE_OK)
        return false;
    if (count > reader->budget.max_records ||
        reader->records > reader->budget.max_records - count) {
        reader->status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return false;
    }
    reader->records += count;
    return true;
}

static bool count_operations(Reader *reader, uint64_t count) {
    if (reader->status != XR_PROGRAM_DECODE_OK)
        return false;
    if (count > reader->budget.max_operations ||
        reader->operations > reader->budget.max_operations - count) {
        reader->status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return false;
    }
    reader->operations += count;
    return true;
}

static Reader section_reader(const Reader *artifact, const XrProgramSectionView *section) {
    Reader reader = {
        .bytes = artifact->bytes + section->offset,
        .size = (size_t) section->size,
        .budget = artifact->budget,
        .status = XR_PROGRAM_DECODE_OK,
    };
    return reader;
}

static bool section_done(Reader *section, Reader *artifact) {
    if (section->status != XR_PROGRAM_DECODE_OK) {
        artifact->status = section->status;
        return false;
    }
    if (section->offset != section->size) {
        artifact->status = XR_PROGRAM_DECODE_NONCANONICAL;
        return false;
    }
    if (!count_records(artifact, section->records) ||
        !count_operations(artifact, section->operations))
        return false;
    return true;
}

static bool encoded_type_id_is_valid(uint64_t type_id, uint64_t dynamic_count) {
    return type_id <= XR_CORE_TYPE_PANIC_INFO ||
           (type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE &&
            type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE < dynamic_count);
}

static bool decode_types(Reader *artifact, const XrProgramSectionView *view,
                         uint64_t *dynamic_count_out, uint64_t *signature_reference_count_out,
                         uint64_t *interface_reference_count_out) {
    Reader section = section_reader(artifact, view);
    uint64_t count = take_uvar(&section);
    if (count < 6u || count > XR_PROGRAM_LIMIT_TYPES ||
        count - 6u > UINT16_MAX - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + 1u ||
        !count_records(&section, count)) {
        section.status = section.status == XR_PROGRAM_DECODE_OK
                             ? XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT
                             : section.status;
        return section_done(&section, artifact);
    }
    for (uint64_t id = 0; id < 6u && section.status == XR_PROGRAM_DECODE_OK; ++id) {
        uint64_t type_id = take_uvar(&section);
        uint64_t kind = take_uvar(&section);
        uint64_t ownership = take_uvar(&section);
        uint64_t copy_contract = take_uvar(&section);
        XrCoreIrTypeOwnership expected_ownership = id == XR_CORE_TYPE_PANIC_INFO
                                                       ? XR_CORE_IR_TYPE_OWNERSHIP_AFFINE
                                                       : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
        XrCoreIrCopyContract expected_copy =
            id == XR_CORE_TYPE_VOID || id == XR_CORE_TYPE_PANIC_INFO ? XR_CORE_IR_COPY_FORBIDDEN
                                                                     : XR_CORE_IR_COPY_TRIVIAL;
        if (type_id != id || kind != id || ownership != expected_ownership ||
            copy_contract != expected_copy)
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
    }
    uint64_t dynamic_count = count - 6u;
    uint64_t signature_reference_count = 0u;
    uint64_t interface_reference_count = 0u;
    uint8_t previous_key[XR_CORE_IR_KEY_SIZE] = {0};
    for (uint64_t index = 0; index < dynamic_count && section.status == XR_PROGRAM_DECODE_OK;
         ++index) {
        uint64_t type_id = take_uvar(&section);
        uint64_t kind = take_uvar(&section);
        uint64_t ownership = take_uvar(&section);
        uint64_t copy_contract = take_uvar(&section);
        uint8_t key[XR_CORE_IR_KEY_SIZE] = {0};
        take_bytes(&section, key, sizeof(key));
        uint64_t shape_head = take_uvar(&section);
        if (type_id != XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE + index ||
            (kind != XR_PROGRAM_TYPE_KIND_AGGREGATE && kind != XR_PROGRAM_TYPE_KIND_VARIANT &&
             kind != XR_PROGRAM_TYPE_KIND_VIEW && kind != XR_PROGRAM_TYPE_KIND_CALLABLE &&
             kind != XR_PROGRAM_TYPE_KIND_EXISTENTIAL) ||
            ownership > XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
            copy_contract > XR_CORE_IR_COPY_FORBIDDEN ||
            ((ownership == XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL) !=
             (copy_contract == XR_CORE_IR_COPY_TRIVIAL)) ||
            (index != 0u && memcmp(previous_key, key, sizeof(key)) >= 0)) {
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            break;
        }
        memcpy(previous_key, key, sizeof(key));
        if (kind == XR_PROGRAM_TYPE_KIND_AGGREGATE) {
            uint64_t field_count = take_uvar(&section);
            if (shape_head > XR_CORE_IR_NOMINAL_ENUM || field_count == 0u ||
                field_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                !count_records(&section, field_count)) {
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
                break;
            }
            for (uint64_t field = 0; field < field_count; ++field) {
                uint64_t field_type = take_uvar(&section);
                if (!encoded_type_id_is_valid(field_type, dynamic_count) ||
                    field_type == XR_CORE_TYPE_VOID)
                    section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            }
        } else if (kind == XR_PROGRAM_TYPE_KIND_VARIANT) {
            uint64_t variant_count = take_uvar(&section);
            if (shape_head > XR_CORE_IR_NOMINAL_ENUM || variant_count == 0u ||
                variant_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                !count_records(&section, variant_count)) {
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
                break;
            }
            for (uint64_t variant = 0; variant < variant_count; ++variant) {
                uint64_t payload_count = take_uvar(&section);
                if (payload_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                    !count_records(&section, payload_count)) {
                    section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
                    break;
                }
                for (uint64_t field = 0; field < payload_count; ++field) {
                    uint64_t field_type = take_uvar(&section);
                    if (!encoded_type_id_is_valid(field_type, dynamic_count) ||
                        field_type == XR_CORE_TYPE_VOID)
                        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                }
            }
        } else if (kind == XR_PROGRAM_TYPE_KIND_VIEW) {
            uint64_t capability = take_uvar(&section);
            if (!encoded_type_id_is_valid(shape_head, dynamic_count) ||
                shape_head == XR_CORE_TYPE_VOID || ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
                copy_contract != XR_CORE_IR_COPY_TRIVIAL ||
                (capability != XR_CORE_IR_VIEW_READ &&
                 capability != XR_CORE_IR_VIEW_WRITE_EXCLUSIVE))
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
        } else if (kind == XR_PROGRAM_TYPE_KIND_CALLABLE) {
            if (ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                copy_contract != XR_CORE_IR_COPY_EXPLICIT)
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            if (shape_head == UINT64_MAX)
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            else if (shape_head + 1u > signature_reference_count)
                signature_reference_count = shape_head + 1u;
        } else {
            uint64_t use_kind = take_uvar(&section);
            bool trivial = use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_READ;
            bool affine = use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF ||
                          use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE ||
                          use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE;
            if ((!trivial && !affine) ||
                (trivial && (ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
                             copy_contract != XR_CORE_IR_COPY_TRIVIAL)) ||
                (affine && (ownership != XR_CORE_IR_TYPE_OWNERSHIP_AFFINE ||
                            copy_contract != XR_CORE_IR_COPY_FORBIDDEN)))
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            if (shape_head == UINT64_MAX)
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            else if (shape_head + 1u > interface_reference_count)
                interface_reference_count = shape_head + 1u;
        }
    }
    *dynamic_count_out = dynamic_count;
    *signature_reference_count_out = signature_reference_count;
    *interface_reference_count_out = interface_reference_count;
    return section_done(&section, artifact);
}

static bool decode_constants(Reader *artifact, const XrProgramSectionView *view,
                             uint64_t *count_out) {
    Reader section = section_reader(artifact, view);
    uint64_t count = take_uvar(&section);
    uint64_t previous_type = 0;
    uint64_t previous_kind = 0;
    int64_t previous_i64 = 0;
    bool previous_bool = false;
    bool have_previous = false;
    if (count > XR_PROGRAM_LIMIT_CONSTANTS || !count_records(&section, count)) {
        section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return section_done(&section, artifact);
    }
    for (uint64_t id = 0; id < count && section.status == XR_PROGRAM_DECODE_OK; ++id) {
        uint64_t encoded_id = take_uvar(&section);
        uint64_t type_id = take_uvar(&section);
        uint64_t kind = take_uvar(&section);
        uint64_t raw_value = take_uvar(&section);
        int64_t i64_value = 0;
        bool bool_value = false;
        if (kind == XR_CORE_IR_CONSTANT_I64)
            i64_value = (raw_value & 1u) == 0u ? (int64_t) (raw_value >> 1u)
                                               : -(int64_t) (raw_value >> 1u) - 1;
        else
            bool_value = raw_value != 0u;
        if (encoded_id != id || (kind == XR_CORE_IR_CONSTANT_I64 && type_id != XR_CORE_TYPE_I64) ||
            (kind == XR_CORE_IR_CONSTANT_BOOL && type_id != XR_CORE_TYPE_BOOL) ||
            (kind != XR_CORE_IR_CONSTANT_I64 && kind != XR_CORE_IR_CONSTANT_BOOL) ||
            (kind == XR_CORE_IR_CONSTANT_BOOL && raw_value > 1u))
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
        if (have_previous && section.status == XR_PROGRAM_DECODE_OK) {
            bool ordered = type_id > previous_type ||
                           (type_id == previous_type && kind > previous_kind) ||
                           (type_id == previous_type && kind == previous_kind &&
                            kind == XR_CORE_IR_CONSTANT_I64 && i64_value > previous_i64) ||
                           (type_id == previous_type && kind == previous_kind &&
                            kind == XR_CORE_IR_CONSTANT_BOOL && bool_value && !previous_bool);
            if (!ordered)
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
        }
        previous_type = type_id;
        previous_kind = kind;
        previous_i64 = i64_value;
        previous_bool = bool_value;
        have_previous = true;
    }
    *count_out = count;
    return section_done(&section, artifact);
}

static bool decode_functions(Reader *artifact, const XrProgramSectionView *view,
                             uint64_t dynamic_type_count, uint64_t *signature_count_out,
                             uint64_t *count_out) {
    Reader section = section_reader(artifact, view);
    uint64_t signature_count = take_uvar(&section);
    if (signature_count == 0u || signature_count > XR_PROGRAM_LIMIT_FUNCTIONS ||
        !count_records(&section, signature_count)) {
        section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return section_done(&section, artifact);
    }
    for (uint64_t id = 0; id < signature_count && section.status == XR_PROGRAM_DECODE_OK; ++id) {
        uint64_t encoded_id = take_uvar(&section);
        uint64_t parameter_count = take_uvar(&section);
        if (parameter_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            !count_records(&section, parameter_count)) {
            section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            break;
        }
        for (uint64_t parameter = 0; parameter < parameter_count; ++parameter) {
            uint64_t type_id = take_uvar(&section);
            uint64_t mode = take_uvar(&section);
            if (!encoded_type_id_is_valid(type_id, dynamic_type_count) || mode > XR_PARAM_MOVE)
                section.status = XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT;
        }
        uint64_t has_receiver = take_uvar(&section);
        uint64_t receiver_mode = take_uvar(&section);
        uint64_t result_type = take_uvar(&section);
        uint64_t result_ownership = take_uvar(&section);
        uint64_t origin_count = take_uvar(&section);
        if (origin_count > XR_PROGRAM_LIMIT_ROOTS_PER_VALUE ||
            !count_records(&section, origin_count)) {
            if (section.status == XR_PROGRAM_DECODE_OK)
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            break;
        }
        uint64_t prior_kind = 0u;
        uint64_t prior_parameter = 0u;
        bool have_prior = false;
        for (uint64_t origin = 0; origin < origin_count; ++origin) {
            uint64_t kind = take_uvar(&section);
            uint64_t parameter = kind == XR_VIEW_ORIGIN_PARAM ? take_uvar(&section) : 0u;
            if (kind > XR_VIEW_ORIGIN_STATIC ||
                (have_prior &&
                 (kind < prior_kind || (kind == prior_kind && parameter <= prior_parameter))))
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            prior_kind = kind;
            prior_parameter = parameter;
            have_prior = true;
        }
        uint64_t error_type = take_uvar(&section);
        uint64_t panic_type = take_uvar(&section);
        (void) take_uvar(&section); /* effect mask */
        (void) take_uvar(&section); /* capability mask */
        if (encoded_id != id || has_receiver > 1u || receiver_mode > XR_PARAM_MOVE ||
            (has_receiver != 0u && parameter_count == 0u) ||
            (has_receiver == 0u && receiver_mode != XR_PARAM_READ) ||
            !encoded_type_id_is_valid(result_type, dynamic_type_count) ||
            !encoded_type_id_is_valid(error_type, dynamic_type_count) ||
            !encoded_type_id_is_valid(panic_type, dynamic_type_count) ||
            result_ownership > XR_CORE_IR_OWNER)
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
    }
    if (section.status != XR_PROGRAM_DECODE_OK)
        return section_done(&section, artifact);
    uint64_t count = take_uvar(&section);
    if (count == 0 || count > XR_PROGRAM_LIMIT_FUNCTIONS || !count_records(&section, count)) {
        section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return section_done(&section, artifact);
    }
    for (uint64_t id = 0; id < count && section.status == XR_PROGRAM_DECODE_OK; ++id) {
        uint64_t encoded_id = take_uvar(&section);
        uint64_t signature_id = take_uvar(&section);
        uint64_t entry_block = take_uvar(&section);
        uint64_t block_count = take_uvar(&section);
        uint64_t value_count = take_uvar(&section);
        (void) take_uvar(&section); /* flags */
        if (encoded_id != id || signature_id >= signature_count || block_count == 0 ||
            block_count > XR_PROGRAM_LIMIT_BLOCKS_PER_FUNCTION || entry_block >= block_count ||
            value_count > XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION)
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
    }
    *signature_count_out = signature_count;
    *count_out = count;
    return section_done(&section, artifact);
}

static bool decode_semantic_metadata(Reader *artifact, const XrProgramSectionView *view,
                                     uint64_t function_count, uint64_t signature_count,
                                     uint64_t dynamic_type_count, uint64_t *interface_count_out) {
    Reader section = section_reader(artifact, view);
    uint64_t interface_count = take_uvar(&section);
    if (interface_count > XR_PROGRAM_LIMIT_TYPES || !count_records(&section, interface_count)) {
        section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return section_done(&section, artifact);
    }
    uint8_t prior_interface_key[XR_CORE_IR_KEY_SIZE] = {0};
    for (uint64_t interface = 0;
         interface < interface_count && section.status == XR_PROGRAM_DECODE_OK; ++interface) {
        uint64_t id = take_uvar(&section);
        uint8_t key[XR_CORE_IR_KEY_SIZE] = {0};
        take_bytes(&section, key, sizeof(key));
        uint64_t slot_count = take_uvar(&section);
        if (id != interface || slot_count == 0u ||
            slot_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            (interface != 0u && memcmp(prior_interface_key, key, sizeof(key)) >= 0) ||
            !count_records(&section, slot_count)) {
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            break;
        }
        memcpy(prior_interface_key, key, sizeof(key));
        for (uint64_t slot = 0; slot < slot_count; ++slot)
            if (take_uvar(&section) >= signature_count)
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
    }
    uint64_t conformance_count = take_uvar(&section);
    if (conformance_count > XR_PROGRAM_LIMIT_TYPES || !count_records(&section, conformance_count)) {
        section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        return section_done(&section, artifact);
    }
    uint8_t prior_conformance_key[XR_CORE_IR_KEY_SIZE] = {0};
    for (uint64_t conformance = 0;
         conformance < conformance_count && section.status == XR_PROGRAM_DECODE_OK; ++conformance) {
        uint64_t id = take_uvar(&section);
        uint8_t key[XR_CORE_IR_KEY_SIZE] = {0};
        take_bytes(&section, key, sizeof(key));
        uint64_t implementor_type = take_uvar(&section);
        uint64_t implementor_kind = take_uvar(&section);
        uint64_t interface_id = take_uvar(&section);
        uint64_t slot_count = take_uvar(&section);
        if (id != conformance || !encoded_type_id_is_valid(implementor_type, dynamic_type_count) ||
            implementor_type < XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE ||
            implementor_kind == XR_CORE_IR_NOMINAL_NONE ||
            implementor_kind > XR_CORE_IR_NOMINAL_ENUM || interface_id >= interface_count ||
            slot_count == 0u || slot_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
            (conformance != 0u && memcmp(prior_conformance_key, key, sizeof(key)) >= 0) ||
            !count_records(&section, slot_count)) {
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            break;
        }
        memcpy(prior_conformance_key, key, sizeof(key));
        for (uint64_t slot = 0; slot < slot_count; ++slot)
            if (take_uvar(&section) >= function_count)
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
    }
    uint64_t count = take_uvar(&section);
    if (count != function_count || !count_records(&section, count)) {
        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
        return section_done(&section, artifact);
    }
    for (uint64_t function = 0; function < count && section.status == XR_PROGRAM_DECODE_OK;
         ++function) {
        uint64_t function_id = take_uvar(&section);
        uint64_t root_count = take_uvar(&section);
        if (function_id != function || root_count > XR_PROGRAM_LIMIT_ROOTS_PER_FUNCTION ||
            !count_records(&section, root_count)) {
            section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            break;
        }
        uint64_t prior_kind = 0u;
        uint64_t prior_payload = 0u;
        bool have_prior = false;
        for (uint64_t root = 0; root < root_count; ++root) {
            uint64_t root_id = take_uvar(&section);
            uint64_t kind = take_uvar(&section);
            uint64_t payload = kind == XR_CORE_IR_ROOT_PARAMETER || kind == XR_CORE_IR_ROOT_LOCAL
                                   ? take_uvar(&section)
                                   : 0u;
            if (root_id != root || kind > XR_CORE_IR_ROOT_LOCAL ||
                (have_prior &&
                 (kind < prior_kind || (kind == prior_kind && payload <= prior_payload))))
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            prior_kind = kind;
            prior_payload = payload;
            have_prior = true;
        }
        uint64_t value_count = take_uvar(&section);
        if (value_count > XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION ||
            !count_records(&section, value_count)) {
            section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
            break;
        }
        uint64_t prior_value = 0u;
        bool have_value = false;
        for (uint64_t value = 0; value < value_count; ++value) {
            uint64_t value_id = take_uvar(&section);
            uint64_t value_root_count = take_uvar(&section);
            if ((have_value && value_id <= prior_value) ||
                value_id >= XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION || value_root_count == 0u ||
                value_root_count > XR_PROGRAM_LIMIT_ROOTS_PER_VALUE ||
                !count_records(&section, value_root_count)) {
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                break;
            }
            uint64_t prior_root = 0u;
            for (uint64_t index = 0; index < value_root_count; ++index) {
                uint64_t root_id = take_uvar(&section);
                if (root_id >= root_count || (index != 0u && root_id <= prior_root))
                    section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                prior_root = root_id;
            }
            prior_value = value_id;
            have_value = true;
        }
    }
    *interface_count_out = interface_count;
    return section_done(&section, artifact);
}

static bool decode_code(Reader *artifact, const XrProgramSectionView *view, uint64_t function_count,
                        uint64_t constant_count, uint64_t dynamic_type_count) {
    Reader section = section_reader(artifact, view);
    uint64_t count = take_uvar(&section);
    if (count != function_count || !count_records(&section, count)) {
        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
        return section_done(&section, artifact);
    }
    for (uint64_t function = 0; function < count && section.status == XR_PROGRAM_DECODE_OK;
         ++function) {
        uint64_t function_id = take_uvar(&section);
        uint64_t block_count = take_uvar(&section);
        if (function_id != function || block_count == 0 ||
            block_count > XR_PROGRAM_LIMIT_BLOCKS_PER_FUNCTION ||
            !count_records(&section, block_count)) {
            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
            break;
        }
        for (uint64_t block = 0; block < block_count && section.status == XR_PROGRAM_DECODE_OK;
             ++block) {
            uint64_t block_id = take_uvar(&section);
            uint64_t argument_count = take_uvar(&section);
            if (block_id != block || argument_count > XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION ||
                !count_records(&section, argument_count)) {
                section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                break;
            }
            for (uint64_t argument = 0; argument < argument_count; ++argument) {
                (void) take_uvar(&section);
                uint64_t type_id = take_uvar(&section);
                uint64_t category = take_uvar(&section);
                uint64_t ownership = take_uvar(&section);
                if (!encoded_type_id_is_valid(type_id, dynamic_type_count) ||
                    category > XR_CORE_IR_PLACE || ownership > XR_CORE_IR_OWNER ||
                    (category == XR_CORE_IR_PLACE && ownership != XR_CORE_IR_NON_OWNER))
                    section.status = XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT;
            }
            uint64_t instruction_count = take_uvar(&section);
            if (instruction_count > XR_PROGRAM_LIMIT_OPERATIONS ||
                !count_operations(&section, instruction_count)) {
                section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
                break;
            }
            for (uint64_t instruction = 0;
                 instruction < instruction_count && section.status == XR_PROGRAM_DECODE_OK;
                 ++instruction) {
                uint64_t operation_id = take_uvar(&section);
                (void) take_uvar(&section); /* result id + 1 */
                uint64_t result_type = take_uvar(&section);
                uint64_t result_category = take_uvar(&section);
                uint64_t result_ownership = take_uvar(&section);
                uint64_t operand_count = take_uvar(&section);
                if (!xr_core_spec_operation_by_id((uint16_t) operation_id) ||
                    operation_id > UINT16_MAX ||
                    !encoded_type_id_is_valid(result_type, dynamic_type_count) ||
                    result_category > XR_CORE_IR_PLACE || result_ownership > XR_CORE_IR_OWNER ||
                    (result_category == XR_CORE_IR_PLACE &&
                     result_ownership != XR_CORE_IR_NON_OWNER) ||
                    operand_count > XR_PROGRAM_LIMIT_OPERANDS_PER_OPERATION ||
                    !count_records(&section, operand_count)) {
                    section.status = XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT;
                    break;
                }
                for (uint64_t operand = 0; operand < operand_count; ++operand)
                    (void) take_uvar(&section);
                uint64_t immediate_kind = take_uvar(&section);
                if (immediate_kind > XR_CORE_IR_IMMEDIATE_TYPE) {
                    section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                    break;
                }
                if (immediate_kind != XR_CORE_IR_IMMEDIATE_NONE) {
                    uint64_t immediate = take_uvar(&section);
                    if ((immediate_kind == XR_CORE_IR_IMMEDIATE_BOOL && immediate > 1u) ||
                        (immediate_kind == XR_CORE_IR_IMMEDIATE_CONSTANT &&
                         immediate >= constant_count) ||
                        (immediate_kind == XR_CORE_IR_IMMEDIATE_FUNCTION &&
                         immediate >= function_count) ||
                        (immediate_kind == XR_CORE_IR_IMMEDIATE_TYPE &&
                         !encoded_type_id_is_valid(immediate, dynamic_type_count)))
                        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                    if (immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT_FIELD) {
                        uint64_t field = take_uvar(&section);
                        if (field > UINT32_MAX)
                            section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                    }
                    if ((immediate_kind == XR_CORE_IR_IMMEDIATE_FIELD ||
                         immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT ||
                         immediate_kind == XR_CORE_IR_IMMEDIATE_VARIANT_FIELD) &&
                        immediate > UINT32_MAX)
                        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                }
                uint64_t successor_count = take_uvar(&section);
                if (successor_count > XR_PROGRAM_LIMIT_SUCCESSORS_PER_OPERATION ||
                    !count_records(&section, successor_count)) {
                    section.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
                    break;
                }
                for (uint64_t successor = 0; successor < successor_count; ++successor) {
                    if (take_uvar(&section) >= block_count)
                        section.status = XR_PROGRAM_DECODE_NONCANONICAL;
                }
            }
        }
    }
    return section_done(&section, artifact);
}

static bool decode_empty_section(Reader *artifact, const XrProgramSectionView *view) {
    Reader section = section_reader(artifact, view);
    if (take_uvar(&section) != 0u)
        section.status = XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT;
    return section_done(&section, artifact);
}

XrProgramDecodeBudget xr_program_decode_default_budget(void) {
    XrProgramDecodeBudget budget = {
        .max_bytes = XR_PROGRAM_LIMIT_ARTIFACT_BYTES,
        .max_records = XR_PROGRAM_LIMIT_VALUES_PER_FUNCTION,
        .max_operations = XR_PROGRAM_LIMIT_OPERATIONS,
    };
    return budget;
}

XrProgramDecodeStatus xr_program_decode_structure(const uint8_t *bytes, size_t size,
                                                  const XrProgramDecodeBudget *budget,
                                                  XrProgramView *view_out, char *diagnostic,
                                                  size_t diagnostic_size) {
    static const uint8_t expected_magic[XR_PROGRAM_MAGIC_SIZE] = XR_PROGRAM_MAGIC_BYTES;
    XrProgramDecodeBudget selected = budget ? *budget : xr_program_decode_default_budget();
    Reader reader = {
        .bytes = bytes, .size = size, .budget = selected, .status = XR_PROGRAM_DECODE_OK};
    XrProgramView view = {0};
    uint8_t magic[XR_PROGRAM_MAGIC_SIZE] = {0};
    if (view_out)
        memset(view_out, 0, sizeof(*view_out));
    if (diagnostic && diagnostic_size != 0)
        diagnostic[0] = '\0';
    if (!bytes || !view_out || selected.max_bytes == 0 || selected.max_records == 0 ||
        selected.max_operations == 0) {
        reader.status = XR_PROGRAM_DECODE_INVALID_INPUT;
        goto done;
    }
    if (size > selected.max_bytes || size > XR_PROGRAM_LIMIT_ARTIFACT_BYTES) {
        reader.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        goto done;
    }
    take_bytes(&reader, magic, sizeof(magic));
    if (reader.status == XR_PROGRAM_DECODE_OK &&
        memcmp(magic, expected_magic, sizeof(magic)) != 0) {
        reader.status = XR_PROGRAM_DECODE_BAD_MAGIC;
        goto done;
    }
    view.format_major = take_u16(&reader);
    view.format_minor = take_u16(&reader);
    if (reader.status == XR_PROGRAM_DECODE_OK && (view.format_major != XR_PROGRAM_FORMAT_MAJOR ||
                                                  view.format_minor > XR_PROGRAM_FORMAT_MINOR)) {
        reader.status = XR_PROGRAM_DECODE_UNSUPPORTED_VERSION;
        goto done;
    }
    uint64_t epoch = take_uvar(&reader);
    if (epoch == 0 || epoch > XR_CORE_SPEC_EPOCH || epoch > UINT32_MAX) {
        reader.status = XR_PROGRAM_DECODE_UNSUPPORTED_FEATURE;
        goto done;
    }
    view.core_spec_epoch = (uint32_t) epoch;
    take_bytes(&reader, view.core_spec_fingerprint, sizeof(view.core_spec_fingerprint));
    take_bytes(&reader, view.semantic_profile_fingerprint,
               sizeof(view.semantic_profile_fingerprint));
    view.required_feature_count = take_uvar(&reader);
    if (view.required_feature_count == 0 ||
        view.required_feature_count > XR_PROGRAM_LIMIT_FEATURES ||
        !count_records(&reader, view.required_feature_count)) {
        reader.status = XR_PROGRAM_DECODE_RESOURCE_LIMIT;
        goto done;
    }
    uint64_t previous_feature = 0;
    for (uint64_t index = 0;
         index < view.required_feature_count && reader.status == XR_PROGRAM_DECODE_OK; ++index) {
        uint64_t feature = take_uvar(&reader);
        if (feature <= previous_feature || feature > UINT16_MAX) {
            reader.status = XR_PROGRAM_DECODE_NONCANONICAL;
            break;
        }
        if (!xr_core_spec_feature_active((uint16_t) feature)) {
            reader.status = XR_PROGRAM_DECODE_UNSUPPORTED_FEATURE;
            break;
        }
        previous_feature = feature;
    }
    if (reader.status != XR_PROGRAM_DECODE_OK)
        goto done;
    uint64_t section_count = take_uvar(&reader);
    if (section_count < XR_PROGRAM_REQUIRED_SECTION_COUNT ||
        section_count > XR_PROGRAM_SECTION_COUNT) {
        reader.status = XR_PROGRAM_DECODE_INVALID_SECTION;
        goto done;
    }
    view.section_count = (uint32_t) section_count;
    uint64_t next_relative_offset = 0;
    for (uint32_t index = 0; index < view.section_count; ++index) {
        uint64_t stable_id = take_uvar(&reader);
        uint64_t relative_offset = take_uvar(&reader);
        uint64_t section_size = take_uvar(&reader);
        if (stable_id != (uint64_t) index + 1u || relative_offset != next_relative_offset ||
            stable_id > UINT16_MAX || section_size > SIZE_MAX - relative_offset) {
            reader.status = XR_PROGRAM_DECODE_NONCANONICAL;
            goto done;
        }
        view.sections[index].stable_id = (uint16_t) stable_id;
        view.sections[index].offset = relative_offset;
        view.sections[index].size = section_size;
        next_relative_offset += section_size;
    }
    view.payload_offset = reader.offset;
    if (view.payload_offset > size || next_relative_offset != size - view.payload_offset) {
        reader.status = XR_PROGRAM_DECODE_INVALID_SECTION;
        goto done;
    }
    for (uint32_t index = 0; index < view.section_count; ++index)
        view.sections[index].offset += view.payload_offset;

    uint64_t constant_count = 0;
    uint64_t function_count = 0;
    uint64_t dynamic_type_count = 0;
    uint64_t signature_count = 0;
    uint64_t signature_reference_count = 0;
    uint64_t interface_count = 0;
    uint64_t interface_reference_count = 0;
    if (!decode_types(&reader, &view.sections[0], &dynamic_type_count, &signature_reference_count,
                      &interface_reference_count) ||
        !decode_constants(&reader, &view.sections[1], &constant_count) ||
        !decode_functions(&reader, &view.sections[2], dynamic_type_count, &signature_count,
                          &function_count) ||
        !decode_code(&reader, &view.sections[3], function_count, constant_count,
                     dynamic_type_count) ||
        !decode_empty_section(&reader, &view.sections[4]) ||
        !decode_empty_section(&reader, &view.sections[5]) ||
        !decode_semantic_metadata(&reader, &view.sections[6], function_count, signature_count,
                                  dynamic_type_count, &interface_count))
        goto done;
    if (signature_reference_count > signature_count ||
        interface_reference_count > interface_count) {
        reader.status = XR_PROGRAM_DECODE_NONCANONICAL;
        goto done;
    }
    if (view.section_count == XR_PROGRAM_SECTION_COUNT && view.sections[7].size != 32u) {
        reader.status = XR_PROGRAM_DECODE_NONCANONICAL;
        goto done;
    }
    view.artifact = bytes;
    view.artifact_size = size;
    xr_program_compute_id(bytes, size, &view.id);
    *view_out = view;

done:
    if (reader.status != XR_PROGRAM_DECODE_OK)
        xr_program_set_diagnostic(diagnostic, diagnostic_size, "XrProgram decode failed: %s",
                                  xr_program_decode_status_name(reader.status));
    return reader.status;
}

XrProgramDecodeStatus xr_program_reencode(const XrProgramView *view,
                                          XrProgramArtifact *artifact_out, char *diagnostic,
                                          size_t diagnostic_size) {
    XrProgramView checked;
    if (artifact_out)
        memset(artifact_out, 0, sizeof(*artifact_out));
    if (!view || !artifact_out || !view->artifact) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "XrProgram reencode requires a decoded view");
        return XR_PROGRAM_DECODE_INVALID_INPUT;
    }
    XrProgramDecodeStatus status = xr_program_decode_structure(
        view->artifact, view->artifact_size, NULL, &checked, diagnostic, diagnostic_size);
    if (status != XR_PROGRAM_DECODE_OK)
        return status;
    uint8_t *copy = xr_malloc(view->artifact_size);
    if (!copy) {
        xr_program_set_diagnostic(diagnostic, diagnostic_size,
                                  "XrProgram reencode allocation failed");
        return XR_PROGRAM_DECODE_OUT_OF_MEMORY;
    }
    memcpy(copy, view->artifact, view->artifact_size);
    artifact_out->bytes = copy;
    artifact_out->size = view->artifact_size;
    artifact_out->id = checked.id;
    return XR_PROGRAM_DECODE_OK;
}

const char *xr_program_decode_status_name(XrProgramDecodeStatus status) {
    switch (status) {
        case XR_PROGRAM_DECODE_OK:
            return "ok";
        case XR_PROGRAM_DECODE_INVALID_INPUT:
            return "invalid-input";
        case XR_PROGRAM_DECODE_TRUNCATED:
            return "truncated";
        case XR_PROGRAM_DECODE_BAD_MAGIC:
            return "bad-magic";
        case XR_PROGRAM_DECODE_UNSUPPORTED_VERSION:
            return "unsupported-version";
        case XR_PROGRAM_DECODE_UNSUPPORTED_FEATURE:
            return "unsupported-feature";
        case XR_PROGRAM_DECODE_NONCANONICAL:
            return "noncanonical";
        case XR_PROGRAM_DECODE_RESOURCE_LIMIT:
            return "resource-limit";
        case XR_PROGRAM_DECODE_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_PROGRAM_DECODE_INVALID_SECTION:
            return "invalid-section";
        case XR_PROGRAM_DECODE_UNSUPPORTED_SECTION_CONTENT:
            return "unsupported-section-content";
        default:
            return "unknown";
    }
}
