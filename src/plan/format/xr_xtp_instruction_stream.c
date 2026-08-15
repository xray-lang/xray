/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_instruction_stream.c - Canonical compact XTP instruction stream
 */

#include "xr_xtp_instruction_stream.h"
#include <limits.h>
#include <string.h>

typedef struct XrXtpStreamWriter {
    uint8_t *bytes;
    size_t capacity;
    size_t cursor;
    bool failed;
} XrXtpStreamWriter;

typedef enum XrXtpSuperToken {
    XR_XTP_SUPER_TOKEN_INVALID = 0,
#define XR_XTP_SUPER_OP(name, token, first, second) XR_XTP_SUPER_TOKEN_##name = token,
#include "xtp_super_ops.def"
#undef XR_XTP_SUPER_OP
    XR_XTP_SUPER_TOKEN_COUNT,
} XrXtpSuperToken;

static bool write_byte(XrXtpStreamWriter *writer, uint8_t value) {
    if (writer->cursor == SIZE_MAX ||
        (writer->bytes && writer->cursor >= writer->capacity)) {
        writer->failed = true;
        return false;
    }
    if (writer->bytes)
        writer->bytes[writer->cursor] = value;
    writer->cursor++;
    return true;
}

static bool write_uleb(XrXtpStreamWriter *writer, uint64_t value) {
    do {
        uint8_t byte = (uint8_t) (value & UINT64_C(0x7f));
        value >>= 7;
        if (value)
            byte |= UINT8_C(0x80);
        if (!write_byte(writer, byte))
            return false;
    } while (value);
    return true;
}

static bool read_uleb(XrXtpInstructionStream *stream, uint64_t *value) {
    uint64_t result = 0;
    for (uint32_t index = 0; index < 10u; index++) {
        if (stream->cursor >= stream->length)
            return false;
        uint8_t byte = stream->bytes[stream->cursor++];
        if (index == 9u && (byte & UINT8_C(0xfe)) != 0)
            return false;
        result |= (uint64_t) (byte & UINT8_C(0x7f)) << (index * 7u);
        if ((byte & UINT8_C(0x80)) == 0) {
            if (index != 0 && byte == 0)
                return false;
            *value = result;
            return true;
        }
    }
    return false;
}

static uint64_t zigzag_encode(uint64_t bits) {
    uint64_t sign = (bits & (UINT64_C(1) << 63)) ? UINT64_MAX : 0;
    return (bits << 1) ^ sign;
}

static uint64_t zigzag_decode(uint64_t value) {
    return (value >> 1) ^ (UINT64_C(0) - (value & UINT64_C(1)));
}

