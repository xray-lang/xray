#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XM_RUNTIME_STUBS_ENTRIES
#include "xm_runtime_stubs_gen.h"

static int g_fail = 0;

static void check_bool(const char *label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static void check_entry(const char *label, XmRuntimeStubId id, XmRuntimeStubAbi abi,
                        const char *c_symbol, uintptr_t expected) {
    const XmRuntimeStubInfo *info = xm_runtime_stub_get(id);
    uintptr_t entry = xm_runtime_stub_entry(id, abi);
    check_bool("metadata row", info != NULL);
    check_bool("metadata id", info != NULL && info->id == id);
    check_bool("metadata ABI", info != NULL && info->abi == abi);
    check_bool("metadata symbol", info != NULL && strcmp(info->c_symbol, c_symbol) == 0);
    check_bool(label, entry == expected);
    check_bool("entry table", xm_runtime_stub_entries[(uint32_t) id] == expected);
    check_bool("entry nonzero", entry != 0);
}

int main(void) {
    check_bool("extern metadata count", XM_RUNTIME_STUB_COUNT == XM_RUNTIME_STUB__COUNT);

    check_entry("alloc entry", XM_RUNTIME_STUB_alloc, XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG,
                "xr_jit_alloc", (uintptr_t) xr_jit_alloc);
    check_entry("barrier_fwd entry", XM_RUNTIME_STUB_barrier_fwd,
                XM_RUNTIME_STUB_ABI_BARRIER_FWD_FIXED, "xr_jit_barrier_fwd",
                (uintptr_t) xr_jit_barrier_fwd);
    check_entry("barrier_back entry", XM_RUNTIME_STUB_barrier_back,
                XM_RUNTIME_STUB_ABI_BARRIER_BACK_FIXED, "xr_jit_barrier_back",
                (uintptr_t) xr_jit_barrier_back);

    check_bool(
        "alloc ABI mismatch",
        xm_runtime_stub_entry(XM_RUNTIME_STUB_alloc, XM_RUNTIME_STUB_ABI_BARRIER_FWD_FIXED) == 0);
    check_bool("invalid stub id", xm_runtime_stub_entry((XmRuntimeStubId) XM_RUNTIME_STUB__COUNT,
                                                        XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG) == 0);

    if (g_fail > 0) {
        fprintf(stderr, "%d runtime stub entry checks failed\n", g_fail);
        return 1;
    }

    fprintf(stderr, "runtime stub entry checks passed (%u rows)\n",
            (uint32_t) XM_RUNTIME_STUB_COUNT);
    return 0;
}
