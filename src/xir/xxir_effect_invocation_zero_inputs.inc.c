/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_zero_inputs.inc.c - Owned immutable zero word storage
 *
 * KEY CONCEPT:
 *   Only physical all-zero payloads share storage inside one equation owner.
 *   Nodes retain independent identities and every semantic input comparison.
 */
static XrXirStatus effect_invocation_zero_input(EffectInvocationOwner *owner,
    const uint64_t *input,uint64_t words,const uint64_t **output) {
    if (!owner || !output || *output || (words && !input)) return XR_XIR_BAD_STRUCTURE;
    if (!words) return XR_XIR_OK;
    if (words>(SIZE_MAX-sizeof(EffectInvocationZeroInput))/sizeof(*input)) return XR_XIR_BUDGET;

    for (size_t w=0;w<(size_t)words;++w) if (input[w]) return XR_XIR_OK;
    for (const EffectInvocationZeroInput *row=owner->zero_inputs;row;row=row->next) {

        if (row->words!=words) continue;
        const uint64_t *owned=(const uint64_t *)(row+1);

        for (size_t w=0;w<(size_t)words;++w)
            if (owned[w]) return XR_XIR_BAD_STRUCTURE;
        *output=owned;return XR_XIR_OK;
    }
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationZeroInput *row=xir_compile_calloc(owner->work,1,
        sizeof(*row)+(size_t)words*sizeof(*input),&status);
    if (!row) return status;

    row->words=(size_t)words;row->next=owner->zero_inputs;owner->zero_inputs=row;
    *output=(const uint64_t *)(row+1);return XR_XIR_OK;
}
