#include "xi_cfg_edit.h"
#include "../base/xchecks.h"
#include <string.h>

static void xi_cfg_phi_remove_arg(XiPhi *phi, uint16_t idx) {
    XR_DCHECK(phi != NULL, "xi_cfg_phi_remove_arg: NULL phi");
    XR_DCHECK(idx < phi->value.nargs, "xi_cfg_phi_remove_arg: index out of range");
    for (uint16_t j = idx; j + 1 < phi->value.nargs; j++)
        phi->value.args[j] = phi->value.args[j + 1];
    phi->value.nargs--;
}

static bool xi_cfg_phi_append_arg(XiBlock *blk, XiPhi *phi, XiValue *arg) {
    XR_DCHECK(blk != NULL, "xi_cfg_phi_append_arg: NULL block");
    XR_DCHECK(phi != NULL, "xi_cfg_phi_append_arg: NULL phi");
    uint16_t old_n = phi->value.nargs;
    XiValue **new_args =
        (XiValue **) xi_func_arena_alloc(blk->func, (uint32_t) (old_n + 1) * sizeof(XiValue *));
    if (!new_args)
        return false;
    if (old_n > 0)
        memcpy(new_args, phi->value.args, (size_t) old_n * sizeof(XiValue *));
    new_args[old_n] = arg;
    phi->value.args = new_args;
    phi->value.nargs = (uint16_t) (old_n + 1);
    return true;
}

XR_FUNC uint32_t xi_cfg_phi_count(const XiBlock *blk) {
    XR_DCHECK(blk != NULL, "xi_cfg_phi_count: NULL block");
    uint32_t n = 0;
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next)
        n++;
    return n;
}

XR_FUNC uint16_t xi_cfg_pred_index(const XiBlock *blk, const XiBlock *pred) {
    XR_DCHECK(blk != NULL, "xi_cfg_pred_index: NULL block");
    XR_DCHECK(pred != NULL, "xi_cfg_pred_index: NULL pred");
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred)
            return i;
    }
    return blk->npreds;
}

static bool xi_cfg_remove_pred_at(XiBlock *blk, uint16_t idx) {
    XR_DCHECK(blk != NULL, "xi_cfg_remove_pred_at: NULL block");
    if (idx >= blk->npreds)
        return false;
    for (uint16_t j = idx; j + 1 < blk->npreds; j++)
        blk->preds[j] = blk->preds[j + 1];
    blk->npreds--;
    for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
        if (idx < phi->value.nargs)
            xi_cfg_phi_remove_arg(phi, idx);
    }
    return true;
}

XR_FUNC bool xi_cfg_replace_successor(XiBlock *pred, XiBlock *old_succ, XiBlock *new_succ) {
    XR_DCHECK(pred != NULL, "xi_cfg_replace_successor: NULL pred");
    XR_DCHECK(old_succ != NULL, "xi_cfg_replace_successor: NULL old_succ");
    XR_DCHECK(new_succ != NULL, "xi_cfg_replace_successor: NULL new_succ");
    for (uint16_t i = 0; i < 2; i++) {
        if (pred->succs[i] == old_succ) {
            pred->succs[i] = new_succ;
            return true;
        }
    }
    return false;
}

XR_FUNC bool xi_cfg_replace_pred(XiBlock *blk, XiBlock *old_pred, XiBlock *new_pred) {
    XR_DCHECK(blk != NULL, "xi_cfg_replace_pred: NULL block");
    XR_DCHECK(old_pred != NULL, "xi_cfg_replace_pred: NULL old_pred");
    XR_DCHECK(new_pred != NULL, "xi_cfg_replace_pred: NULL new_pred");
    uint16_t idx = xi_cfg_pred_index(blk, old_pred);
    if (idx == blk->npreds)
        return false;
    blk->preds[idx] = new_pred;
    return true;
}

XR_FUNC bool xi_cfg_remove_pred(XiBlock *blk, XiBlock *pred) {
    XR_DCHECK(blk != NULL, "xi_cfg_remove_pred: NULL block");
    XR_DCHECK(pred != NULL, "xi_cfg_remove_pred: NULL pred");
    uint16_t idx = xi_cfg_pred_index(blk, pred);
    if (idx == blk->npreds)
        return false;
    return xi_cfg_remove_pred_at(blk, idx);
}

