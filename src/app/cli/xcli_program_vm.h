/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_program_vm.h - Shared CLI Program execution owner
 *
 * KEY CONCEPT:
 *   Source run has one route: source owner to validated XrProgram, exact
 *   target profile and instance, then a VM-private executable view.
 */

#ifndef XCLI_PROGRAM_VM_H
#define XCLI_PROGRAM_VM_H
#include "../../vm/xr_program_vm.h"
#include "../../runtime/abi/xr_builtin_provider_contract.h"

typedef struct XrCliProviderBindings {
    XrProviderBinding providers[XR_RUNTIME_ABI_MAX_PROVIDERS];
    XrProviderOperationBinding operations[XR_RUNTIME_ABI_MAX_PROVIDERS]
                                         [XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS];
    uint32_t count;
} XrCliProviderBindings;

typedef struct XrCliProgramVm {
    XrCliProviderBindings bindings;
    XrVmCode *code;
    XrInstance *instance;
} XrCliProgramVm;

typedef struct XrCliVmResult {
    XrVmOutcomeKind kind;
    XrVmTrap trap;
    XrVmValueKind value_kind;
    XrVmPanicInfo panic_info;
    uint64_t steps;
    bool timed_out;
    bool error_reported;
    bool panic_reported;
} XrCliVmResult;

XR_FUNC bool xr_cli_program_vm_open(XrCliProgramVm *vm, XrValidatedProgram *program,
                                    XrTargetProfile *profile, char *error, size_t error_size);
XR_FUNC bool xr_cli_program_vm_close(XrCliProgramVm *vm);
/* deadline_ms uses the CLI monotonic clock; zero means no deadline. An optional
 * error sink observes the borrowed typed failure before its execution is freed. */
XR_FUNC XrCliVmResult xr_cli_program_vm_invoke(XrCliProgramVm *vm, uint32_t function_id,
                                               double deadline_ms, const XrValueFormatSink *errors);
#endif
