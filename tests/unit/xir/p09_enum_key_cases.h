/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Enum keys require receiving lineage, not equal arena ordinals or pointers. */
#include "p09_enum_key_fixture.h"
static void enum_key_receivers(void) {
    ValueCompileOwner compiler={0};CHECK(value_compile_owner_new(&compiler,value_compile_limits(),65536)==XR_XIR_OK);
    EnumKeyPool source={0},receiving={0},unrelated={0},other_module={0},other_argument={0};
    EnumKeyShape shape={"keys::module","Choice",0,XR_XIR_I64,false};
    CHECK(enum_key_pool_new(&compiler,shape,&source)==XR_XIR_VALUE_OK);
    shape.shift=1;CHECK(enum_key_pool_new(&compiler,shape,&receiving)==XR_XIR_VALUE_OK);
    CHECK(enum_key_pool_new(&compiler,shape,&unrelated)==XR_XIR_VALUE_OK);
    shape.module="keys::other";CHECK(enum_key_pool_new(&compiler,shape,&other_module)==XR_XIR_VALUE_OK);
    shape.module="keys::module";shape.argument=XR_XIR_U64;
    CHECK(enum_key_pool_new(&compiler,shape,&other_argument)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={source.key,receiving.key};
    CHECK(xir_compile_type_arena_receive(&compiler.context,receiving.arena,source.arena,&root,1)==XR_XIR_VALUE_OK);
    root=(XirTypeReceivingRoot){source.key,other_module.key};
    CHECK(xir_compile_type_arena_receive(&compiler.context,other_module.arena,source.arena,&root,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    root=(XirTypeReceivingRoot){source.key,other_argument.key};
    CHECK(xir_compile_type_arena_receive(&compiler.context,other_argument.arena,source.arena,&root,1)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValue a=enum_key_value(source,0,domain),b=enum_key_value(receiving,0,domain);
    XrXirValue c=enum_key_value(receiving,1,domain),foreign=enum_key_value(unrelated,0,domain);
    XrXirValue other=enum_key_value(other_module,0,domain),phantom=enum_key_value(other_argument,0,domain);
    CHECK(a.payload!=b.payload&&a.type!=b.type&&foreign.type==b.type);
    XrXirValueAdmission admission={receiving.arena,domain,NULL,NULL,UINT64_C(10000000),65536};
    bool equal=false;uint64_t ah=0,bh=0;
    CHECK(xr_xir_key_equal(&a,&b,&admission,&equal)==XR_XIR_VALUE_OK&&equal);
    CHECK(xr_xir_key_equal(&b,&a,&admission,&equal)==XR_XIR_VALUE_OK&&equal);
    CHECK(xr_xir_key_hash(&a,&admission,&ah)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_key_hash(&b,&admission,&bh)==XR_XIR_VALUE_OK&&ah==bh);
    CHECK(xr_xir_key_equal(&a,&c,&admission,&equal)==XR_XIR_VALUE_OK&&!equal);
    equal=true;ah=77;
    CHECK(xr_xir_key_equal(&b,&foreign,&admission,&equal)==XR_XIR_VALUE_BAD_ARGUMENT&&equal);
    CHECK(xr_xir_key_equal(&b,&other,&admission,&equal)==XR_XIR_VALUE_BAD_ARGUMENT&&equal);
    CHECK(xr_xir_key_equal(&b,&phantom,&admission,&equal)==XR_XIR_VALUE_BAD_ARGUMENT&&equal);
    CHECK(xr_xir_key_hash(&foreign,&admission,&ah)==XR_XIR_VALUE_BAD_ARGUMENT&&ah==77);
    XirNominalValue *record=(XirNominalValue *)object_pointer(&b);uint32_t saved=record->variant;
    record->variant=2;XrXirValueStatus malformed=xr_xir_key_hash(&b,&admission,&ah);record->variant=saved;
    CHECK(malformed==XR_XIR_VALUE_BAD_ARGUMENT&&ah==77);
    admission.work=0;
    CHECK(xr_xir_key_hash(&a,&admission,&ah)==XR_XIR_VALUE_LIMIT&&ah==77);
    equal=true;CHECK(xr_xir_key_equal(&a,&c,&admission,&equal)==XR_XIR_VALUE_LIMIT&&equal);
    xr_xir_value_drop(&a);xr_xir_value_drop(&b);xr_xir_value_drop(&c);xr_xir_value_drop(&foreign);
    xr_xir_value_drop(&other);xr_xir_value_drop(&phantom);
    enum_key_pool_drop(&source);enum_key_pool_drop(&receiving);enum_key_pool_drop(&unrelated);
    enum_key_pool_drop(&other_module);enum_key_pool_drop(&other_argument);xr_xir_domain_drop(domain);
    value_compile_owner_release(&compiler);CHECK(!live&&!value_compile_live&&!value_compile_bytes);
}
