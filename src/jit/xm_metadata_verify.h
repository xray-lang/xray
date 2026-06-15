#ifndef XM_METADATA_VERIFY_H
#define XM_METADATA_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "xm_codegen.h"  // XmCodegenResult, XmOsrEntry, XmRtDeoptEntry, XmDeoptLocKind
#include "xm_offsets.h"

/* Spill area in XrJitScratch.deopt_spill_save[32]: 32 × 8 bytes = 256 bytes. */
#define XM_DEOPT_SPILL_SAVE_BYTES (32 * 8)

typedef enum {
    XM_META_VERIFY_OK = 0,
    /* Stackmap */
    XM_META_VERIFY_STACKMAP_BAD_MAGIC,
    XM_META_VERIFY_STACKMAP_COUNT_OVERFLOW,
    XM_META_VERIFY_STACKMAP_PC_OUT_OF_RANGE,
    XM_META_VERIFY_STACKMAP_PC_NOT_SORTED,
    XM_META_VERIFY_STACKMAP_PC_MISALIGNED,
    /* Deopt table */
    XM_META_VERIFY_DEOPT_COUNT_OVERFLOW,
    XM_META_VERIFY_DEOPT_ID_MISMATCH,
    XM_META_VERIFY_DEOPT_BC_PC_INVALID,
    XM_META_VERIFY_DEOPT_NSLOTS_OVERFLOW,
    XM_META_VERIFY_DEOPT_SLOT_LOC_KIND,
    XM_META_VERIFY_DEOPT_SPILL_OFFSET,
    /* OSR table */
    XM_META_VERIFY_OSR_COUNT_OVERFLOW,
    XM_META_VERIFY_OSR_ENTRY_OOB,
    XM_META_VERIFY_OSR_ENTRY_MISALIGNED,
    XM_META_VERIFY_OSR_NSLOTS_OVERFLOW,
    XM_META_VERIFY_OSR_BC_DUP,
    XM_META_VERIFY_SUSPEND_COUNT_OVERFLOW,
    XM_META_VERIFY_SUSPEND_MISSING_RESUME,
    XM_META_VERIFY_SUSPEND_RESUME_WITHOUT_POINTS,
    XM_META_VERIFY_SUSPEND_CONT_OOB,
    XM_META_VERIFY_SUSPEND_CONT_MISALIGNED,
    XM_META_VERIFY_SUSPEND_SMAP_OOB,
    XM_META_VERIFY_SUSPEND_TAG_OFFSET,
    XM_META_VERIFY_RESUME_ENTRY_OOB,
    XM_META_VERIFY_RESUME_ENTRY_MISALIGNED,
} XmMetaVerifyError;

static inline const char *xm_meta_verify_strerror(XmMetaVerifyError e) {
    switch (e) {
        case XM_META_VERIFY_OK:
            return "ok";
        case XM_META_VERIFY_STACKMAP_BAD_MAGIC:
            return "stackmap: bad magic";
        case XM_META_VERIFY_STACKMAP_COUNT_OVERFLOW:
            return "stackmap: count overflow";
        case XM_META_VERIFY_STACKMAP_PC_OUT_OF_RANGE:
            return "stackmap: pc_offset out of range";
        case XM_META_VERIFY_STACKMAP_PC_NOT_SORTED:
            return "stackmap: pc_offset not strictly ascending";
        case XM_META_VERIFY_STACKMAP_PC_MISALIGNED:
            return "stackmap: pc_offset misaligned";
        case XM_META_VERIFY_DEOPT_COUNT_OVERFLOW:
            return "deopt: ndeopt overflow";
        case XM_META_VERIFY_DEOPT_ID_MISMATCH:
            return "deopt: deopt_id != index";
        case XM_META_VERIFY_DEOPT_BC_PC_INVALID:
            return "deopt: bc_pc == UINT32_MAX";
        case XM_META_VERIFY_DEOPT_NSLOTS_OVERFLOW:
            return "deopt: nslots overflow";
        case XM_META_VERIFY_DEOPT_SLOT_LOC_KIND:
            return "deopt: slot loc_kind out of range";
        case XM_META_VERIFY_DEOPT_SPILL_OFFSET:
            return "deopt: spill_offset out of range or misaligned";
        case XM_META_VERIFY_OSR_COUNT_OVERFLOW:
            return "osr: nosr overflow";
        case XM_META_VERIFY_OSR_ENTRY_OOB:
            return "osr: entry_offset out of range";
        case XM_META_VERIFY_OSR_ENTRY_MISALIGNED:
            return "osr: entry_offset misaligned";
        case XM_META_VERIFY_OSR_NSLOTS_OVERFLOW:
            return "osr: nslots overflow";
        case XM_META_VERIFY_OSR_BC_DUP:
            return "osr: duplicate bc_offset";
        case XM_META_VERIFY_SUSPEND_COUNT_OVERFLOW:
            return "suspend: nsuspend overflow";
        case XM_META_VERIFY_SUSPEND_MISSING_RESUME:
            return "suspend: missing resume entry";
        case XM_META_VERIFY_SUSPEND_RESUME_WITHOUT_POINTS:
            return "suspend: resume entry without suspend points";
        case XM_META_VERIFY_SUSPEND_CONT_OOB:
            return "suspend: cont_offset out of range";
        case XM_META_VERIFY_SUSPEND_CONT_MISALIGNED:
            return "suspend: cont_offset misaligned";
        case XM_META_VERIFY_SUSPEND_SMAP_OOB:
            return "suspend: smap_id out of range";
        case XM_META_VERIFY_SUSPEND_TAG_OFFSET:
            return "suspend: result_tag_offset out of range";
        case XM_META_VERIFY_RESUME_ENTRY_OOB:
            return "resume: resume_entry_offset out of range";
        case XM_META_VERIFY_RESUME_ENTRY_MISALIGNED:
            return "resume: resume_entry_offset misaligned";
    }
    return "unknown";
}

