/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Receiving enum identities publish only after complete metadata allocation. */
static XrXirValueStatus enum_key_owner_sequence(ValueCompileOwner *compiler) {
    EnumKeyPool source={0},receiving={0};EnumKeyShape shape={"keys::module","Choice",0,XR_XIR_I64,false};
    XrXirValueStatus status=enum_key_pool_new(compiler,shape,&source);
    if(status!=XR_XIR_VALUE_OK)goto done;
    shape.shift=1;status=enum_key_pool_new(compiler,shape,&receiving);
    if(status!=XR_XIR_VALUE_OK)goto done;
    XirTypeReceivingRoot root={source.key,receiving.key};
    status=xir_compile_type_arena_receive(&compiler->context,receiving.arena,source.arena,&root,1);
 done:
    enum_key_pool_drop(&receiving);enum_key_pool_drop(&source);return status;
}
static void enum_key_owner_faults(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    size_t blocks=value_compile_live,bytes=value_compile_bytes,first=value_compile_calls;
    CHECK(enum_key_owner_sequence(&compiler)==XR_XIR_VALUE_OK);
    size_t sites=value_compile_calls-first;CHECK(sites>0&&value_compile_live==blocks&&value_compile_bytes==bytes);
    for(size_t point=0;point<sites;++point) {
        value_compile_injected=false;value_compile_fail_at=value_compile_calls+point;
        XrXirValueStatus status=enum_key_owner_sequence(&compiler);value_compile_fail_at=SIZE_MAX;
        CHECK(value_compile_injected&&status==XR_XIR_VALUE_OOM);
        CHECK(value_compile_live==blocks&&value_compile_bytes==bytes);
    }
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
