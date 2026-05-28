#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XM_RUNTIME_STUBS_IMPL
#include "xm_runtime_stubs_gen.h"

static int g_fail = 0;

static void check_bool(const char *label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static const XmRuntimeStubInfo *find_stub(const char *c_symbol) {
    for (uint32_t i = 0; i < XM_RUNTIME_STUB_COUNT; ++i) {
        const XmRuntimeStubInfo *s = &xm_runtime_stub_info[i];
        if (strcmp(s->c_symbol, c_symbol) == 0)
            return s;
    }
    return NULL;
}

static void check_stub(const char *label, XmRuntimeStubId id, const char *name,
                       const char *c_symbol, XmRuntimeStubAbi abi, uint8_t nargs, uint8_t ret_rep,
                       uint16_t flags, uint8_t pointer_trust, uint8_t post_call) {
    const XmRuntimeStubInfo *s = find_stub(c_symbol);
    if (!s) {
        fprintf(stderr, "FAIL: %s: missing runtime stub\n", label);
        g_fail++;
        return;
    }
    check_bool("id", s->id == id);
    check_bool("indexed lookup", xm_runtime_stub_get(id) == s);
    check_bool("abi lookup", xm_runtime_stub_has_abi(id, abi));
    check_bool("name", strcmp(s->name, name) == 0);
    check_bool("abi", s->abi == abi);
    check_bool("nargs", s->nargs == nargs);
    check_bool("ret_rep", s->ret_rep == ret_rep);
    check_bool("flags", s->flags == flags);
    check_bool("pointer_trust", s->pointer_trust == pointer_trust);
    check_bool("post_call", s->post_call == post_call);
}

int main(void) {
    check_bool("stub count", XM_RUNTIME_STUB_COUNT == 3);
    check_bool("stub id count", XM_RUNTIME_STUB__COUNT == 3);
    check_bool("ABI count", XM_RUNTIME_STUB_ABI__COUNT == 3);

    check_stub("alloc", XM_RUNTIME_STUB_alloc, "alloc", "xr_jit_alloc",
               XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG, 2, XR_REP_PTR, XM_HF_GC | XM_HF_STACKMAP,
               XM_HPT_GC, 0);
    check_stub("barrier_fwd", XM_RUNTIME_STUB_barrier_fwd, "barrier_fwd", "xr_jit_barrier_fwd",
               XM_RUNTIME_STUB_ABI_BARRIER_FWD_FIXED, 3, XR_REP_VOID, XM_HF_GC | XM_HF_STACKMAP,
               XM_HPT_NONE, 0);
    check_stub("barrier_back", XM_RUNTIME_STUB_barrier_back, "barrier_back", "xr_jit_barrier_back",
               XM_RUNTIME_STUB_ABI_BARRIER_BACK_FIXED, 2, XR_REP_VOID, XM_HF_GC | XM_HF_STACKMAP,
               XM_HPT_NONE, 0);

    if (g_fail > 0) {
        fprintf(stderr, "%d runtime stub checks failed\n", g_fail);
        return 1;
    }
    fprintf(stderr, "runtime stub checks passed (%u rows)\n", (uint32_t) XM_RUNTIME_STUB_COUNT);
    return 0;
}
