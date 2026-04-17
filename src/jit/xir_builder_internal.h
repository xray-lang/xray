/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder_internal.h - Internal shared declarations for XIR builder
 */

#ifndef XIR_BUILDER_INTERNAL_H
#define XIR_BUILDER_INTERNAL_H

#include "xir_builder.h"
#include "xir_blueprint.h"
#include "xir_fold.h"
#include "../runtime/value/xtype_feedback.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xshape.h"
#include "../vm/xic_field_table.h"
#include "../frontend/analyzer/xanalyzer_symbol.h"
#include "../runtime/gc/xgc_header.h"
#include "xir_jit_runtime.h"
#include "xir_offsets.h"
#include "../runtime/object/xjson.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xdefs.h"

#define JIT_CALL_ARGS_OFFSET      XIR_JIT_CALL_ARGS_OFFSET
#define JIT_CALL_PROTO_OFFSET     XIR_JIT_CALL_PROTO_OFFSET
#define JIT_RET_COUNT_OFFSET      XIR_JIT_RET_COUNT_OFFSET
#define JIT_RET_VALS_OFFSET       XIR_JIT_RET_VALS_OFFSET
#define JIT_RET_TAGS_OFFSET       XIR_JIT_RET_TAGS_OFFSET
#define XGC_HEADER_SIZE           XIR_GC_HEADER_SIZE
#define XR_XRVALUE_SIZE           XIR_XRVALUE_SIZE
#define XR_XRVALUE_TAG_OFFSET     XIR_XRVALUE_TAG_OFFSET
#define XR_INSTANCE_FIELDS_OFFSET XIR_INSTANCE_FIELDS_OFFSET
#define XR_JSON_FIELDS_OFFSET     XIR_JSON_FIELDS_OFFSET

// AOT sentinel functions (defined in xir_builder.c)
XR_FUNC int64_t xrt_invoke_method_sentinel(struct XrCoroutine *c, int64_t x);
XR_FUNC int64_t xrt_strbuf_new_sentinel(struct XrCoroutine *c, int64_t x);
XR_FUNC int64_t xrt_strbuf_append_sentinel(struct XrCoroutine *c, int64_t x);
XR_FUNC int64_t xrt_strbuf_finish_sentinel(struct XrCoroutine *c, int64_t x);

// Returns the XirVReg* for the current SSA def in slot reg, or NULL.
static inline XirVReg *builder_vreg_for_slot(XirBuilder *b, int reg) {
    if (reg < 0 || reg >= 256) return NULL;
    XirRef ref = b->slot_map[reg];
    if (!xir_ref_is_vreg(ref)) return NULL;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= b->func->nvreg) return NULL;
    return &b->func->vregs[vi];
}

// Same as builder_vreg_for_slot but works on an arbitrary XirRef.
static inline XirVReg *builder_vreg_ref(XirBuilder *b, XirRef ref) {
    if (!xir_ref_is_vreg(ref)) return NULL;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= b->func->nvreg) return NULL;
    return &b->func->vregs[vi];
}

// Shared helpers (defined in xir_builder.c)
XR_FUNC uint8_t xr_slot_to_rep(uint8_t slot_type);
XR_FUNC int builder_derive_type_hint(XirBuilder *b, int recv_reg);
XR_FUNC bool builder_is_numeric_union(XirBuilder *b, int reg);
XR_FUNC struct XrType *builder_slot_xrtype(XirBuilder *b, int reg);
XR_FUNC struct XrType *builder_inst_xrtype(XirBuilder *b, uint32_t pc);
XR_FUNC struct XrType *builder_find_reg_type(XirBuilder *b, int reg);
XR_FUNC uint8_t builder_slot_xr_tag(XirBuilder *b, int slot);
XR_FUNC void builder_refine_slot_from_inst_type(XirBuilder *b, int slot);
XR_FUNC int builder_add_deopt_info(XirBuilder *b, uint32_t bc_pc);
XR_FUNC uint8_t builder_ref_sf_tag(XirBuilder *b, XirRef ref, int slot_reg);
XR_FUNC XirRef builder_get_slot(XirBuilder *b, XirBlock *blk, int reg);
XR_FUNC void builder_tag_vreg(XirBuilder *b, XirRef ref, uint8_t xr_tag, uint16_t heap_type);
XR_FUNC void builder_tag_from_slot(XirBuilder *b, XirRef ref, int dest_slot);

// Tag a vreg as boolean result. Use this for all ops that produce 0/1.
#define builder_tag_bool(b, ref) builder_tag_vreg((b), (ref), XRVREG_TAG_BOOL, 0)
XR_FUNC void builder_set_slot(XirBuilder *b, int reg, XirRef ref);
XR_FUNC void builder_set_slot_in_block(XirBuilder *b, uint32_t blk_id, int slot, XirRef ref);
XR_FUNC void builder_emit_shape_guard(XirBuilder *b, XirBlock *blk, XirRef obj, struct XrShape *shape, uint32_t pc);
XR_FUNC uint8_t ref_xir_type(XirFunc *func, XirRef ref);
XR_FUNC uint8_t ref_vtag(XirFunc *func, XirRef ref);
XR_FUNC XirRef braun_read_var(XirBuilder *b, uint32_t blk_id, int slot);
XR_FUNC void braun_seal_block(XirBuilder *b, XirBlock *blk);

/*
 * Bind call arguments to a CALL instruction's dst vreg via call_arg_pool.
 * Codegen will write these to coro->jit_ctx->call_args[] before the call.
 * This replaces the old STORE_CORO arg-passing pattern.
 */
static inline void builder_bind_call_args(XirBuilder *b, XirRef call_dst,
                                           const XirRef *args, uint16_t nargs) {
    xir_func_bind_call_args(b->func, call_dst, args, nargs);
}

// Sub-translation functions (defined in split files)
XR_FUNC bool xir_translate_object_ops(XirBuilder *b, XirBlock **cur_blk, uint32_t pc, XrInstruction inst, OpCode op);
XR_FUNC bool xir_translate_call_ops(XirBuilder *b, XirBlock **cur_blk, uint32_t pc, XrInstruction inst, OpCode op);
XR_FUNC bool xir_translate_misc_ops(XirBuilder *b, XirBlock **cur_blk, uint32_t pc, XrInstruction inst, OpCode op);

#endif // XIR_BUILDER_INTERNAL_H
