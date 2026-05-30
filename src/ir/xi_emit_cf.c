/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_cf.c - Block emission, phi elimination, jump patching
 *
 * Emits blocks in RPO order with branch fusion, handles phi-move
 * insertion at control-flow edges, and patches forward jumps and
 * try/catch target addresses after all blocks are emitted.
 */

#include "xi_emit_internal.h"
#include "../runtime/value/xtype.h"
#include "../module/xmodule.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"

/* ========== Phi Elimination ========== */

/* Emit MOVE instructions for phi nodes when transitioning from
 * predecessor 'pred' to successor 'succ'. */
XR_FUNC void emit_phi_moves(EmitCtx *ctx, XiBlock *pred, XiBlock *succ) {
    /* Find which predecessor index 'pred' is in succ->preds */
    int pred_idx = -1;
    for (uint16_t p = 0; p < succ->npreds; p++) {
        if (succ->preds[p] == pred) {
            pred_idx = (int) p;
            break;
        }
    }
    if (pred_idx < 0)
        return;

    for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
        if ((uint16_t) pred_idx >= phi->value.nargs)
            continue;
        XiValue *src = phi->value.args[pred_idx];
        if (!src)
            continue;

        uint8_t dst_reg = reg_of(ctx, &phi->value);
        uint8_t src_reg = reg_of(ctx, src);
        if (ctx->status != XI_EMIT_OK)
            return;

        if (dst_reg != src_reg) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst_reg, src_reg, 0));
        }
    }
}

/* ========== Block Emission ========== */

/* Check if a comparison value is only used as the block control (no other
 * consumers).  If so, we can fuse it into the branch-form opcode.
 * Must also check successor blocks' phi nodes — a phi can reference the
 * comparison as a cross-block operand (e.g. && short-circuit merge). */
static bool can_fuse_cmp(XiBlock *blk, XiValue *ctrl) {
    XR_DCHECK(ctrl != NULL, "ctrl must not be NULL");
    uint16_t op = ctrl->op;
    if (op < XI_EQ || op > XI_GE || ctrl->nargs < 2)
        return false;
    /* Ensure no other value in this block uses the comparison result */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v == ctrl)
            continue;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (v->args[a] == ctrl)
                return false;
        }
    }
    /* Ensure no phi anywhere in the function references this comparison.
     * The && short-circuit pattern creates phis two hops away
     * (IF → skip → merge), so checking only direct successors is
     * insufficient. Walking all blocks is O(n) but n is small. */
    XiFunc *f = blk->func;
    XR_DCHECK(f != NULL, "block must belong to a function");
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *b = f->blocks[bi];
        for (XiPhi *phi = b->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == ctrl)
                    return false;
            }
        }
    }
    return true;
}

/* Emit OP_SET_EXPORT for all named exports in the module-level function.
 * Called just before the return terminator.  Each export is written to
 * a compile-time-known dense slot in the current module's export table.
 *
 * Format:  OP_SET_EXPORT A=slot, B=reg(value), C=const? */
static void emit_module_exports(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    if (!ctx->isolate)
        return;

    XiModule *mod = f->module;
    if (!mod || !mod->exports || mod->nexports == 0)
        return;

    uint8_t tmp = ctx->next_reg;
    if ((int) (tmp + 1) > ctx->max_reg)
        ctx->max_reg = (uint8_t) (tmp + 1);

    for (uint16_t ei = 0; ei < mod->nexports; ei++) {
        const XiModuleExport *exp = &mod->exports[ei];
        XR_DCHECK(exp->name != NULL, "emit_module_exports: NULL export name");
        XR_DCHECK(ei <= 255, "emit_module_exports: too many exports for ABC format");
        int name_idx = add_const_string(ctx, exp->name);
        if (ctx->status != XI_EMIT_OK)
            return;
        XR_DCHECK(name_idx <= 255, "emit_module_exports: name const index overflow");
        emit_inst(ctx, CREATE_ABx(OP_GETSHARED, tmp, (int) exp->shared_slot));
        emit_inst(ctx, CREATE_ABC(OP_SET_EXPORT, (uint8_t) ei, tmp, (uint8_t) name_idx));
    }
}

/* Emit re-export bytecodes using OP_LOAD_MODULE_SLOT + OP_SET_EXPORT.
 * For star re-exports without graph resolution, falls back to
 * GETPROP-based extraction from the module object. */
