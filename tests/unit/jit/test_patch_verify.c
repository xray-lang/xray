#include <stdint.h>
#include <stdio.h>

#include "xm_patch_verify.h"

static int g_fail = 0;

static void check_bool(const char *label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        g_fail++;
    }
}

static void check_offset(const char *label, int ok, int32_t actual, int32_t expected) {
    check_bool(label, ok && actual == expected);
}

static void check_record(const char *label, XmPatchRecord record, XmPatchMcinsn mcinsn_id,
                         uint32_t offset, uint8_t width_bits, int is_signed) {
    check_bool(label, record.mcinsn_id == mcinsn_id && record.offset == offset &&
                          record.width_bits == width_bits && record.is_signed == (is_signed != 0) &&
                          xm_patch_record_valid(&record));
}

int main(void) {
    int32_t out = 0;
    XmPatchRecord record = xm_patch_record_make(XM_PATCH_MCINSN_ARM64_B_COND, 123, 19, true);
    check_record("patch record b.cond", record, XM_PATCH_MCINSN_ARM64_B_COND, 123, 19, 1);
    record = xm_patch_record_make(XM_PATCH_MCINSN_NONE, 0, 0, false);
    check_bool("patch record invalid none", !xm_patch_record_valid(&record));

    check_offset("x64 rel32 forward", xm_patch_calc_x64_rel32(10, 30, &out), out, 16);
    check_offset("x64 rel32 backward", xm_patch_calc_x64_rel32(30, 10, &out), out, -24);
    check_bool("x64 rel32 null out", !xm_patch_calc_x64_rel32(0, 0, NULL));

    check_offset("rv64 jal forward", xm_patch_calc_rv64_jal(4, 9, &out), out, 20);
    check_offset("rv64 jal backward", xm_patch_calc_rv64_jal(9, 4, &out), out, -20);
    check_offset("rv64 jal min", xm_patch_calc_rv64_jal(262144, 0, &out), out,
                 XM_PATCH_RV64_JAL_MIN);
    check_offset("rv64 jal max", xm_patch_calc_rv64_jal(0, 262143, &out), out,
                 XM_PATCH_RV64_JAL_MAX);
    check_bool("rv64 jal below min", !xm_patch_calc_rv64_jal(262145, 0, &out));
    check_bool("rv64 jal above max", !xm_patch_calc_rv64_jal(0, 262144, &out));

    check_offset("rv64 branch forward", xm_patch_calc_rv64_branch(2, 7, &out), out, 20);
    check_offset("rv64 branch backward", xm_patch_calc_rv64_branch(7, 2, &out), out, -20);
    check_offset("rv64 branch min", xm_patch_calc_rv64_branch(1024, 0, &out), out,
                 XM_PATCH_RV64_BRANCH_MIN);
    check_offset("rv64 branch max", xm_patch_calc_rv64_branch(0, 1023, &out), out,
                 XM_PATCH_RV64_BRANCH_MAX);
    check_bool("rv64 branch below min", !xm_patch_calc_rv64_branch(1025, 0, &out));
    check_bool("rv64 branch above max", !xm_patch_calc_rv64_branch(0, 1024, &out));
    check_bool("generic alignment reject", !xm_patch_offset_i32(6, -16, 16, 4, &out));

    /* ARM64 B / BL: 26-bit signed offset in instructions, range
     * [-(1<<25), (1<<25)-1] = [-33_554_432, 33_554_431]. */
    check_offset("arm64 b forward", xm_patch_calc_arm64_b(10, 30, &out), out, 20);
    check_offset("arm64 b backward", xm_patch_calc_arm64_b(30, 10, &out), out, -20);
    check_offset("arm64 b min boundary", xm_patch_calc_arm64_b((uint32_t) (1u << 25), 0, &out), out,
                 XM_PATCH_ARM64_B_MIN);
    check_offset("arm64 b max boundary",
                 xm_patch_calc_arm64_b(0, (uint32_t) ((1u << 25) - 1), &out), out,
                 XM_PATCH_ARM64_B_MAX);
    check_bool("arm64 b below min", !xm_patch_calc_arm64_b((uint32_t) ((1u << 25) + 1), 0, &out));
    check_bool("arm64 b above max", !xm_patch_calc_arm64_b(0, (uint32_t) (1u << 25), &out));

    /* ARM64 B.cond / CBZ / CBNZ: 19-bit signed offset, range
     * [-(1<<18), (1<<18)-1]. */
    check_offset("arm64 bcond forward", xm_patch_calc_arm64_bcond(10, 30, &out), out, 20);
    check_offset("arm64 bcond backward", xm_patch_calc_arm64_bcond(30, 10, &out), out, -20);
    check_offset("arm64 bcond min boundary",
                 xm_patch_calc_arm64_bcond((uint32_t) (1u << 18), 0, &out), out,
                 XM_PATCH_ARM64_BCOND_MIN);
    check_offset("arm64 bcond max boundary",
                 xm_patch_calc_arm64_bcond(0, (uint32_t) ((1u << 18) - 1), &out), out,
                 XM_PATCH_ARM64_BCOND_MAX);
    check_bool("arm64 bcond below min",
               !xm_patch_calc_arm64_bcond((uint32_t) ((1u << 18) + 1), 0, &out));
    check_bool("arm64 bcond above max", !xm_patch_calc_arm64_bcond(0, (uint32_t) (1u << 18), &out));

    /* ARM64 TBZ / TBNZ: 14-bit signed offset, range [-(1<<13), (1<<13)-1]. */
    check_offset("arm64 tb forward", xm_patch_calc_arm64_tb(10, 30, &out), out, 20);
    check_offset("arm64 tb min boundary", xm_patch_calc_arm64_tb((uint32_t) (1u << 13), 0, &out),
                 out, XM_PATCH_ARM64_TB_MIN);
    check_offset("arm64 tb max boundary",
                 xm_patch_calc_arm64_tb(0, (uint32_t) ((1u << 13) - 1), &out), out,
                 XM_PATCH_ARM64_TB_MAX);
    check_bool("arm64 tb below min", !xm_patch_calc_arm64_tb((uint32_t) ((1u << 13) + 1), 0, &out));
    check_bool("arm64 tb above max", !xm_patch_calc_arm64_tb(0, (uint32_t) (1u << 13), &out));

    if (g_fail > 0) {
        fprintf(stderr, "%d patch verifier checks failed\n", g_fail);
        return 1;
    }
    fprintf(stderr, "patch verifier checks passed\n");
    return 0;
}
