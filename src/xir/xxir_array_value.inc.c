/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_array_value.inc.c - Owned arrays and transactional compact storage mutation
 */
static XrXirValue array_value(XirArray *array) {
    XrXirValue value = {(uint32_t) array->object.type, 0, 0};
    memcpy(&value.payload, &array, sizeof(array));
    return value;
}

static bool array_capacity(size_t required, uint32_t stride, size_t *output) {
    size_t maximum = stride ? SIZE_MAX / stride : SIZE_MAX;
    if (maximum > (uint64_t) INT64_MAX) maximum = (size_t) INT64_MAX;
    if (required > maximum) return false;
    size_t capacity = required ? 4 : 0;
    while (capacity < required) {
        if (capacity > maximum / 2) { capacity = required; break; }
        capacity *= 2;
    }
    *output = capacity;
    return true;
}

static XrXirValueStatus array_allocate(XrXirType type, size_t capacity,
    XrXirValueAdmission *admission, XirArray **output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    if (!admission || !admission->domain ||
        !array_layout(admission->arena, type, &element, &stride)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (capacity > (uint64_t) INT64_MAX || (stride && capacity > SIZE_MAX / stride)) return XR_XIR_VALUE_LIMIT;
    uint64_t header = array_header_bytes(admission->arena, element);
    if (header > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirArray *array = (XirArray *) constructed_allocate(admission->domain,
        (XrXirTypeArena *) admission->arena, type, (size_t)header, &status);
    if (!array) return status;
    array->data = NULL; array->length = 0; array->capacity = capacity;
    array->element = element; array->stride = stride;
    if (capacity && stride) {
        array->data = xr_xir_domain_allocate(admission->domain, capacity * stride, &status);
        if (!array->data) { constructed_discard(&array->object, (size_t)header); return status; }
    }
    xr_xir_value_object_publish(&array->object);
    *output = array;
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus xr_xir_array_new_graph_operation(XrXirType type, const XrXirValue *values,
    size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    if (!unit_value(output) || !admission || !admission->domain || (count && !values) ||
        !array_layout(admission->arena, type, &element, &stride)) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t capacity = 0;
    if (!array_capacity(count, stride, &capacity) || !admission->work || count >= admission->work)
        return XR_XIR_VALUE_LIMIT;
    --admission->work;
    for (size_t i = 0; i < count; ++i) {
        XrXirValueStatus status = xr_xir_value_admit(&values[i], element, admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    XirArray *array = NULL;
    XrXirValueStatus status = array_allocate(type, capacity, admission, &array);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue result = array_value(array);
    StoragePack pack = {0};
    status = storage_pack_begin(admission, element, &pack);
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&result); return status; }
    for (size_t i = 0; i < count; ++i) {
        status = storage_pack_value(&pack, &values[i], array_slot(array, i));
        if (status != XR_XIR_VALUE_OK) {
            storage_pack_end(&pack); xr_xir_value_drop(&result); return status;
        }
        ++array->length;
    }
    storage_pack_end(&pack);
    *output = result;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_array_new(XrXirType type, const XrXirValue *values,
    size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_new_graph_operation(type, values, count, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}

static XrXirValueStatus string_copy_bytes_graph_operation(const XrXirValue *source,
    XrXirType type,XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirType element=XR_XIR_UNIT;uint32_t stride=0;const char *bytes=NULL;size_t count=0;
    if (!unit_value(output) || !admission || !admission->domain ||
        !array_layout(admission->arena,type,&element,&stride) || element!=XR_XIR_U8 || stride!=1 ||
        !xr_xir_string_view(source,&bytes,&count)) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t capacity=0;
    if (!array_capacity(count,stride,&capacity) || !admission->work || count>=admission->work)
        return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_domain_work(admission->domain,(uint64_t)count+1)) return XR_XIR_VALUE_LIMIT;
    admission->work-=(uint64_t)count+1;
    XirArray *array=NULL;
    XrXirValueStatus status=array_allocate(type,capacity,admission,&array);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (count) memcpy(array->data,bytes,count);
    array->length=count;*output=array_value(array);
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_string_copy_bytes(const XrXirValue *source,
    XrXirType type,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=string_copy_bytes_graph_operation(source,type,admission,output);
    xr_xir_value_graph_end();return status;
}

static XrXirValueStatus xr_xir_array_repeat_graph_operation(XrXirType type, int64_t length, const XrXirValue *fill,
    XrXirValueAdmission *admission, XrXirValue *output) {
    XrXirType element = XR_XIR_UNIT; uint32_t stride = 0;
    if (!unit_value(output) || !fill || length < 0 || !admission || !admission->domain ||
        !array_layout(admission->arena, type, &element, &stride)) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t capacity = 0;
    if (!array_capacity((size_t) length, stride, &capacity) || !admission->work || (uint64_t) length >= admission->work)
        return XR_XIR_VALUE_LIMIT;
    --admission->work;
    XrXirValueStatus status = xr_xir_value_admit(fill, element, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *array = NULL;
    status = array_allocate(type, capacity, admission, &array);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue result = array_value(array);
    StoragePack pack = {0};
    status = storage_pack_begin(admission, element, &pack);
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&result); return status; }
    for (int64_t i = 0; i < length; ++i) {
        status = storage_pack_value(&pack, fill, array_slot(array, (size_t) i));
        if (status != XR_XIR_VALUE_OK) {
            storage_pack_end(&pack); xr_xir_value_drop(&result); return status;
        }
        ++array->length;
    }
    storage_pack_end(&pack);
    *output = result;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_array_repeat(XrXirType type, int64_t length, const XrXirValue *fill,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_repeat_graph_operation(type, length, fill, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}

static XrXirValueStatus xr_xir_array_len_graph_operation(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!array || !admission || !output) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    if (!xr_xir_value_argument(array, admission->arena, (XrXirType) array->type) ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena), (XrXirType) array->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = (int64_t) ((XirArray *) object_pointer(array))->length;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_array_len(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(array,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_len_graph_operation(array, &local, output);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}

static XrXirValueStatus xr_xir_array_capacity_graph_operation(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!array || !admission || !admission->domain || !output ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena), (XrXirType)array->type) ||
        !xr_xir_value_argument(array, admission->arena, (XrXirType)array->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work || !xr_xir_domain_work(admission->domain, 1)) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    *output = (int64_t)((XirArray *)object_pointer(array))->capacity;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_array_capacity(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(array,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_capacity_graph_operation(array, &local, output);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}

static XrXirValueStatus xr_xir_array_with_capacity_graph_operation(XrXirType type, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (capacity < 0 || !unit_value(output) || !admission || !admission->domain)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if ((uint64_t)capacity > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    if (!admission->work || !xr_xir_domain_work(admission->domain, 1)) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    XirArray *array = NULL;
    XrXirValueStatus status = array_allocate(type, (size_t)capacity, admission, &array);
    if (status == XR_XIR_VALUE_OK) *output = array_value(array);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_array_with_capacity(XrXirType type, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_with_capacity_graph_operation(type, capacity, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}

static XrXirValueStatus xr_xir_array_reserve_graph_operation(const XrXirValue *value, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (capacity < 0 || !value || !unit_value(output) || !admission || !admission->domain ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena), (XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if ((uint64_t)capacity > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType)value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *source = (XirArray *)object_pointer(value), *copy = NULL;
    /* Admission remains complete; allocation affinity only selects physical
     * sharing. A later write separates the independently owned logical copy. */
    if (source->object.domain == admission->domain && source->object.arena == admission->arena &&
        (uint64_t)capacity <= source->capacity)
        return xr_xir_value_copy(value, output);
    if (source->length > admission->work || !xr_xir_domain_work(admission->domain, source->length))
        return XR_XIR_VALUE_LIMIT;
    admission->work -= source->length;
    size_t desired = (uint64_t)capacity > source->capacity ? (size_t)capacity : source->capacity;
    status = array_allocate((XrXirType)value->type, desired, admission, &copy);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue owned = array_value(copy); StoragePack pack = {0};
    status = storage_pack_begin(admission, source->element, &pack);
    if (status == XR_XIR_VALUE_OK) {
        for (size_t i = 0; i < source->length; ++i) {
            status = storage_pack_copy(&pack, array_slot(source, i), array_slot(copy, i));
            if (status != XR_XIR_VALUE_OK) break;
            ++copy->length;
        }
        storage_pack_end(&pack);
    }
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned); return status; }
    *output = owned; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_array_reserve(const XrXirValue *value, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_reserve_graph_operation(value, capacity, &local, output);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}

static XrXirValueStatus xr_xir_array_get_graph_operation(const XrXirValue *value, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail) {0};
    if (!value || !fault || !unit_value(output) || !admission ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena), (XrXirType) value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType) value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *array = (XirArray *) object_pointer(value);
    if (index < 0 || (uint64_t) index >= array->length) {
        *fault = (XrXirFaultDetail) {430, 0, index, (int64_t) array->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    StorageSpan span = {array->element, array_slot(array, (size_t)index)};
    return storage_unpack(span, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_array_get(const XrXirValue *value, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (fault) *fault=(XrXirFaultDetail){0};
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&local);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_get_graph_operation(value, index, &local, output, fault);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}

static XrXirValueStatus array_observe_operation(const XrXirValue *value, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault,bool receiving) {
    if (fault) *fault=(XrXirFaultDetail){0};
    if (!value || !fault || !unit_value(output) || !admission || !admission->domain || !admission->arena)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (admission->work<64 || !xr_xir_domain_work(admission->domain,64)) return XR_XIR_VALUE_LIMIT;
    admission->work-=64;
    if (!xr_xir_value_argument(value,admission->arena,(XrXirType)value->type) ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XirArray *array=(XirArray *)object_pointer(value);
    if (!receiving && array->object.domain!=admission->domain) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (index<0 || (uint64_t)index>=array->length) {
        *fault=(XrXirFaultDetail){430,0,index,(int64_t)array->length};return XR_XIR_VALUE_BOUNDS;
    }
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    StorageSpan span={array->element,array_slot(array,(size_t)index)};
    return receiving?storage_unpack_operation(span,admission,output,true):storage_unpack(span,admission,output);
}
XR_FUNC XrXirValueStatus xr_xir_array_observe(const XrXirValue *array, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=array_observe_operation(array,index,admission,output,fault,false);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_compile_value_observe(XrCompileResources *resources,
    const XrXirValue *value,uint64_t index,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!resources || !value || !admission || !admission->arena || !unit_value(output) || !xr_xir_domain_current(admission->domain) ||
        admission->domain->compile_resources!=resources || index>INT64_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type);
    XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (node && node->kind==XR_XIR_TYPE_ARRAY) {
        XrXirFaultDetail fault={0};
        status=array_observe_operation(value,(int64_t)index,admission,output,&fault,true);
    } else if (node && xr_xir_type_is_struct(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type) && index<=UINT32_MAX)
        status=struct_observe_operation(value,(uint32_t)index,admission,output,true);
    else if (node) {
        if (admission->work<64 || !xr_xir_domain_work(admission->domain,64)) status=XR_XIR_VALUE_LIMIT;
        else {
            admission->work-=64;
            if (xr_xir_value_argument(value,admission->arena,(XrXirType)value->type)) {
                const XrXirValue *child=NULL;
                if (node->kind==XR_XIR_TYPE_TUPLE || node->kind==XR_XIR_TYPE_RECORD) {
                    const XirTuple *tuple=(const XirTuple *)object_pointer(value);
                    if (index<tuple->count) child=&tuple->fields[index];
                } else if (node->kind==XR_XIR_TYPE_NULLABLE ||
                    xr_xir_type_is_enum(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type)) {
                    const XirNominalValue *record=(const XirNominalValue *)object_pointer(value);
                    if (index<record->count) child=&record->fields[index];
                }
                if (child) status=value_copy_on_domain(child,admission->domain,output);
            }
        }
    }
    xr_xir_value_graph_end();return status;
}

static XrXirValueStatus array_grow(XirArray *array, size_t capacity) {
    XrXirDomain *domain = array->object.domain;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    size_t bytes = capacity * array->stride;
    if (!array->stride) { array->capacity = capacity; return XR_XIR_VALUE_OK; }
    if (!array->data) {
        unsigned char *data = xr_xir_domain_allocate(domain, bytes, &status);
        if (!data) return status;
        array->data = data; array->capacity = capacity;
        return XR_XIR_VALUE_OK;
    }
    domain_lock(domain);
    size_t extra = bytes - array->capacity * array->stride;
    if (extra > domain->limit - domain->stats.live_bytes || domain->stats.reallocations == UINT64_MAX ||
        (domain->budget.bound && (bytes > domain->budget.requested_limit - domain->budget.requested_bytes ||
            domain->budget.work == domain->budget.work_limit))) {
        domain_unlock(domain); return XR_XIR_VALUE_LIMIT;
    }
    if (domain->budget.bound) { domain->budget.requested_bytes += bytes; ++domain->budget.work; }
    void *replacement = array->data;
    status = domain_memory_resize(domain, &replacement, bytes);
    if (status != XR_XIR_VALUE_OK) { domain_unlock(domain); return status; }
    array->data = replacement; array->capacity = capacity;
    domain->stats.live_bytes += extra;
    if (domain->stats.live_bytes > domain->stats.peak_bytes) domain->stats.peak_bytes = domain->stats.live_bytes;
    ++domain->stats.reallocations;
    domain_unlock(domain);
    return XR_XIR_VALUE_OK;
}

typedef struct ArrayMutation {
    bool append, admitted, domain_sensitive;
    XrXirClassPublication *publication;
} ArrayMutation;
static XrXirValueStatus array_mutate(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault,
    const ArrayMutation *mutation) {
    bool append = mutation->append, admitted = mutation->admitted;
    bool domain_sensitive = mutation->domain_sensitive;
    if (fault) *fault = (XrXirFaultDetail) {0};
    if (!place || !place->payload || !admission || !admission->domain || (!append && !fault) ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena), place->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue current = {(uint32_t) place->type, 0, 0};
    memcpy(&current.payload, place->payload, sizeof(current.payload));
    XrXirValueStatus status = xr_xir_value_admit(&current, place->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirArray *array = (XirArray *) object_pointer(&current);
    status = admitted ? (xr_xir_value_argument(element, admission->arena, array->element) ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT) :
        value_admit_summary(element, array->element, admission, &domain_sensitive);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!append && (index < 0 || (uint64_t) index >= array->length)) {
        *fault = (XrXirFaultDetail) {430, 0, index, (int64_t) array->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    if (append && array->length == (uint64_t) INT64_MAX) return XR_XIR_VALUE_LIMIT;
    size_t length = array->length + (append ? 1u : 0u), capacity = 0;
    if (!array_capacity(length, array->stride, &capacity)) return XR_XIR_VALUE_LIMIT;
    if (capacity < array->capacity) capacity = array->capacity;
    StoragePrepared prepared = {0};
    status = storage_prepared_begin(&prepared,array->element,element,array->stride,admission);
    if (status != XR_XIR_VALUE_OK) return status;
    bool unique = atomic_load_explicit(&array->object.references, memory_order_acquire) == 1;
    bool grow = length > array->capacity;
    bool detach = array->object.domain != admission->domain && domain_sensitive;
    if (!unique || grow || detach) {
        if (admission->work < array->length) {
            storage_prepared_end(&prepared); return XR_XIR_VALUE_LIMIT;
        }
        admission->work -= array->length;
    }
    if (unique && !detach && (!grow || array->object.domain == admission->domain)) {
        if (grow) status = array_grow(array, capacity);
        if (status != XR_XIR_VALUE_OK) { storage_prepared_end(&prepared); return status; }
        size_t offset = append ? array->length : (size_t) index;
        if (mutation->publication) xr_xir_class_publication_commit(mutation->publication);
        XirObject *pending = NULL;
        if (!append) {
            StorageCursor cursor = {0};
            XR_CHECK(storage_cursor_init(admission->arena, (StorageSpan){array->element, array_slot(array, offset)},
                (StorageFrame *)prepared.pack.frames, prepared.pack.capacity, true, &cursor) == XR_XIR_VALUE_OK,
                "array overwrite retains reserved ownership traversal");
            storage_queue_release(&cursor, UINT64_MAX, &pending);
        }
        array_prepared_transfer(&prepared, array, offset); array->length = length;
        release_pending(pending); storage_prepared_end(&prepared);
        return XR_XIR_VALUE_OK;
    }
    XirArray *replacement = NULL;
    status = array_allocate(place->type, capacity, admission, &replacement);
    if (status != XR_XIR_VALUE_OK) { storage_prepared_end(&prepared); return status; }
    XrXirValue result = array_value(replacement);
    XirObject *pending = NULL;
    if (unique) {
        if (array->length && array->stride) memcpy(replacement->data, array->data, array->length * array->stride);
        replacement->length = array->length;
        if (!append) {
            StorageCursor cursor = {0};
            XR_CHECK(storage_cursor_init(admission->arena,
                (StorageSpan){array->element, array_slot(replacement, (size_t)index)},
                (StorageFrame *)prepared.pack.frames, prepared.pack.capacity, true, &cursor) == XR_XIR_VALUE_OK,
                "relocated overwrite retains reserved ownership traversal");
            storage_queue_release(&cursor, UINT64_MAX, &pending);
            array_prepared_transfer(&prepared, replacement, (size_t)index);
        }
    } else {
        for (size_t i = 0; i < array->length; ++i) {
            if (!append && i == (size_t) index) array_prepared_transfer(&prepared, replacement, i);
            else {
                status = storage_pack_copy(&prepared.pack, array_slot(array, i), array_slot(replacement, i));
                if (status != XR_XIR_VALUE_OK) {
                    xr_xir_value_drop(&result); storage_prepared_end(&prepared); return status;
                }
            }
            ++replacement->length;
        }
    }
    if (append) {
        array_prepared_transfer(&prepared, replacement, replacement->length); ++replacement->length;
    }
    if (mutation->publication) xr_xir_class_publication_commit(mutation->publication);
    memcpy(place->payload, &result.payload, sizeof(result.payload));
    if (unique) array->length = 0;
    xr_xir_value_drop(&current);
    release_pending(pending); storage_prepared_end(&prepared);
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus xr_xir_array_set_graph_operation(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    const ArrayMutation mutation = {0};
    return array_mutate(place, index, element, admission, fault, &mutation);
}
XR_FUNC XrXirValueStatus xr_xir_array_set(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (fault) *fault=(XrXirFaultDetail){0};
    XrXirValueAdmission local={0};XrXirValuePlace storage={0};
    XrXirValueStatus status=storage_place_admission(place,admission,&local,&storage);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_set_graph_operation(&storage, index, element, &local, fault);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}
static XrXirValueStatus xr_xir_array_push_graph_operation(const XrXirValuePlace *place,
    const XrXirValue *element, XrXirValueAdmission *admission) {
    const ArrayMutation mutation = {.append = true};
    return array_mutate(place, 0, element, admission, NULL, &mutation);
}
XR_FUNC XrXirValueStatus xr_xir_array_push(const XrXirValuePlace *place,
    const XrXirValue *element, XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};XrXirValuePlace storage={0};
    XrXirValueStatus status=storage_place_admission(place,admission,&local,&storage);
    if (status!=XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_array_push_graph_operation(&storage, element, &local);
    xr_xir_value_graph_end();
    return storage_admission_end(admission,&local,graph_outcome);
}
