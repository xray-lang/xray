/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_final_view.inc.c - Fresh views of admitted finite origins
 *
 * KEY CONCEPT:
 *   Receiving scratch projects Final without an origin solver or discovery.
 *   Only bounded scalar dimensions survive allocation. Actual owned input
 *   heads and the selected key are framed after the allocator callback.
 */
typedef struct EffectInvocationViewLayout {
    EffectInvocationBasis basis;
    uint32_t body,root,values,rows,parameters,instructions,sites,bindings;
} EffectInvocationViewLayout;

/* These untrusted dimensions size bounded scratch only. No input head is
 * borrowed or permission accepted before the allocator callback returns. */
