#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xm_dispatch_meta.h"

static int g_fail = 0;

static void check_bool(const char *label, bool cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static void check_entry(const char *label, XmOp op, XmDispatchBackend backend,
                        XmIselPattern pattern, uint8_t mcinsn_count, bool custom,
                        const char *mcinsns) {
    const XmDispatchMeta *m = xm_dispatch_meta_find(op, backend);
    if (!m) {
        fprintf(stderr, "FAIL: %s: missing metadata entry\n", label);
        g_fail++;
        return;
    }
    check_bool("pattern", m->pattern == pattern);
    check_bool("mcinsn_count", m->mcinsn_count == mcinsn_count);
    check_bool("custom", m->custom == custom);
    check_bool("mcinsns", strcmp(m->mcinsns, mcinsns) == 0);
}

int main(void) {
    check_bool("backend count", XM_DISPATCH_BACKEND__COUNT == 3);
    check_bool("meta count", XM_DISPATCH_META_COUNT == XM_OP_COUNT * XM_DISPATCH_BACKEND__COUNT);

    for (uint32_t op = 0; op < XM_OP_COUNT; ++op) {
        for (uint32_t backend = 0; backend < XM_DISPATCH_BACKEND__COUNT; ++backend) {
            const XmDispatchMeta *m = xm_dispatch_meta_find((XmOp) op, (XmDispatchBackend) backend);
            if (!m) {
                fprintf(stderr, "FAIL: missing metadata row op=%u backend=%u\n", op, backend);
                g_fail++;
                continue;
            }
            if ((uint32_t) m->op >= XM_OP_COUNT) {
                fprintf(stderr, "FAIL: op out of range op=%u backend=%u\n", op, backend);
                g_fail++;
                continue;
            }
            if ((uint32_t) m->backend >= XM_DISPATCH_BACKEND__COUNT) {
                fprintf(stderr, "FAIL: backend out of range op=%u backend=%u\n", op, backend);
                g_fail++;
                continue;
            }
            if ((uint32_t) m->op != op || (uint32_t) m->backend != backend) {
                fprintf(stderr, "FAIL: wrong metadata row op=%u backend=%u\n", op, backend);
                g_fail++;
                continue;
            }
            check_bool("mcinsn_count matches placeholder",
                       (m->mcinsn_count == 0) == (strcmp(m->mcinsns, "-") == 0));
            check_bool("custom flag matches pattern",
                       m->custom == (m->pattern == XM_ISEL_PATTERN_CUSTOM));
        }
    }

    check_entry("ADD x64", XM_ADD, XM_DISPATCH_BACKEND_X64, XM_ISEL_PATTERN_GP_RR_COMM, 1, false,
                "x64.add.rr");
    check_entry("ADD arm64", XM_ADD, XM_DISPATCH_BACKEND_ARM64, XM_ISEL_PATTERN_GP_RRR, 1, false,
                "arm64.add.rrr");
    check_entry("MOD arm64", XM_MOD, XM_DISPATCH_BACKEND_ARM64, XM_ISEL_PATTERN_CUSTOM, 2, true,
                "arm64.sdiv.rrr+arm64.msub.rrrr");
    check_entry("ALLOC x64", XM_ALLOC, XM_DISPATCH_BACKEND_X64, XM_ISEL_PATTERN_CUSTOM, 0, true,
                "-");
    check_bool("missing op lookup",
               xm_dispatch_meta_find((XmOp) XM_OP_COUNT, XM_DISPATCH_BACKEND_X64) == NULL);
    check_bool("missing backend lookup",
               xm_dispatch_meta_find(XM_ADD, XM_DISPATCH_BACKEND__COUNT) == NULL);
    check_bool("mcinsns helper", strcmp(xm_dispatch_meta_mcinsns(XM_ADD, XM_DISPATCH_BACKEND_ARM64),
                                        "arm64.add.rrr") == 0);

    if (g_fail > 0) {
        fprintf(stderr, "%d dispatch metadata checks failed\n", g_fail);
        return 1;
    }
    fprintf(stderr, "dispatch metadata checks passed (%u rows)\n",
            (uint32_t) XM_DISPATCH_META_COUNT);
    return 0;
}
