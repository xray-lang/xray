/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_scalar_program_authority_internal.h - Frozen scalar snapshot storage
 */

#ifndef XA_SCALAR_PROGRAM_AUTHORITY_INTERNAL_H
#define XA_SCALAR_PROGRAM_AUTHORITY_INTERNAL_H

#include "xa_scalar_program_authority.h"

struct XaScalarProgramAuthority {
    uint32_t schema;
    uint8_t verified;
    uint8_t reserved[3];
    XrFingerprint policy_fingerprint;
    XrFingerprint fingerprint;
    XaScalarModuleAuthority module;
    XaScalarFunctionAuthority functions[XA_SCALAR_PROGRAM_FUNCTION_COUNT];
    XaScalarCallAuthority call;
};

#endif  // XA_SCALAR_PROGRAM_AUTHORITY_INTERNAL_H
