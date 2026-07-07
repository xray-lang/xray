/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cha.c - Collect class metadata from Xi IR and build CHA snapshots
 */

#include "xi_cha.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"

XR_FUNC const XrClassInfo *xi_cha_snapshot_info(const XaClassHierarchy *cha,
                                                const XrClassInfo *info) {
    if (!cha || !info)
        return info;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        if (cha->nodes[i].origin_info == info || cha->nodes[i].info == info)
            return cha->nodes[i].info;
    }
    return info;
}

XR_FUNC const XrClassInfo *xi_cha_origin_info(const XaClassHierarchy *cha,
                                              const XrClassInfo *snapshot_info) {
    if (!cha || !snapshot_info)
        return snapshot_info;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        if (cha->nodes[i].info == snapshot_info)
            return cha->nodes[i].origin_info ? cha->nodes[i].origin_info : snapshot_info;
    }
    return snapshot_info;
}

typedef struct XiChaCollector {
    XrClassInfo **infos;
    uint32_t ninfos;
    uint32_t cap;
} XiChaCollector;

static bool cha_collector_push(XiChaCollector *col, XrClassInfo *info) {
    if (!col || !info)
        return true;
    for (uint32_t i = 0; i < col->ninfos; i++) {
        if (col->infos[i] == info)
            return true;
    }
    if (col->ninfos >= col->cap) {
        uint32_t new_cap = col->cap ? col->cap * 2 : 8;
        XrClassInfo **next =
            (XrClassInfo **) xr_realloc(col->infos, new_cap * sizeof(XrClassInfo *));
        if (!next)
            return false;
        col->infos = next;
        col->cap = new_cap;
    }
    col->infos[col->ninfos++] = info;
    return true;
}

static bool cha_collect_info(XiChaCollector *col, XrClassInfo *info) {
    if (!info)
        return true;
    if (!cha_collector_push(col, info))
        return false;
    if (info->base && !cha_collector_push(col, info->base))
        return false;
    return true;
}

static bool cha_collect_type(XiChaCollector *col, const XrType *type) {
    if (!type || type->kind != XR_KIND_INSTANCE)
        return true;
    return cha_collect_info(col, type->instance.class_ref);
}

static bool cha_collect_value(XiChaCollector *col, const XiValue *v) {
    if (!v)
        return true;
    return cha_collect_type(col, v->type);
}

static bool cha_collect_func(const XiFunc *f, XiChaCollector *col) {
    if (!f)
        return true;
    if (!cha_collect_type(col, f->return_type))
        return false;
    for (uint16_t pi = 0; pi < f->nparams; pi++) {
        XiValue *param = f->params[pi];
        if (!cha_collect_type(col, param ? param->type : NULL))
            return false;
    }
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (!cha_collect_value(col, blk->values[vi]))
                return false;
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!cha_collect_type(col, phi->value.type))
                return false;
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!cha_collect_func(f->children[ci], col))
            return false;
    }
    return true;
}

static bool cha_build_snapshot(const XiChaCollector *col, XaClassHierarchy *out,
                               XrClassInfo **out_copies) {
    if (!col || !out || !out_copies)
        return false;

    if (col->ninfos == 0) {
        *out_copies = NULL;
        return xa_cha_build(out, NULL, 0);
    }

    XrClassInfo *copies = (XrClassInfo *) xr_calloc(col->ninfos, sizeof(XrClassInfo));
    XrClassInfo **ptrs = (XrClassInfo **) xr_malloc(col->ninfos * sizeof(XrClassInfo *));
    if (!copies || !ptrs) {
        xr_free(copies);
        xr_free(ptrs);
        return false;
    }

    for (uint32_t i = 0; i < col->ninfos; i++) {
        const XrClassInfo *src = col->infos[i];
        copies[i] = *src;
        copies[i].base = NULL;
        ptrs[i] = &copies[i];
    }

    for (uint32_t i = 0; i < col->ninfos; i++) {
        const XrClassInfo *src = col->infos[i];
        if (src->base) {
            for (uint32_t j = 0; j < col->ninfos; j++) {
                if (col->infos[j] == src->base) {
                    copies[i].base = &copies[j];
                    break;
                }
            }
        }
    }

    if (!xa_cha_build(out, ptrs, col->ninfos)) {
        xr_free(copies);
        xr_free(ptrs);
        return false;
    }

    xr_free(ptrs);
    for (uint32_t i = 0; i < out->nnodes && i < col->ninfos; i++)
        out->nodes[i].origin_info = col->infos[i];
    *out_copies = copies;
    return true;
}

XR_FUNC bool xi_cha_build_for_func(const XiFunc *f, XaClassHierarchy *out,
                                   XrClassInfo **out_copies) {
    if (!f || !out || !out_copies)
        return false;
    *out_copies = NULL;

    XiChaCollector col = {0};
    if (!cha_collect_func(f, &col))
        return false;

    bool ok = cha_build_snapshot(&col, out, out_copies);
    xr_free(col.infos);
    return ok;
}
