/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_trace.inc.c - Producer-independent actual equation cause traces
 */
typedef struct EffectInvocationTraceStep {
    uint32_t equation,root,function;
    XrXirRootEffectWitness witness;
} EffectInvocationTraceStep;
typedef struct EffectInvocationCauseTrace {
    XrCompileResources *resources;
    XrXirRootEffects facts;
    uint32_t counts[2];
    EffectInvocationTraceStep *steps;
} EffectInvocationCauseTrace;
typedef struct EffectInvocationTraceRequest {
    uint32_t function,domain;
    EffectInvocationTraceStep *steps;
} EffectInvocationTraceRequest;

/* A selection is a value local to one completed trace request. It never
 * survives the caller, changes an owner, or substitutes for a fresh query. */
typedef struct EffectInvocationTraceSelection {
    uint32_t atom;
    XrXirRootEffectWitness witness;
} EffectInvocationTraceSelection;

/* The selected atom retains the namespace's complete physical input identity
 * for the whole descent. Repeated declaration IDs cannot replace an actual
 * equation edge, and public ordinals are emitted only after that proof. */
