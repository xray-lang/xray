/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_calls.inc.c - Actual physical invocation and latent reference edges
 */

/* Complete environments are temporary until exact structural admission and
 * deep-copy interning succeed. The caller supplies the complete row read from
 * this request's real ordinal; no subsequent queue visit reuses its address. */
/* Coordinates borrow only this call's genuine target head. The tuple has
 * no authority and cannot outlive its actual stack replay. */
typedef struct EffectInvocationActual {
    uint32_t target,producer;
    const uint64_t *closure;
} EffectInvocationActual;

/* A captured leaf has no environment columns. The enclosing closure's flat
 * relation is not part of that leaf value. Symbolic or owning producers keep
 * their complete relation and remain subject to the ordinary exact reader. */

/* A prefix projection reads the complete flat relation of this actual value.
 * Nested environments stay in the same finite columns as their producer. */

/* Consumers without a current head perform the same complete status read.
 * The argument algorithm is shared; no cached role or permission is accepted. */

/* A deferred call is indexed by its actual local instruction. Propagation
 * projects it to the caller's real frontier rather than copying a child ID. */

/* An unbound REF callable retains the invocation condition, including its
 * actual argument mapping. Ordinary symbolic parameters retain the original
 * PARAMETER obligation; fixed opaque values consume their advertised bound. */

/* Constructing a callable schedules its latent equation but never executes
 * that body in its producer. Unbound suffixes are genuine UNKNOWN inputs;
 * later calls create a separate key with their complete physical actuals. */
/* Replay the original complete latent input without linking another key.
 * Unbound suffixes remain opaque; captured inputs keep their physical rows. */

/* GO installs a real actual environment and schedules the target equation
 * for admission queries. It never joins that body into the creating stream. */

/* The caller owns this complete decoded stack row for only this request.
 * It was read from the same BodyRequest at this actual ordinal; it is never
 * retained by an equation, descriptor or another queue visit. */

/* Callers without a decoded row perform the original complete status-returning
 * read. A failed callback or canonical record cannot reach the dispatcher. */
