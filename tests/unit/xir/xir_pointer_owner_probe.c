/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pointer_owner_probe.c - One actual page-provider close refusal
 *
 * KEY CONCEPT:
 *   Only the selected unmap attempt fails. Every other operation delegates to
 *   the actual provider without replacing the ledger or its cleanup algorithm.
 */
#define xr_mem_unmap pointer_probe_unmap
#include "../../../src/xir/xxir_value.c"
#undef xr_mem_unmap
XR_FUNC bool xr_mem_unmap(void *,size_t);
static unsigned unmap_until_failure=UINT32_MAX;
XR_FUNC void pointer_fail_next_unmap(void) {unmap_until_failure=0;}
XR_FUNC void pointer_fail_unmap_after(unsigned successful) {unmap_until_failure=successful;}
XR_FUNC bool pointer_probe_unmap(void *address,size_t bytes) {
    if (unmap_until_failure==0) {unmap_until_failure=UINT32_MAX;return false;}
    if (unmap_until_failure!=UINT32_MAX) --unmap_until_failure;
    return xr_mem_unmap(address,bytes);
}
