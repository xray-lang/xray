#include "xi_opt_devirt.h"
#include "xi_cha.h"
#include "xi_effect.h"
#include "xi_tbaa.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xtype.h"

/* Check if a method is marked final (no subclass overrides) in the
 * class's vtable.  Safe for devirtualization even when has_subclass. */
static bool is_method_final_in_vtable(const XrClassInfo *info, int32_t method_symbol) {
    if (!info || method_symbol <= 0 || !info->vtable)
        return false;
    for (int i = 0; i < info->vtable_size; i++) {
        if (info->vtable[i].symbol_id == method_symbol)
            return info->vtable[i].is_final;
    }
    return false;
}

static const XrClassInfo *class_parent_info(const XiClassData *data, const XrClassInfo *info) {
    if (data && data->class_info && data->class_info->base)
        return data->class_info->base;
    return info ? info->base : NULL;
}

static const XiClassData *find_class_data_in_func(const XiFunc *f, const XrClassInfo *info,
                                                  const XiFunc **out_owner) {
    if (!f || !info)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CLASS_CREATE || !v->aux)
                continue;
            const XiClassData *data = (const XiClassData *) v->aux;
            if (data->class_info == info) {
                if (out_owner)
                    *out_owner = f;
                return data;
            }
        }
    }
    return NULL;
}

static const XiClassData *find_class_data(const XiFunc *f, const XiFunc *root,
                                          const XrClassInfo *info, const XiFunc **out_owner) {
    const XiClassData *data = find_class_data_in_func(f, info, out_owner);
    if (data || root == f)
        return data;
    return find_class_data_in_func(root, info, out_owner);
}

static int find_instance_method_index(const XiClassData *data, int32_t method_symbol) {
    if (!data || method_symbol <= 0 || !data->methods || !data->child_idx)
        return -1;
    for (uint16_t i = 0; i < data->ninst && i < data->nmethod; i++) {
        const XiClassMethod *m = &data->methods[i];
        if (m->is_static || m->is_static_constructor)
            continue;
        if (m->symbol_id == method_symbol)
            return (int) i;
    }
    return -1;
}

/* True iff `info` or any of its ancestors declares an instance method
 * with `method_symbol`. */
static bool method_in_class_or_ancestors(const XiFunc *f, const XiFunc *root,
                                         const XrClassInfo *info, int32_t method_symbol,
                                         int depth) {
    if (!info || method_symbol <= 0 || depth >= 8)
        return false;
    const XiFunc *owner = NULL;
    const XiClassData *data = find_class_data(f, root, info, &owner);
    if (!data)
        return false;
    if (find_instance_method_index(data, method_symbol) >= 0)
        return true;
    return method_in_class_or_ancestors(f, root, class_parent_info(data, info), method_symbol,
                                        depth + 1);
}

/* Count of instance methods in the FLATTENED runtime layout of `info`,
 * matching finalize_methods(): parent's flat count + own non-override
 * instance methods.  Overrides reuse the parent slot and do not grow it. */
static int flat_instance_method_count(const XiFunc *f, const XiFunc *root, const XrClassInfo *info,
                                      int depth) {
    if (!info || depth >= 8)
        return 0;
    const XiFunc *owner = NULL;
    const XiClassData *data = find_class_data(f, root, info, &owner);
    if (!data)
        return 0;
    const XrClassInfo *parent = class_parent_info(data, info);
    int parent_count = flat_instance_method_count(f, root, parent, depth + 1);
    int own_nonoverride = 0;
    for (uint16_t i = 0; i < data->ninst && i < data->nmethod; i++) {
        const XiClassMethod *m = &data->methods[i];
        if (m->is_static || m->is_static_constructor)
            continue;
        if (m->symbol_id > 0 && !method_in_class_or_ancestors(f, root, parent, m->symbol_id, 0))
            own_nonoverride++;
    }
    return parent_count + own_nonoverride;
}

/* Flat runtime index of `method_symbol` in `info`'s flattened methods[],
 * matching the layout the VM's OP_INVOKE_DIRECT indexes (cls->methods[]).
 * Returns -1 if not found.  Mirrors finalize_methods(): inherited methods
 * occupy [0, parent_flat_count); an own override reuses the parent's slot;
 * an own new method is appended after the parent block in declaration order. */
static int flat_method_index(const XiFunc *f, const XiFunc *root, const XrClassInfo *info,
                             int32_t method_symbol, int depth) {
    if (!info || method_symbol <= 0 || depth >= 8)
        return -1;
    const XiFunc *owner = NULL;
    const XiClassData *data = find_class_data(f, root, info, &owner);
    if (!data)
        return -1;
    const XrClassInfo *parent = class_parent_info(data, info);
    int parent_count = flat_instance_method_count(f, root, parent, depth + 1);
    int own_nonoverride = 0;
    for (uint16_t i = 0; i < data->ninst && i < data->nmethod; i++) {
        const XiClassMethod *m = &data->methods[i];
        if (m->is_static || m->is_static_constructor)
            continue;
        bool is_override =
            m->symbol_id > 0 && method_in_class_or_ancestors(f, root, parent, m->symbol_id, 0);
        if (m->symbol_id == method_symbol) {
            if (is_override)
                return flat_method_index(f, root, parent, method_symbol, depth + 1);
            return parent_count + own_nonoverride;
        }
        if (!is_override)
            own_nonoverride++;
    }
    /* Not declared here — inherited from a parent. */
    return flat_method_index(f, root, parent, method_symbol, depth + 1);
}