/* Verify GC stack map invariants.
 *  - magic == XR_STACK_MAP_MAGIC
 *  - count <= XM_MAX_STACK_MAP_ENTRIES
 *  - every pc_offset < code_size
 *  - every pc_offset is a multiple of arch_align (1 for x64, 4 for arm64/rv64)
 *  - pc_offset strictly monotonically ascending (xstackmap.h header contract)
 * NULL table is treated as "no GC stack map produced" and accepted (used by
 * x64/riscv64 today: their GC scanner is not yet wired). */
static inline XmMetaVerifyError xm_verify_stackmap(const XrStackMapTable *table, uint32_t code_size,
                                                   uint32_t arch_align, uint32_t *err_idx) {
    if (table == NULL)
        return XM_META_VERIFY_OK;
    if (table->magic != XR_STACK_MAP_MAGIC) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_STACKMAP_BAD_MAGIC;
    }
    if (table->count > XM_MAX_STACK_MAP_ENTRIES) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_STACKMAP_COUNT_OVERFLOW;
    }
    bool has_prev = false;
    uint32_t prev_pc = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        uint32_t pc = table->entries[i].pc_offset;
        if (pc >= code_size) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_STACKMAP_PC_OUT_OF_RANGE;
        }
        if (arch_align > 1 && (pc % arch_align) != 0) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_STACKMAP_PC_MISALIGNED;
        }
        if (has_prev && pc <= prev_pc) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_STACKMAP_PC_NOT_SORTED;
        }
        prev_pc = pc;
        has_prev = true;
    }
    return XM_META_VERIFY_OK;
}

/* Verify runtime deopt table invariants.
 *  - entry.deopt_id == index (xm_jit_deopt_recover indexes by deopt_id)
 *  - entry.bc_pc != UINT32_MAX (record_deopt rejects this anchor)
 *  - per slot:
 *      loc_kind in [DEOPT_LOC_REG .. DEOPT_LOC_CONST_PTR]
 *      LOC_SPILL: spill_offset in [0, XM_DEOPT_SPILL_SAVE_BYTES) and 8-aligned
 * Table size and per-entry slot count are unbounded (dynamically allocated).
 * No bounds on bc_slot (negative is the "unmapped" sentinel and runtime skips). */
static inline XmMetaVerifyError xm_verify_deopt(const XmRtDeoptEntry *entries, uint32_t ndeopt,
                                                uint32_t *err_idx) {
    for (uint32_t i = 0; i < ndeopt; i++) {
        const XmRtDeoptEntry *e = &entries[i];
        if (e->deopt_id != (uint16_t) i) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_DEOPT_ID_MISMATCH;
        }
        if (e->bc_pc == UINT32_MAX) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_DEOPT_BC_PC_INVALID;
        }
        for (uint16_t s = 0; s < e->nslots; s++) {
            const XmRtDeoptSlot *sl = &e->slots[s];
            if (sl->loc_kind > (uint8_t) DEOPT_LOC_CONST_PTR) {
                if (err_idx)
                    *err_idx = i;
                return XM_META_VERIFY_DEOPT_SLOT_LOC_KIND;
            }
            if (sl->loc_kind == (uint8_t) DEOPT_LOC_SPILL) {
                int16_t off = sl->loc.spill_offset;
                if (off < 0 || off >= XM_DEOPT_SPILL_SAVE_BYTES || (off % 8) != 0) {
                    if (err_idx)
                        *err_idx = i;
                    return XM_META_VERIFY_DEOPT_SPILL_OFFSET;
                }
            }
        }
    }
    return XM_META_VERIFY_OK;
}

/* Verify OSR entry table invariants.
 *  - nosr <= XM_MAX_OSR_ENTRIES
 *  - entry.entry_offset < code_size
 *  - entry.entry_offset is a multiple of arch_align (1 for x64, 4 for fixed-32)
 *  - entry.nslots <= XM_MAX_OSR_SLOTS
 *  - bc_offset is unique across all entries (xm_jit_runtime_coro.c does linear
 *    match; duplicates would silently shadow later entries) */
