#ifndef XM_PATCH_VERIFY_H
#define XM_PATCH_VERIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "xm_patch_ranges_gen.h"

typedef enum {
    XM_PATCH_MCINSN_NONE = 0,
    XM_PATCH_MCINSN_X64_CALL_REL32,
    XM_PATCH_MCINSN_X64_JMP_REL32,
    XM_PATCH_MCINSN_X64_JCC_REL32,
    XM_PATCH_MCINSN_ARM64_B,
    XM_PATCH_MCINSN_ARM64_BL,
    XM_PATCH_MCINSN_ARM64_B_COND,
    XM_PATCH_MCINSN_ARM64_CBZ,
    XM_PATCH_MCINSN_ARM64_CBNZ,
    XM_PATCH_MCINSN_ARM64_TBZ,
    XM_PATCH_MCINSN_ARM64_TBNZ,
    XM_PATCH_MCINSN_RV64_JAL,
    XM_PATCH_MCINSN_RV64_BRANCH,
} XmPatchMcinsn;

typedef struct {
    XmPatchMcinsn mcinsn_id;
    uint32_t offset;
    uint8_t width_bits;
    bool is_signed;
} XmPatchRecord;

static inline XmPatchRecord xm_patch_record_make(XmPatchMcinsn mcinsn_id, uint32_t offset,
                                                 uint8_t width_bits, bool is_signed) {
    XmPatchRecord record;
    record.mcinsn_id = mcinsn_id;
    record.offset = offset;
    record.width_bits = width_bits;
    record.is_signed = is_signed;
    return record;
}

static inline bool xm_patch_record_valid(const XmPatchRecord *record) {
    return record != NULL && record->mcinsn_id != XM_PATCH_MCINSN_NONE && record->width_bits > 0;
}

#define XM_PATCH_ARM64_TB_MIN (-(1 << 13)) /* TBZ / TBNZ: 14-bit signed */
#define XM_PATCH_ARM64_TB_MAX ((1 << 13) - 1)
#define XM_PATCH_ARM64_TB_ALIGN 1

static inline bool xm_patch_offset_i32(int64_t offset, int32_t min_offset, int32_t max_offset,
                                       int32_t alignment, int32_t *out) {
    if (out == NULL || alignment <= 0)
        return false;
    if (offset < (int64_t) min_offset || offset > (int64_t) max_offset)
        return false;
    if ((offset % alignment) != 0)
        return false;
    *out = (int32_t) offset;
    return true;
}

static inline bool xm_patch_calc_x64_rel32(uint32_t patch_pos, uint32_t target_offset,
                                           int32_t *out) {
    int64_t offset = (int64_t) target_offset - ((int64_t) patch_pos + 4);
    return xm_patch_offset_i32(offset, XM_PATCH_X64_REL32_MIN, XM_PATCH_X64_REL32_MAX,
                               XM_PATCH_X64_REL32_ALIGN, out);
}

static inline bool xm_patch_calc_rv64_jal(uint32_t emit_idx, uint32_t target_idx, int32_t *out) {
    int64_t offset = ((int64_t) target_idx - (int64_t) emit_idx) * 4;
    return xm_patch_offset_i32(offset, XM_PATCH_RV64_JAL_MIN, XM_PATCH_RV64_JAL_MAX,
                               XM_PATCH_RV64_JAL_ALIGN, out);
}

static inline bool xm_patch_calc_rv64_branch(uint32_t emit_idx, uint32_t target_idx, int32_t *out) {
    int64_t offset = ((int64_t) target_idx - (int64_t) emit_idx) * 4;
    return xm_patch_offset_i32(offset, XM_PATCH_RV64_BRANCH_MIN, XM_PATCH_RV64_BRANCH_MAX,
                               XM_PATCH_RV64_BRANCH_ALIGN, out);
}

static inline bool xm_patch_calc_arm64_b(uint32_t emit_idx, uint32_t target_idx, int32_t *out) {
    int64_t offset = (int64_t) target_idx - (int64_t) emit_idx;
    return xm_patch_offset_i32(offset, XM_PATCH_ARM64_B_MIN, XM_PATCH_ARM64_B_MAX,
                               XM_PATCH_ARM64_B_ALIGN, out);
}

static inline bool xm_patch_calc_arm64_bcond(uint32_t emit_idx, uint32_t target_idx, int32_t *out) {
    int64_t offset = (int64_t) target_idx - (int64_t) emit_idx;
    return xm_patch_offset_i32(offset, XM_PATCH_ARM64_BCOND_MIN, XM_PATCH_ARM64_BCOND_MAX,
                               XM_PATCH_ARM64_BCOND_ALIGN, out);
}

static inline bool xm_patch_calc_arm64_tb(uint32_t emit_idx, uint32_t target_idx, int32_t *out) {
    int64_t offset = (int64_t) target_idx - (int64_t) emit_idx;
    return xm_patch_offset_i32(offset, XM_PATCH_ARM64_TB_MIN, XM_PATCH_ARM64_TB_MAX,
                               XM_PATCH_ARM64_TB_ALIGN, out);
}

#endif  // XM_PATCH_VERIFY_H