static uint8_t super_token_for(uint16_t first, uint16_t second) {
#define XR_XTP_SUPER_OP(name, token, first_name, second_name)                                      \
    if (first == XR_TARGET_INSTRUCTION_##first_name &&                                             \
        second == XR_TARGET_INSTRUCTION_##second_name)                                             \
        return (uint8_t) (token);
#include "xtp_super_ops.def"
#undef XR_XTP_SUPER_OP
    return XR_XTP_SUPER_TOKEN_INVALID;
}

static uint16_t super_first_opcode(uint8_t token) {
    switch (token) {
#define XR_XTP_SUPER_OP(name, token_value, first, second)                                          \
    case token_value:                                                                              \
        return XR_TARGET_INSTRUCTION_##first;
#include "xtp_super_ops.def"
#undef XR_XTP_SUPER_OP
        default:
            return XR_TARGET_INSTRUCTION_INVALID;
    }
}

static uint16_t super_second_opcode(uint8_t token) {
    switch (token) {
#define XR_XTP_SUPER_OP(name, token_value, first, second)                                         \
    case token_value:                                                                             \
        return XR_TARGET_INSTRUCTION_##second;
#include "xtp_super_ops.def"
#undef XR_XTP_SUPER_OP
        default:
            return XR_TARGET_INSTRUCTION_INVALID;
    }
}

static bool is_return_pair(const XrTargetInstructionRecord *first,
                           const XrTargetInstructionRecord *second) {
    if (!first || !second || first->id == UINT32_MAX || second->id != first->id + 1u ||
        first->function != second->function ||
        !super_token_for(first->opcode, second->opcode))
        return false;
    return first->result_slot != XR_TARGET_INSTRUCTION_SLOT_NONE &&
           second->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
           second->operand_slots[0] == first->result_slot &&
           second->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
           second->operand_count == 1u && second->immediate_bits == 0 &&
           second->reserved == 0;
}

static bool write_slot(XrXtpStreamWriter *writer, uint32_t slot) {
    uint64_t encoded = slot == XR_TARGET_INSTRUCTION_SLOT_NONE
                           ? 0
                           : (uint64_t) slot + UINT64_C(1);
    return write_uleb(writer, encoded);
}

static bool write_payload(XrXtpStreamWriter *writer,
                          const XrTargetInstructionRecord *row,
                          uint32_t *previous_function) {
    const XrTargetInstructionContract *contract =
        xr_target_instruction_contract(row->opcode);
    if (!contract || !contract->name || row->function < *previous_function ||
        row->operand_count != contract->arity || row->reserved != 0)
        return false;
    uint32_t function_delta = row->function - *previous_function;
    if (!write_uleb(writer, function_delta))
        return false;
    *previous_function = row->function;
    if (contract->result_rep != XR_TARGET_INSTRUCTION_REP_NONE) {
        if (!write_slot(writer, row->result_slot))
            return false;
    } else if (row->result_slot != XR_TARGET_INSTRUCTION_SLOT_NONE) {
        return false;
    }
    for (uint32_t operand = 0; operand < contract->arity; operand++)
        if (!write_slot(writer, row->operand_slots[operand]))
            return false;
    for (uint32_t operand = contract->arity; operand < 2u; operand++)
        if (row->operand_slots[operand] != XR_TARGET_INSTRUCTION_SLOT_NONE)
            return false;
    if (contract->immediate_kind == XR_TARGET_INSTRUCTION_IMMEDIATE_I64)
        return write_uleb(writer, zigzag_encode(row->immediate_bits));
    if (contract->immediate_kind != XR_TARGET_INSTRUCTION_IMMEDIATE_NONE)
        return write_uleb(writer, row->immediate_bits);
    return row->immediate_bits == 0;
}

static bool encode_stream(const XrTargetInstructionRecord *rows, uint32_t count,
                          uint8_t *bytes, size_t capacity, size_t *written) {
    if (written)
        *written = 0;
    if (!written || (count && !rows))
        return false;
    XrXtpStreamWriter writer = {bytes, capacity, 0, false};
    uint32_t previous_function = 0;
    for (uint32_t index = 0; index < count;) {
        const XrTargetInstructionRecord *row = &rows[index];
        if (row->id != index)
            return false;
        bool super = index + 1u < count && is_return_pair(row, &rows[index + 1u]);
        uint8_t token = super ? super_token_for(row->opcode, rows[index + 1u].opcode)
                              : XR_XTP_INSTRUCTION_TOKEN_PRIMITIVE;
        if (!write_byte(&writer, token))
            return false;
        if (!super && !write_uleb(&writer, row->opcode))
            return false;
        if (!write_payload(&writer, row, &previous_function))
            return false;
        index += super ? 2u : 1u;
    }
    if (writer.failed)
        return false;
    *written = writer.cursor;
    return true;
}

XR_FUNC bool xr_xtp_instruction_stream_size(const XrTargetInstructionRecord *rows,
                                             uint32_t count, size_t *size) {
    return encode_stream(rows, count, NULL, 0, size);
}

XR_FUNC bool xr_xtp_instruction_stream_encode(const XrTargetInstructionRecord *rows,
                                               uint32_t count, uint8_t *bytes,
                                               size_t capacity, size_t *written) {
    return encode_stream(rows, count, bytes, capacity, written);
}

static bool read_slot(XrXtpInstructionStream *stream, uint32_t *slot) {
    uint64_t encoded = 0;
    if (!read_uleb(stream, &encoded) || encoded > UINT32_MAX)
        return false;
    *slot = encoded == 0 ? XR_TARGET_INSTRUCTION_SLOT_NONE : (uint32_t) (encoded - 1u);
    return true;
}

static bool read_payload(XrXtpInstructionStream *stream, uint16_t opcode,
                         XrTargetInstructionRecord *row) {
    const XrTargetInstructionContract *contract = xr_target_instruction_contract(opcode);
    uint64_t value = 0;
    if (!contract || !contract->name || !read_uleb(stream, &value) || value > UINT32_MAX ||
        stream->previous_function > UINT32_MAX - (uint32_t) value)
        return false;
    row->id = stream->expanded_count;
    row->function = stream->previous_function + (uint32_t) value;
    row->result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    row->operand_slots[0] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    row->operand_slots[1] = XR_TARGET_INSTRUCTION_SLOT_NONE;
    row->opcode = opcode;
    row->operand_count = contract->arity;
    row->reserved = 0;
    if (contract->result_rep != XR_TARGET_INSTRUCTION_REP_NONE &&
        !read_slot(stream, &row->result_slot))
        return false;
    for (uint32_t operand = 0; operand < contract->arity; operand++)
        if (!read_slot(stream, &row->operand_slots[operand]))
            return false;
    row->immediate_bits = 0;
    if (contract->immediate_kind != XR_TARGET_INSTRUCTION_IMMEDIATE_NONE) {
        if (!read_uleb(stream, &value))
            return false;
        row->immediate_bits = contract->immediate_kind == XR_TARGET_INSTRUCTION_IMMEDIATE_I64
                                  ? zigzag_decode(value)
                                  : value;
    }
    stream->previous_function = row->function;
    return true;
}

XR_FUNC bool xr_xtp_instruction_stream_init(XrXtpInstructionStream *stream,
                                             const uint8_t *bytes, size_t length,
                                             uint32_t expanded_count) {
    if (!stream || (length && !bytes))
        return false;
    memset(stream, 0, sizeof(*stream));
    stream->bytes = bytes;
    stream->length = length;
    stream->expected_count = expanded_count;
    return true;
}

XR_FUNC bool xr_xtp_instruction_stream_next(XrXtpInstructionStream *stream,
                                             XrTargetInstructionRecord *row) {
    if (!stream || !row || stream->failed ||
        stream->expanded_count >= stream->expected_count)
        return false;
    bool primitive = false;
    if (stream->has_pending) {
        *row = stream->pending;
        stream->has_pending = false;
    } else {
        if (stream->cursor >= stream->length) {
            stream->failed = true;
            return false;
        }
        uint8_t token = stream->bytes[stream->cursor++];
        uint16_t opcode = XR_TARGET_INSTRUCTION_INVALID;
        primitive = token == XR_XTP_INSTRUCTION_TOKEN_PRIMITIVE;
        if (primitive) {
            uint64_t encoded_opcode = 0;
            if (!read_uleb(stream, &encoded_opcode) || encoded_opcode > UINT16_MAX)
                opcode = XR_TARGET_INSTRUCTION_INVALID;
            else
                opcode = (uint16_t) encoded_opcode;
        } else {
            opcode = super_first_opcode(token);
        }
        if (opcode == XR_TARGET_INSTRUCTION_INVALID || !read_payload(stream, opcode, row)) {
            stream->failed = true;
            return false;
        }
        if (!primitive) {
            uint16_t second_opcode = super_second_opcode(token);
            if (stream->expanded_count + 1u >= stream->expected_count ||
                row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE ||
                second_opcode == XR_TARGET_INSTRUCTION_INVALID) {
                stream->failed = true;
                return false;
            }
            stream->pending = (XrTargetInstructionRecord) {
                .id = row->id + 1u,
                .function = row->function,
                .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
                .operand_slots = {row->result_slot, XR_TARGET_INSTRUCTION_SLOT_NONE},
                .immediate_bits = 0,
                .opcode = second_opcode,
                .operand_count = 1,
                .reserved = 0,
            };
            stream->has_pending = true;
        }
    }
    if (primitive && stream->has_previous && stream->previous_was_primitive &&
        is_return_pair(&stream->previous, row)) {
        stream->failed = true;
        return false;
    }
    stream->previous = *row;
    stream->has_previous = true;
    stream->previous_was_primitive = primitive;
    stream->expanded_count++;
    return true;
}

XR_FUNC bool xr_xtp_instruction_stream_finish(const XrXtpInstructionStream *stream) {
    return stream && !stream->failed && !stream->has_pending &&
           stream->expanded_count == stream->expected_count &&
           stream->cursor == stream->length;
}

XR_FUNC bool xr_xtp_instruction_stream_validate(const uint8_t *bytes, size_t length,
                                                 uint32_t expanded_count,
                                                 uint64_t *decode_work_units) {
    if (decode_work_units)
        *decode_work_units = 0;
    if (!decode_work_units || length > UINT64_MAX - expanded_count)
        return false;
    XrXtpInstructionStream stream;
    XrTargetInstructionRecord row;
    if (!xr_xtp_instruction_stream_init(&stream, bytes, length, expanded_count))
        return false;
    while (stream.expanded_count < stream.expected_count)
        if (!xr_xtp_instruction_stream_next(&stream, &row))
            return false;
    if (!xr_xtp_instruction_stream_finish(&stream))
        return false;
    *decode_work_units = (uint64_t) length + expanded_count;
    return true;
}

XR_FUNC bool xr_xtp_instruction_stream_decode(const uint8_t *bytes, size_t length,
                                               uint32_t expanded_count,
                                               XrTargetInstructionRecord *rows) {
    if (expanded_count && !rows)
        return false;
    XrXtpInstructionStream stream;
    if (!xr_xtp_instruction_stream_init(&stream, bytes, length, expanded_count))
        return false;
    for (uint32_t index = 0; index < expanded_count; index++)
        if (!xr_xtp_instruction_stream_next(&stream, &rows[index]))
            return false;
    return xr_xtp_instruction_stream_finish(&stream);
}
