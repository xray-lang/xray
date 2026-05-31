#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

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
    bool ok;

    XmPatchRecord record = xm_patch_record_make(XM_PATCH_MCINSN_ARM64_B_COND, 123, 19, true);
    check_record("patch record b.cond", record, XM_PATCH_MCINSN_ARM64_B_COND, 123, 19, 1);
    record = xm_patch_record_make(XM_PATCH_MCINSN_NONE, 0, 0, false);
    check_bool("patch record invalid none", !xm_patch_record_valid(&record));

    /* x64 rel32 */
    ok = xm_patch_calc_x64_rel32(10, 30, &out);
    check_offset("x64 rel32 forward", ok, out, 16);
    ok = xm_patch_calc_x64_rel32(30, 10, &out);
    check_offset("x64 rel32 backward", ok, out, -24);
    check_bool("x64 rel32 null out", !xm_patch_calc_x64_rel32(0, 0, NULL));

    /* rv64 JAL */
    ok = xm_patch_calc_rv64_jal(4, 9, &out);
    check_offset("rv64 jal forward", ok, out, 20);
    ok = xm_patch_calc_rv64_jal(9, 4, &out);
    check_offset("rv64 jal backward", ok, out, -20);
    ok = xm_patch_calc_rv64_jal(262144, 0, &out);
    check_offset("rv64 jal min", ok, out, XM_PATCH_RV64_JAL_MIN);
    ok = xm_patch_calc_rv64_jal(0, 262143, &out);
    check_offset("rv64 jal max", ok, out, XM_PATCH_RV64_JAL_MAX);
    check_bool("rv64 jal below min", !xm_patch_calc_rv64_jal(262145, 0, &out));
    check_bool("rv64 jal above max", !xm_patch_calc_rv64_jal(0, 262144, &out));

    /* rv64 branch */
    ok = xm_patch_calc_rv64_branch(2, 7, &out);
    check_offset("rv64 branch forward", ok, out, 20);
    ok = xm_patch_calc_rv64_branch(7, 2, &out);
    check_offset("rv64 branch backward", ok, out, -20);
    ok = xm_patch_calc_rv64_branch(1024, 0, &out);
    check_offset("rv64 branch min", ok, out, XM_PATCH_RV64_BRANCH_MIN);
    ok = xm_patch_calc_rv64_branch(0, 1023, &out);
    check_offset("rv64 branch max", ok, out, XM_PATCH_RV64_BRANCH_MAX);
    check_bool("rv64 branch below min", !xm_patch_calc_rv64_branch(1025, 0, &out));
    check_bool("rv64 branch above max", !xm_patch_calc_rv64_branch(0, 1024, &out));
    check_bool("generic alignment reject", !xm_patch_offset_i32(6, -16, 16, 4, &out));

    /* ARM64 B / BL: 26-bit signed offset in instructions */
    ok = xm_patch_calc_arm64_b(10, 30, &out);
    check_offset("arm64 b forward", ok, out, 20);
    ok = xm_patch_calc_arm64_b(30, 10, &out);
    check_offset("arm64 b backward", ok, out, -20);
    ok = xm_patch_calc_arm64_b((uint32_t) (1u << 25), 0, &out);
    check_offset("arm64 b min boundary", ok, out, XM_PATCH_ARM64_B_MIN);
    ok = xm_patch_calc_arm64_b(0, (uint32_t) ((1u << 25) - 1), &out);
    check_offset("arm64 b max boundary", ok, out, XM_PATCH_ARM64_B_MAX);
    check_bool("arm64 b below min", !xm_patch_calc_arm64_b((uint32_t) ((1u << 25) + 1), 0, &out));
    check_bool("arm64 b above max", !xm_patch_calc_arm64_b(0, (uint32_t) (1u << 25), &out));

    /* ARM64 B.cond / CBZ / CBNZ: 19-bit signed offset */
    ok = xm_patch_calc_arm64_bcond(10, 30, &out);
    check_offset("arm64 bcond forward", ok, out, 20);
    ok = xm_patch_calc_arm64_bcond(30, 10, &out);
    check_offset("arm64 bcond backward", ok, out, -20);
    ok = xm_patch_calc_arm64_bcond((uint32_t) (1u << 18), 0, &out);
    check_offset("arm64 bcond min boundary", ok, out, XM_PATCH_ARM64_BCOND_MIN);
    ok = xm_patch_calc_arm64_bcond(0, (uint32_t) ((1u << 18) - 1), &out);
    check_offset("arm64 bcond max boundary", ok, out, XM_PATCH_ARM64_BCOND_MAX);
    check_bool("arm64 bcond below min",
               !xm_patch_calc_arm64_bcond((uint32_t) ((1u << 18) + 1), 0, &out));
    check_bool("arm64 bcond above max", !xm_patch_calc_arm64_bcond(0, (uint32_t) (1u << 18), &out));

    /* ARM64 TBZ / TBNZ: 14-bit signed offset */
    ok = xm_patch_calc_arm64_tb(10, 30, &out);
    check_offset("arm64 tb forward", ok, out, 20);
    ok = xm_patch_calc_arm64_tb((uint32_t) (1u << 13), 0, &out);
    check_offset("arm64 tb min boundary", ok, out, XM_PATCH_ARM64_TB_MIN);
    ok = xm_patch_calc_arm64_tb(0, (uint32_t) ((1u << 13) - 1), &out);
    check_offset("arm64 tb max boundary", ok, out, XM_PATCH_ARM64_TB_MAX);
    check_bool("arm64 tb below min", !xm_patch_calc_arm64_tb((uint32_t) ((1u << 13) + 1), 0, &out));
    check_bool("arm64 tb above max", !xm_patch_calc_arm64_tb(0, (uint32_t) (1u << 13), &out));

    if (g_fail > 0) {
        fprintf(stderr, "%d patch verifier checks failed\n", g_fail);
        return 1;
    }
    fprintf(stderr, "patch verifier checks passed\n");
    return 0;
}