static inline XmMetaVerifyError xm_verify_osr(const XmOsrEntry *entries, uint32_t nosr,
                                              uint32_t code_size, uint32_t arch_align,
                                              uint32_t *err_idx) {
    if (nosr > XM_MAX_OSR_ENTRIES) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_OSR_COUNT_OVERFLOW;
    }
    for (uint32_t i = 0; i < nosr; i++) {
        const XmOsrEntry *e = &entries[i];
        if (e->entry_offset >= code_size) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_OSR_ENTRY_OOB;
        }
        if (arch_align > 1 && (e->entry_offset % arch_align) != 0) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_OSR_ENTRY_MISALIGNED;
        }
        if (e->nslots > XM_MAX_OSR_SLOTS) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_OSR_NSLOTS_OVERFLOW;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (entries[j].bc_offset == e->bc_offset) {
                if (err_idx)
                    *err_idx = i;
                return XM_META_VERIFY_OSR_BC_DUP;
            }
        }
    }
    return XM_META_VERIFY_OK;
}

static inline XmMetaVerifyError xm_verify_suspend(const XmCodegenResult *result,
                                                  uint32_t arch_align, uint32_t *err_idx) {
    if (result->nsuspend > XM_MAX_SUSPEND_ENTRIES) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_SUSPEND_COUNT_OVERFLOW;
    }
    if (result->nsuspend == 0) {
        if (result->resume_entry_offset != 0) {
            if (err_idx)
                *err_idx = 0;
            return XM_META_VERIFY_SUSPEND_RESUME_WITHOUT_POINTS;
        }
        return XM_META_VERIFY_OK;
    }
    if (result->resume_entry_offset == 0) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_SUSPEND_MISSING_RESUME;
    }
    for (uint32_t i = 0; i < result->nsuspend; i++) {
        const XmSuspendEntry *e = &result->suspend_entries[i];
        if (e->cont_offset >= result->code_size) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_SUSPEND_CONT_OOB;
        }
        if (arch_align > 1 && (e->cont_offset % arch_align) != 0) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_SUSPEND_CONT_MISALIGNED;
        }
        if (result->stack_map != NULL && e->smap_id >= result->stack_map->count) {
            if (err_idx)
                *err_idx = i;
            return XM_META_VERIFY_SUSPEND_SMAP_OOB;
        }
        if (e->result_tag_offset >= 0) {
            int32_t min_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET;
            int32_t max_off = min_off + (int32_t) XR_JIT_MAX_VREG_TAGS;
            if (e->result_tag_offset < min_off || e->result_tag_offset >= max_off) {
                if (err_idx)
                    *err_idx = i;
                return XM_META_VERIFY_SUSPEND_TAG_OFFSET;
            }
        }
    }
    return XM_META_VERIFY_OK;
}

static inline XmMetaVerifyError xm_verify_resume_entry(uint32_t resume_entry_offset,
                                                       uint32_t code_size, uint32_t arch_align,
                                                       uint32_t *err_idx) {
    if (resume_entry_offset == 0)
        return XM_META_VERIFY_OK;
    if (resume_entry_offset >= code_size) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_RESUME_ENTRY_OOB;
    }
    if (arch_align > 1 && (resume_entry_offset % arch_align) != 0) {
        if (err_idx)
            *err_idx = 0;
        return XM_META_VERIFY_RESUME_ENTRY_MISALIGNED;
    }
    return XM_META_VERIFY_OK;
}

/* Composite verifier: checks stackmap, deopt, OSR, suspend, then resume metadata.
 * Returns first violation encountered; *err_idx points at the offending entry,
 * *err_kind is set to a static category string. */
static inline XmMetaVerifyError xm_verify_metadata(const XmCodegenResult *result,
                                                   uint32_t arch_align, uint32_t *err_idx,
                                                   const char **err_kind) {
    XmMetaVerifyError e =
        xm_verify_stackmap(result->stack_map, result->code_size, arch_align, err_idx);
    if (e != XM_META_VERIFY_OK) {
        if (err_kind)
            *err_kind = "stackmap";
        return e;
    }
    e = xm_verify_deopt(result->deopt_entries, result->ndeopt, err_idx);
    if (e != XM_META_VERIFY_OK) {
        if (err_kind)
            *err_kind = "deopt";
        return e;
    }
    e = xm_verify_osr(result->osr_entries, result->nosr, result->code_size, arch_align, err_idx);
    if (e != XM_META_VERIFY_OK) {
        if (err_kind)
            *err_kind = "osr";
        return e;
    }
    e = xm_verify_suspend(result, arch_align, err_idx);
    if (e != XM_META_VERIFY_OK) {
        if (err_kind)
            *err_kind = "suspend";
        return e;
    }
    e = xm_verify_resume_entry(result->resume_entry_offset, result->code_size, arch_align, err_idx);
    if (e != XM_META_VERIFY_OK) {
        if (err_kind)
            *err_kind = "resume";
        return e;
    }
    return XM_META_VERIFY_OK;
}

XR_FUNC bool xm_verify_metadata_or_fail(XmCodegenResult *result, uint32_t arch_align,
                                        const char *arch_name);

#endif  // XM_METADATA_VERIFY_H
