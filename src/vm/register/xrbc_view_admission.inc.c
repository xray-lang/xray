/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_view_admission.inc.c - Typed activation capabilities at external input
 */
typedef struct RbcViewSignature {
    XrRbcRegisterKind result, parameters[6];
    uint32_t count;
    bool produces;
} RbcViewSignature;

#define RBC_VIEW_KIND_XR_LIR_UNIT XR_RBC_BITS
#define RBC_VIEW_KIND_XR_LIR_VIEW_FRAME XR_RBC_VIEW_FRAME
#define RBC_VIEW_KIND_XR_LIR_VIEW_CELL XR_RBC_VIEW_CELL
#define RBC_VIEW_KIND_XR_LIR_SLICE_I64 XR_RBC_SLICE_I64
#define RBC_VIEW_KIND_XR_LIR_ARRAY XR_RBC_ARRAY_I64
#define RBC_VIEW_KIND_XR_LIR_STATE XR_RBC_STATE
#define RBC_VIEW_KIND_XR_LIR_STATE_REF XR_RBC_STATE_REF
#define RBC_VIEW_KIND_XR_LIR_I64 XR_RBC_BITS
#define RBC_VIEW_KIND_XR_LIR_U32 XR_RBC_BITS
#define RBC_VIEW_KIND_XR_LIR_I1 XR_RBC_BITS
#define RBC_VIEW_KIND_0 XR_RBC_BITS
#define RBC_VIEW_RESULT_XR_LIR_UNIT false
#define RBC_VIEW_RESULT_XR_LIR_VIEW_CELL true
#define RBC_VIEW_RESULT_XR_LIR_SLICE_I64 true
#define RBC_VIEW_RESULT_XR_LIR_ARRAY true
#define RBC_VIEW_RESULT_XR_LIR_STATE_REF true
#define RBC_VIEW_RESULT_XR_LIR_I64 true
static const RbcViewSignature rbc_view_signatures[] = {
    {0},
#define XR_LIR_VIEW_OP(name, symbol, result, count, a, b, c, d, e, f) \
    {RBC_VIEW_KIND_##result, {RBC_VIEW_KIND_##a, RBC_VIEW_KIND_##b, RBC_VIEW_KIND_##c, \
        RBC_VIEW_KIND_##d, RBC_VIEW_KIND_##e, RBC_VIEW_KIND_##f}, count, RBC_VIEW_RESULT_##result},
#include "lir/xlir_view_ops.def"
#undef XR_LIR_VIEW_OP
    {XR_RBC_VIEW_FRAME, {0}, 0, true},
    {XR_RBC_BITS, {XR_RBC_VIEW_FRAME}, 1, false}
};
_Static_assert(sizeof(rbc_view_signatures) / sizeof(*rbc_view_signatures) == XR_RBC_VIEW_FRAME_LEAVE + 1,
    "Every view operation retains one closed signature");

static bool rbc_view_metadata_valid(const XrRbcFunction *f) {
    if (f->view_accesses ? f->view_access_count > 65536 : f->view_access_count != 0) return false;
    for (size_t n = 0; n < f->view_access_count; ++n) {
        const XrRbcViewAccess *row = &f->view_accesses[n];
        if (row->operation <= XR_RBC_VIEW_NONE || row->operation > XR_RBC_VIEW_FRAME_LEAVE) return false;
        const RbcViewSignature *shape = &rbc_view_signatures[row->operation];
        if (row->count != shape->count || (row->bits && row->operation != XR_RBC_VIEW_FRAME_ENTER &&
            row->operation != XR_RBC_VIEW_CELL_CONSTRUCT)) return false;
        if (shape->produces) {
            if (!rbc_array_location_valid(f, row->result) ||
                rbc_array_location_kind(f, row->result) != shape->result) return false;
        } else if (row->result.index || row->result.spill) return false;
        for (uint32_t p = 0; p < row->count; ++p)
            if (!rbc_array_location_valid(f, row->operands[p]) ||
                rbc_array_location_kind(f, row->operands[p]) != shape->parameters[p]) return false;
        for (uint32_t p = row->count; p < 6; ++p)
            if (row->operands[p].index || row->operands[p].spill) return false;
    }
    return true;
}

static bool rbc_view_module_valid(const XrRbcModule *m, const XrRbcFunction *f) {
    for (size_t n = 0; n < f->view_access_count; ++n) {
        const XrRbcViewAccess *row = &f->view_accesses[n];
        if (row->operation != XR_RBC_VIEW_CELL_CONSTRUCT) continue;
        XrtModuleLayoutSpec layout = {0};
        if (xrt_module_layout_view(m->state_layout, &layout) != XR_XIR_VALUE_OK ||
            row->bits >= layout.slot_count) return false;
        XrtModuleSlotSpec slot = layout.slots[row->bits];
        if (slot.type >= layout.type_count || layout.types[slot.type].kind != XRT_MODULE_CELL_ARRAY_I64 ||
            !(slot.flags & XRT_MODULE_SLOT_MUTABLE)) return false;
    }
    return true;
}

static bool rbc_view_function_valid(const XrRbcFunction *f) {
    bool needed = f->view_access_count || f->result_kind == XR_RBC_SLICE_I64;
    for (uint32_t p = 0; p < f->parameter_count; ++p) needed |= xr_rbc_view_kind(rbc_register_kind(f, p));
    if (!needed) return true;
    if ((f->code[0] & 255) != XR_RBC_VIEW_ACCESS ||
        f->code[0] >> 16 >= f->view_access_count ||
        f->view_accesses[f->code[0] >> 16].operation != XR_RBC_VIEW_FRAME_ENTER) return false;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t word = f->code[pc], op = word & 255;
        if (op == XR_RBC_VIEW_ACCESS) {
            uint32_t index = word >> 16;
            if (((word >> 8) & 255) || index >= f->view_access_count) return false;
            XrRbcViewOperation role = f->view_accesses[index].operation;
            if (role == XR_RBC_VIEW_FRAME_ENTER && pc) return false;
            if (role == XR_RBC_VIEW_FRAME_LEAVE && (pc + 1 == f->code_count ||
                ((f->code[pc + 1] & 255) != XR_RBC_RET && (f->code[pc + 1] & 255) != XR_RBC_RET0 &&
                    !xr_rbc_aggregate_return_word(f->aggregate_accesses, f->aggregate_access_count, f->code[pc + 1])))) return false;
        }
        if (op == XR_RBC_RET || op == XR_RBC_RET0 ||
            xr_rbc_aggregate_return_word(f->aggregate_accesses, f->aggregate_access_count, word)) {
            if (!pc || (f->code[pc - 1] & 255) != XR_RBC_VIEW_ACCESS ||
                f->code[pc - 1] >> 16 >= f->view_access_count ||
                f->view_accesses[f->code[pc - 1] >> 16].operation != XR_RBC_VIEW_FRAME_LEAVE) return false;
        }
    }
    return true;
}