static void emit_reexports(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    if (!f->reexports || f->reexport_count == 0 || !ctx->isolate)
        return;

    uint8_t val_reg = ctx->next_reg;
    if ((int) (val_reg + 1) > ctx->max_reg)
        ctx->max_reg = (uint8_t) (val_reg + 1);

    /* Re-exports are appended after direct exports, so the export
     * slot starts at f->module->nexports (the module's own exports). */
    XiModule *mod = f->module;
    uint16_t next_export_slot = mod ? mod->nexports : 0;

    for (uint16_t i = 0; i < f->reexport_count; i++) {
        XiReexportEntry *re = &f->reexports[i];
        if (!re->from_path)
            continue;
        if (ctx->status != XI_EMIT_OK)
            return;

        /* Resolve the source module (already loaded via topo-order pre-load
         * or runtime import cache). */
        XrValue mod_val = xr_module_import(ctx->isolate, re->from_path);
        if (XR_IS_NULL(mod_val))
            continue;
        XrModule *src = xr_value_to_module(mod_val);
        if (!src)
            continue;

        /* Find the source module's topo index for graph-resolved emit */
        XrModuleRegistry *mreg = (XrModuleRegistry *) xr_isolate_get_module_registry(ctx->isolate);
        int src_topo = -1;
        if (mreg && mreg->module_table) {
            for (int ti = 0; ti < mreg->module_table_count; ti++) {
                if (mreg->module_table[ti] == src) {
                    src_topo = ti;
                    break;
                }
            }
        }

        uint8_t member_reg = val_reg;
        if ((int) (member_reg + 1) > ctx->max_reg)
            ctx->max_reg = (uint8_t) (member_reg + 1);

        if (!re->name) {
            /* Star re-export: expand all exports from source module.
             * Enumerate source exports and emit load + SET_EXPORT for each. */
            XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);

            /* Need a second scratch reg for IMPORT + GETPROP fallback */
            uint8_t mod_scratch = (uint8_t) (member_reg + 1);
            if ((int) (mod_scratch + 1) > ctx->max_reg)
                ctx->max_reg = (uint8_t) (mod_scratch + 1);

            for (uint16_t ei = 0; ei < src->export_count; ei++) {
                if (ctx->status != XI_EMIT_OK)
                    return;
                XR_DCHECK(next_export_slot <= 255, "emit_reexports: too many slots");

                const char *exp_name =
                    xr_symbol_get_name_in_table(sym_table, src->export_symbols[ei]);
                if (!exp_name)
                    continue;

                if (src_topo >= 0 && src_topo <= 255 && ei <= 255) {
                    /* Graph-resolved fast path */
                    emit_inst(ctx, CREATE_ABC(OP_LOAD_MODULE_SLOT, member_reg, (uint8_t) src_topo,
                                              (uint8_t) ei));
                } else {
                    /* Fallback: OP_IMPORT + OP_GETPROP */
                    int path_idx = add_const_string(ctx, re->from_path);
                    if (ctx->status != XI_EMIT_OK)
                        return;
                    int sym_idx = add_symbol(ctx, exp_name);
                    if (ctx->status != XI_EMIT_OK)
                        return;
                    emit_inst(ctx, CREATE_ABx(OP_IMPORT, mod_scratch, path_idx));
                    emit_inst(ctx,
                              CREATE_ABC(OP_GETPROP, member_reg, mod_scratch, (uint8_t) sym_idx));
                }

                int exp_name_idx = add_const_string(ctx, exp_name);
                if (ctx->status != XI_EMIT_OK)
                    return;
                XR_DCHECK(exp_name_idx <= 255, "emit_reexports: name overflow");
                emit_inst(ctx, CREATE_ABC(OP_SET_EXPORT, (uint8_t) next_export_slot, member_reg,
                                          (uint8_t) exp_name_idx));
                next_export_slot++;
            }
            continue;
        }

        /* Selective re-export: find member's export slot in source module */
        XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);
        SymbolId sym = xr_symbol_lookup_in_table(sym_table, re->name);
        int src_slot = -1;
        if (sym >= 0) {
            if (src->symbol_to_index && sym >= src->min_symbol && sym <= src->max_symbol) {
                src_slot = src->symbol_to_index[sym - src->min_symbol];
            }
            if (src_slot < 0) {
                for (uint16_t ei = 0; ei < src->export_count; ei++) {
                    if (src->export_symbols[ei] == sym) {
                        src_slot = (int) ei;
                        break;
                    }
                }
            }
        }

        if (src_topo >= 0 && src_slot >= 0 && src_topo <= 255 && src_slot <= 255) {
            emit_inst(ctx, CREATE_ABC(OP_LOAD_MODULE_SLOT, member_reg, (uint8_t) src_topo,
                                      (uint8_t) src_slot));
        } else {
            int path_idx = add_const_string(ctx, re->from_path);
            if (ctx->status != XI_EMIT_OK)
                return;
            int sym_idx = add_symbol(ctx, re->name);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABx(OP_IMPORT, member_reg, path_idx));
            emit_inst(ctx, CREATE_ABC(OP_GETPROP, member_reg, member_reg, (uint8_t) sym_idx));
        }

        XR_DCHECK(next_export_slot <= 255, "emit_reexports: too many export slots");
        const char *export_name = re->alias ? re->alias : re->name;
        int exp_name_idx = add_const_string(ctx, export_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        XR_DCHECK(exp_name_idx <= 255, "emit_reexports: name const index overflow");
        emit_inst(ctx, CREATE_ABC(OP_SET_EXPORT, (uint8_t) next_export_slot, member_reg,
                                  (uint8_t) exp_name_idx));
        next_export_slot++;
    }
}

