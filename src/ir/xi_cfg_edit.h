#ifndef XI_CFG_EDIT_H
#define XI_CFG_EDIT_H

#include "xi.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

XR_FUNC uint32_t xi_cfg_phi_count(const XiBlock *blk);
XR_FUNC uint16_t xi_cfg_pred_index(const XiBlock *blk, const XiBlock *pred);
XR_FUNC bool xi_cfg_replace_successor(XiBlock *pred, XiBlock *old_succ, XiBlock *new_succ);
XR_FUNC bool xi_cfg_replace_pred(XiBlock *blk, XiBlock *old_pred, XiBlock *new_pred);
XR_FUNC bool xi_cfg_remove_pred(XiBlock *blk, XiBlock *pred);
XR_FUNC bool xi_cfg_append_pred(XiBlock *blk, XiBlock *pred, XiValue **phi_args,
                                uint32_t nphi_args);
XR_FUNC bool xi_cfg_redirect_edge(XiBlock *pred, XiBlock *old_succ, XiBlock *new_succ,
                                  XiValue **new_succ_phi_args, uint32_t nphi_args);
XR_FUNC bool xi_cfg_mark_unreachable_if_isolated(XiFunc *f, XiBlock *blk);
XR_FUNC uint32_t xi_cfg_compact_blocks(XiFunc *f);

#endif  // XI_CFG_EDIT_H