XR_FUNC bool xi_cfg_append_pred(XiBlock *blk, XiBlock *pred, XiValue **phi_args,
                                uint32_t nphi_args) {
    XR_DCHECK(blk != NULL, "xi_cfg_append_pred: NULL block");
    XR_DCHECK(pred != NULL, "xi_cfg_append_pred: NULL pred");
    uint32_t nphis = xi_cfg_phi_count(blk);
    if (nphis != nphi_args)
        return false;

    uint16_t appended_idx = blk->npreds;
    xi_block_add_pred(blk, pred);
    if (blk->npreds == 0 || blk->preds[blk->npreds - 1] != pred)
        return false;

    uint32_t i = 0;
    for (XiPhi *phi = blk->phis; phi; phi = phi->next, i++) {
        if (!xi_cfg_phi_append_arg(blk, phi, phi_args ? phi_args[i] : NULL)) {
            xi_cfg_remove_pred_at(blk, appended_idx);
            return false;
        }
    }
    return true;
}

XR_FUNC bool xi_cfg_redirect_edge(XiBlock *pred, XiBlock *old_succ, XiBlock *new_succ,
                                  XiValue **new_succ_phi_args, uint32_t nphi_args) {
    XR_DCHECK(pred != NULL, "xi_cfg_redirect_edge: NULL pred");
    XR_DCHECK(old_succ != NULL, "xi_cfg_redirect_edge: NULL old_succ");
    XR_DCHECK(new_succ != NULL, "xi_cfg_redirect_edge: NULL new_succ");
    if (old_succ == new_succ)
        return false;
    if (xi_cfg_phi_count(new_succ) != nphi_args)
        return false;
    uint16_t old_idx = xi_cfg_pred_index(old_succ, pred);
    if (old_idx == old_succ->npreds)
        return false;
    uint32_t old_nphis = xi_cfg_phi_count(old_succ);
    XiValue **old_phi_args = NULL;
    if (old_nphis > 0) {
        old_phi_args =
            (XiValue **) xi_func_arena_alloc(old_succ->func, old_nphis * sizeof(XiValue *));
        if (!old_phi_args)
            return false;
        uint32_t i = 0;
        for (XiPhi *phi = old_succ->phis; phi; phi = phi->next, i++)
            old_phi_args[i] = old_idx < phi->value.nargs ? phi->value.args[old_idx] : NULL;
    }
    if (!xi_cfg_replace_successor(pred, old_succ, new_succ))
        return false;
    if (!xi_cfg_remove_pred_at(old_succ, old_idx)) {
        xi_cfg_replace_successor(pred, new_succ, old_succ);
        return false;
    }
    if (!xi_cfg_append_pred(new_succ, pred, new_succ_phi_args, nphi_args)) {
        xi_cfg_replace_successor(pred, new_succ, old_succ);
        xi_cfg_append_pred(old_succ, pred, old_phi_args, old_nphis);
        return false;
    }
    return true;
}

XR_FUNC bool xi_cfg_mark_unreachable_if_isolated(XiFunc *f, XiBlock *blk) {
    XR_DCHECK(f != NULL, "xi_cfg_mark_unreachable_if_isolated: NULL func");
    if (!blk || blk == f->entry || blk->npreds != 0)
        return false;
    if (blk->kind == XI_BLOCK_UNREACHABLE)
        return false;

    XiBlock *succ0 = blk->succs[0];
    XiBlock *succ1 = blk->succs[1];
    blk->kind = XI_BLOCK_UNREACHABLE;
    blk->control = NULL;
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;

    if (succ0) {
        while (xi_cfg_remove_pred(succ0, blk)) {
        }
    }
    if (succ1 && succ1 != succ0) {
        while (xi_cfg_remove_pred(succ1, blk)) {
        }
    }
    return true;
}

XR_FUNC uint32_t xi_cfg_compact_blocks(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_cfg_compact_blocks: NULL func");
    uint32_t write = 0;
    uint32_t orig = f->nblocks;
    for (uint32_t i = 0; i < f->nblocks; i++) {
        XiBlock *blk = f->blocks[i];
        if (blk->kind != XI_BLOCK_UNREACHABLE || blk == f->entry || blk->npreds > 0) {
            f->blocks[write] = blk;
            f->blocks[write]->id = write;
            write++;
        }
    }
    f->nblocks = write;
    f->next_block_id = write;
    return orig - write;
}
