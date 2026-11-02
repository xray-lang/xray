/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * class_inheritance_defaults.h - Declaration-owned defaults across field transactions
 *
 * KEY CONCEPT:
 *   A fresh instance checks default evaluation without adding any objects to
 *   the independently observed bound-receiver lifetime transaction.
 */
#ifndef CLASS_INHERITANCE_DEFAULTS_H
#define CLASS_INHERITANCE_DEFAULTS_H
#define CLASS_INHERITANCE_DEFAULT_DECLARATIONS \
    "var defaultOwnedCalls:i64=0\n" \
    "fn makeDefaultOwned()->Array<string>{defaultOwnedCalls=defaultOwnedCalls+1;return [\"default-owned-first-long-value\",\"default-owned-second-long-value\"]}\n" \
    "class DefaultOwnedBase{items:Array<string>\n" \
    " init(items:Array<string> = makeDefaultOwned()){this.items=items}\n}\n" \
    "class DefaultViaSuper extends DefaultOwnedBase{\n" \
    " init(){super()}\n}\n" \
    "class DefaultOwnChild extends DefaultOwnedBase{\n" \
    " init(items:Array<string> = makeDefaultOwned()){super(items)}\n}\n" \
    "export fn checkClassDefaults()->i64{\n" \
    " var root=DefaultOwnedBase();assert(defaultOwnedCalls==1)\n" \
    " var inherited=DefaultViaSuper();assert(defaultOwnedCalls==2)\n" \
    " var own=DefaultOwnChild();assert(defaultOwnedCalls==3)\n" \
    " var supplied=[\"explicit-owned-long-value\"];var explicit=DefaultOwnChild(supplied)\n" \
    " assert(defaultOwnedCalls==3);supplied[0]=\"caller-changed-long-value\"\n" \
    " assert(explicit.items[0]==\"explicit-owned-long-value\")\n" \
    " own.items[0]=\"derived-changed-long-value\"\n" \
    " assert(root.items[0]==\"default-owned-first-long-value\")\n" \
    " assert(inherited.items[0]==\"default-owned-first-long-value\")\n" \
    " assert(len(root.items)==2 && len(inherited.items)==2 && len(own.items)==2)\n" \
    " return 42\n}\n"

static void class_inheritance_defaults_execute(XrXirProgram *program,uint32_t entry) {
    size_t before_live=cell_owner_live,before_bytes=cell_owner_bytes;
    ClassInheritanceOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,class_inheritance_output,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,64).outcome.status;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);CHECK(!output.length);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(cell_owner_live==before_live && cell_owner_bytes==before_bytes);
}
#endif // CLASS_INHERITANCE_DEFAULTS_H
