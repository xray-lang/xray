/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_solution.inc.c - Exact closed equations and staged canonical formulas
 *
 * KEY CONCEPT:
 *   A staged result owns its canonical records. No borrowed producer, pending
 *   equation or merely empty lattice state can become an executable permission.
 */
/* This is the final receiving owner. Each term vector is written once and
 * transfers to its public contract only after independent comparison. */
static void effect_invocation_result_clear(EffectFinalFunctions *result) {
    if (!result) return;
    for (uint32_t f=0;f<result->count;++f)
        xr_compile_resources_free(result->functions[f].storage);
    xr_compile_resources_free(result->functions);
    xr_compile_resources_free(result->witnesses);
}

static bool effect_invocation_frontier(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_CALL_DEFAULT ||
        op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_REQUIREMENT ||
        op==XR_XIR_CLEANUP_REGISTER || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT ||
        effect_invocation_iterator_operation(op);
}

/* Compare the exact union of local contributions and executing children.
 * An inflated OR result is rejected, including an unrooted recursive fact.
 * Certificate forests separately prove a genuine local terminal for every bit. */

/* Construction completion is checked only while its real scratch exists.
 * The shared semantic closure reader consumes the sole Final directly. */

/* The same canonical enumerator counts, writes or compares a bounded
 * receiving span. Comparison reads the actual stored kind/index directly;
 * no expected array or permission result survives this one request. */
/* Formula construction and published readers use the same owned numeric
 * parameter rows. No second body walk reconstructs their role classification. */
typedef struct EffectInvocationTermSource {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *equations;
} EffectInvocationTermSource;
typedef struct EffectInvocationTermTarget {
    uint32_t limit,kind,index;
    bool *present;
} EffectInvocationTermTarget;

/* The same constructor owns the final term spans for generating equations or
 * for an independent replay of an immutable certificate. Neither adapter
 * retains its caller, and no second canonical formula inventory is stored. */

/* The local relation has one actual canonical result, created directly from
 * this producer's complete input. Stage transfers this owner; seal independently
 * derives the actual input again before comparing any result or permission. */

/* Staging never changes public contracts. Closure and subsequent independent
 * comparison both consume the complete current equations before transfer. */
