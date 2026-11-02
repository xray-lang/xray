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
    "var methodDefaultCalls:i64=0;var methodOverrideDefaultCalls:i64=0\n" \
    "var methodReceiverCalls:i64=0;var methodExplicitCalls:i64=0;var methodOrder:i64=0\n" \
    "fn methodDefault()->i64{methodDefaultCalls=methodDefaultCalls+1;methodOrder=methodOrder*10+3;return 1}\n" \
    "fn methodOverrideDefault()->i64{methodOverrideDefaultCalls=methodOverrideDefaultCalls+1;return 2}\n" \
    "fn methodExplicit()->i64{methodExplicitCalls=methodExplicitCalls+1;methodOrder=methodOrder*10+2;return 1}\n" \
    "class MethodDefaultBase{value:i64\ninit(value:i64){this.value=value}\n" \
    " pick(left:i64,right:i64 = methodDefault())->i64{return -1}\n}\n" \
    "class MethodDefaultChild extends MethodDefaultBase{init(){super(40)}\n" \
    " override pick(left:i64,right:i64 = methodOverrideDefault())->i64{return this.value+left+right}\n}\n" \
    "fn methodReceiver()->MethodDefaultBase{methodReceiverCalls=methodReceiverCalls+1;methodOrder=methodOrder*10+1;return MethodDefaultChild()}\n" \
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
    " assert(methodReceiver().pick(methodExplicit())==42)\n" \
    " assert(methodOrder==123 && methodReceiverCalls==1 && methodExplicitCalls==1)\n" \
    " assert(methodDefaultCalls==1 && methodOverrideDefaultCalls==0)\n" \
    " methodOrder=0;assert(methodReceiver().pick(methodExplicit(),methodExplicit())==42)\n" \
    " assert(methodOrder==122 && methodReceiverCalls==2 && methodExplicitCalls==3)\n" \
    " assert(methodDefaultCalls==1 && methodOverrideDefaultCalls==0)\n" \
    " var derived=MethodDefaultChild();assert(derived.pick(1)==43)\n" \
    " assert(methodDefaultCalls==1 && methodOverrideDefaultCalls==1)\n" \
    " const bound=methodReceiver().pick;assert(bound(1,1)==42)\n" \
    " assert(methodReceiverCalls==3 && methodDefaultCalls==1 && methodOverrideDefaultCalls==1)\n" \
    " return 42\n}\n"

static void class_inheritance_defaults_execute(XrXirProgram *program,uint32_t entry) {
    size_t before_live=cell_owner_live,before_bytes=cell_owner_bytes;
    ClassInheritanceOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,class_inheritance_output,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;XrXirInstanceResult observed={0};
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);observed=xr_xir_instance_poll_bounded(instance,64);status=observed.outcome.status;
    }
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"constructor defaults entry%u status%u panic%u value%u/%llu\n",
        entry,(unsigned)status,observed.outcome.panic.detail.code,observed.outcome.value.type,
        (unsigned long long)observed.outcome.value.payload);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);CHECK(!output.length);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(cell_owner_live==before_live && cell_owner_bytes==before_bytes);
}
#endif // CLASS_INHERITANCE_DEFAULTS_H
