#include "xi_opt_devirt.h"
#include "xi_effect.h"
#include "xi_tbaa.h"
#include "../base/xchecks.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xtype.h"
#include <string.h>

static bool class_name_eq(const XiClassData *data, const char *name) {
    if (!data || !name)
        return false;
    if (data->class_name && strcmp(data->class_name, name) == 0)
        return true;
    if (data->display_name && strcmp(data->display_name, name) == 0)
        return true;
    return data->generic_origin_name && strcmp(data->generic_origin_name, name) == 0;
}

static const XiClassData *find_class_data_in_func(const XiFunc *f, const char *name,
                                                  const XiFunc **out_owner) {
    if (!f || !name)
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
            if (class_name_eq(data, name)) {
                if (out_owner)
                    *out_owner = f;
                return data;
            }
        }
    }
    return NULL;
}

static const XiClassData *find_class_data(const XiFunc *f, const XiFunc *root, const char *name,
                                          const XiFunc **out_owner) {
    const XiClassData *data = find_class_data_in_func(f, name, out_owner);
    if (data || root == f)
        return data;
    return find_class_data_in_func(root, name, out_owner);
}

static int find_instance_method_index(const XiClassData *data, const char *method) {
    if (!data || !method || !data->methods || !data->child_idx)
        return -1;
    for (uint16_t i = 0; i < data->ninst && i < data->nmethod; i++) {
        const XiClassMethod *m = &data->methods[i];
        if (m->is_static || m->is_static_constructor)
            continue;
        if (m->name && strcmp(m->name, method) == 0)
            return (int) i;
    }
    return -1;
}

static bool devirt_resolve(const XiFunc *f, const XiFunc *root, const XiValue *v, int *out_idx) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !v->args[0] || !v->aux || !out_idx)
        return false;
    if ((v->aux_int & 1) != 0)
        return false;
    if (v->nargs - 1 > 127)
        return false;

    const XiValue *recv = v->args[0];
    if (!recv->type || recv->type->kind != XR_KIND_INSTANCE)
        return false;

    XrClassInfo *info = recv->type->instance.class_ref;
    if (!info || info->base || info->base_name || info->has_subclass || info->struct_layout)
        return false;

    const char *class_name = info->name ? info->name : recv->type->instance.class_name;
    const XiFunc *owner = NULL;
    const XiClassData *data = find_class_data(f, root, class_name, &owner);
    if (!data || data->super_name)
        return false;
    int method_idx = find_instance_method_index(data, (const char *) v->aux);
    if (method_idx < 0 || method_idx > 255)
        return false;
    if (!owner || data->child_idx[method_idx] >= owner->nchildren ||
        !owner->children[data->child_idx[method_idx]])
        return false;

    *out_idx = method_idx;
    return true;
}

static XiPassChange devirt_func(XiFunc *f, const XiFunc *root) {
    XiPassChange chg = xi_pass_no_change();
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            int method_idx = -1;
            if (!devirt_resolve(f, root, v, &method_idx))
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

static XiPassChange devirt_walk(XiFunc *f, const XiFunc *root) {
    XiPassChange total = devirt_func(f, root);
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            total = xi_pass_merge(total, devirt_walk(f->children[i], root));
    }
    return total;
}

XR_FUNC XiPassChange xi_opt_devirt(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_devirt: NULL func");
    return devirt_walk(f, f);
}
