/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_instruction_stream.h - Canonical compact XTP instruction stream
 *
 * KEY CONCEPT:
 *   Compact tokens are a wire policy only. Iteration always yields the exact
 *   canonical TargetPlan instruction rows consumed by verification and the VM.
 */

#ifndef XR_XTP_INSTRUCTION_STREAM_H
#define XR_XTP_INSTRUCTION_STREAM_H

#include "../target/xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_XTP_INSTRUCTION_TOKEN_PRIMITIVE UINT8_C(0)

typedef struct XrXtpInstructionStream {
    const uint8_t *bytes;
    size_t length;
    size_t cursor;
    uint32_t expected_count;
    uint32_t expanded_count;
    uint32_t previous_function;
    XrTargetInstructionRecord previous;
    XrTargetInstructionRecord pending;
    bool has_previous;
    bool previous_was_primitive;
    bool has_pending;
    bool failed;
} XrXtpInstructionStream;

XR_FUNC bool xr_xtp_instruction_stream_size(const XrTargetInstructionRecord *rows,
                                             uint32_t count, size_t *size);
XR_FUNC bool xr_xtp_instruction_stream_encode(const XrTargetInstructionRecord *rows,
                                               uint32_t count, uint8_t *bytes,
                                               size_t capacity, size_t *written);
XR_FUNC bool xr_xtp_instruction_stream_init(XrXtpInstructionStream *stream,
                                             const uint8_t *bytes, size_t length,
                                             uint32_t expanded_count);
XR_FUNC bool xr_xtp_instruction_stream_next(XrXtpInstructionStream *stream,
                                             XrTargetInstructionRecord *row);
XR_FUNC bool xr_xtp_instruction_stream_finish(const XrXtpInstructionStream *stream);
XR_FUNC bool xr_xtp_instruction_stream_validate(const uint8_t *bytes, size_t length,
                                                 uint32_t expanded_count,
                                                 uint64_t *decode_work_units);
XR_FUNC bool xr_xtp_instruction_stream_decode(const uint8_t *bytes, size_t length,
                                               uint32_t expanded_count,
                                               XrTargetInstructionRecord *rows);

#endif  // XR_XTP_INSTRUCTION_STREAM_H