XR_FUNC void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk) {
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Record block start PC */
    XR_DCHECK(blk->id < ctx->block_pc_size, "block_id out of range");
    ctx->block_pc[blk->id] = current_pc(ctx);

    /* Detect fuseable comparison for IF blocks */
    ctx->fused_cmp = NULL;
    if (blk->kind == XI_BLOCK_IF && blk->control)
        if (can_fuse_cmp(blk, blk->control))
            ctx->fused_cmp = blk->control;

    /* Emit instruction values with register recycling */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        ctx->current_ordinal++;
        ctx->current_line = (int) v->line;
        emit_value(ctx, v);
        if (ctx->status != XI_EMIT_OK)
            return;
        /* Recycle registers of args whose last use was this instruction.
         * Skip recycling for the fused comparison's args — they are still
         * needed by the branch-form opcode in the terminator. */
        if (ctx->last_use && v != ctx->fused_cmp)
            try_free_args(ctx, v);
        /* Dead values (never referenced as an arg by any later instruction)
         * still need a destination register for bytecode emission, but that
         * register can be freed immediately for reuse.
         * Coalesced registers (var_id != 0xFF) are never freed. */
        if (ctx->last_use && v->id < ctx->reg_map_size && ctx->last_use[v->id] == 0 &&
            v->var_id == 0xFF) {
            uint8_t r = ctx->reg_map[v->id];
            if (r != NO_REG) {
                ctx->reg_map[v->id] = NO_REG;
                free_reg(ctx, r);
            }
        }
    }

    ctx->current_ordinal++; /* ordinal for terminator */
    /* Emit terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            /* Emit SET_EXPORT instructions for module-level exports
             * before returning, so the module's export_values[] is populated. */
            emit_module_exports(ctx);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_reexports(ctx);
            if (ctx->status != XI_EMIT_OK)
                return;

            if (blk->control && blk->control->op == XI_MULTI_RET) {
                /* Multi-value return: values in consecutive regs */
                uint8_t base = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK)
                    return;
                uint8_t nret = (uint8_t) blk->control->nargs;
                emit_inst(ctx, CREATE_ABC(OP_RETURN, base, nret, 0));
            } else if (blk->control) {
                uint8_t r = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK)
                    return;
                /* If the return value was cell-wrapped for closure capture,
                 * dereference the cell to return the actual value. */
                if ((blk->control->id < ctx->reg_map_size && ctx->cell_wrapped[blk->control->id]) ||
                    (blk->control->var_id != 0xFF &&
                     ctx->cell_side_reg[blk->control->var_id] != NO_REG)) {
                    emit_inst(ctx, CREATE_ABC(OP_CELL_GET, r, r, 0));
                }
                emit_inst(ctx, CREATE_ABC(OP_RETURN1, r, 0, 0));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_RETURN0, 0, 0, 0));
            }
            break;

        case XI_BLOCK_PLAIN: {
            XiBlock *succ = blk->succs[0];
            if (!succ)
                break;
            /* Emit phi moves for successor */
            emit_phi_moves(ctx, blk, succ);
            if (ctx->status != XI_EMIT_OK)
                return;
            /* Jump to successor (skip if it's the next block in RPO) */
            if (succ != next_blk) {
                int jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0)); /* placeholder */
                xi_emit_add_patch(ctx, jmp_pc, succ->id);
            }
            break;
        }

        case XI_BLOCK_IF: {
            if (!blk->control) {
                emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                return;
            }

            XiBlock *then_b = blk->succs[0];
            XiBlock *else_b = blk->succs[1];
            XR_DCHECK(then_b && else_b, "IF block missing successor");

            if (ctx->fused_cmp) {
                /* Fused comparison-branch: emit OP_LT/LE/EQ directly.
                 * Saves one instruction vs OP_CMP_* + OP_TEST. */
                XiValue *cmp = ctx->fused_cmp;
                XiValue *lhs = cmp->args[0];
                XiValue *rhs = cmp->args[1];
                uint8_t a = reg_of(ctx, lhs);
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK)
                    return;

                /* Unwrap cells: fused cmp bypasses emit_value arg unwrap.
                 * Every cell-wrapped variable must be dereferenced here;
                 * failure to do so means comparing the cell pointer instead
                 * of the contained value. */
                if (lhs->var_id != 0xFF && ctx->cell_side_reg[lhs->var_id] != NO_REG) {
                    uint8_t tmp = ctx->next_reg++;
                    if (ctx->next_reg > ctx->max_reg)
                        ctx->max_reg = ctx->next_reg;
                    emit_inst(ctx, CREATE_ABC(OP_CELL_GET, tmp, a, 0));
                    a = tmp;
                }
                if (rhs->var_id != 0xFF && ctx->cell_side_reg[rhs->var_id] != NO_REG) {
                    uint8_t tmp = ctx->next_reg++;
                    if (ctx->next_reg > ctx->max_reg)
                        ctx->max_reg = ctx->next_reg;
                    emit_inst(ctx, CREATE_ABC(OP_CELL_GET, tmp, b, 0));
                    b = tmp;
                }

                /* Verify cell unwrap: after unwrapping, the comparison
                 * registers must NOT be the raw cell registers. */
                XR_DCHECK(lhs->var_id == 0xFF || ctx->cell_side_reg[lhs->var_id] == NO_REG ||
                              a != ctx->cell_side_reg[lhs->var_id],
                          "fused cmp LHS still holds cell register");
                XR_DCHECK(rhs->var_id == 0xFF || ctx->cell_side_reg[rhs->var_id] == NO_REG ||
                              b != ctx->cell_side_reg[rhs->var_id],
                          "fused cmp RHS still holds cell register");

                /* Determine branch-form opcode and sense.  GT/GE swap args. */
                OpCode branch_op;
                int k = 0;
                bool swap = false;
                switch (cmp->op) {
                    case XI_LT:
                        branch_op = OP_LT;
                        break;
                    case XI_LE:
                        branch_op = OP_LE;
                        break;
                    case XI_GT:
                        branch_op = OP_LT;
                        swap = true;
                        break;
                    case XI_GE:
                        branch_op = OP_LE;
                        swap = true;
                        break;
                    case XI_EQ:
                        branch_op = OP_EQ;
                        break;
                    case XI_NE:
                        branch_op = OP_EQ;
                        k = 1;
                        break;
                    default:
                        branch_op = OP_EQ;
                        break;
                }
                if (swap) {
                    uint8_t t = a;
                    a = b;
                    b = t;
                }

                /* Try immediate form (OP_LTI/LEI/EQI) when RHS is small int */
                bool is_imm = false;
                XiValue *imm_arg = swap ? lhs : rhs; /* the "B" operand */
                if (imm_arg->op == XI_CONST && imm_arg->type &&
                    imm_arg->type->kind == XR_KIND_INT && imm_arg->aux_int >= -128 &&
                    imm_arg->aux_int <= 127 &&
                    (branch_op == OP_LT || branch_op == OP_LE || branch_op == OP_EQ)) {
                    OpCode imm_op = branch_op == OP_LT   ? OP_LTI
                                    : branch_op == OP_LE ? OP_LEI
                                                         : OP_EQI;
                    /* Use already cell-unwrapped 'a' register (post-swap). */
                    uint8_t ra = a;
                    int8_t imm = (int8_t) imm_arg->aux_int;
                    emit_inst(ctx, CREATE_ABC(imm_op, ra, (uint8_t) imm, (uint8_t) k));
                    is_imm = true;
                }

                if (!is_imm) {
                    emit_inst(ctx, CREATE_ABC(branch_op, a, b, (uint8_t) k));
                }
            } else {
                /* Non-fused path: TEST cond, skip next if cond is false */
                uint8_t cond = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK)
                    return;
                emit_inst(ctx, CREATE_ABC(OP_TEST, cond, 0, 0));
            }

            /* Emit phi moves for both branch edges.
             *
             * The CMP/TEST instruction conditionally skips the next instruction,
             * so we cannot insert moves between it and the JMP.  When the else
             * path needs phi resolution we route through a trampoline:
             *
             *   CMP a b k
             *   JMP → trampoline           (skip if condition true)
             *   [then phi moves]
             *   JMP → then_b
             *   trampoline:
             *   [else phi moves]
             *   JMP → else_b  (or fall through)
             */
            bool else_has_phis = (else_b->phis != NULL);

            if (!else_has_phis) {
                /* Fast path: no phis on else edge, direct JMP */
                int else_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0)); /* placeholder */
                xi_emit_add_patch(ctx, else_jmp_pc, else_b->id);

                emit_phi_moves(ctx, blk, then_b);
                if (ctx->status != XI_EMIT_OK)
                    return;

                if (then_b != next_blk) {
                    int then_jmp_pc = current_pc(ctx);
                    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                    xi_emit_add_patch(ctx, then_jmp_pc, then_b->id);
                }
            } else {
                /* Else target has phis — use trampoline for phi moves */
                int else_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0)); /* placeholder → trampoline */

                emit_phi_moves(ctx, blk, then_b);
                if (ctx->status != XI_EMIT_OK)
                    return;

                /* Always emit JMP to then_b (trampoline follows) */
                int then_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                xi_emit_add_patch(ctx, then_jmp_pc, then_b->id);

                /* Trampoline: patch the else JMP to land here */
                int trampoline_pc = current_pc(ctx);
                int else_offset = trampoline_pc - (else_jmp_pc + 1);
                PROTO_SET_CODE(ctx->proto, else_jmp_pc, CREATE_sJ(OP_JMP, else_offset));

                emit_phi_moves(ctx, blk, else_b);
                if (ctx->status != XI_EMIT_OK)
                    return;

                if (else_b != next_blk) {
                    int final_jmp_pc = current_pc(ctx);
                    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                    xi_emit_add_patch(ctx, final_jmp_pc, else_b->id);
                }
            }
            break;
        }

        case XI_BLOCK_UNREACHABLE:
            /* Emit a NOP as placeholder */
            emit_inst(ctx, CREATE_ABC(OP_NOP, 0, 0, 0));
            break;

        default:
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
    }
}

