/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_error_messages.h - Runtime-neutral user-visible error message constants.
 */

#ifndef XR_ERROR_MESSAGES_H
#define XR_ERROR_MESSAGES_H

#define XR_ERROR_CORE_DIVISION_BY_ZERO_MSG "division by zero"
#define XR_ERROR_CORE_MODULO_BY_ZERO_MSG "modulo by zero"
#define XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG "modulo requires integer types"
#define XR_ERROR_CORE_NUMERIC_CONVERSION_RANGE_MSG "numeric conversion is out of range"
/* Spec 13.2 -- int(s) / float(s) throw when the string is not a whole decimal
 * number. Shared verbatim by the VM, hosted AOT and freestanding AOT so the
 * same input reports the same text on every profile. */
#define XR_ERROR_CORE_I64_PARSE_MSG "i64.parse expects a decimal integer"
#define XR_ERROR_CORE_F64_PARSE_MSG "f64.parse expects a decimal number"
#define XR_ERROR_CORE_BYTE_ARRAY_CONSTRUCTOR_EXPECTS_MSG                                           \
    "Array<u8>(length, fill) expects integers"
#define XR_ERROR_CORE_BYTE_ARRAY_CONSTRUCTOR_FILL_EXPECTS_MSG                                      \
    "Array<u8>(length, fill): both args must be integers"
#define XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG "slice bounds must be integers"
#define XR_ERROR_CORE_ARRAY_CAPACITY_EXPECTS_MSG "Array capacity must be an integer"
#define XR_ERROR_CORE_ARRAY_RESERVE_EXPECTS_MSG "Array.reserve(capacity) expects an integer"
#define XR_ERROR_CORE_ARRAY_RESERVE_FAILED_MSG "Array.reserve failed"
#define XR_ERROR_CORE_ARRAY_RESIZE_EXPECTS_MSG "Array.resize(length, fill) expects integer length"
#define XR_ERROR_CORE_ARRAY_RESIZE_REQUIRES_FILL_MSG                                               \
    "Array.resize(length, fill) requires fill value"
#define XR_ERROR_CORE_ARRAY_RESIZE_FAILED_MSG "Array.resize failed"
#define XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG "cannot push to array slice"
#define XR_ERROR_CORE_BYTE_SLICE_ENDIAN_EXPECTS_MSG "Slice<u8> load/store expects Endian"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG                                           \
    "Slice<u8>.load<T>() expects integer offset"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_EXPECTS_MSG                                              \
    "Slice<u8>.load<u16>(offset) expects Slice<u8> and integer"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG                                             \
    "Slice<u8>.load<u16>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG "Slice<u8>.load<u16>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_EXPECTS_MSG                                              \
    "Slice<u8>.load<u32>(offset) expects Slice<u8> and integer"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG                                             \
    "Slice<u8>.load<u32>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG "Slice<u8>.load<u32>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_EXPECTS_MSG                                              \
    "Slice<u8>.load<u64>(offset) expects Slice<u8> and integer"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG                                             \
    "Slice<u8>.load<u64>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG "Slice<u8>.load<u64>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_RECEIVER_MSG                                             \
    "Slice<u8>.load<f32>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_OOB_MSG "Slice<u8>.load<f32>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_RECEIVER_MSG                                             \
    "Slice<u8>.load<f64>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_OOB_MSG "Slice<u8>.load<f64>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG                                           \
    "Slice<u8>.store<T>() expects integer offset and value"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_FLOAT_VALUE_EXPECTS_MSG                                     \
    "Slice<u8>.store<T>() expects integer offset and float value"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U16_RECEIVER_MSG                                            \
    "Slice<u8>.store<u16>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U16_OOB_MSG "Slice<u8>.store<u16>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U32_RECEIVER_MSG                                            \
    "Slice<u8>.store<u32>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U32_OOB_MSG "Slice<u8>.store<u32>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U64_RECEIVER_MSG                                            \
    "Slice<u8>.store<u64>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_U64_OOB_MSG "Slice<u8>.store<u64>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_F32_RECEIVER_MSG                                            \
    "Slice<u8>.store<f32>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_F32_OOB_MSG "Slice<u8>.store<f32>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_F64_RECEIVER_MSG                                            \
    "Slice<u8>.store<f64>() receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_STORE_F64_OOB_MSG "Slice<u8>.store<f64>() offset out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG "cannot write through readonly Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_FILL_RECEIVER_MSG "Slice<u8>.fill(value) expects Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_FILL_VALUE_EXPECTS_MSG                                            \
    "Slice<u8>.fill(value) expects integer byte value"
