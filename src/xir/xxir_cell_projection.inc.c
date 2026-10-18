/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_projection.inc.c - Scoped handles over one actual storage identity
 *
 * KEY CONCEPT:
 *   Selectors are owned; leaf addresses and logical values are never retained.
 */
_Static_assert(sizeof(XirCell) % _Alignof(XrXirValuePathStep) == 0,
    "cell projection tail preserves selector alignment");
static bool cell_projection_allocation_bytes(size_t count, size_t *bytes) {
    if (!bytes || count > (SIZE_MAX - sizeof(XirCell)) / sizeof(XrXirValuePathStep)) return false;
    *bytes = sizeof(XirCell) + count * sizeof(XrXirValuePathStep);
    return true;
}
static bool cell_projection_valid(const XirCell *cell) {
    size_t bytes = 0;
    if (!cell || !cell->initialized || !cell->projection_count || cell->module_storage ||
        cell->borrow_top || cell->prepared_loan || cell->module_owner || cell->module_slot != UINT32_MAX ||
        !cell_authority_valid(&cell->projection_authority) ||
        !cell_projection_allocation_bytes(cell->projection_count, &bytes) ||
        cell->allocation_bytes != bytes ||
        !value_header_valid(&cell->value)) return false;
    XirObject *owner = object_pointer(&cell->value);
    if ((owner->kind!=XIR_OBJECT_CLASS && (owner->kind!=XIR_OBJECT_CELL ||
            ((const XirCell *)owner)->projection_count)) || owner->domain!=cell->object.domain ||
        !xr_xir_value_valid(&cell->value)) return false;
    XrXirType root=owner->kind==XIR_OBJECT_CLASS ? owner->type :
        xr_xir_cell_element(xr_xir_compile_type_arena_types(owner->arena),owner->type);
    const XrXirValuePathStep *steps=(const XrXirValuePathStep *)(cell+1);
    return xir_type_arena_same_type(owner->arena,root,cell->object.arena,steps[0].container);
}
static bool cell_projection_work(XrXirValueAdmission *admission, uint64_t work) {
    if (!admission || !admission->domain || work > admission->work) return false;
    admission->work -= work;
    return xr_xir_domain_work(admission->domain, work);
}
static XrXirValueStatus cell_projection_type(XrXirValueAdmission *admission,
    const XrXirTypeArena *arena,XrXirType source,XrXirType *output) {
    if (arena==admission->arena) {*output=source;return XR_XIR_VALUE_OK;}
    if (!cell_projection_work(admission,xir_type_arena_lookup_work(admission->arena))) return XR_XIR_VALUE_LIMIT;
    return xir_type_arena_local_type(admission->arena,arena,source,output) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus cell_projection_shape(const XrXirValuePath *path,
    XrXirType start, XrXirValueAdmission *admission, XrXirType *leaf) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    XrXirType current = start;
    for (uint32_t i = 0; i < path->count; ++i) {
        const XrXirValuePathStep *step = &path->steps[i];
        if (step->container != current) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (step->kind == XR_XIR_PATH_INDEX) {
            if (!xr_xir_type_is_array(types, current)) return XR_XIR_VALUE_BAD_ARGUMENT;
            current = xr_xir_array_element(types, current);
        } else if (step->kind == XR_XIR_PATH_KEY) {
            if (!xr_xir_type_is_map(types, current)) return XR_XIR_VALUE_BAD_ARGUMENT;
            XrXirValue key = path_key_value(step, admission->arena);
            XrXirValueStatus status = xr_xir_value_admit(&key, (XrXirType)key.type, admission);
            if (status != XR_XIR_VALUE_OK) return status;
            current = xr_xir_map_element(types, current);
        } else if (step->kind == XR_XIR_PATH_FIELD) {
            const XrXirTypeNode *node = xr_xir_type_node(types, current);
            if (node && node->kind == XR_XIR_TYPE_RECORD) {
                if (step->selector < 0 || (uint64_t)step->selector >= node->parameter_count) return XR_XIR_VALUE_BAD_ARGUMENT;
                current = node->parameters[step->selector].type;continue;
            }
            bool identity_root = i == 0 && xr_xir_type_is_class(types, start);
            if (!node || (!xr_xir_type_is_struct(types, current) && !identity_root) || step->selector < 0 ||
                (uint64_t)step->selector >= node->nominal.field_count)
                return XR_XIR_VALUE_BAD_ARGUMENT;
            uint32_t field = (uint32_t)step->selector;
            if (!(types->nominals->identities[node->nominal.declaration].fields[field].flags &
                    XR_XIR_FIELD_MUTABLE)) return XR_XIR_VALUE_BAD_ARGUMENT;
            current = node->nominal.fields[field];
        } else return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    /* A second identity requires its own evaluated root and cannot extend this loan. */
    if (current == XR_XIR_UNIT || xr_xir_type_is_class(types, current) ||
        xr_xir_type_is_cell(types, current))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *leaf = current;
    return XR_XIR_VALUE_OK;
}
static bool cell_project_class_authority(const XirClassObject *identity,
    const XrXirCellAuthority *authority) {
    uint32_t state = class_origin_state(identity);
    const XrXirCellLoan *top = identity->borrow_top;
    if ((state & XIR_CLASS_ORIGIN_PREPARED) ||
        (((state & XIR_CLASS_ORIGIN_BORROWED) != 0) != (top != NULL))) return false;
    return !top || (!top->scope && cell_authority_equal(&top->authority, authority));
}
static XrXirValueStatus cell_project_operation(XrXirType type,
    const XrXirValue *value, const XrXirValuePath *path,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission,
    XrXirValue *output) {
    if (!value || !path || !path->count || !path->steps || !unit_value(output) ||
        !cell_authority_valid(authority) || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell = cell_owner(value);
    XirObject *identity = !cell && owned_carrier_type((XrXirType)value->type) ? object_pointer(value) : NULL;
    bool object_root = identity && identity->kind == XIR_OBJECT_CLASS;
    XirObject *owner = cell ? cell_root_object(cell) : identity;
    const XrXirValue *root=object_root ? value : xr_xir_cell_canonical_root(value);
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(root,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    if ((!cell && !object_root) || (cell && !cell_access(cell, authority)) ||
        (object_root && !cell_project_class_authority((const XirClassObject *)identity, authority)) ||
        !admission_owner(&local,owner->arena,owner->domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    XrXirType input_type=XR_XIR_UNIT;
    status=cell_projection_type(admission,xr_xir_value_arena(value),(XrXirType)value->type,&input_type);
    if (status!=XR_XIR_VALUE_OK) return status;
    /* A Cell containing a Class is a binding owner, never the object owner. */
    if (cell && xr_xir_type_is_class(types,xr_xir_cell_element(types,input_type)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t prefix = cell ? cell->projection_count : 0;
    if (!xr_xir_type_is_cell(types, type) || path->count > UINT32_MAX - prefix)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t count = prefix + path->count;
    size_t bytes = 0;
    if (!cell_projection_allocation_bytes(count, &bytes)) return XR_XIR_VALUE_LIMIT;
    if (!cell_projection_work(admission, (uint64_t)count + bytes)) return XR_XIR_VALUE_LIMIT;
    XrXirType leaf = XR_XIR_UNIT;
    status=cell_projection_shape(path,object_root ? input_type : xr_xir_cell_element(types,input_type),admission,&leaf);
    if (status != XR_XIR_VALUE_OK) return status;
    if (xr_xir_cell_element(types, type) != leaf) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (prefix && cell->object.arena!=admission->arena) {
        uint64_t work=xir_type_arena_lookup_work(admission->arena);
        if (work>UINT64_MAX/prefix || !cell_projection_work(admission,work*prefix)) return XR_XIR_VALUE_LIMIT;
    }
    XirCell *projection = (XirCell *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena,type,bytes,&status);
    if (!projection) return status;
    projection->value = (XrXirValue){0};
    projection->borrow_top = NULL;
    projection->prepared_loan = NULL;
    projection->module_owner = NULL;
    projection->module_slot = UINT32_MAX;
    projection->initialized = false;
    projection->module_storage = false;
    projection->projection_count = count;
    projection->allocation_bytes = bytes;
    projection->projection_authority = *authority;
    status = xr_xir_value_copy(root, &projection->value);
    if (status != XR_XIR_VALUE_OK) { constructed_discard(&projection->object, bytes); return status; }
    XrXirValuePathStep *steps = (XrXirValuePathStep *)(projection + 1);
    if (prefix) memcpy(steps, cell + 1, (size_t)prefix * sizeof(*steps));
    for (uint32_t i=0;i<prefix && cell->object.arena!=admission->arena;++i) {
        if (!xir_type_arena_local_type(admission->arena,cell->object.arena,steps[i].container,&steps[i].container)) {
            xr_xir_value_drop(&projection->value);constructed_discard(&projection->object,bytes);
            return XR_XIR_VALUE_BAD_ARGUMENT;
        }
    }
    memcpy(steps + prefix, path->steps, (size_t)path->count * sizeof(*steps));
    /* Each flattened descriptor owns its keys independently of its caller and
     * any parent descriptor. Retention finishes before the object is visible. */
    uint32_t acquired = 0;
    for (; acquired < count; ++acquired) {
        XrXirValue key = path_key_value(&steps[acquired], admission->arena), owned = {0};
        status = xr_xir_value_copy(&key, &owned);
        if (status != XR_XIR_VALUE_OK) break;
    }
    if (status != XR_XIR_VALUE_OK) {
        while (acquired) {
            XrXirValue key = path_key_value(&steps[--acquired], admission->arena);
            xr_xir_value_drop(&key);
        }
        xr_xir_value_drop(&projection->value);
        constructed_discard(&projection->object, bytes); return status;
    }
    projection->initialized = true;
    xr_xir_value_object_publish(&projection->object);
    output->type = (uint32_t)type;
    memcpy(&output->payload, &projection, sizeof(projection));
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_cell_project_authorized(XrXirType type,
    const XrXirValue *root, const XrXirValuePath *path,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission,
    XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = cell_project_operation(type, root, path, authority, admission, output);
    xr_xir_value_graph_end();
    return status;
}
static ValuePathRoute cell_projection_route(const XirCell *cell,
    const XrXirValuePath *suffix) {
    XrXirValuePath prefix = {cell->projection_count ? (const XrXirValuePathStep *)(cell + 1) : NULL,
        cell->projection_count};
    XirObject *owner = cell_root_object(cell);
    bool identity = owner && owner->kind == XIR_OBJECT_CLASS;
    bool published = identity ?
        (atomic_load_explicit(&((const XirClassObject *)owner)->publication_state, memory_order_acquire) &
            XIR_CLASS_ORIGIN_MODULE) != 0 : owner && ((const XirCell *)owner)->module_storage;
    const XrXirTypeArena *arena=identity ? owner->arena : ((const XirCell *)owner)->object.arena;
    return (ValuePathRoute){prefix,*suffix,identity ? owner : NULL,published,arena,cell->object.arena};
}
XR_FUNC XrXirValueStatus xr_xir_cell_loan_prepare_path(const XrXirValue *value,
    const XrXirCellAuthority *parent, XrXirValueAdmission *admission,
    XrXirFaultDetail *fault, XrXirCellLoan *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!fault || !cell_loan_empty(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail){0};
    XirCell *cell = cell_owner(value);
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!cell || !admission_owner(&local,cell->object.arena,cell->object.domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    /* The finite scope comparison is charged before the ordinary authority check. */
    if (cell->projection_count && !cell_projection_work(admission, cell->projection_count))
        return XR_XIR_VALUE_LIMIT;
    local.work=admission->work;local.scratch_bytes=admission->scratch_bytes;
    status=cell_loan_prepare_operation(value,parent,output,&local);
    status=storage_admission_end(admission,&local,status);
    if (status != XR_XIR_VALUE_OK || !cell->projection_count) return status;
    xr_xir_value_graph_begin();
    XirObject *owner = cell_root_object(cell);
    XrXirValue *root = owner->kind == XIR_OBJECT_CLASS ? &cell->value : &((XirCell *)owner)->value;
    XrXirType root_type=owner->kind==XIR_OBJECT_CLASS ? owner->type :
        xr_xir_cell_element(xr_xir_compile_type_arena_types(owner->arena),owner->type);
    XrXirValuePlace place = {root_type,&root->payload};
    XrXirValuePath empty = {0}; ValuePathRoute route = cell_projection_route(cell, &empty);
    ValuePathSlot slot = {0}; uint32_t count = 0;
    status = path_route_begin(&place, &route, admission, &slot, &count, fault);
    for (uint32_t i = 0; status == XR_XIR_VALUE_OK && i < count; ++i)
        status = path_step(&slot,path_route_step(&route,i,admission->arena),admission,NULL,fault,i ? NULL : route.borrow_owner);
    if (status == XR_XIR_VALUE_OK && !xir_type_arena_same_type(slot.arena,slot.type,
            cell->object.arena,xr_xir_cell_element(xr_xir_compile_type_arena_types(cell->object.arena),cell->object.type)))
        status = XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_end();
    if (status != XR_XIR_VALUE_OK) xr_xir_cell_loan_abort(output);
    return status;
}
typedef struct CellPathOperation {
    const XrXirValuePath *suffix;
    const XrXirValue *replacement;
    XrXirValue *output;
    XrXirFaultDetail *fault;
    bool append;
} CellPathOperation;
static XrXirValueStatus cell_path_operation(const XrXirValue *value,
    const XrXirCellAuthority *authority, XrXirValueAdmission *admission,
    const CellPathOperation *operation) {
    const XrXirValuePath *suffix = operation->suffix;
    XirCell *cell = cell_owner(value);
    if (!cell || !suffix || !(operation->output ? cell_access(cell,authority) :
            cell_write_access(cell,authority)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!admission_owner(&local,cell->object.arena,cell->object.domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    /* Descriptor selectors add real traversal to every access. */
    if (cell->projection_count && !cell_projection_work(admission, cell->projection_count))
        return XR_XIR_VALUE_LIMIT;
    XirObject *owner = cell_root_object(cell);
    XrXirValue *root = owner->kind == XIR_OBJECT_CLASS ? &cell->value : &((XirCell *)owner)->value;
    XrXirType root_type=owner->kind==XIR_OBJECT_CLASS ? owner->type :
        xr_xir_cell_element(xr_xir_compile_type_arena_types(owner->arena),owner->type);
    XrXirValuePlace place = {root_type,&root->payload};
    ValuePathRoute route = cell_projection_route(cell, suffix);
    if (operation->output) return value_path_read_route(&place,&route,admission,operation->output,operation->fault);
    if (!cell->projection_count && !suffix->count && !operation->append)
        return xr_xir_cell_write_graph_operation(value,operation->replacement,admission,authority);
    status=path_mutate_route(&place,&route,operation->replacement,admission,operation->fault,operation->append);
    if (status==XR_XIR_VALUE_OK && owner->kind==XIR_OBJECT_CELL) {
        XrXirValue actual={0};
        XR_CHECK(xr_xir_value_from_payload(owner->arena,root_type,root->payload,&actual),
            "successful root publication preserves the actual carrier header");
        *root=actual;
    }
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_path_read(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValuePath *suffix,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    CellPathOperation operation = {suffix, NULL, output, fault, false};
    XrXirValueStatus status = cell_path_operation(cell, authority, admission, &operation);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_path_write(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValuePath *suffix,
    const XrXirValue *value, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    CellPathOperation operation = {suffix, value, NULL, fault, false};
    XrXirValueStatus status = cell_path_operation(cell, authority, admission, &operation);
    xr_xir_value_graph_end();
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_cell_authorized_path_push(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValuePath *suffix,
    const XrXirValue *value, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    CellPathOperation operation = {suffix, value, NULL, fault, true};
    XrXirValueStatus status = cell_path_operation(cell, authority, admission, &operation);
    xr_xir_value_graph_end();
    return status;
}
static XrXirValueStatus cell_projection_write(const XrXirValue *cell,
    const XrXirCellAuthority *authority, const XrXirValue *value,
    XrXirValueAdmission *admission) {
    XrXirValuePath empty = {0}; XrXirFaultDetail fault = {0};
    return xr_xir_cell_authorized_path_write(cell, authority, &empty, value, admission, &fault);
}
