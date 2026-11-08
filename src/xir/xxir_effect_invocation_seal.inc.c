/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_seal.inc.c - Independent owned invocation and typed origin certificates
 *
 * KEY CONCEPT:
 *   The certificate owns its private body correspondence and complete inputs.
 *   This copy is evidence metadata, never an independently admitted executable.
 */

typedef struct EffectInvocationCertificate {
    XrCompileResources *resources;
    EffectTerms terms;
    EffectBodyMaterial *body;
    XrXirDeclarations *declarations;
    XrXirGeneric *generics;
    XrXirDefaultTable defaults;
    XrXirOrigin *origins;
    uint32_t *owners;
    EffectInvocationDeclaredBounds *declared;
    EffectInvocationFinal *final;
} EffectInvocationCertificate;

/* The builder reads its live derivation. A receiving request reads the actual
 * sealed certificate. These storage views carry claims, never admission. */

/* The receiving request borrows the actual Final. Its reverse index is fresh
 * verification scratch and never contains a solver queue or copied inventory. */

_Static_assert(sizeof(EffectInvocationDeclaredBounds)%_Alignof(EffectInvocationFunctionBounds)==0 &&
    sizeof(EffectInvocationFunctionBounds)%_Alignof(EffectInvocationValueBound)==0,
    "Certificate bounds preserve descriptor and value alignment");

/* Both outputs consume this call's fresh selected values. The receiver is
 * private parent scratch, never a saved authorization or a public output. */
typedef struct EffectInvocationProjectedCause {
    uint32_t atom;
    const XrXirRootEffectWitness *witness;
} EffectInvocationProjectedCause;

typedef struct EffectInvocationProjectionSink {
    XrCompileResources *resources;
    uint32_t count;
    void *owner;
    XrXirStatus (*write)(const XrXirCompileContext *work,
        const EffectInvocationCertificate *certificate,uint32_t function,
        const uint32_t *selected,EffectFinalFunctions *result,void *owner);
} EffectInvocationProjectionSink;

typedef struct EffectInvocationSealRequest {
    EffectInvocationWorkspace **equations;
    EffectFinalFunctions **result;
    const EffectOrdinaryContexts *dense;
} EffectInvocationSealRequest;

/* This generation already owns every body field. Seal borrows it while
 * all independent proofs run and transfers that same owner only at commit. */

/* Bounds were copied or written before the first equation consumes them.
 * Their sorted complete physical membership remains independently checked
 * at seal; this boundary does not allocate or copy another bound owner. */

/* Origins and owner links come from the actual common context owner.
 * Role claims belong only to Final and are independently checked there. */

/* This independently derived relation has no formal or call terms. Actual
 * canonical result fields are still compared, including every empty owner. */

/* A complete zero relation and its actual rows are checked in one uninterrupted
 * Input read. A local-layout mismatch matters only when every Input row proves
 * the relation empty; malformed Input and exhausted work always fail immediately.
 * No callback can invalidate an earlier comparison or a temporary stack value. */

/* Derive primitive contributions from current Input, not its stored seed or
 * a receiving local row. Dynamic calls remain obligations of the complete
 * invocation relation. No equation generator supplies an expected result. */

/* All failure paths keep both input owners and the occupied output intact.
 * Ownership moves only after complete copied correspondence and closure proof;
 * construction-only pointers are then removed from the published certificate. */

/* Replaying an immutable certificate uses the same constructor and role
 * inventory. This helper does not publish or retain another result in it. */
