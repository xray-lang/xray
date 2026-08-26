/*
 * test_vm_decoded_cache_runtime_archive.c - Runtime-only cache link boundary
 */

#include "vm/xr_vm_decoded_cache.h"
#include <stdio.h>

int main(void) {
    XrFingerprint fingerprint = {{0}};
    XrVmDecodedCache *cache = (XrVmDecodedCache *) (uintptr_t) 1;
    XrVmDecodedFunctionView function;
    XrVmDecodedCacheStats stats;
    size_t bytes = 0;
    if (xr_typed_decoded_cache_create(NULL, &fingerprint, NULL, &cache) !=
            XR_VM_DECODED_CACHE_INVALID_ARGUMENT ||
        cache != NULL || xr_typed_decoded_cache_function(NULL, 0, &function) ||
        xr_typed_decoded_cache_stats(NULL, &stats) ||
        !xr_typed_decoded_cache_size_within_budget(1, 1, 1, &bytes) || bytes == 0 ||
        bytes > XR_VM_DECODED_CACHE_MAX_BYTES) {
        fputs("runtime-only VM decoded cache boundary failed\n", stderr);
        return 1;
    }
    xr_typed_decoded_cache_free(NULL);
    puts("runtime-only VM decoded cache boundary passed");
    return 0;
}
