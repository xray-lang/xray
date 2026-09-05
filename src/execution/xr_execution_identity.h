/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution_identity.h - Pure program/profile execution identity
 */

#ifndef XR_EXECUTION_IDENTITY_H
#define XR_EXECUTION_IDENTITY_H

#include "../plan/target/xr_target_profile.h"
#include "../program/xr_program_verify.h"

typedef XrFingerprint XrExecutionId;

/* ExecutionId names the immutable semantic and target input shared by VM and
 * AOT. Provider instances, generations, leases, and backend realizations are
 * deliberately outside this pure identity boundary. */
XR_FUNC bool xr_execution_id_compute(const XrValidatedProgram *program,
                                     const XrTargetProfile *profile,
                                     XrExecutionId *execution_id_out);

#endif  // XR_EXECUTION_IDENTITY_H