#define XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG "Slice<u8>.fill(value) range out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_COPY_RECEIVER_MSG                                                 \
    "Slice<u8>.copyFrom(src) receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COPY_SOURCE_MSG                                                   \
    "Slice<u8>.copyFrom(src) source must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG "Slice<u8>.copyFrom(src) range out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_COMPARE_RECEIVER_MSG                                              \
    "Slice<u8>.compare(other) receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COMPARE_OPERAND_MSG                                               \
    "Slice<u8>.compare(other) operand must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG "Slice<u8>.compare(other) span has no data"
#define XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_RECEIVER_MSG                                        \
    "Slice<u8>.commonPrefix(other) receiver must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_OPERAND_MSG                                         \
    "Slice<u8>.commonPrefix(other) operand must be Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG                                         \
    "Slice<u8>.commonPrefix(other) span has no data"
#define XR_ERROR_CORE_BYTE_SLICE_REPEAT_RECEIVER_MSG                                               \
    "Slice<u8>.repeatFrom(dstOffset, distance, count) expects Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_REPEAT_INTS_EXPECTS_MSG                                           \
    "Slice<u8>.repeatFrom(dstOffset, distance, count) expects integers"
#define XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG                                                    \
    "Slice<u8>.repeatFrom(dstOffset, distance, count) range out of bounds"
#define XR_ERROR_CORE_BYTE_SLICE_ARG_EXPECTS_MSG                                                   \
    "Slice<u8> argument expects Array<u8> or Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISSING_METADATA_MSG                                  \
    "Slice<u8>.reinterpret<T>() missing metadata"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG                                      \
    "Slice<u8>.reinterpret<T>() requires POD target type"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG                                 \
    "Slice<u8>.reinterpret<T>() target metadata mismatch"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_EXPECTS_MSG                                           \
    "Slice<u8>.reinterpret<T>() expects Slice<u8>"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_RECEIVER_MSG                                          \
    "Slice<u8>.reinterpret<T>() expects Slice<u8> receiver"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG                                          \
    "Slice<u8>.reinterpret<T>() byte length overflow"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG                                         \
    "Slice<u8>.reinterpret<T>() length is not divisible by target size"
#define XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISALIGNED_MSG                                        \
    "Slice<u8>.reinterpret<T>() source address is not aligned for target type"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_EXPECTS_MSG                                           \
    "Array<u8> copy-within expects integer offsets and count"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_RECEIVER_MSG                                          \
    "Array<u8> copy-within receiver must be Array<u8>"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_OOB_MSG "Array<u8> copy-within range out of bounds"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_EXPECTS_MSG                                             \
    "Array<u8> copy range expects Array<u8> operands and integer ranges"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OPERANDS_MSG                                            \
    "Array<u8> copy range operands must be Array<u8>"
#define XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OOB_MSG "Array<u8> copy range out of bounds"
#define XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_EXPECTS_MSG                                           \
    "Array<u8>.appendFrom(src) expects Slice<u8>"
#define XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG                                          \
    "Array<u8>.appendFrom receiver/source must use byte storage"
#define XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG "Array<u8>.appendFrom range/grow failed"
#define XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_EXPECTS_MSG                                           \
    "Array<u8>.repeatFrom(distance, count) expects integers"
#define XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG                                          \
    "Array<u8>.repeatFrom receiver must be Array<u8>"
#define XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG "Array<u8>.repeatFrom range/grow failed"
#define XR_ERROR_CORE_RANGE_TO_ARRAY_TOO_LARGE_MSG "Range.toArray range too large"
#define XR_ERROR_CORE_ENUM_VARIANT_INDEX_OOB_MSG "enum variant index out of bounds"
#define XR_ERROR_CORE_ENUM_PAYLOAD_INDEX_OOB_MSG "enum payload field index out of bounds"
#define XR_ERROR_CORE_ITERATOR_EXHAUSTED_NEXT_MSG                                                  \
    "Iterator.next() on an exhausted iterator: every next() must follow a hasNext() that "         \
    "returned true"
#define XR_ERROR_CORE_ITERATOR_EXHAUSTED_NTH_MSG                                                   \
    "Iterator.nth(index) ran past the end: the iterator was exhausted before reaching index"

/* Spec 8.3.1 rule D3 — an error escaped a `defer` body. Shared verbatim by the
 * VM and AOT so both backends emit identical text, as the rule requires. */
#define XR_ERROR_CORE_DEFER_THROW_MSG "error escaped a defer body"
#define XR_ERROR_CORE_DEFER_THROW_IN_FLIGHT_MSG "in-flight error at the time of cleanup"
#define XR_ERROR_CORE_DEFER_THROW_HINT_MSG                                                         \
    "a defer body must not let errors escape (spec 8.3.1); this is not catchable"
#define XR_ERROR_CORE_NO_MESSAGE_MSG "<no message>"

#endif  // XR_ERROR_MESSAGES_H
