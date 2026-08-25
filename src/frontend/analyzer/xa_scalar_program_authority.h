/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_scalar_program_authority.h - Bounded pre-Xi scalar semantic snapshot
 *
 * KEY CONCEPT:
 *   This deliberately narrow analyzer-owned publication contains one source
 *   module, two sealed scalar functions, and one resolved direct call. Every
 *   row is pointer-free. Unsupported programs publish no partial authority.
 */

#ifndef XA_SCALAR_PROGRAM_AUTHORITY_H
#define XA_SCALAR_PROGRAM_AUTHORITY_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XA_SCALAR_PROGRAM_AUTHORITY_SCHEMA UINT32_C(1)
#define XA_SCALAR_PROGRAM_FUNCTION_COUNT UINT32_C(2)

struct AstNode;
struct XaAnalyzer;
struct XrModuleSpec;

typedef struct XaScalarProgramAuthority XaScalarProgramAuthority;

typedef enum XaScalarProgramAuthorityStatus {
    XA_SCALAR_PROGRAM_AUTHORITY_INVALID = 0,
    XA_SCALAR_PROGRAM_AUTHORITY_READY,
    XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED,
    XA_SCALAR_PROGRAM_AUTHORITY_RESOURCE_FAILURE,
} XaScalarProgramAuthorityStatus;

typedef struct XaScalarSourceSpan {
    uint32_t kind;
    uint32_t start_line;
    uint32_t start_column;
    uint32_t end_line;
    uint32_t end_column;
} XaScalarSourceSpan;

typedef struct XaScalarModuleAuthority {
    XrStableId module_identity;
    XrFingerprint module_authority_fingerprint;
    XrFingerprint source_fingerprint;
    XrFingerprint export_fingerprint;
} XaScalarModuleAuthority;

typedef enum XaScalarProgramFunctionFlag {
    XA_SCALAR_PROGRAM_FUNCTION_ENTRY = 1u << 0,
} XaScalarProgramFunctionFlag;

typedef struct XaScalarFunctionAuthority {
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
    XaScalarSourceSpan declaration_span;
    uint64_t capability_mask;
    uint8_t flags;
    uint8_t parameter_count;
    uint8_t reserved[6];
} XaScalarFunctionAuthority;

typedef struct XaScalarCallAuthority {
    XrStableId callsite_identity;
    XrStableId caller_declaration_identity;
    XrStableId caller_instance_identity;
    XrStableId callee_declaration_identity;
    XrStableId callee_instance_identity;
    XrFingerprint contract_fingerprint;
    XaScalarSourceSpan callsite_span;
    uint8_t reserved[8];
} XaScalarCallAuthority;

XR_FUNC XaScalarProgramAuthorityStatus xa_scalar_program_authority_publish(
    struct XaAnalyzer *analyzer, const struct AstNode *syntax,
    const struct XrModuleSpec *module_spec, XaScalarProgramAuthority **out);
XR_FUNC void xa_scalar_program_authority_free(XaScalarProgramAuthority *authority);
XR_FUNC bool xa_scalar_program_authority_verify(const XaScalarProgramAuthority *authority,
                                                char *error, size_t error_size);

XR_FUNC XrFingerprint
xa_scalar_program_authority_policy(const XaScalarProgramAuthority *authority);
XR_FUNC XrFingerprint
xa_scalar_program_authority_fingerprint(const XaScalarProgramAuthority *authority);
XR_FUNC const XaScalarModuleAuthority *
xa_scalar_program_authority_module(const XaScalarProgramAuthority *authority);
XR_FUNC const XaScalarFunctionAuthority *
xa_scalar_program_authority_function(const XaScalarProgramAuthority *authority, uint32_t index);
XR_FUNC const XaScalarCallAuthority *
xa_scalar_program_authority_call(const XaScalarProgramAuthority *authority);

#endif  // XA_SCALAR_PROGRAM_AUTHORITY_H
