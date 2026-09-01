/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_verify.h - Semantic admission and immutable validated program
 */

#ifndef XR_PROGRAM_VERIFY_H
#define XR_PROGRAM_VERIFY_H

#include "xr_program_decode.h"

#define XR_PROGRAM_LOCATION_NONE UINT32_MAX
#define XR_PROGRAM_VERIFIER_VERSION 1u

typedef enum XrProgramDiagnosticKind {
    XR_PROGRAM_DIAGNOSTIC_NONE = 0,
    XR_PROGRAM_DIAGNOSTIC_STRUCTURAL,
    XR_PROGRAM_DIAGNOSTIC_RESOURCE_LIMIT,
    XR_PROGRAM_DIAGNOSTIC_OUT_OF_MEMORY,
    XR_PROGRAM_DIAGNOSTIC_CORE_SPEC_IDENTITY,
    XR_PROGRAM_DIAGNOSTIC_TYPE,
    XR_PROGRAM_DIAGNOSTIC_FUNCTION,
    XR_PROGRAM_DIAGNOSTIC_VALUE_DEFINITION,
    XR_PROGRAM_DIAGNOSTIC_VALUE_USE,
    XR_PROGRAM_DIAGNOSTIC_CONTROL_FLOW,
    XR_PROGRAM_DIAGNOSTIC_OPERATION_ARITY,
    XR_PROGRAM_DIAGNOSTIC_OPERATION_TYPE,
    XR_PROGRAM_DIAGNOSTIC_OPERATION_IMMEDIATE,
    XR_PROGRAM_DIAGNOSTIC_EFFECT,
    XR_PROGRAM_DIAGNOSTIC_CAPABILITY,
    XR_PROGRAM_DIAGNOSTIC_ENTRY_POINT,
} XrProgramDiagnosticKind;

typedef struct XrProgramSemanticLocation {
    uint16_t section_id;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
    uint32_t value_id;
} XrProgramSemanticLocation;

typedef struct XrProgramDiagnostic {
    XrProgramDiagnosticKind kind;
    XrProgramDecodeStatus decode_status;
    XrProgramSemanticLocation location;
} XrProgramDiagnostic;

typedef struct XrProgramVerifyBudget {
    XrProgramDecodeBudget decode;
    uint64_t max_work;
    uint32_t max_functions;
    uint32_t max_blocks_per_function;
    uint32_t max_values_per_function;
    uint32_t max_operations;
} XrProgramVerifyBudget;

typedef enum XrProgramVerifyStatus {
    XR_PROGRAM_VERIFY_OK = 0,
    XR_PROGRAM_VERIFY_INVALID_INPUT,
    XR_PROGRAM_VERIFY_STRUCTURAL_REJECTED,
    XR_PROGRAM_VERIFY_SEMANTIC_REJECTED,
    XR_PROGRAM_VERIFY_RESOURCE_LIMIT,
    XR_PROGRAM_VERIFY_OUT_OF_MEMORY,
} XrProgramVerifyStatus;

typedef struct XrValidatedProgram XrValidatedProgram;

XR_FUNC XrProgramVerifyBudget xr_program_verify_default_budget(void);
XR_FUNC XrProgramVerifyStatus xr_program_validate(const uint8_t *bytes, size_t size,
                                                  const XrProgramVerifyBudget *budget,
                                                  XrValidatedProgram **program_out,
                                                  XrProgramDiagnostic *diagnostic_out);
XR_FUNC XrValidatedProgram *xr_validated_program_retain(XrValidatedProgram *program);
XR_FUNC void xr_validated_program_free(XrValidatedProgram *program);
XR_FUNC XrProgramId xr_validated_program_id(const XrValidatedProgram *program);
XR_FUNC uint32_t xr_validated_program_function_count(const XrValidatedProgram *program);
XR_FUNC uint32_t xr_validated_program_entry_function(const XrValidatedProgram *program);
XR_FUNC uint64_t xr_validated_program_verifier_work(const XrValidatedProgram *program);
XR_FUNC const uint8_t *xr_validated_program_bytes(const XrValidatedProgram *program,
                                                  size_t *size_out);
XR_FUNC const char *xr_program_verify_status_name(XrProgramVerifyStatus status);
XR_FUNC const char *xr_program_diagnostic_kind_name(XrProgramDiagnosticKind kind);

#endif /* XR_PROGRAM_VERIFY_H */
