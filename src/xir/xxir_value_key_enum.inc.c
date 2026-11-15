/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Enum key classes live inside an authenticated receiving nominal identity. */
static XrXirValueStatus key_enum_receive(const XrXirValue *value,XrXirValueAdmission *admission,
    XrXirType *local,uint32_t *variant) {
    if(!admission->arena||value->reserved||!value->payload)return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *object=object_pointer(value);
    if((uint32_t)object->type!=value->type)return XR_XIR_VALUE_BAD_ARGUMENT;
    if(object->kind!=XR_XIR_TYPE_NOMINAL)return XR_XIR_VALUE_UNSUPPORTED;
    const XrXirTypeArena *source=object->arena;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(source);
    const XrXirTypeNode *node=xr_xir_type_node(types,(XrXirType)value->type);
    if(!node||node->kind!=XR_XIR_TYPE_NOMINAL||!types->nominals||!types->nominals->identities||
        node->nominal.declaration>=types->nominals->count)return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirNominalIdentity identity=types->nominals->identities[node->nominal.declaration];
    if(identity.kind!=XR_XIR_NOMINAL_ENUM||identity.field_count||node->nominal.field_count)
        return XR_XIR_VALUE_UNSUPPORTED;
    uint64_t work=xir_type_arena_lookup_work(admission->arena)+1;
    XrXirValueStatus status=key_charge(admission,work);if(status!=XR_XIR_VALUE_OK)return status;
    if(!xr_xir_compile_type_arena_key_eligible(source,(XrXirType)value->type))return XR_XIR_VALUE_UNSUPPORTED;
    if(object->domain||!xr_xir_value_valid(value)||!xir_type_arena_local_type(admission->arena,source,
        (XrXirType)value->type,local)||!xr_xir_value_argument(value,admission->arena,*local))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirNominalValue *record=(const XirNominalValue *)object_pointer(value);
    if(record->variant>=identity.variant_count||record->count||record->fields)return XR_XIR_VALUE_BAD_ARGUMENT;
    *variant=record->variant;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus key_enum_hash(const XrXirValue *value,XrXirValueAdmission *admission,uint64_t *output) {
    XrXirType local=XR_XIR_UNIT;uint32_t variant=0;
    XrXirValueStatus status=key_enum_receive(value,admission,&local,&variant);
    if(status!=XR_XIR_VALUE_OK)return status;
    /* Canonical identity establishes the key domain first. Hash classes inside
     * that exact domain depend on the variant, never on descriptor addresses. */
    uint8_t bytes[8];uint64_t bits=variant;
    for(uint32_t b=0;b<8;++b)bytes[b]=(uint8_t)(bits>>(b*8));
    status=key_charge(admission,9);if(status==XR_XIR_VALUE_OK)*output=xr_hash_bytes64(bytes,sizeof(bytes));
    return status;
}
static XrXirValueStatus key_enum_equal(const XrXirValue *left,const XrXirValue *right,
    XrXirValueAdmission *admission,bool *output) {
    XrXirType a=XR_XIR_UNIT,b=XR_XIR_UNIT;uint32_t av=0,bv=0;
    XrXirValueStatus status=key_enum_receive(left,admission,&a,&av);
    if(status==XR_XIR_VALUE_OK)status=key_enum_receive(right,admission,&b,&bv);
    if(status!=XR_XIR_VALUE_OK)return status;
    if(a!=b)return XR_XIR_VALUE_BAD_ARGUMENT;
    status=key_charge(admission,1);if(status==XR_XIR_VALUE_OK)*output=av==bv;
    return status;
}
