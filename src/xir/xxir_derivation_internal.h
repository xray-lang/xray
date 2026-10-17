/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_derivation_internal.h - Definition-bound derivation declaration data
 *
 * KEY CONCEPT:
 *   A grant binds an ordinary implementation and exact canonical receiver.
 *   Membership in the declaring nominal never widens a derived operation.
 */
#ifndef XXIR_DERIVATION_INTERNAL_H
#define XXIR_DERIVATION_INTERNAL_H
#include "xxir_interface.h"
#include "xxir_generated.h"

#define XR_XIR_DERIVE_DESCRIBE 1u
#define XR_XIR_DERIVE_READ 2u
#define XR_XIR_DERIVE_WRITE 4u
#define XR_XIR_DERIVE_MEMBER_MASK 7u

typedef struct XrXirDerivationMember {
    uint32_t field, operations;
} XrXirDerivationMember;

/* A definition policy exists before its generator runs or creates functions.
 * Its original Source owns every identity and the explicit member operations.
 * constructor is zero or one plus the only explicitly authorized init. */
typedef struct XrXirDerivationPolicy {
    uint32_t grantor_module;
    XrXirType target;
    XrXirInterfaceApplication protocol;
    uint32_t generator, constructor;
    const XrXirDerivationMember *members;
    uint32_t member_count;
    XrXirGeneratedOrigin origin;
} XrXirDerivationPolicy;

/* function and target belong to the current module. policy, source_row and
 * root name original Source rows. A binding cannot create or widen a policy.
 * Source binding rows name themselves by ordinal; instance rows retain that
 * ordinal while ordinary provenance substitutes their function and target. */
typedef struct XrXirDerivationGrant {
    uint32_t function, policy, source_row, root;
    XrXirType target;
    XrXirGeneratedFunctionOrigins origins;
} XrXirDerivationGrant;

typedef struct XrXirDerivationTable {
    const XrXirDerivationPolicy *policies;
    uint32_t policy_count;
    const XrXirDerivationGrant *records;
    uint32_t count;
} XrXirDerivationTable;

/* NULL is the only empty table. Rows are ordered by current function and
 * fields by declaration ordinal. Every count, pointer and nested origin is
 * admitted before copying; no body/effect or execution proof is stored. */
XR_FUNC XrXirStatus xir_derivations_copy(const XrXirCompileContext *,
    const XrXirDerivationTable *, XrXirDerivationTable **);
XR_FUNC XrXirStatus xir_derivations_structure_verify(const XrXirCompileContext *,
    const XrXirDerivationTable *);
XR_FUNC void xir_derivations_free(XrXirDerivationTable *);
/* Exact initialized data correspondence; NULL same rejects any difference.
 * Private change detection never suppresses malformed storage or budget errors. */
XR_FUNC XrXirStatus xir_derivations_match(const XrXirCompileContext *,
    const XrXirDerivationTable *, const XrXirDerivationTable *, bool *same);
XR_FUNC XrXirStatus xir_derivations_verify(const XrXirCompileContext *,
    const XrXirModule *);
/* Validate one authentic definition before its generated methods exist.
 * Imported grants remain intact; complete module verification still follows. */
XR_FUNC XrXirStatus xir_derivation_policy_verify(const XrXirCompileContext *,
    const XrXirModule *, const XrXirDerivationPolicy *);

typedef struct XrXirDerivationAccess {
    uint32_t function, field, operation;
    XrXirType target;
} XrXirDerivationAccess;
/* Called after current module declaration admission. A matching derived
 * receiver must prove this exact member operation even when its ordinary
 * method identity has nominal_owner. A missing row is never an exemption. */
XR_FUNC XrXirStatus xir_derivation_access(const XrXirCompileContext *,
    const XrXirModule *, const XrXirDerivationAccess *, bool *);
/* Ordinary function and constructor visibility remains independently checked.
 * A generated caller cannot inherit all private declarations of its module. */
XR_FUNC XrXirStatus xir_derivation_call(const XrXirCompileContext *,
    const XrXirModule *, uint32_t, const XrXirInstruction *, bool, bool *);
#endif // XXIR_DERIVATION_INTERNAL_H
