/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_literal.inc.c - Private construction with explicit-key provenance
 *
 * KEY CONCEPT:
 *   Dense ordinals locate earlier explicit keys without another hash lookup.
 *   Only spread construction needs history; private roots never escape on fault.
 */
typedef struct HashLiteralInsert {
    size_t *history;
    size_t ordinal;
    XrXirMapLiteralDuplicate duplicate;
    bool explicit_entry;
} HashLiteralInsert;
/* This target was just allocated by the enclosing constructor, with capacity
 * for the complete input. It has never escaped and cannot require COW or growth.
 * Inputs still receive the ordinary key and value admission before publication. */
static XrXirValueStatus hash_literal_write(XirHash *map,const XrXirValue *key,const XrXirValue *value,
    XrXirValueAdmission *admission,HashLiteralInsert *literal) {
    XR_DCHECK(map->object.domain==admission->domain && xir_object_unique(&map->object),
        "literal construction holds one private receiving owner");
    HashProbe probe={0};XrXirValueStatus status=hash_probe(map,key,admission,&probe);
    if(status!=XR_XIR_VALUE_OK)return status;
    bool sensitive=false;status=hash_admit_field(value,map->value_type,admission,&sensitive);
    if(status!=XR_XIR_VALUE_OK)return status;
    size_t entry=probe.found?probe.entry:map->length;
    if(literal->explicit_entry && probe.found) {
        size_t first=literal->history?literal->history[entry]:entry;
        if(first!=SIZE_MAX) {
            literal->duplicate=(XrXirMapLiteralDuplicate){first,literal->ordinal};
            return XR_XIR_VALUE_DUPLICATE_KEY;
        }
    }
    if(!probe.found) {
        status=hash_charge(admission,hash_order_work(map->capacity));
        if(status!=XR_XIR_VALUE_OK)return status;
    }
    StoragePrepared prepared_key={0},prepared_value={0};
    if(!probe.found)status=hash_prepare_field(&prepared_key,key,map->key_type,admission);
    if(status==XR_XIR_VALUE_OK)status=hash_prepare_field(&prepared_value,value,map->value_type,admission);
    if(status==XR_XIR_VALUE_OK) {
        if(!probe.found)hash_publish_prepared(map,probe.hash,probe.bucket,&prepared_key,&prepared_value);
        else {
            XirObject *pending=NULL;hash_queue_field(map,probe.entry,true,&pending);
            if(prepared_value.owns)hash_transfer(&prepared_value,map,probe.entry,true);
            release_pending(pending);
        }
        if(literal->explicit_entry && literal->history && literal->history[entry]==SIZE_MAX)
            literal->history[entry]=literal->ordinal;
    }
    storage_prepared_end(&prepared_value);storage_prepared_end(&prepared_key);return status;
}
static XrXirValueStatus hash_literal_extent(XrXirType type,const XrXirValue *keys,const XrXirValue *values,
    const XrXirMapLiteralKind *kinds,size_t count,XrXirValueAdmission *admission,size_t *extent,bool *spread) {
    *extent=count;*spread=false;
    if(!kinds)return XR_XIR_VALUE_OK;
    size_t total=0;
    for(size_t c=0;c<count;++c) {
        XrXirValueStatus charged=hash_charge(admission,1);if(charged!=XR_XIR_VALUE_OK)return charged;
        size_t width=1;
        if(kinds[c]==XR_XIR_MAP_LITERAL_SPREAD) {
            if(!unit_value(&keys[c]) || values[c].type!=(uint32_t)type)return XR_XIR_VALUE_BAD_ARGUMENT;
            XirHash *source=NULL;
            XrXirValueStatus status=hash_admit(&values[c],XR_XIR_TYPE_MAP,admission,&source);
            if(status!=XR_XIR_VALUE_OK)return status;
            width=source->length;*spread=true;
        } else if(kinds[c]!=XR_XIR_MAP_LITERAL_ENTRY)return XR_XIR_VALUE_BAD_ARGUMENT;
        if(width>SIZE_MAX-total)return XR_XIR_VALUE_OVERFLOW;
        total+=width;
    }
    *extent=total;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus hash_literal_spread(XirHash *target,const XrXirValue *value,
    XrXirValueAdmission *admission,HashLiteralInsert *insert) {
    XirHash *source=NULL;
    XrXirValueStatus status=hash_admit(value,XR_XIR_TYPE_MAP,admission,&source);
    if(status!=XR_XIR_VALUE_OK)return status;
    XrXirLayout key_layout={0},value_layout={0};
    if(!xr_xir_compile_type_arena_layout(source->object.arena,source->key_type,&key_layout) ||
        !xr_xir_compile_type_arena_layout(source->object.arena,source->value_type,&value_layout))return XR_XIR_VALUE_BAD_ARGUMENT;
    insert->explicit_entry=false;
    for(size_t n=0;n<source->length && status==XR_XIR_VALUE_OK;++n) {
        status=hash_charge(admission,hash_order_work(source->capacity));if(status!=XR_XIR_VALUE_OK)break;
        size_t entry=hash_order_entry(source,n);XrXirValue key={0},element={0};
        if(!hash_key_borrow(source->object.arena,source->key_type,hash_span(source,entry,false).bytes,key_layout.size,&key) ||
            !storage_leaf_value(source->object.arena,source->value_type,hash_span(source,entry,true).bytes,value_layout.size,&element))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        status=hash_literal_write(target,&key,&element,admission,insert);
    }
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_map_literal_new(XrXirType type,const XrXirValue *keys,const XrXirValue *values,
    const XrXirMapLiteralKind *kinds,size_t count,XrXirValueAdmission *admission,XrXirValue *output,
    XrXirMapLiteralDuplicate *duplicate) {
    if(!unit_value(output) || !duplicate || (count && (!keys || !values)) || !hash_admission_current(admission) ||
        !xr_xir_type_is_map(xr_xir_compile_type_arena_types(admission->arena),type))return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    size_t extent=0,capacity=0,history_bytes=0;bool spread=false;HashLayout layout={0};
    XrXirValue result={0};HashLiteralInsert insert={0};XirHash *map=NULL;
    XrXirValueStatus status=hash_literal_extent(type,keys,values,kinds,count,admission,&extent,&spread);
    if(status==XR_XIR_VALUE_OK)status=hash_layout(admission->arena,type,&layout);
    if(status==XR_XIR_VALUE_OK && !hash_capacity(extent,layout.stride,&capacity))status=XR_XIR_VALUE_LIMIT;
    if(status==XR_XIR_VALUE_OK && spread && extent)status=hash_charge(admission,extent);
    if(status==XR_XIR_VALUE_OK && spread && extent) {
        if(extent>SIZE_MAX/sizeof(*insert.history))status=XR_XIR_VALUE_OVERFLOW;
        else {
            history_bytes=extent*sizeof(*insert.history);
            insert.history=xr_xir_domain_allocate(admission->domain,history_bytes,&status);
            if(insert.history)for(size_t n=0;n<extent;++n)insert.history[n]=SIZE_MAX;
        }
    }
    if(status==XR_XIR_VALUE_OK)status=hash_allocate(type,capacity,admission,&map);
    if(status==XR_XIR_VALUE_OK)result=hash_value(map);
    for(size_t c=0;c<count && status==XR_XIR_VALUE_OK;++c) {
        insert.ordinal=c;insert.explicit_entry=!kinds || kinds[c]==XR_XIR_MAP_LITERAL_ENTRY;
        if(insert.explicit_entry)status=hash_literal_write(map,&keys[c],&values[c],admission,&insert);
        else status=hash_literal_spread(map,&values[c],admission,&insert);
    }
    if(insert.history)xr_xir_domain_deallocate(admission->domain,insert.history,history_bytes);
    if(status==XR_XIR_VALUE_OK)*output=result;
    else {xr_xir_value_drop(&result);if(status==XR_XIR_VALUE_DUPLICATE_KEY)*duplicate=insert.duplicate;}
    xr_xir_value_graph_end();return status;
}
