/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_projection.inc.c - Owned advertisement and constant projections
 *
 * KEY CONCEPT:
 *   Four projections select rooted equation atoms. Actual edge identities stay
 *   in the same certificate instead of becoming a declaration-only call chain.
 */

/* Constant domains consume only true intrinsic atoms. Projected domains also
 * consume the owned original formal advertisements and unresolved dependencies.
 * NONE advertisements never authorize an unrooted or incomplete equation. */
/* This value belongs only to the present projection request. Each domain
 * keeps its own atom and ordering; no result is retained between requests. */

/* One complete four-domain request uses the same actual atom/cause and
 * advertisement reader as the direct scalar selector below. Each active
 * atom is proved once, with independent projected/constant ROOT and UNKNOWN
 * orderings. This request is never used for a single-domain output.
 * Its output is private caller scratch and may be partially filled on failure.
 * Callers consume it only after success; public outputs have separate gates. */
typedef struct EffectProjectionCauseView {
    const XrXirRootEffectWitness *witness;
    uint32_t callee;
} EffectProjectionCauseView;

/* This view dies before its certificate. No full witness value is copied. */

/* This is the original ordering, with the public callee mapping read once. */

/* All records remain private to this not-yet-published certificate. No external
 * Effects, contract, carrier or entry permission changes on any failure path. */

/* A fact value is the presence of its canonical witness, not another stored
 * permission row. Receiving admission independently compares every witness
 * with the actual selected atom and rooted cause before using either value. */
static XrXirStatus effect_invocation_certificate_facts(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,XrXirRootEffects *output) {
    if (!xir_compile_context_valid(work) || !certificate || !output ||
        !effect_body_material_shape(certificate->body) || certificate->resources!=work->resources ||
        certificate->body->resources!=work->resources ||
        !certificate->final || certificate->final->summary.resources!=work->resources ||
        certificate->final->summary.count!=effect_body_count(certificate->body) || !certificate->final->summary.witnesses ||
        function>=certificate->final->summary.count) return XR_XIR_BAD_STRUCTURE;

    const EffectFinalFunctions *summary=&certificate->final->summary;
    *output=(XrXirRootEffects){summary->witnesses[function].cause!=XR_XIR_ROOT_CAUSE_NONE,
        summary->witnesses[summary->count+function].cause!=XR_XIR_ROOT_CAUSE_NONE};
    return XR_XIR_OK;
}

/* The caller provides a freshly proved domain selection. Compare the unique
 * receiving record to its true transformed cause, never to another mirror. */
