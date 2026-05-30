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
#include <string.h>

XR_FUNC const XrClassInfo *xi_cha_snapshot_info(const XaClassHierarchy *cha,
                                                const XrClassInfo *info) {
    if (!cha || !info || !info->name)
        return info;
    for (uint32_t i = 0; i < cha->nnodes; i++) {
        XrClassInfo *node_info = cha->nodes[i].info;
        if (node_info && node_info->name && strcmp(node_info->name, info->name) == 0)
            return node_info;
    }
    return info;
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

static XrClassInfo *cha_find_copy_by_name(XrClassInfo *copies, uint32_t n, const char *name) {
    if (!name)
        return NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (copies[i].name && strcmp(copies[i].name, name) == 0)
            return &copies[i];
    }
    return NULL;
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
        if (src->base_name) {
            copies[i].base = cha_find_copy_by_name(copies, col->ninfos, src->base_name);
        } else if (src->base && src->base->name) {
            copies[i].base = cha_find_copy_by_name(copies, col->ninfos, src->base->name);
        }
    }

    if (!xa_cha_build(out, ptrs, col->ninfos)) {
        xr_free(copies);
        xr_free(ptrs);
        return false;
    }

    xr_free(ptrs);
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