/* ========== Jump Patching ========== */

XR_FUNC void patch_jumps(EmitCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        int pc = ctx->patches[i].pc;
        uint32_t bid = ctx->patches[i].target_bid;

        XR_DCHECK(bid < ctx->block_pc_size, "patch: bad block id");
        int target_pc = ctx->block_pc[bid];

        if (target_pc < 0) {
            /* Unreachable target — should not happen in valid IR.
             * Point to current PC as fallback. */
            target_pc = pc + 1;
        }

        /* sJ = target_pc - (pc + 1) */
        int offset = target_pc - (pc + 1);
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_sJ(OP_JMP, offset);
    }

    /* Patch OP_TRY instructions: Bx = absolute catch PC (0 = no catch);
     * Patch the following NOP with finally PC (Bx). */
    for (uint32_t i = 0; i < ctx->ntry_patch; i++) {
        int pc = ctx->try_patches[i].pc;
        uint32_t bid = ctx->try_patches[i].target_bid;
        uint32_t fbid = ctx->try_patches[i].finally_bid;

        /* catch_offset: 0 means "no catch clause" — the VM will skip to
         * the finally handler on exception instead of running OP_CATCH. */
        int target_pc = 0;
        if (bid > 0) {
            XR_DCHECK(bid < ctx->block_pc_size, "try_patch: bad catch block id");
            target_pc = ctx->block_pc[bid];
            if (target_pc < 0)
                target_pc = pc + 1;
        }
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_ABx(OP_TRY, 0, target_pc);

        /* Patch NOP at pc+1 with finally absolute PC */
        int fin_pc = -1;
        if (fbid > 0 && fbid < ctx->block_pc_size) {
            fin_pc = ctx->block_pc[fbid];
            if (fin_pc >= 0) {
                XrInstruction *fin_inst = PROTO_CODE_PTR(ctx->proto, pc + 1);
                *fin_inst = CREATE_ABx(OP_NOP, 0, fin_pc);
            }
        }
    }
}
