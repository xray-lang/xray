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

/* The caller provides a freshly proved domain selection. Compare the unique
 * receiving record to its true transformed cause, never to another mirror. */