/* Walk the inheritance chain to find a method.  If the concrete class
 * does not define the method, check the parent class data. */
static const XiClassData *find_method_in_hierarchy(const XiFunc *f, const XiFunc *root,
                                                   const XrClassInfo *info, int32_t method_symbol,
                                                   int *out_idx, const XiFunc **out_owner) {
    if (!info || method_symbol <= 0)
        return NULL;

    int depth = 0;
    const XrClassInfo *cur_info = info;

    while (cur_info && depth < 8) {
        const XiFunc *owner = NULL;
        const XiClassData *data = find_class_data(f, root, cur_info, &owner);
        if (!data)
            return NULL;

        int idx = find_instance_method_index(data, method_symbol);
        if (idx >= 0) {
            *out_idx = idx;
            *out_owner = owner;
            return data;
        }

        cur_info = class_parent_info(data, cur_info);
        depth++;
    }
    return NULL;
}

static bool devirt_resolve(const XiFunc *f, const XiFunc *root, const XaClassHierarchy *cha,
                           const XiValue *v, int *out_idx) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->args[0] || !v->aux || !out_idx)
        return false;
    if ((v->aux_int & 1) != 0)
        return false;
    int32_t method_symbol = (int32_t) (v->aux_int >> 1);
    if (method_symbol <= 0)
        return false;
    if (v->nargs - 1 > 127)
        return false;

    const XiValue *recv = v->args[0];
    if (!recv->type || recv->type->kind != XR_KIND_INSTANCE)
        return false;

    XrClassInfo *info = recv->type->instance.class_ref;
    if (!info || info->struct_layout)
        return false;

    const XrClassInfo *static_info = cha ? xi_cha_snapshot_info(cha, info) : info;
    const XrClassInfo *target_info = static_info;
    if (cha) {
        const XrClassInfo *single = xa_cha_single_implementor(cha, static_info, method_symbol);
        if (single)
            target_info = single;
    }

    /* Guard: if the receiver's class has both a base class AND subclasses,
     * and CHA didn't narrow to a single implementor, then we can't be sure
     * which version of the method to call. If receiver has no subclasses,
     * inheritance is safe — nobody overrides. */
    if (info->base && info->has_subclass && target_info == static_info)
        return false;

    bool cha_final =
        target_info->has_subclass && is_method_final_in_vtable(target_info, method_symbol);
    if (target_info->has_subclass && !cha_final)
        return false;

    const XrClassInfo *target_origin = cha ? xi_cha_origin_info(cha, target_info) : target_info;
    const XiFunc *owner = NULL;
    int method_idx = -1;
    const XiClassData *data =
        find_method_in_hierarchy(f, root, target_origin, method_symbol, &method_idx, &owner);
    if (!data || method_idx < 0 || method_idx > 255)
        return false;
    /* method_idx is the own-method index within `data`; it indexes child_idx
     * (the IR child-function table) for the validity guard below. */
    if (!owner || !data->child_idx || data->child_idx[method_idx] >= owner->nchildren ||
        !owner->children[data->child_idx[method_idx]])
        return false;

    /* OP_INVOKE_DIRECT indexes the receiver class's FLATTENED runtime
     * methods[] (inherited methods first), which differs from the own-method
     * index when the class has inherited methods.  Emit the flat index. */
    int flat_idx = flat_method_index(f, root, target_origin, method_symbol, 0);
    if (flat_idx < 0 || flat_idx > 255)
        return false;

    *out_idx = flat_idx;
    return true;
}

static XiPassChange devirt_func(XiFunc *f, const XiFunc *root, const XaClassHierarchy *cha) {
    XiPassChange chg = xi_pass_no_change();
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            int method_idx = -1;
            if (!devirt_resolve(f, root, cha, v, &method_idx))
                continue;
            v->op = XI_CALL_METHOD_DIRECT;
            v->aux_int = method_idx;
            v->flags |= xi_op_default_effects(XI_CALL_METHOD_DIRECT);
            v->mem_group = XI_MEM_TOP;
            chg.values_changed = true;
        }
    }
    return chg;
}

static XiPassChange devirt_walk(XiFunc *f, const XiFunc *root, const XaClassHierarchy *cha) {
    XiPassChange total = devirt_func(f, root, cha);
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            total = xi_pass_merge(total, devirt_walk(f->children[i], root, cha));
    }
    return total;
}

XR_FUNC XiPassChange xi_opt_devirt(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_devirt: NULL func");

    XaClassHierarchy cha = {0};
    XrClassInfo *cha_copies = NULL;
    if (!xi_cha_build_for_func(f, &cha, &cha_copies)) {
        return devirt_walk(f, f, NULL);
    }

    XiPassChange chg = devirt_walk(f, f, &cha);
    xa_cha_free(&cha);
    xr_free(cha_copies);
    return chg;
}
